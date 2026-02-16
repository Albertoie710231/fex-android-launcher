/*
 * FEX Thunk ICD Shim — GIPA-only dispatch + dispatch table fixup
 *
 * ROOT CAUSE: The Vulkan loader patches *(void**)device (offset 0) after
 * vkCreateDevice, overwriting the thunk's/host driver's dispatch table.
 * When any device function is called, the host driver reads this corrupted
 * offset and crashes or hangs.
 *
 * SOLUTION:
 * 1. Use GIPA for all GDPA lookups (thunk's GDPA crashes at vkQueueSubmit)
 * 2. Save the thunk's original dispatch table from offset 0 before the
 *    loader patches it
 * 3. For all VkDevice functions, generate x86-64 trampolines that
 *    restore the dispatch before calling the thunk and undo it after
 * 4. VkQueue/VkCommandBuffer functions don't need fixup (not patched)
 *
 * Build: x86_64-linux-gnu-gcc -shared -fPIC -O2 -o libfex_thunk_icd.so
 *        fex_thunk_icd.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(void*, const char*);
typedef int VkResult;

static void* thunk_lib = NULL;
static PFN_vkGetInstanceProcAddr real_gipa = NULL;
static int init_done = 0;

/* Saved handles */
static void* saved_instance = NULL;
static void* thunk_device = NULL;
static void* thunk_device_dispatch = NULL;
static volatile int dispatch_lock = 0;

#define LOG(...) do { fprintf(stderr, "fex_thunk_icd: " __VA_ARGS__); fflush(stderr); } while(0)

static void icd_marker(const char* msg) {
    FILE* f = fopen("/tmp/icd_trace.log", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

/* ---- x86-64 trampoline generator (thread-safe with spinlock) ----
 *
 * Generates a small x86-64 code stub that:
 * 1. Saves callee-saved registers (rbx, r12, r13)
 * 2. Acquires a spinlock (dispatch_lock) via lock xchg
 * 3. Saves the device's current dispatch table from offset 0
 * 4. Writes the thunk's original dispatch to *(void**)device
 * 5. Calls the real thunk function
 * 6. Restores the loader's dispatch table
 * 7. Releases the spinlock
 * 8. Returns
 *
 * The spinlock serializes all VkDevice function calls to prevent races
 * where two threads modify *(void**)device concurrently. Without this,
 * Thread B could save the thunk dispatch (written by Thread A) instead
 * of the loader dispatch, corrupting the device state.
 *
 * Assembly (AT&T syntax):
 *   push %rbx
 *   push %r12
 *   push %r13
 *   mov  %rdi, %rbx            # save device in callee-saved
 *   movabs $<lock_ptr>, %r13   # address of dispatch_lock
 * .spin:
 *   mov  $1, %eax
 *   lock xchg (%r13), %eax     # atomic swap: [lock] <-> eax
 *   test %eax, %eax
 *   jz   .acquired
 *   pause                       # hint: spin-wait
 *   jmp  .spin
 * .acquired:
 *   mov  (%rbx), %r12          # save current dispatch
 *   movabs $<dispatch_ptr>, %rax
 *   mov  (%rax), %rax          # load thunk_device_dispatch
 *   mov  %rax, (%rbx)          # write thunk dispatch to device
 *   movabs $<real_func>, %rax
 *   call *%rax
 *   mov  %r12, (%rbx)          # restore loader dispatch
 *   movl $0, (%r13)            # release spinlock
 *   pop  %r13
 *   pop  %r12
 *   pop  %rbx
 *   ret
 */

#define TRAMPOLINE_SIZE 96  /* enough for ~84 bytes of code + padding */

static uint8_t* trampoline_pages[32] = {0};
static int trampoline_page_idx = 0;
static int trampoline_offset = 0;

static PFN_vkVoidFunction make_dispatch_trampoline(PFN_vkVoidFunction real_func) {
    if (!trampoline_pages[trampoline_page_idx] ||
        trampoline_offset + TRAMPOLINE_SIZE > 4096) {
        void* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) return real_func;
        if (trampoline_page_idx < 31) {
            trampoline_pages[++trampoline_page_idx] = page;
        } else {
            trampoline_pages[trampoline_page_idx] = page;
        }
        trampoline_offset = 0;
    }

    uint8_t* c = trampoline_pages[trampoline_page_idx] + trampoline_offset;
    int i = 0;

    /* push rbx */
    c[i++] = 0x53;
    /* push r12 */
    c[i++] = 0x41; c[i++] = 0x54;
    /* push r13 */
    c[i++] = 0x41; c[i++] = 0x55;
    /* mov rbx, rdi  (save device pointer) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0xFB;

    /* movabs r13, &dispatch_lock  (spinlock address) */
    c[i++] = 0x49; c[i++] = 0xBD;
    void* lock_addr = (void*)&dispatch_lock;
    memcpy(c + i, &lock_addr, 8); i += 8;

    /* .spin: mov eax, 1 */
    int spin_label = i;
    c[i++] = 0xB8; c[i++] = 0x01; c[i++] = 0x00; c[i++] = 0x00; c[i++] = 0x00;
    /* lock xchg [r13+0], eax  (atomic: eax <-> [lock]) */
    c[i++] = 0xF0; c[i++] = 0x41; c[i++] = 0x87; c[i++] = 0x45; c[i++] = 0x00;
    /* test eax, eax */
    c[i++] = 0x85; c[i++] = 0xC0;
    /* jz .acquired  (skip pause+jmp if lock was free) */
    c[i++] = 0x74; c[i++] = 0x04;  /* +4 bytes forward */
    /* pause  (spin-wait hint) */
    c[i++] = 0xF3; c[i++] = 0x90;
    /* jmp .spin */
    c[i] = 0xEB;
    c[i+1] = (uint8_t)(spin_label - (i + 2));  /* rel8 back to .spin */
    i += 2;

    /* .acquired: */
    /* mov r12, [rbx]  (save current dispatch) */
    c[i++] = 0x4C; c[i++] = 0x8B; c[i++] = 0x23;
    /* movabs rax, &thunk_device_dispatch */
    c[i++] = 0x48; c[i++] = 0xB8;
    void* dispatch_addr = &thunk_device_dispatch;
    memcpy(c + i, &dispatch_addr, 8); i += 8;
    /* mov rax, [rax]  (load the dispatch value) */
    c[i++] = 0x48; c[i++] = 0x8B; c[i++] = 0x00;
    /* mov [rbx], rax  (write thunk dispatch to device) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0x03;

    /* movabs rax, real_func */
    c[i++] = 0x48; c[i++] = 0xB8;
    memcpy(c + i, &real_func, 8); i += 8;
    /* call rax */
    c[i++] = 0xFF; c[i++] = 0xD0;

    /* mov [rbx], r12  (restore loader dispatch) */
    c[i++] = 0x4C; c[i++] = 0x89; c[i++] = 0x23;
    /* mov dword [r13+0], 0  (release spinlock) */
    c[i++] = 0x41; c[i++] = 0xC7; c[i++] = 0x45; c[i++] = 0x00;
    c[i++] = 0x00; c[i++] = 0x00; c[i++] = 0x00; c[i++] = 0x00;

    /* pop r13 */
    c[i++] = 0x41; c[i++] = 0x5D;
    /* pop r12 */
    c[i++] = 0x41; c[i++] = 0x5C;
    /* pop rbx */
    c[i++] = 0x5B;
    /* ret */
    c[i++] = 0xC3;

    trampoline_offset += TRAMPOLINE_SIZE;
    return (PFN_vkVoidFunction)c;
}

/* Check if a function takes VkQueue or VkCommandBuffer as first arg
 * (these don't need dispatch fixup since loader doesn't patch them) */
static int is_queue_or_cmdbuf_func(const char* pName) {
    if (strncmp(pName, "vkQueue", 7) == 0) return 1;
    if (strncmp(pName, "vkCmd", 5) == 0) return 1;
    if (strcmp(pName, "vkBeginCommandBuffer") == 0) return 1;
    if (strcmp(pName, "vkEndCommandBuffer") == 0) return 1;
    if (strcmp(pName, "vkResetCommandBuffer") == 0) return 1;
    return 0;
}

/* ---- Standard init ---- */

static void ensure_init(void) {
    if (init_done) return;
    init_done = 1;
    icd_marker("ensure_init");

    const char* paths[] = {
        "/opt/fex/share/fex-emu/GuestThunks/libvulkan-guest.so",
        "/opt/fex/share/fex-emu/GuestThunks_32/libvulkan-guest.so",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        LOG("Trying: %s\n", paths[i]);
        thunk_lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (thunk_lib) {
            LOG("Loaded FEX thunk from: %s\n", paths[i]);
            icd_marker("thunk_loaded");
            break;
        }
        LOG("Failed: %s\n", dlerror());
    }

    if (!thunk_lib) {
        LOG("ERROR: Could not load FEX Vulkan thunk!\n");
        icd_marker("thunk_load_FAILED");
        return;
    }

    real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(thunk_lib, "vkGetInstanceProcAddr");
    if (!real_gipa) {
        LOG("ERROR: vkGetInstanceProcAddr not found in thunk!\n");
        return;
    }
    LOG("vkGetInstanceProcAddr resolved: %p\n", (void*)real_gipa);
    icd_marker("init_done_ok");
}

/* ---- vkCreateInstance wrapper ---- */

typedef VkResult (*PFN_vkCreateInstance)(const void*, const void*, void**);
static PFN_vkCreateInstance real_create_instance = NULL;

static VkResult wrapped_CreateInstance(const void* pCreateInfo, const void* pAllocator, void** pInstance) {
    if (!real_create_instance) return -3;
    VkResult res = real_create_instance(pCreateInfo, pAllocator, pInstance);
    if (res == 0 && pInstance && *pInstance) {
        saved_instance = *pInstance;
        LOG("CreateInstance OK: instance=%p\n", *pInstance);
    }
    return res;
}

/* ---- vkCreateDevice wrapper ---- */

typedef VkResult (*PFN_vkCreateDevice)(void*, const void*, const void*, void**);
static PFN_vkCreateDevice real_create_device = NULL;

static VkResult wrapped_CreateDevice(void* physDev, const void* pCreateInfo,
                                     const void* pAllocator, void** pDevice) {
    if (!real_create_device) return -3;
    VkResult res = real_create_device(physDev, pCreateInfo, pAllocator, pDevice);
    if (res == 0 && pDevice && *pDevice) {
        thunk_device = *pDevice;
        thunk_device_dispatch = *(void**)*pDevice;
        LOG("CreateDevice OK: thunk_device=%p dispatch=%p\n",
            *pDevice, thunk_device_dispatch);
        icd_marker("CreateDevice_saved");
    }
    return res;
}

/* ---- vkDestroyInstance wrapper ---- */

typedef void (*PFN_vkDestroyInstance)(void*, const void*);
static PFN_vkDestroyInstance real_destroy_instance = NULL;

static void wrapped_DestroyInstance(void* instance, const void* pAllocator) {
    if (real_destroy_instance) real_destroy_instance(instance, pAllocator);
    saved_instance = NULL;
}

/* ---- vkDestroyDevice wrapper ---- */

typedef void (*PFN_vkDestroyDevice)(void*, const void*);

static PFN_vkVoidFunction wrapped_DestroyDevice_fn = NULL;

/* DestroyDevice needs special handling: acquire lock, restore dispatch, call, clear state */
static void wrapped_DestroyDevice(void* device, const void* pAllocator) {
    /* Acquire spinlock — prevent other threads from using the device mid-destroy */
    while (__sync_lock_test_and_set(&dispatch_lock, 1)) { /* spin */ }

    /* Restore dispatch for the destroy call */
    if (thunk_device && thunk_device_dispatch)
        *(void**)thunk_device = thunk_device_dispatch;
    PFN_vkDestroyDevice fn = (PFN_vkDestroyDevice)wrapped_DestroyDevice_fn;
    if (fn) fn(thunk_device ? thunk_device : device, pAllocator);
    thunk_device = NULL;
    thunk_device_dispatch = NULL;

    /* Release spinlock */
    __sync_lock_release(&dispatch_lock);
}

/* ---- vkGetDeviceProcAddr: GIPA-based + dispatch trampolines ---- */

static int gdpa_count = 0;

static PFN_vkVoidFunction wrapped_GDPA(void* device, const char* pName) {
    if (!pName) return NULL;
    gdpa_count++;

    /* Use GIPA for all lookups — thunk's GDPA crashes */
    PFN_vkVoidFunction fn = NULL;
    if (real_gipa && saved_instance) {
        fn = real_gipa(saved_instance, pName);
    }
    if (!fn && thunk_lib) {
        fn = (PFN_vkVoidFunction)dlsym(thunk_lib, pName);
    }

    /* Self-reference */
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)wrapped_GDPA;
    }

    /* DestroyDevice needs special cleanup */
    if (strcmp(pName, "vkDestroyDevice") == 0 && fn) {
        wrapped_DestroyDevice_fn = fn;
        return (PFN_vkVoidFunction)wrapped_DestroyDevice;
    }

    if (!fn) return NULL;

    /* VkQueue and VkCommandBuffer functions: no dispatch fixup needed */
    if (is_queue_or_cmdbuf_func(pName)) {
        if (gdpa_count <= 5 || strncmp(pName, "vkQueue", 7) == 0)
            LOG("GDPA[%d]: %s -> %p (no fixup)\n", gdpa_count, pName, (void*)fn);
        return fn;
    }

    /* All other device functions: generate dispatch-fixing trampoline */
    PFN_vkVoidFunction tramp = make_dispatch_trampoline(fn);
    if (gdpa_count <= 10 ||
        strncmp(pName, "vkGetDeviceQueue", 16) == 0 ||
        strncmp(pName, "vkCreate", 8) == 0) {
        LOG("GDPA[%d]: %s -> %p (trampoline=%p)\n", gdpa_count, pName,
            (void*)fn, (void*)tramp);
    }
    return tramp;
}

/* ---- ICD entry points ---- */

__attribute__((visibility("default")))
uint32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    ensure_init();
    LOG("NegotiateVersion: %u\n", *pVersion);
    if (*pVersion > 5) *pVersion = 5;
    return 0;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(void *instance, const char *pName) {
    ensure_init();
    if (!real_gipa || !pName) return NULL;

    if (strcmp(pName, "vkCreateInstance") == 0) {
        real_create_instance = (PFN_vkCreateInstance)real_gipa(instance, pName);
        return (PFN_vkVoidFunction)wrapped_CreateInstance;
    }
    if (strcmp(pName, "vkDestroyInstance") == 0) {
        real_destroy_instance = (PFN_vkDestroyInstance)real_gipa(instance, pName);
        return (PFN_vkVoidFunction)wrapped_DestroyInstance;
    }
    if (strcmp(pName, "vkCreateDevice") == 0) {
        real_create_device = (PFN_vkCreateDevice)real_gipa(instance, pName);
        LOG("GIPA: vkCreateDevice -> %p\n", (void*)real_create_device);
        return (PFN_vkVoidFunction)wrapped_CreateDevice;
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        LOG("GIPA: vkGetDeviceProcAddr -> wrapped_GDPA\n");
        return (PFN_vkVoidFunction)wrapped_GDPA;
    }

    return real_gipa(instance, pName);
}

__attribute__((visibility("default")))
void* vk_icdGetPhysicalDeviceProcAddr(void *instance, const char *pName) {
    (void)instance; (void)pName;
    return NULL;
}

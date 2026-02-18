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
#include <unistd.h>
#include <sys/syscall.h>

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

/* Per-device dispatch table: maps device handles to their original ICD dispatch pointers.
 * Each device from the ICD gets its own dispatch table allocated during vkCreateDevice.
 * Trampolines must restore the CORRECT dispatch for the specific device being called,
 * because the thunk may use the dispatch pointer to identify the host-side device. */
#define MAX_TRACKED_DEVICES 8
static struct {
    void* device;
    void* dispatch;
} device_dispatch_table[MAX_TRACKED_DEVICES];
static int device_dispatch_count = 0;

/* Look up the correct dispatch pointer for a specific device handle.
 * Called from x86-64 trampoline machine code via function pointer. */
static void* get_dispatch_for_device(void* device) {
    for (int i = 0; i < device_dispatch_count; i++) {
        if (device_dispatch_table[i].device == device)
            return device_dispatch_table[i].dispatch;
    }
    return thunk_device_dispatch;  /* fallback */
}

static void register_device_dispatch(void* device, void* dispatch) {
    for (int i = 0; i < device_dispatch_count; i++) {
        if (device_dispatch_table[i].device == device) {
            device_dispatch_table[i].dispatch = dispatch;
            return;
        }
    }
    if (device_dispatch_count < MAX_TRACKED_DEVICES) {
        device_dispatch_table[device_dispatch_count].device = device;
        device_dispatch_table[device_dispatch_count].dispatch = dispatch;
        device_dispatch_count++;
    } else {
        fprintf(stderr, "fex_thunk_icd: WARNING: device table full, can't track %p\n", device);
    }
}

static void remove_device_dispatch(void* device) {
    for (int i = 0; i < device_dispatch_count; i++) {
        if (device_dispatch_table[i].device == device) {
            for (int j = i; j < device_dispatch_count - 1; j++)
                device_dispatch_table[j] = device_dispatch_table[j + 1];
            device_dispatch_count--;
            return;
        }
    }
}

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

#define TRAMPOLINE_SIZE 128  /* enough for ~105 bytes of code + padding */

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
    /* mov r12, [rbx]  (save current dispatch — loader's) */
    c[i++] = 0x4C; c[i++] = 0x8B; c[i++] = 0x23;

    /* Per-device dispatch lookup: call get_dispatch_for_device(device)
     * Save all argument registers, call C function, restore args.
     * Result in rax is the correct ICD dispatch for this specific device. */
    c[i++] = 0x57;                    /* push rdi */
    c[i++] = 0x56;                    /* push rsi */
    c[i++] = 0x52;                    /* push rdx */
    c[i++] = 0x51;                    /* push rcx */
    c[i++] = 0x41; c[i++] = 0x50;    /* push r8  */
    c[i++] = 0x41; c[i++] = 0x51;    /* push r9  */
    /* mov rdi, rbx  (device pointer as arg) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0xDF;
    /* movabs rax, get_dispatch_for_device */
    c[i++] = 0x48; c[i++] = 0xB8;
    void* lookup_fn = (void*)get_dispatch_for_device;
    memcpy(c + i, &lookup_fn, 8); i += 8;
    /* call rax */
    c[i++] = 0xFF; c[i++] = 0xD0;
    /* Restore argument registers (rax = dispatch result, untouched by pops) */
    c[i++] = 0x41; c[i++] = 0x59;    /* pop r9  */
    c[i++] = 0x41; c[i++] = 0x58;    /* pop r8  */
    c[i++] = 0x59;                    /* pop rcx */
    c[i++] = 0x5A;                    /* pop rdx */
    c[i++] = 0x5E;                    /* pop rsi */
    c[i++] = 0x5F;                    /* pop rdi */
    /* mov [rbx], rax  (write correct ICD dispatch to device) */
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

/* Lock-free trampoline for VkCommandBuffer functions.
 * Same dispatch fixup as make_dispatch_trampoline but WITHOUT spinlock.
 * Safe because Vulkan spec requires external synchronization for command buffers
 * (each command buffer is used by at most one thread at a time).
 * This avoids serializing the thousands of vkCmd* calls per frame. */
static PFN_vkVoidFunction make_dispatch_trampoline_nolock(PFN_vkVoidFunction real_func) {
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

    /* Prologue: save callee-saved registers + align stack (3 pushes = 24 bytes,
     * entry rsp ≡ 8 mod 16, after 3 pushes rsp ≡ 0 mod 16 for C call) */
    c[i++] = 0x53;                    /* push rbx */
    c[i++] = 0x41; c[i++] = 0x54;    /* push r12 */
    c[i++] = 0x41; c[i++] = 0x55;    /* push r13 (for alignment) */
    /* mov rbx, rdi  (save handle pointer — VkCommandBuffer) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0xFB;
    /* mov r12, [rbx]  (save current dispatch — loader's) */
    c[i++] = 0x4C; c[i++] = 0x8B; c[i++] = 0x23;

    /* Save argument registers for C function call */
    c[i++] = 0x57;                    /* push rdi */
    c[i++] = 0x56;                    /* push rsi */
    c[i++] = 0x52;                    /* push rdx */
    c[i++] = 0x51;                    /* push rcx */
    c[i++] = 0x41; c[i++] = 0x50;    /* push r8  */
    c[i++] = 0x41; c[i++] = 0x51;    /* push r9  */
    /* mov rdi, rbx  (handle pointer as arg) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0xDF;
    /* movabs rax, get_dispatch_for_device */
    c[i++] = 0x48; c[i++] = 0xB8;
    void* lookup_fn = (void*)get_dispatch_for_device;
    memcpy(c + i, &lookup_fn, 8); i += 8;
    /* call rax */
    c[i++] = 0xFF; c[i++] = 0xD0;
    /* Restore argument registers (rax = dispatch result, untouched by pops) */
    c[i++] = 0x41; c[i++] = 0x59;    /* pop r9  */
    c[i++] = 0x41; c[i++] = 0x58;    /* pop r8  */
    c[i++] = 0x59;                    /* pop rcx */
    c[i++] = 0x5A;                    /* pop rdx */
    c[i++] = 0x5E;                    /* pop rsi */
    c[i++] = 0x5F;                    /* pop rdi */
    /* mov [rbx], rax  (write correct ICD dispatch to handle) */
    c[i++] = 0x48; c[i++] = 0x89; c[i++] = 0x03;

    /* movabs rax, real_func */
    c[i++] = 0x48; c[i++] = 0xB8;
    memcpy(c + i, &real_func, 8); i += 8;
    /* call rax */
    c[i++] = 0xFF; c[i++] = 0xD0;

    /* mov [rbx], r12  (restore loader dispatch) */
    c[i++] = 0x4C; c[i++] = 0x89; c[i++] = 0x23;

    /* Epilogue */
    c[i++] = 0x41; c[i++] = 0x5D;    /* pop r13 */
    c[i++] = 0x41; c[i++] = 0x5C;    /* pop r12 */
    c[i++] = 0x5B;                    /* pop rbx */
    c[i++] = 0xC3;                    /* ret */

    trampoline_offset += TRAMPOLINE_SIZE;
    return (PFN_vkVoidFunction)c;
}

/* Check if a function takes VkCommandBuffer as first arg.
 * These get lock-free trampolines (not locked ones) for dispatch fixup. */
static int is_cmdbuf_func(const char* pName) {
    if (strncmp(pName, "vkCmd", 5) == 0) return 1;
    if (strcmp(pName, "vkBeginCommandBuffer") == 0) return 1;
    if (strcmp(pName, "vkEndCommandBuffer") == 0) return 1;
    if (strcmp(pName, "vkResetCommandBuffer") == 0) return 1;
    return 0;
}

/* ---- Diagnostic: logged wrappers for command buffer functions ---- */

static inline long get_tid(void) { return syscall(SYS_gettid); }

typedef VkResult (*PFN_vkBeginCmdBuf)(void*, const void*);
typedef VkResult (*PFN_vkEndCmdBuf)(void*);
typedef VkResult (*PFN_vkResetCmdBuf)(void*, uint32_t);

static PFN_vkBeginCmdBuf real_begin_cmdbuf = NULL;
static PFN_vkEndCmdBuf real_end_cmdbuf = NULL;
static PFN_vkResetCmdBuf real_reset_cmdbuf = NULL;
static volatile int begin_cmdbuf_count = 0;

static VkResult logged_BeginCommandBuffer(void* cmdBuf, const void* pBeginInfo) {
    long tid = get_tid();
    int n = __sync_add_and_fetch(&begin_cmdbuf_count, 1);
    LOG("[tid=%ld] vkBeginCommandBuffer #%d ENTER cmdBuf=%p dispatch_at_0=%p lock=%d\n",
        tid, n, cmdBuf, cmdBuf ? *(void**)cmdBuf : NULL, dispatch_lock);
    VkResult r = real_begin_cmdbuf(cmdBuf, pBeginInfo);
    LOG("[tid=%ld] vkBeginCommandBuffer #%d EXIT result=%d\n", tid, n, r);
    return r;
}

static VkResult logged_EndCommandBuffer(void* cmdBuf) {
    long tid = get_tid();
    LOG("[tid=%ld] vkEndCommandBuffer cmdBuf=%p\n", tid, cmdBuf);
    VkResult r = real_end_cmdbuf(cmdBuf);
    LOG("[tid=%ld] vkEndCommandBuffer EXIT result=%d\n", tid, r);
    return r;
}

static VkResult logged_ResetCommandBuffer(void* cmdBuf, uint32_t flags) {
    long tid = get_tid();
    LOG("[tid=%ld] vkResetCommandBuffer cmdBuf=%p flags=%u\n", tid, cmdBuf, flags);
    VkResult r = real_reset_cmdbuf(cmdBuf, flags);
    LOG("[tid=%ld] vkResetCommandBuffer EXIT result=%d\n", tid, r);
    return r;
}

/* ---- Diagnostic: vkMapMemory / vkAllocateMemory / vkGetDeviceQueue ---- */

typedef VkResult (*PFN_vkMapMemory)(void* device, uint64_t memory, uint64_t offset,
                                     uint64_t size, uint32_t flags, void** ppData);
typedef VkResult (*PFN_vkAllocMemory)(void* device, const void* pAllocInfo,
                                       const void* pAllocator, uint64_t* pMemory);
typedef void (*PFN_vkGetDeviceQueue)(void* device, uint32_t qfi, uint32_t qi, void** pQueue);

static PFN_vkMapMemory real_map_memory = NULL;
static PFN_vkAllocMemory real_alloc_memory = NULL;
static PFN_vkGetDeviceQueue real_get_device_queue = NULL;

static VkResult logged_MapMemory(void* device, uint64_t memory, uint64_t offset,
                                  uint64_t size, uint32_t flags, void** ppData) {
    LOG("vkMapMemory: dev=%p mem=0x%llx off=%llu size=%llu flags=0x%x\n",
        device, (unsigned long long)memory, (unsigned long long)offset,
        (unsigned long long)size, flags);
    VkResult r = real_map_memory(device, memory, offset, size, flags, ppData);
    LOG("vkMapMemory: result=%d ppData=%p\n", r, ppData ? *ppData : NULL);
    return r;
}

static VkResult logged_AllocateMemory(void* device, const void* pAllocInfo,
                                       const void* pAllocator, uint64_t* pMemory) {
    /* VkMemoryAllocateInfo: sType(4) + pad(4) + pNext(8) + allocationSize(8) + memoryTypeIndex(4)
     * On x86-64: offset 0=sType, 8=pNext(ptr), 16=allocationSize, 24=memoryTypeIndex */
    uint64_t allocSize = 0;
    uint32_t memTypeIdx = 0;
    if (pAllocInfo) {
        allocSize = *(const uint64_t*)((const char*)pAllocInfo + 16);
        memTypeIdx = *(const uint32_t*)((const char*)pAllocInfo + 24);
    }
    LOG("vkAllocateMemory: dev=%p size=%llu typeIdx=%u\n",
        device, (unsigned long long)allocSize, memTypeIdx);
    VkResult r = real_alloc_memory(device, pAllocInfo, pAllocator, pMemory);
    LOG("vkAllocateMemory: result=%d mem=0x%llx\n", r, pMemory ? (unsigned long long)*pMemory : 0);
    return r;
}

static void logged_GetDeviceQueue(void* device, uint32_t qfi, uint32_t qi, void** pQueue) {
    real_get_device_queue(device, qfi, qi, pQueue);
    LOG("vkGetDeviceQueue: dev=%p qfi=%u qi=%u queue=%p dispatch=%p\n",
        device, qfi, qi, pQueue ? *pQueue : NULL,
        (pQueue && *pQueue) ? *(void**)*pQueue : NULL);
}

/* ---- Diagnostic: logged wrapper for vkAllocateCommandBuffers ---- */

typedef VkResult (*PFN_vkAllocCmdBufs)(void* device, const void* pAllocInfo,
                                        void** pCmdBufs);
static PFN_vkAllocCmdBufs real_alloc_cmdbufs = NULL;

static VkResult logged_AllocateCommandBuffers(void* device, const void* pAllocInfo,
                                               void** pCmdBufs) {
    /* VkCommandBufferAllocateInfo on x86-64:
     * offset 0: sType(4), offset 8: pNext(8), offset 16: commandPool(8),
     * offset 24: level(4), offset 28: commandBufferCount(4) */
    uint64_t pool = 0;
    uint32_t count = 0;
    if (pAllocInfo) {
        pool = *(const uint64_t*)((const char*)pAllocInfo + 16);
        count = *(const uint32_t*)((const char*)pAllocInfo + 28);
    }
    LOG("vkAllocateCommandBuffers: dev=%p pool=0x%llx count=%u dispatch@0=%p\n",
        device, (unsigned long long)pool, count,
        device ? *(void**)device : NULL);
    icd_marker("ICD_ACB_ENTER");
    VkResult r = real_alloc_cmdbufs(device, pAllocInfo, pCmdBufs);
    LOG("vkAllocateCommandBuffers: result=%d cmdBuf0=%p\n", r,
        (pCmdBufs && count > 0) ? pCmdBufs[0] : NULL);
    char buf[128];
    snprintf(buf, sizeof(buf), "ICD_ACB_RESULT=%d", r);
    icd_marker(buf);
    return r;
}

/* ---- Diagnostic: logged wrapper for vkCreateCommandPool ---- */

typedef VkResult (*PFN_vkCreateCmdPool)(void* device, const void* pCreateInfo,
                                         const void* pAllocator, uint64_t* pPool);
static PFN_vkCreateCmdPool real_create_cmdpool = NULL;

static VkResult logged_CreateCommandPool(void* device, const void* pCreateInfo,
                                          const void* pAllocator, uint64_t* pPool) {
    LOG("vkCreateCommandPool: dev=%p dispatch@0=%p\n",
        device, device ? *(void**)device : NULL);
    icd_marker("ICD_CCP_ENTER");
    VkResult r = real_create_cmdpool(device, pCreateInfo, pAllocator, pPool);
    LOG("vkCreateCommandPool: result=%d pool=0x%llx\n", r,
        pPool ? (unsigned long long)*pPool : 0);
    char buf[128];
    snprintf(buf, sizeof(buf), "ICD_CCP_RESULT=%d pool=0x%llx", r,
             pPool ? (unsigned long long)*pPool : 0);
    icd_marker(buf);
    return r;
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
        void* new_dispatch = *(void**)*pDevice;
        /* Register per-device dispatch: each device gets its own ICD dispatch pointer.
         * The thunk allocates a separate dispatch table per device, so the pointer
         * at *(void**)device differs between devices even from the same ICD.
         * Trampolines must restore the CORRECT dispatch for each specific device. */
        register_device_dispatch(*pDevice, new_dispatch);
        thunk_device_dispatch = new_dispatch;  /* always update fallback to latest device */
        thunk_device = *pDevice;
        LOG("CreateDevice OK: device=%p dispatch=%p (tracked=%d)\n",
            *pDevice, new_dispatch, device_dispatch_count);
        icd_marker("CreateDevice_saved");
    }
    return res;
}

/* ---- vkDestroyInstance wrapper ---- */

typedef void (*PFN_vkDestroyInstance)(void*, const void*);
static PFN_vkDestroyInstance real_destroy_instance = NULL;

static void wrapped_DestroyInstance(void* instance, const void* pAllocator) {
    if (real_destroy_instance) real_destroy_instance(instance, pAllocator);
    /* Only clear saved_instance if THIS is the one we saved — other instances
     * (e.g., watchdog probe) should not clobber DXVK's active instance. */
    if (instance == saved_instance)
        saved_instance = NULL;
}

/* ---- vkDestroyDevice wrapper ---- */

typedef void (*PFN_vkDestroyDevice)(void*, const void*);

static PFN_vkVoidFunction wrapped_DestroyDevice_fn = NULL;

/* DestroyDevice needs special handling: acquire lock, restore correct per-device
 * dispatch, call thunk's destroy, remove from tracking table.
 *
 * CRITICAL: Must restore THIS device's own dispatch pointer (not another device's),
 * because the thunk may use the dispatch pointer to identify the host-side device. */
static void wrapped_DestroyDevice(void* device, const void* pAllocator) {
    while (__sync_lock_test_and_set(&dispatch_lock, 1)) { /* spin */ }

    LOG("DestroyDevice: device=%p thunk_device=%p tracked=%d\n",
        device, thunk_device, device_dispatch_count);

    /* Restore THIS device's own ICD dispatch before destroying */
    if (device) {
        void* dispatch = get_dispatch_for_device(device);
        if (dispatch) {
            LOG("DestroyDevice: restoring dispatch=%p for device=%p\n", dispatch, device);
            *(void**)device = dispatch;
        }
    }

    PFN_vkDestroyDevice fn = (PFN_vkDestroyDevice)wrapped_DestroyDevice_fn;
    if (fn) fn(device, pAllocator);

    /* Remove from per-device tracking table */
    remove_device_dispatch(device);

    /* Update fallback dispatch to a surviving device's dispatch (avoid dangling pointer) */
    if (device_dispatch_count > 0)
        thunk_device_dispatch = device_dispatch_table[0].dispatch;

    if (device == thunk_device)
        thunk_device = NULL;

    LOG("DestroyDevice: done, tracked=%d remaining, fallback=%p\n",
        device_dispatch_count, thunk_device_dispatch);
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

    /* Block extensions that Wine misuses — placed memory mapping crashes through
     * FEX thunks. Wine's wine_vkMapMemory checks if p_vkMapMemory2KHR is non-NULL
     * (via GDPA), and if so, uses VK_MEMORY_MAP_PLACED_BIT_EXT for ALL mappings.
     * Since our GIPA returns non-NULL for these (thunk exposes them), Wine thinks
     * placed mapping is available, but it crashes because Vortek/thunks don't
     * properly support VK_EXT_map_memory_placed. Returning NULL forces Wine to
     * fall back to standard vkMapMemory, which works fine through our trampolines. */
    if (strcmp(pName, "vkMapMemory2KHR") == 0 ||
        strcmp(pName, "vkUnmapMemory2KHR") == 0) {
        LOG("GDPA: %s -> NULL (blocked: placed memory not supported through thunks)\n", pName);
        return NULL;
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

    if (!fn) {
        /* Log NULL returns for important functions — thunk may not expose them */
        if (strncmp(pName, "vkMap", 5) == 0 || strncmp(pName, "vkAlloc", 7) == 0 ||
            strncmp(pName, "vkFree", 6) == 0 || strncmp(pName, "vkUnmap", 7) == 0 ||
            strncmp(pName, "vkFlush", 7) == 0 || strncmp(pName, "vkInvalidate", 12) == 0 ||
            strncmp(pName, "vkBind", 6) == 0 || strncmp(pName, "vkGet", 5) == 0 ||
            strncmp(pName, "vkCreate", 8) == 0 || strncmp(pName, "vkDestroy", 9) == 0 ||
            strncmp(pName, "vkQueue", 7) == 0) {
            LOG("GDPA[%d]: %s -> NULL (thunk doesn't expose!)\n", gdpa_count, pName);
        }
        return NULL;
    }

    /* VkCommandBuffer functions: need lock-free dispatch fixup (loader patches
     * *(void**)cmdBuf just like device/queue) + diagnostic logging for key ones. */
    if (strcmp(pName, "vkBeginCommandBuffer") == 0) {
        real_begin_cmdbuf = (PFN_vkBeginCmdBuf)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline_nolock((PFN_vkVoidFunction)logged_BeginCommandBuffer);
        LOG("GDPA: vkBeginCommandBuffer -> %p (logged+nolock tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (strcmp(pName, "vkEndCommandBuffer") == 0) {
        real_end_cmdbuf = (PFN_vkEndCmdBuf)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline_nolock((PFN_vkVoidFunction)logged_EndCommandBuffer);
        LOG("GDPA: vkEndCommandBuffer -> %p (logged+nolock tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (strcmp(pName, "vkResetCommandBuffer") == 0) {
        real_reset_cmdbuf = (PFN_vkResetCmdBuf)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline_nolock((PFN_vkVoidFunction)logged_ResetCommandBuffer);
        LOG("GDPA: vkResetCommandBuffer -> %p (logged+nolock tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (is_cmdbuf_func(pName)) {
        PFN_vkVoidFunction tramp = make_dispatch_trampoline_nolock(fn);
        if (gdpa_count <= 5)
            LOG("GDPA[%d]: %s -> %p (nolock tramp=%p)\n", gdpa_count, pName, (void*)fn, (void*)tramp);
        return tramp;
    }

    /* Diagnostic: logged wrappers for memory and queue operations */
    if (strcmp(pName, "vkMapMemory") == 0) {
        real_map_memory = (PFN_vkMapMemory)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline((PFN_vkVoidFunction)logged_MapMemory);
        LOG("GDPA: vkMapMemory -> %p (logged tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (strcmp(pName, "vkAllocateMemory") == 0) {
        real_alloc_memory = (PFN_vkAllocMemory)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline((PFN_vkVoidFunction)logged_AllocateMemory);
        LOG("GDPA: vkAllocateMemory -> %p (logged tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (strcmp(pName, "vkGetDeviceQueue") == 0) {
        real_get_device_queue = (PFN_vkGetDeviceQueue)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline((PFN_vkVoidFunction)logged_GetDeviceQueue);
        LOG("GDPA: vkGetDeviceQueue -> %p (logged tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }

    /* Diagnostic: logged wrappers for command buffer and pool operations */
    if (strcmp(pName, "vkAllocateCommandBuffers") == 0) {
        real_alloc_cmdbufs = (PFN_vkAllocCmdBufs)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline((PFN_vkVoidFunction)logged_AllocateCommandBuffers);
        LOG("GDPA: vkAllocateCommandBuffers -> %p (logged tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }
    if (strcmp(pName, "vkCreateCommandPool") == 0) {
        real_create_cmdpool = (PFN_vkCreateCmdPool)fn;
        PFN_vkVoidFunction tramp = make_dispatch_trampoline((PFN_vkVoidFunction)logged_CreateCommandPool);
        LOG("GDPA: vkCreateCommandPool -> %p (logged tramp=%p)\n", (void*)fn, (void*)tramp);
        return tramp;
    }

    /* All other device/queue functions: generate dispatch-fixing trampoline.
     * VkQueue functions need this too — the loader patches *(void**)queue. */
    PFN_vkVoidFunction tramp = make_dispatch_trampoline(fn);
    /* Log ALL GDPA lookups — essential for diagnosing PE→Unix assertion failures */
    LOG("GDPA[%d]: %s -> %p (trampoline=%p)\n", gdpa_count, pName,
        (void*)fn, (void*)tramp);
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

/* SDL3 stub: key functions return success values
 * v3: debug logging + fix SDL_GetCurrentVideoDriver, SDL_GetWindowFlags */
#include <stddef.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
static const char empty_str[] = "";
static const char revision_str[] = "stub";
static const char video_driver[] = "x11";
static const char platform_str[] = "Linux";

/* Debug logging to stderr (goes to logcat as FexOutput) */
static void sdl_log(const char *msg) {
    write(2, msg, strlen(msg));
}

/* Fake SDL_Window — just needs to be non-NULL */
static char fake_window[256];
/* Fake SDL_Renderer — just needs to be non-NULL */
static char fake_renderer[256];
/* Fake SDL_DisplayMode */
typedef struct { unsigned int format; int w; int h; float refresh_rate; float pixel_density; void *internal; } SDL_DisplayMode;
static SDL_DisplayMode fake_display_mode = { 0x16462004, 1920, 1080, 60.0f, 1.0f, 0 };
/* Fake display ID */
typedef unsigned int SDL_DisplayID;
static SDL_DisplayID fake_displays[] = { 1, 0 };

/* v2: Real X11 window for CEF browser creation.
 * Steam calls SDL_GetPointerProperty(props, "SDL.window.x11.window", NULL)
 * to get the X11 window handle. Without it, CEF can't create a browser. */
static unsigned long real_x11_window = 0;
static void *real_x11_display = NULL;

static void ensure_x11_window(void) {
    if (real_x11_window) return;
    /* Dynamically resolve X11 functions */
    void *libx11 = dlopen("libX11.so.6", 2); /* RTLD_NOW */
    if (!libx11) return;
    void *(*pXOpenDisplay)(const char *) = dlsym(libx11, "XOpenDisplay");
    unsigned long (*pXCreateSimpleWindow)(void *, unsigned long, int, int,
        unsigned int, unsigned int, unsigned int, unsigned long, unsigned long)
        = dlsym(libx11, "XCreateSimpleWindow");
    unsigned long (*pXRootWindow)(void *, int) = dlsym(libx11, "XRootWindow");
    if (!pXOpenDisplay || !pXCreateSimpleWindow || !pXRootWindow) return;

    real_x11_display = pXOpenDisplay(NULL);
    if (!real_x11_display) return;
    unsigned long root = pXRootWindow(real_x11_display, 0);
    real_x11_window = pXCreateSimpleWindow(real_x11_display, root,
        0, 0, 1920, 1080, 0, 0, 0);
}

void *JNI_OnLoad(void) { return 0; }
void *SDL_abs(void) { return 0; }
void *SDL_acos(void) { return 0; }
void *SDL_acosf(void) { return 0; }
void *SDL_AcquireCameraFrame(void) { return 0; }
void *SDL_AcquireGPUCommandBuffer(void) { return 0; }
void *SDL_AcquireGPUSwapchainTexture(void) { return 0; }
void *SDL_AddAtomicInt(void) { return 0; }
void *SDL_AddAtomicU32(void) { return 0; }
void *SDL_AddEventWatch(void) { return 0; }
void *SDL_AddGamepadMapping(void) { return 0; }
void *SDL_AddGamepadMappingsFromFile(void) { return 0; }
void *SDL_AddGamepadMappingsFromIO(void) { return 0; }
void *SDL_AddHintCallback(void) { return 0; }
void *SDL_AddSurfaceAlternateImage(void) { return 0; }
void *SDL_AddTimer(void) { return 0; }
void *SDL_AddTimerNS(void) { return 0; }
void *SDL_AddVulkanRenderSemaphores(void) { return 0; }
void *SDL_aligned_alloc(void) { return 0; }
void *SDL_aligned_free(void) { return 0; }
void *SDL_asin(void) { return 0; }
void *SDL_asinf(void) { return 0; }
void *SDL_asprintf(void) { return 0; }
void *SDL_AsyncIOFromFile(void) { return 0; }
void *SDL_atan(void) { return 0; }
void *SDL_atan2(void) { return 0; }
void *SDL_atan2f(void) { return 0; }
void *SDL_atanf(void) { return 0; }
void *SDL_atof(void) { return 0; }
void *SDL_atoi(void) { return 0; }
void *SDL_AttachVirtualJoystick(void) { return 0; }
void *SDL_AudioDevicePaused(void) { return 0; }
void *SDL_AudioStreamDevicePaused(void) { return 0; }
void *SDL_BeginGPUComputePass(void) { return 0; }
void *SDL_BeginGPUCopyPass(void) { return 0; }
void *SDL_BeginGPURenderPass(void) { return 0; }
void *SDL_BindAudioStream(void) { return 0; }
void *SDL_BindAudioStreams(void) { return 0; }
void *SDL_BindGPUComputePipeline(void) { return 0; }
void *SDL_BindGPUComputeSamplers(void) { return 0; }
void *SDL_BindGPUComputeStorageBuffers(void) { return 0; }
void *SDL_BindGPUComputeStorageTextures(void) { return 0; }
void *SDL_BindGPUFragmentSamplers(void) { return 0; }
void *SDL_BindGPUFragmentStorageBuffers(void) { return 0; }
void *SDL_BindGPUFragmentStorageTextures(void) { return 0; }
void *SDL_BindGPUGraphicsPipeline(void) { return 0; }
void *SDL_BindGPUIndexBuffer(void) { return 0; }
void *SDL_BindGPUVertexBuffers(void) { return 0; }
void *SDL_BindGPUVertexSamplers(void) { return 0; }
void *SDL_BindGPUVertexStorageBuffers(void) { return 0; }
void *SDL_BindGPUVertexStorageTextures(void) { return 0; }
void *SDL_BlitGPUTexture(void) { return 0; }
void *SDL_BlitSurface(void) { return 0; }
void *SDL_BlitSurface9Grid(void) { return 0; }
void *SDL_BlitSurfaceScaled(void) { return 0; }
void *SDL_BlitSurfaceTiled(void) { return 0; }
void *SDL_BlitSurfaceTiledWithScale(void) { return 0; }
void *SDL_BlitSurfaceUnchecked(void) { return 0; }
void *SDL_BlitSurfaceUncheckedScaled(void) { return 0; }
void *SDL_BroadcastCondition(void) { return 0; }
void *SDL_bsearch(void) { return 0; }
void *SDL_bsearch_r(void) { return 0; }
void *SDL_CalculateGPUTextureFormatSize(void) { return 0; }
void *SDL_calloc(void) { return 0; }
void *SDL_CancelGPUCommandBuffer(void) { return 0; }
void *SDL_CaptureMouse(void) { return 0; }
void *SDL_ceil(void) { return 0; }
void *SDL_ceilf(void) { return 0; }
void *SDL_ClaimWindowForGPUDevice(void) { return 0; }
void *SDL_CleanupTLS(void) { return 0; }
void *SDL_ClearAudioStream(void) { return 0; }
void *SDL_ClearClipboardData(void) { return 0; }
void *SDL_ClearComposition(void) { return 0; }
void *SDL_ClearError(void) { return 0; }
void *SDL_ClearProperty(void) { return 0; }
void *SDL_ClearSurface(void) { return 0; }
void *SDL_ClickTrayEntry(void) { return 0; }
void *SDL_CloseAsyncIO(void) { return 0; }
void *SDL_CloseAudioDevice(void) { return 0; }
void *SDL_CloseCamera(void) { return 0; }
void *SDL_CloseGamepad(void) { return 0; }
void *SDL_CloseHaptic(void) { return 0; }
void *SDL_CloseIO(void) { return 0; }
void *SDL_CloseJoystick(void) { return 0; }
void *SDL_CloseSensor(void) { return 0; }
void *SDL_CloseStorage(void) { return 0; }
void *SDL_CompareAndSwapAtomicInt(void) { return 0; }
void *SDL_CompareAndSwapAtomicPointer(void) { return 0; }
void *SDL_CompareAndSwapAtomicU32(void) { return 0; }
void *SDL_ComposeCustomBlendMode(void) { return 0; }
void *SDL_ConvertAudioSamples(void) { return 0; }
void *SDL_ConvertEventToRenderCoordinates(void) { return 0; }
void *SDL_ConvertPixels(void) { return 0; }
void *SDL_ConvertPixelsAndColorspace(void) { return 0; }
void *SDL_ConvertSurface(void) { return 0; }
void *SDL_ConvertSurfaceAndColorspace(void) { return 0; }
void *SDL_CopyFile(void) { return 0; }
void *SDL_CopyGPUBufferToBuffer(void) { return 0; }
void *SDL_CopyGPUTextureToTexture(void) { return 0; }
void *SDL_CopyProperties(void) { return 0; }
void *SDL_copysign(void) { return 0; }
void *SDL_copysignf(void) { return 0; }
void *SDL_CopyStorageFile(void) { return 0; }
void *SDL_cos(void) { return 0; }
void *SDL_cosf(void) { return 0; }
void *SDL_crc16(void) { return 0; }
void *SDL_crc32(void) { return 0; }
void *SDL_CreateAnimatedCursor(void) { return 0; }
void *SDL_CreateAsyncIOQueue(void) { return 0; }
void *SDL_CreateAudioStream(void) { return 0; }
void *SDL_CreateColorCursor(void) { return 0; }
void *SDL_CreateCondition(void) { return 0; }
void *SDL_CreateCursor(void) { return 0; }
void *SDL_CreateDirectory(void) { return 0; }
void *SDL_CreateEnvironment(void) { return 0; }
void *SDL_CreateGPUBuffer(void) { return 0; }
void *SDL_CreateGPUComputePipeline(void) { return 0; }
void *SDL_CreateGPUDevice(void) { return 0; }
void *SDL_CreateGPUDeviceWithProperties(void) { return 0; }
void *SDL_CreateGPUGraphicsPipeline(void) { return 0; }
void *SDL_CreateGPURenderer(void) { return 0; }
void *SDL_CreateGPURenderState(void) { return 0; }
void *SDL_CreateGPUSampler(void) { return 0; }
void *SDL_CreateGPUShader(void) { return 0; }
void *SDL_CreateGPUTexture(void) { return 0; }
void *SDL_CreateGPUTransferBuffer(void) { return 0; }
void *SDL_CreateHapticEffect(void) { return 0; }
void *SDL_CreateMutex(void) { return 0; }
void *SDL_CreatePalette(void) { return 0; }
void *SDL_CreatePopupWindow(void) { return 0; }
void *SDL_CreateProcess(void) { return 0; }
void *SDL_CreateProcessWithProperties(void) { return 0; }
/* Properties ID counter — 0 is invalid in SDL3, start at 100 */
static unsigned int next_props_id = 100;
unsigned int SDL_CreateProperties(void) {
    unsigned int id = __sync_fetch_and_add(&next_props_id, 1);
    sdl_log("SDL3-STUB: SDL_CreateProperties → returning valid ID\n");
    return id;
}
void *SDL_CreateRenderer(void *window, const char *name) {
    sdl_log("SDL3-STUB: SDL_CreateRenderer called\n");
    return (void *)fake_renderer;
}
void *SDL_CreateRendererWithProperties(unsigned int props) {
    sdl_log("SDL3-STUB: SDL_CreateRendererWithProperties called\n");
    return (void *)fake_renderer;
}
void *SDL_CreateRWLock(void) { return 0; }
void *SDL_CreateSemaphore(void) { return 0; }
void *SDL_CreateSoftwareRenderer(void *surface) {
    sdl_log("SDL3-STUB: SDL_CreateSoftwareRenderer called\n");
    return (void *)fake_renderer;
}
void *SDL_CreateStorageDirectory(void) { return 0; }
void *SDL_CreateSurface(void) { return 0; }
void *SDL_CreateSurfaceFrom(void) { return 0; }
void *SDL_CreateSurfacePalette(void) { return 0; }
void *SDL_CreateSystemCursor(void) { return 0; }
static char fake_texture[256];
void *SDL_CreateTexture(void *renderer, unsigned int format, int access, int w, int h) {
    return (void *)fake_texture;
}
void *SDL_CreateTextureFromSurface(void *renderer, void *surface) {
    sdl_log("SDL3-STUB: SDL_CreateTextureFromSurface called\n");
    return (void *)fake_texture;
}
void *SDL_CreateTextureWithProperties(void) { return 0; }
void *SDL_CreateThreadRuntime(void) { return 0; }
void *SDL_CreateThreadWithPropertiesRuntime(void) { return 0; }
void *SDL_CreateTray(void) { return 0; }
void *SDL_CreateTrayMenu(void) { return 0; }
void *SDL_CreateTraySubmenu(void) { return 0; }
void *SDL_CreateWindow(const char *title, int w, int h, unsigned int flags) {
    sdl_log("SDL3-STUB: SDL_CreateWindow called\n");
    ensure_x11_window(); /* Create real X11 window for CEF */
    return (void *)fake_window;
}
void *SDL_CreateWindowAndRenderer(void) { return 0; }
void *SDL_CreateWindowWithProperties(unsigned int props) {
    sdl_log("SDL3-STUB: SDL_CreateWindowWithProperties called\n");
    ensure_x11_window();
    return (void *)fake_window;
}
void *SDL_CursorVisible(void) { return 0; }
void *SDL_DateTimeToTime(void) { return 0; }
void *SDL_Delay(void) { return 0; }
void *SDL_DelayNS(void) { return 0; }
void *SDL_DelayPrecise(void) { return 0; }
void *SDL_DestroyAsyncIOQueue(void) { return 0; }
void *SDL_DestroyAudioStream(void) { return 0; }
void *SDL_DestroyCondition(void) { return 0; }
void *SDL_DestroyCursor(void) { return 0; }
void *SDL_DestroyEnvironment(void) { return 0; }
void *SDL_DestroyGPUDevice(void) { return 0; }
void *SDL_DestroyGPURenderState(void) { return 0; }
void *SDL_DestroyHapticEffect(void) { return 0; }
void *SDL_DestroyMutex(void) { return 0; }
void *SDL_DestroyPalette(void) { return 0; }
void *SDL_DestroyProcess(void) { return 0; }
void SDL_DestroyProperties(unsigned int props) { }
void SDL_DestroyRenderer(void *renderer) { sdl_log("SDL3-STUB: SDL_DestroyRenderer called\n"); }
void *SDL_DestroyRWLock(void) { return 0; }
void *SDL_DestroySemaphore(void) { return 0; }
void *SDL_DestroySurface(void) { return 0; }
void SDL_DestroyTexture(void *texture) { }
void *SDL_DestroyTray(void) { return 0; }
void *SDL_DestroyWindow(void) { return 0; }
void *SDL_DestroyWindowSurface(void) { return 0; }
void *SDL_DetachThread(void) { return 0; }
void *SDL_DetachVirtualJoystick(void) { return 0; }
void *SDL_DisableScreenSaver(void) { return 0; }
void *SDL_DispatchGPUCompute(void) { return 0; }
void *SDL_DispatchGPUComputeIndirect(void) { return 0; }
void *SDL_DownloadFromGPUBuffer(void) { return 0; }
void *SDL_DownloadFromGPUTexture(void) { return 0; }
void *SDL_DrawGPUIndexedPrimitives(void) { return 0; }
void *SDL_DrawGPUIndexedPrimitivesIndirect(void) { return 0; }
void *SDL_DrawGPUPrimitives(void) { return 0; }
void *SDL_DrawGPUPrimitivesIndirect(void) { return 0; }
void *SDL_DuplicateSurface(void) { return 0; }
/* SDL3 DYNAPI: return -1 (failure) to force fallback to direct symbol resolution.
 * If we return 0 (success), the caller uses the empty jump table → NULL function ptrs. */
int SDL_DYNAPI_entry(unsigned int apiver, void *table, unsigned int tablesize) {
    sdl_log("SDL3-STUB: SDL_DYNAPI_entry called → returning -1 (force direct symbols)\n");
    return -1;
}
void *SDL_EGL_GetCurrentConfig(void) { return 0; }
void *SDL_EGL_GetCurrentDisplay(void) { return 0; }
void *SDL_EGL_GetProcAddress(void) { return 0; }
void *SDL_EGL_GetWindowSurface(void) { return 0; }
void *SDL_EGL_SetAttributeCallbacks(void) { return 0; }
void *SDL_EnableScreenSaver(void) { return 0; }
void *SDL_EndGPUComputePass(void) { return 0; }
void *SDL_EndGPUCopyPass(void) { return 0; }
void *SDL_EndGPURenderPass(void) { return 0; }
void *SDL_EnterAppMainCallbacks(void) { return 0; }
void *SDL_EnumerateDirectory(void) { return 0; }
void *SDL_EnumerateProperties(void) { return 0; }
void *SDL_EnumerateStorageDirectory(void) { return 0; }
void *SDL_EventEnabled(void) { return 0; }
void *SDL_exp(void) { return 0; }
void *SDL_expf(void) { return 0; }
void *SDL_fabs(void) { return 0; }
void *SDL_fabsf(void) { return 0; }
void *SDL_FillSurfaceRect(void) { return 0; }
void *SDL_FillSurfaceRects(void) { return 0; }
void *SDL_FilterEvents(void) { return 0; }
void *SDL_FlashWindow(void) { return 0; }
void *SDL_FlipSurface(void) { return 0; }
void *SDL_floor(void) { return 0; }
void *SDL_floorf(void) { return 0; }
void *SDL_FlushAudioStream(void) { return 0; }
void *SDL_FlushEvent(void) { return 0; }
void *SDL_FlushEvents(void) { return 0; }
void *SDL_FlushIO(void) { return 0; }
void *SDL_FlushRenderer(void) { return 0; }
void *SDL_fmod(void) { return 0; }
void *SDL_fmodf(void) { return 0; }
void *SDL_free(void) { return 0; }
void *SDL_GamepadConnected(void) { return 0; }
void *SDL_GamepadEventsEnabled(void) { return 0; }
void *SDL_GamepadHasAxis(void) { return 0; }
void *SDL_GamepadHasButton(void) { return 0; }
void *SDL_GamepadHasSensor(void) { return 0; }
void *SDL_GamepadSensorEnabled(void) { return 0; }
void *SDL_GDKResumeGPU(void) { return 0; }
void *SDL_GDKSuspendComplete(void) { return 0; }
void *SDL_GDKSuspendGPU(void) { return 0; }
void *SDL_GenerateMipmapsForGPUTexture(void) { return 0; }
void *SDL_GetAndroidActivity(void) { return 0; }
void *SDL_GetAndroidCachePath(void) { return 0; }
void *SDL_GetAndroidExternalStoragePath(void) { return 0; }
void *SDL_GetAndroidExternalStorageState(void) { return 0; }
void *SDL_GetAndroidInternalStoragePath(void) { return 0; }
void *SDL_GetAndroidJNIEnv(void) { return 0; }
void *SDL_GetAndroidSDKVersion(void) { return 0; }
void *SDL_GetAppMetadataProperty(void) { return 0; }
void *SDL_GetAssertionHandler(void) { return 0; }
void *SDL_GetAssertionReport(void) { return 0; }
void *SDL_GetAsyncIOResult(void) { return 0; }
void *SDL_GetAsyncIOSize(void) { return 0; }
void *SDL_GetAtomicInt(void) { return 0; }
void *SDL_GetAtomicPointer(void) { return 0; }
void *SDL_GetAtomicU32(void) { return 0; }
void *SDL_GetAudioDeviceChannelMap(void) { return 0; }
void *SDL_GetAudioDeviceFormat(void) { return 0; }
void *SDL_GetAudioDeviceGain(void) { return 0; }
void *SDL_GetAudioDeviceName(void) { return 0; }
void *SDL_GetAudioDriver(void) { return 0; }
void *SDL_GetAudioFormatName(void) { return 0; }
void *SDL_GetAudioPlaybackDevices(void) { return 0; }
void *SDL_GetAudioRecordingDevices(void) { return 0; }
void *SDL_GetAudioStreamAvailable(void) { return 0; }
void *SDL_GetAudioStreamData(void) { return 0; }
void *SDL_GetAudioStreamDevice(void) { return 0; }
void *SDL_GetAudioStreamFormat(void) { return 0; }
void *SDL_GetAudioStreamFrequencyRatio(void) { return 0; }
void *SDL_GetAudioStreamGain(void) { return 0; }
void *SDL_GetAudioStreamInputChannelMap(void) { return 0; }
void *SDL_GetAudioStreamOutputChannelMap(void) { return 0; }
void *SDL_GetAudioStreamProperties(void) { return 0; }
void *SDL_GetAudioStreamQueued(void) { return 0; }
void *SDL_GetBasePath(void) { return 0; }
int SDL_GetBooleanProperty(unsigned int props, const char *name, int default_value) { return default_value; }
void *SDL_GetCameraDriver(void) { return 0; }
void *SDL_GetCameraFormat(void) { return 0; }
void *SDL_GetCameraID(void) { return 0; }
void *SDL_GetCameraName(void) { return 0; }
void *SDL_GetCameraPermissionState(void) { return 0; }
void *SDL_GetCameraPosition(void) { return 0; }
void *SDL_GetCameraProperties(void) { return 0; }
void *SDL_GetCameras(void) { return 0; }
void *SDL_GetCameraSupportedFormats(void) { return 0; }
void *SDL_GetClipboardData(void) { return 0; }
void *SDL_GetClipboardMimeTypes(void) { return 0; }
void *SDL_GetClipboardText(void) { return 0; }
void *SDL_GetClosestFullscreenDisplayMode(void) { return 0; }
void *SDL_GetCPUCacheLineSize(void) { return 0; }
void *SDL_GetCurrentAudioDriver(void) { return 0; }
void *SDL_GetCurrentCameraDriver(void) { return 0; }
void *SDL_GetCurrentDirectory(void) { return 0; }
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID id) { return &fake_display_mode; }
void *SDL_GetCurrentDisplayOrientation(void) { return 0; }
void *SDL_GetCurrentRenderOutputSize(void) { return 0; }
void *SDL_GetCurrentThreadID(void) { return 0; }
void *SDL_GetCurrentTime(void) { return 0; }
const char *SDL_GetCurrentVideoDriver(void) { sdl_log("SDL3-STUB: SDL_GetCurrentVideoDriver → x11\n"); return video_driver; }
void *SDL_GetCursor(void) { return 0; }
void *SDL_GetDateTimeLocalePreferences(void) { return 0; }
void *SDL_GetDayOfWeek(void) { return 0; }
void *SDL_GetDayOfYear(void) { return 0; }
void *SDL_GetDaysInMonth(void) { return 0; }
void *SDL_GetDefaultAssertionHandler(void) { return 0; }
void *SDL_GetDefaultCursor(void) { return 0; }
void *SDL_GetDefaultLogOutputFunction(void) { return 0; }
void *SDL_GetDefaultTextureScaleMode(void) { return 0; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id) { return &fake_display_mode; }
void *SDL_GetDirect3D9AdapterIndex(void) { return 0; }
int SDL_GetDisplayBounds(SDL_DisplayID id, void *rect) {
    if (rect) { int *r = (int *)rect; r[0] = 0; r[1] = 0; r[2] = 1920; r[3] = 1080; }
    return 1; /* SDL3: true=success */
}
void *SDL_GetDisplayContentScale(void) { return 0; }
void *SDL_GetDisplayForPoint(void) { return 0; }
void *SDL_GetDisplayForRect(void) { return 0; }
void *SDL_GetDisplayForWindow(void) { return 0; }
void *SDL_GetDisplayName(void) { return 0; }
void *SDL_GetDisplayProperties(void) { return 0; }
SDL_DisplayID *SDL_GetDisplays(int *count) { if (count) *count = 1; return fake_displays; }
void *SDL_GetDisplayUsableBounds(void) { return 0; }
void *SDL_GetDXGIOutputInfo(void) { return 0; }
void *SDL_getenv(void) { return 0; }
void *SDL_GetEnvironment(void) { return 0; }
void *SDL_GetEnvironmentVariable(void) { return 0; }
void *SDL_GetEnvironmentVariables(void) { return 0; }
void *SDL_getenv_unsafe(void) { return 0; }
const char *SDL_GetError(void) { sdl_log("SDL3-STUB: SDL_GetError called\n"); return empty_str; }
void *SDL_GetEventDescription(void) { return 0; }
void *SDL_GetEventFilter(void) { return 0; }
float SDL_GetFloatProperty(unsigned int props, const char *name, float default_value) { return default_value; }
void *SDL_GetFullscreenDisplayModes(void) { return 0; }
void *SDL_GetGamepadAppleSFSymbolsNameForAxis(void) { return 0; }
void *SDL_GetGamepadAppleSFSymbolsNameForButton(void) { return 0; }
void *SDL_GetGamepadAxis(void) { return 0; }
void *SDL_GetGamepadAxisFromString(void) { return 0; }
void *SDL_GetGamepadBindings(void) { return 0; }
void *SDL_GetGamepadButton(void) { return 0; }
void *SDL_GetGamepadButtonFromString(void) { return 0; }
void *SDL_GetGamepadButtonLabel(void) { return 0; }
void *SDL_GetGamepadButtonLabelForType(void) { return 0; }
void *SDL_GetGamepadConnectionState(void) { return 0; }
void *SDL_GetGamepadFirmwareVersion(void) { return 0; }
void *SDL_GetGamepadFromID(void) { return 0; }
void *SDL_GetGamepadFromPlayerIndex(void) { return 0; }
void *SDL_GetGamepadGUIDForID(void) { return 0; }
void *SDL_GetGamepadID(void) { return 0; }
void *SDL_GetGamepadJoystick(void) { return 0; }
void *SDL_GetGamepadMapping(void) { return 0; }
void *SDL_GetGamepadMappingForGUID(void) { return 0; }
void *SDL_GetGamepadMappingForID(void) { return 0; }
void *SDL_GetGamepadMappings(void) { return 0; }
void *SDL_GetGamepadName(void) { return 0; }
void *SDL_GetGamepadNameForID(void) { return 0; }
void *SDL_GetGamepadPath(void) { return 0; }
void *SDL_GetGamepadPathForID(void) { return 0; }
void *SDL_GetGamepadPlayerIndex(void) { return 0; }
void *SDL_GetGamepadPlayerIndexForID(void) { return 0; }
void *SDL_GetGamepadPowerInfo(void) { return 0; }
void *SDL_GetGamepadProduct(void) { return 0; }
void *SDL_GetGamepadProductForID(void) { return 0; }
void *SDL_GetGamepadProductVersion(void) { return 0; }
void *SDL_GetGamepadProductVersionForID(void) { return 0; }
void *SDL_GetGamepadProperties(void) { return 0; }
void *SDL_GetGamepads(void) { return 0; }
void *SDL_GetGamepadSensorData(void) { return 0; }
void *SDL_GetGamepadSensorDataRate(void) { return 0; }
void *SDL_GetGamepadSerial(void) { return 0; }
void *SDL_GetGamepadSteamHandle(void) { return 0; }
void *SDL_GetGamepadStringForAxis(void) { return 0; }
void *SDL_GetGamepadStringForButton(void) { return 0; }
void *SDL_GetGamepadStringForType(void) { return 0; }
void *SDL_GetGamepadTouchpadFinger(void) { return 0; }
void *SDL_GetGamepadType(void) { return 0; }
void *SDL_GetGamepadTypeForID(void) { return 0; }
void *SDL_GetGamepadTypeFromString(void) { return 0; }
void *SDL_GetGamepadVendor(void) { return 0; }
void *SDL_GetGamepadVendorForID(void) { return 0; }
void *SDL_GetGDKDefaultUser(void) { return 0; }
void *SDL_GetGDKTaskQueue(void) { return 0; }
void *SDL_GetGlobalMouseState(void) { return 0; }
void *SDL_GetGlobalProperties(void) { return 0; }
void *SDL_GetGPUDeviceDriver(void) { return 0; }
void *SDL_GetGPUDeviceProperties(void) { return 0; }
void *SDL_GetGPUDriver(void) { return 0; }
void *SDL_GetGPURendererDevice(void) { return 0; }
void *SDL_GetGPUShaderFormats(void) { return 0; }
void *SDL_GetGPUSwapchainTextureFormat(void) { return 0; }
void *SDL_GetGPUTextureFormatFromPixelFormat(void) { return 0; }
void *SDL_GetGrabbedWindow(void) { return 0; }
void *SDL_GetHapticEffectStatus(void) { return 0; }
void *SDL_GetHapticFeatures(void) { return 0; }
void *SDL_GetHapticFromID(void) { return 0; }
void *SDL_GetHapticID(void) { return 0; }
void *SDL_GetHapticName(void) { return 0; }
void *SDL_GetHapticNameForID(void) { return 0; }
void *SDL_GetHaptics(void) { return 0; }
const char *SDL_GetHint(const char *name) {
    if (name) { sdl_log("SDL3-STUB: SDL_GetHint: "); sdl_log(name); sdl_log("\n"); }
    return NULL;
}
int SDL_GetHintBoolean(const char *name, int default_value) { return default_value; }
void *SDL_GetIOProperties(void) { return 0; }
void *SDL_GetIOSize(void) { return 0; }
void *SDL_GetIOStatus(void) { return 0; }
void *SDL_GetJoystickAxis(void) { return 0; }
void *SDL_GetJoystickAxisInitialState(void) { return 0; }
void *SDL_GetJoystickBall(void) { return 0; }
void *SDL_GetJoystickButton(void) { return 0; }
void *SDL_GetJoystickConnectionState(void) { return 0; }
void *SDL_GetJoystickFirmwareVersion(void) { return 0; }
void *SDL_GetJoystickFromID(void) { return 0; }
void *SDL_GetJoystickFromPlayerIndex(void) { return 0; }
void *SDL_GetJoystickGUID(void) { return 0; }
void *SDL_GetJoystickGUIDForID(void) { return 0; }
void *SDL_GetJoystickGUIDInfo(void) { return 0; }
void *SDL_GetJoystickHat(void) { return 0; }
void *SDL_GetJoystickID(void) { return 0; }
void *SDL_GetJoystickName(void) { return 0; }
void *SDL_GetJoystickNameForID(void) { return 0; }
void *SDL_GetJoystickPath(void) { return 0; }
void *SDL_GetJoystickPathForID(void) { return 0; }
void *SDL_GetJoystickPlayerIndex(void) { return 0; }
void *SDL_GetJoystickPlayerIndexForID(void) { return 0; }
void *SDL_GetJoystickPowerInfo(void) { return 0; }
void *SDL_GetJoystickProduct(void) { return 0; }
void *SDL_GetJoystickProductForID(void) { return 0; }
void *SDL_GetJoystickProductVersion(void) { return 0; }
void *SDL_GetJoystickProductVersionForID(void) { return 0; }
void *SDL_GetJoystickProperties(void) { return 0; }
void *SDL_GetJoysticks(void) { return 0; }
void *SDL_GetJoystickSerial(void) { return 0; }
void *SDL_GetJoystickType(void) { return 0; }
void *SDL_GetJoystickTypeForID(void) { return 0; }
void *SDL_GetJoystickVendor(void) { return 0; }
void *SDL_GetJoystickVendorForID(void) { return 0; }
void *SDL_GetKeyboardFocus(void) { return 0; }
void *SDL_GetKeyboardNameForID(void) { return 0; }
void *SDL_GetKeyboards(void) { return 0; }
void *SDL_GetKeyboardState(void) { return 0; }
void *SDL_GetKeyFromName(void) { return 0; }
void *SDL_GetKeyFromScancode(void) { return 0; }
void *SDL_GetKeyName(void) { return 0; }
void *SDL_GetLogOutputFunction(void) { return 0; }
void *SDL_GetLogPriority(void) { return 0; }
void *SDL_GetMasksForPixelFormat(void) { return 0; }
void *SDL_GetMaxHapticEffects(void) { return 0; }
void *SDL_GetMaxHapticEffectsPlaying(void) { return 0; }
void *SDL_GetMemoryFunctions(void) { return 0; }
void *SDL_GetMice(void) { return 0; }
void *SDL_GetModState(void) { return 0; }
void *SDL_GetMouseFocus(void) { return 0; }
void *SDL_GetMouseNameForID(void) { return 0; }
void *SDL_GetMouseState(void) { return 0; }
void *SDL_GetNaturalDisplayOrientation(void) { return 0; }
void *SDL_GetNumAllocations(void) { return 0; }
void *SDL_GetNumAudioDrivers(void) { return 0; }
long long SDL_GetNumberProperty(unsigned int props, const char *name, long long default_value) {
    if (name) {
        sdl_log("SDL3-STUB: SDL_GetNumberProperty: ");
        sdl_log(name);
        sdl_log("\n");
    }
    if (name && strstr(name, "x11.window")) {
        ensure_x11_window();
        if (real_x11_window) return (long long)real_x11_window;
    }
    if (name && strstr(name, "x11.screen")) return 0;
    return default_value;
}
void *SDL_GetNumCameraDrivers(void) { return 0; }
void *SDL_GetNumGamepadTouchpadFingers(void) { return 0; }
void *SDL_GetNumGamepadTouchpads(void) { return 0; }
void *SDL_GetNumGPUDrivers(void) { return 0; }
void *SDL_GetNumHapticAxes(void) { return 0; }
void *SDL_GetNumJoystickAxes(void) { return 0; }
void *SDL_GetNumJoystickBalls(void) { return 0; }
void *SDL_GetNumJoystickButtons(void) { return 0; }
void *SDL_GetNumJoystickHats(void) { return 0; }
void *SDL_GetNumLogicalCPUCores(void) { return 0; }
void *SDL_GetNumRenderDrivers(void) { return 0; }
void *SDL_GetNumVideoDrivers(void) { return 0; }
void *SDL_GetOriginalMemoryFunctions(void) { return 0; }
void *SDL_GetPathInfo(void) { return 0; }
void *SDL_GetPenDeviceType(void) { return 0; }
void *SDL_GetPerformanceCounter(void) { return 0; }
void *SDL_GetPerformanceFrequency(void) { return 0; }
void *SDL_GetPixelFormatDetails(void) { return 0; }
void *SDL_GetPixelFormatForMasks(void) { return 0; }
void *SDL_GetPixelFormatFromGPUTextureFormat(void) { return 0; }
void *SDL_GetPixelFormatName(void) { return 0; }
const char *SDL_GetPlatform(void) { return platform_str; }
void *SDL_GetPointerProperty(unsigned int props, const char *name, void *default_value) {
    if (name) {
        sdl_log("SDL3-STUB: SDL_GetPointerProperty: ");
        sdl_log(name);
        sdl_log("\n");
    }
    if (name && strstr(name, "x11.display")) {
        ensure_x11_window();
        return real_x11_display;
    }
    if (name && strstr(name, "x11.window")) {
        ensure_x11_window();
        return (void *)real_x11_window;
    }
    return default_value;
}
void *SDL_GetPowerInfo(void) { return 0; }
void *SDL_GetPreferredLocales(void) { return 0; }
void *SDL_GetPrefPath(void) { return 0; }
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
void *SDL_GetPrimarySelectionText(void) { return 0; }
void *SDL_GetProcessInput(void) { return 0; }
void *SDL_GetProcessOutput(void) { return 0; }
void *SDL_GetProcessProperties(void) { return 0; }
void *SDL_GetPropertyType(void) { return 0; }
void *SDL_GetRealGamepadType(void) { return 0; }
void *SDL_GetRealGamepadTypeForID(void) { return 0; }
void *SDL_GetRectAndLineIntersection(void) { return 0; }
void *SDL_GetRectAndLineIntersectionFloat(void) { return 0; }
void *SDL_GetRectEnclosingPoints(void) { return 0; }
void *SDL_GetRectEnclosingPointsFloat(void) { return 0; }
void *SDL_GetRectIntersection(void) { return 0; }
void *SDL_GetRectIntersectionFloat(void) { return 0; }
void *SDL_GetRectUnion(void) { return 0; }
void *SDL_GetRectUnionFloat(void) { return 0; }
void *SDL_GetRelativeMouseState(void) { return 0; }
void *SDL_GetRenderClipRect(void) { return 0; }
void *SDL_GetRenderColorScale(void) { return 0; }
void *SDL_GetRenderDrawBlendMode(void) { return 0; }
void *SDL_GetRenderDrawColor(void) { return 0; }
void *SDL_GetRenderDrawColorFloat(void) { return 0; }
void *SDL_GetRenderDriver(void) { return 0; }
void *SDL_GetRenderer(void) { return 0; }
void *SDL_GetRendererFromTexture(void) { return 0; }
void *SDL_GetRendererName(void) { return 0; }
void *SDL_GetRendererProperties(void) { return 0; }
void *SDL_GetRenderLogicalPresentation(void) { return 0; }
void *SDL_GetRenderLogicalPresentationRect(void) { return 0; }
void *SDL_GetRenderMetalCommandEncoder(void) { return 0; }
void *SDL_GetRenderMetalLayer(void) { return 0; }
void *SDL_GetRenderOutputSize(void) { return 0; }
void *SDL_GetRenderSafeArea(void) { return 0; }
void *SDL_GetRenderScale(void) { return 0; }
void *SDL_GetRenderTarget(void) { return 0; }
void *SDL_GetRenderTextureAddressMode(void) { return 0; }
void *SDL_GetRenderViewport(void) { return 0; }
void *SDL_GetRenderVSync(void) { return 0; }
void *SDL_GetRenderWindow(void) { return 0; }
const char *SDL_GetRevision(void) { return revision_str; }
void *SDL_GetRGB(void) { return 0; }
void *SDL_GetRGBA(void) { return 0; }
void *SDL_GetSandbox(void) { return 0; }
void *SDL_GetScancodeFromKey(void) { return 0; }
void *SDL_GetScancodeFromName(void) { return 0; }
void *SDL_GetScancodeName(void) { return 0; }
void *SDL_GetSemaphoreValue(void) { return 0; }
void *SDL_GetSensorData(void) { return 0; }
void *SDL_GetSensorFromID(void) { return 0; }
void *SDL_GetSensorID(void) { return 0; }
void *SDL_GetSensorName(void) { return 0; }
void *SDL_GetSensorNameForID(void) { return 0; }
void *SDL_GetSensorNonPortableType(void) { return 0; }
void *SDL_GetSensorNonPortableTypeForID(void) { return 0; }
void *SDL_GetSensorProperties(void) { return 0; }
void *SDL_GetSensors(void) { return 0; }
void *SDL_GetSensorType(void) { return 0; }
void *SDL_GetSensorTypeForID(void) { return 0; }
void *SDL_GetSilenceValueForFormat(void) { return 0; }
void *SDL_GetSIMDAlignment(void) { return 0; }
void *SDL_GetStorageFileSize(void) { return 0; }
void *SDL_GetStoragePathInfo(void) { return 0; }
void *SDL_GetStorageSpaceRemaining(void) { return 0; }
const char *SDL_GetStringProperty(unsigned int props, const char *name, const char *default_value) { return default_value; }
void *SDL_GetSurfaceAlphaMod(void) { return 0; }
void *SDL_GetSurfaceBlendMode(void) { return 0; }
void *SDL_GetSurfaceClipRect(void) { return 0; }
void *SDL_GetSurfaceColorKey(void) { return 0; }
void *SDL_GetSurfaceColorMod(void) { return 0; }
void *SDL_GetSurfaceColorspace(void) { return 0; }
void *SDL_GetSurfaceImages(void) { return 0; }
void *SDL_GetSurfacePalette(void) { return 0; }
void *SDL_GetSurfaceProperties(void) { return 0; }
void *SDL_GetSystemPageSize(void) { return 0; }
void *SDL_GetSystemRAM(void) { return 0; }
void *SDL_GetSystemTheme(void) { return 0; }
void *SDL_GetTextInputArea(void) { return 0; }
void *SDL_GetTextureAlphaMod(void) { return 0; }
void *SDL_GetTextureAlphaModFloat(void) { return 0; }
void *SDL_GetTextureBlendMode(void) { return 0; }
void *SDL_GetTextureColorMod(void) { return 0; }
void *SDL_GetTextureColorModFloat(void) { return 0; }
void *SDL_GetTexturePalette(void) { return 0; }
void *SDL_GetTextureProperties(void) { return 0; }
void *SDL_GetTextureScaleMode(void) { return 0; }
void *SDL_GetTextureSize(void) { return 0; }
void *SDL_GetThreadID(void) { return 0; }
void *SDL_GetThreadName(void) { return 0; }
void *SDL_GetThreadState(void) { return 0; }
void *SDL_GetTicks(void) { return 0; }
void *SDL_GetTicksNS(void) { return 0; }
void *SDL_GetTLS(void) { return 0; }
void *SDL_GetTouchDeviceName(void) { return 0; }
void *SDL_GetTouchDevices(void) { return 0; }
void *SDL_GetTouchDeviceType(void) { return 0; }
void *SDL_GetTouchFingers(void) { return 0; }
void *SDL_GetTrayEntries(void) { return 0; }
void *SDL_GetTrayEntryChecked(void) { return 0; }
void *SDL_GetTrayEntryEnabled(void) { return 0; }
void *SDL_GetTrayEntryLabel(void) { return 0; }
void *SDL_GetTrayEntryParent(void) { return 0; }
void *SDL_GetTrayMenu(void) { return 0; }
void *SDL_GetTrayMenuParentEntry(void) { return 0; }
void *SDL_GetTrayMenuParentTray(void) { return 0; }
void *SDL_GetTraySubmenu(void) { return 0; }
void *SDL_GetUserFolder(void) { return 0; }
int SDL_GetVersion(void) { return 3002000; } /* 3.2.0 */
const char *SDL_GetVideoDriver(int index) { return video_driver; }
void *SDL_GetWindowAspectRatio(void) { return 0; }
void *SDL_GetWindowBordersSize(void) { return 0; }
void *SDL_GetWindowDisplayScale(void) { return 0; }
unsigned long long SDL_GetWindowFlags(void *window) {
    sdl_log("SDL3-STUB: SDL_GetWindowFlags called\n");
    /* SDL_WINDOW_HIDDEN=0x08, SDL_WINDOW_RESIZABLE=0x20 — return reasonable flags */
    /* Bit 2 (0x4) = SHOWN is not used in SDL3, SDL3 uses absence of HIDDEN */
    return 0x00000020; /* RESIZABLE, not hidden */
}
void *SDL_GetWindowFromEvent(void) { return 0; }
void *SDL_GetWindowFromID(unsigned int id) { return (void *)fake_window; }
void *SDL_GetWindowFullscreenMode(void) { return 0; }
void *SDL_GetWindowICCProfile(void) { return 0; }
unsigned int SDL_GetWindowID(void *window) { sdl_log("SDL3-STUB: SDL_GetWindowID called\n"); return 1; }
void *SDL_GetWindowKeyboardGrab(void) { return 0; }
void *SDL_GetWindowMaximumSize(void) { return 0; }
void *SDL_GetWindowMinimumSize(void) { return 0; }
void *SDL_GetWindowMouseGrab(void) { return 0; }
void *SDL_GetWindowMouseRect(void) { return 0; }
void *SDL_GetWindowOpacity(void) { return 0; }
void *SDL_GetWindowParent(void) { return 0; }
void *SDL_GetWindowPixelDensity(void) { return 0; }
void *SDL_GetWindowPixelFormat(void) { return 0; }
int SDL_GetWindowPosition(void *window, int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 1; }
void *SDL_GetWindowProgressState(void) { return 0; }
void *SDL_GetWindowProgressValue(void) { return 0; }
unsigned int SDL_GetWindowProperties(void *window) { sdl_log("SDL3-STUB: SDL_GetWindowProperties called\n"); return 1; }
void *SDL_GetWindowRelativeMouseMode(void) { return 0; }
void *SDL_GetWindows(void) { return 0; }
void *SDL_GetWindowSafeArea(void) { return 0; }
int SDL_GetWindowSize(void *window, int *w, int *h) { if (w) *w = 1920; if (h) *h = 1080; return 1; }
int SDL_GetWindowSizeInPixels(void *window, int *w, int *h) { if (w) *w = 1920; if (h) *h = 1080; return 1; }
/* Fake SDL_Surface for GetWindowSurface */
static unsigned char fake_pixels[1920 * 1080 * 4]; /* RGBA buffer */
static struct {
    unsigned int flags;       /* SDL_SurfaceFlags */
    unsigned int format;      /* SDL_PixelFormat (SDL_PIXELFORMAT_XRGB8888 = 0x16161804) */
    int w, h;
    int pitch;
    void *pixels;
    int refcount;
    void *reserved;
    void *internal;           /* SDL_SurfaceData* */
} fake_surface = {
    0,                        /* flags */
    0x16161804,               /* SDL_PIXELFORMAT_XRGB8888 */
    1920, 1080,               /* w, h */
    1920 * 4,                 /* pitch */
    fake_pixels,              /* pixels */
    1,                        /* refcount */
    0, 0
};

void *SDL_GetWindowSurface(void *window) {
    sdl_log("SDL3-STUB: SDL_GetWindowSurface → returning fake surface\n");
    return &fake_surface;
}
void *SDL_GetWindowSurfaceVSync(void) { return 0; }
const char *SDL_GetWindowTitle(void *window) { sdl_log("SDL3-STUB: SDL_GetWindowTitle called\n"); return "Steam"; }
void *SDL_GL_CreateContext(void) { return 0; }
void *SDL_GL_DestroyContext(void) { return 0; }
void *SDL_GL_ExtensionSupported(void) { return 0; }
void *SDL_GL_GetAttribute(void) { return 0; }
void *SDL_GL_GetCurrentContext(void) { return 0; }
void *SDL_GL_GetCurrentWindow(void) { return 0; }
void *SDL_GL_GetProcAddress(void) { return 0; }
void *SDL_GL_GetSwapInterval(void) { return 0; }
void *SDL_GL_LoadLibrary(void) { return 0; }
void *SDL_GL_MakeCurrent(void) { return 0; }
void *SDL_GlobDirectory(void) { return 0; }
void *SDL_GlobStorageDirectory(void) { return 0; }
void *SDL_GL_ResetAttributes(void) { return 0; }
void *SDL_GL_SetAttribute(void) { return 0; }
void *SDL_GL_SetSwapInterval(void) { return 0; }
void *SDL_GL_SwapWindow(void) { return 0; }
void *SDL_GL_UnloadLibrary(void) { return 0; }
void *SDL_GPUSupportsProperties(void) { return 0; }
void *SDL_GPUSupportsShaderFormats(void) { return 0; }
void *SDL_GPUTextureFormatTexelBlockSize(void) { return 0; }
void *SDL_GPUTextureSupportsFormat(void) { return 0; }
void *SDL_GPUTextureSupportsSampleCount(void) { return 0; }
void *SDL_GUIDToString(void) { return 0; }
void *SDL_HapticEffectSupported(void) { return 0; }
void *SDL_HapticRumbleSupported(void) { return 0; }
void *SDL_HasAltiVec(void) { return 0; }
void *SDL_HasARMSIMD(void) { return 0; }
void *SDL_HasAVX(void) { return 0; }
void *SDL_HasAVX2(void) { return 0; }
void *SDL_HasAVX512F(void) { return 0; }
void *SDL_HasClipboardData(void) { return 0; }
void *SDL_HasClipboardText(void) { return 0; }
void *SDL_HasEvent(void) { return 0; }
void *SDL_HasEvents(void) { return 0; }
void *SDL_HasGamepad(void) { return 0; }
void *SDL_HasJoystick(void) { return 0; }
void *SDL_HasKeyboard(void) { return 0; }
void *SDL_HasLASX(void) { return 0; }
void *SDL_HasLSX(void) { return 0; }
void *SDL_HasMMX(void) { return 0; }
void *SDL_HasMouse(void) { return 0; }
void *SDL_HasNEON(void) { return 0; }
void *SDL_HasPrimarySelectionText(void) { return 0; }
void *SDL_HasProperty(void) { return 0; }
void *SDL_HasRectIntersection(void) { return 0; }
void *SDL_HasRectIntersectionFloat(void) { return 0; }
void *SDL_HasScreenKeyboardSupport(void) { return 0; }
void *SDL_HasSSE(void) { return 0; }
void *SDL_HasSSE2(void) { return 0; }
void *SDL_HasSSE3(void) { return 0; }
void *SDL_HasSSE41(void) { return 0; }
void *SDL_HasSSE42(void) { return 0; }
void *SDL_hid_ble_scan(void) { return 0; }
void *SDL_hid_close(void) { return 0; }
void *SDL_hid_device_change_count(void) { return 0; }
void *SDL_HideCursor(void) { return 0; }
void *SDL_hid_enumerate(void) { return 0; }
int SDL_HideWindow(void *window) { return 1; }
void *SDL_hid_exit(void) { return 0; }
void *SDL_hid_free_enumeration(void) { return 0; }
void *SDL_hid_get_device_info(void) { return 0; }
void *SDL_hid_get_feature_report(void) { return 0; }
void *SDL_hid_get_indexed_string(void) { return 0; }
void *SDL_hid_get_input_report(void) { return 0; }
void *SDL_hid_get_manufacturer_string(void) { return 0; }
void *SDL_hid_get_product_string(void) { return 0; }
void *SDL_hid_get_properties(void) { return 0; }
void *SDL_hid_get_report_descriptor(void) { return 0; }
void *SDL_hid_get_serial_number_string(void) { return 0; }
void *SDL_hid_init(void) { return 0; }
void *SDL_hid_open(void) { return 0; }
void *SDL_hid_open_path(void) { return 0; }
void *SDL_hid_read(void) { return 0; }
void *SDL_hid_read_timeout(void) { return 0; }
void *SDL_hid_send_feature_report(void) { return 0; }
void *SDL_hid_set_nonblocking(void) { return 0; }
void *SDL_hid_write(void) { return 0; }
void *SDL_iconv(void) { return 0; }
void *SDL_iconv_close(void) { return 0; }
void *SDL_iconv_open(void) { return 0; }
void *SDL_iconv_string(void) { return 0; }
int SDL_Init(unsigned int flags) { sdl_log("SDL3-STUB: SDL_Init called\n"); return 1; } /* SDL3: true=success */
void *SDL_InitHapticRumble(void) { return 0; }
int SDL_InitSubSystem(unsigned int flags) { sdl_log("SDL3-STUB: SDL_InitSubSystem called\n"); return 1; }
void *SDL_InsertGPUDebugLabel(void) { return 0; }
void *SDL_InsertTrayEntryAt(void) { return 0; }
void *SDL_IOFromConstMem(void) { return 0; }
void *SDL_IOFromDynamicMem(void) { return 0; }
void *SDL_IOFromFile(void) { return 0; }
void *SDL_IOFromMem(void) { return 0; }
void *SDL_IOprintf(void) { return 0; }
void *SDL_IOvprintf(void) { return 0; }
void *SDL_isalnum(void) { return 0; }
void *SDL_isalpha(void) { return 0; }
void *SDL_IsAudioDevicePhysical(void) { return 0; }
void *SDL_IsAudioDevicePlayback(void) { return 0; }
void *SDL_isblank(void) { return 0; }
void *SDL_IsChromebook(void) { return 0; }
void *SDL_iscntrl(void) { return 0; }
void *SDL_IsDeXMode(void) { return 0; }
void *SDL_isdigit(void) { return 0; }
void *SDL_IsGamepad(void) { return 0; }
void *SDL_isgraph(void) { return 0; }
void *SDL_isinf(void) { return 0; }
void *SDL_isinff(void) { return 0; }
void *SDL_IsJoystickHaptic(void) { return 0; }
void *SDL_IsJoystickVirtual(void) { return 0; }
void *SDL_islower(void) { return 0; }
void *SDL_IsMainThread(void) { return 0; }
void *SDL_IsMouseHaptic(void) { return 0; }
void *SDL_isnan(void) { return 0; }
void *SDL_isnanf(void) { return 0; }
void *SDL_isprint(void) { return 0; }
void *SDL_ispunct(void) { return 0; }
void *SDL_isspace(void) { return 0; }
void *SDL_IsTablet(void) { return 0; }
void *SDL_IsTV(void) { return 0; }
void *SDL_isupper(void) { return 0; }
void *SDL_isxdigit(void) { return 0; }
void *SDL_itoa(void) { return 0; }
void *SDL_JoystickConnected(void) { return 0; }
void *SDL_JoystickEventsEnabled(void) { return 0; }
void *SDL_KillProcess(void) { return 0; }
void *SDL_lltoa(void) { return 0; }
void *SDL_LoadBMP(void) { return 0; }
void *SDL_LoadBMP_IO(void) { return 0; }
void *SDL_LoadFile(void) { return 0; }
void *SDL_LoadFileAsync(void) { return 0; }
void *SDL_LoadFile_IO(void) { return 0; }
void *SDL_LoadFunction(void) { return 0; }
void *SDL_LoadObject(void) { return 0; }
void *SDL_LoadPNG(void) { return 0; }
void *SDL_LoadPNG_IO(void) { return 0; }
void *SDL_LoadSurface(void) { return 0; }
void *SDL_LoadSurface_IO(void) { return 0; }
void *SDL_LoadWAV(void) { return 0; }
void *SDL_LoadWAV_IO(void) { return 0; }
void *SDL_LockAudioStream(void) { return 0; }
void *SDL_LockJoysticks(void) { return 0; }
void *SDL_LockMutex(void) { return 0; }
void *SDL_LockProperties(void) { return 0; }
void *SDL_LockRWLockForReading(void) { return 0; }
void *SDL_LockRWLockForWriting(void) { return 0; }
void *SDL_LockSpinlock(void) { return 0; }
int SDL_LockSurface(void *surface) { return 1; }
void *SDL_LockTexture(void) { return 0; }
void *SDL_LockTextureToSurface(void) { return 0; }
void *SDL_log(void) { return 0; }
void *SDL_Log(void) { return 0; }
void *SDL_log10(void) { return 0; }
void *SDL_log10f(void) { return 0; }
void *SDL_LogCritical(void) { return 0; }
void *SDL_LogDebug(void) { return 0; }
void *SDL_LogError(void) { return 0; }
void *SDL_logf(void) { return 0; }
void *SDL_LogInfo(void) { return 0; }
void *SDL_LogMessage(void) { return 0; }
void *SDL_LogMessageV(void) { return 0; }
void *SDL_LogTrace(void) { return 0; }
void *SDL_LogVerbose(void) { return 0; }
void *SDL_LogWarn(void) { return 0; }
void *SDL_lround(void) { return 0; }
void *SDL_lroundf(void) { return 0; }
void *SDL_ltoa(void) { return 0; }
void *SDL_malloc(void) { return 0; }
void *SDL_MapGPUTransferBuffer(void) { return 0; }
void *SDL_MapRGB(void) { return 0; }
void *SDL_MapRGBA(void) { return 0; }
void *SDL_MapSurfaceRGB(void) { return 0; }
void *SDL_MapSurfaceRGBA(void) { return 0; }
void *SDL_MaximizeWindow(void) { return 0; }
void *SDL_memcmp(void) { return 0; }
void *SDL_memcpy(void) { return 0; }
void *SDL_memmove(void) { return 0; }
void *SDL_MemoryBarrierAcquireFunction(void) { return 0; }
void *SDL_MemoryBarrierReleaseFunction(void) { return 0; }
void *SDL_memset(void) { return 0; }
void *SDL_memset4(void) { return 0; }
void *SDL_Metal_CreateView(void) { return 0; }
void *SDL_Metal_DestroyView(void) { return 0; }
void *SDL_Metal_GetLayer(void) { return 0; }
void *SDL_MinimizeWindow(void) { return 0; }
void *SDL_MixAudio(void) { return 0; }
void *SDL_modf(void) { return 0; }
void *SDL_modff(void) { return 0; }
void *SDL_murmur3_32(void) { return 0; }
void *SDL_OnApplicationDidChangeStatusBarOrientation(void) { return 0; }
void *SDL_OnApplicationDidEnterBackground(void) { return 0; }
void *SDL_OnApplicationDidEnterForeground(void) { return 0; }
void *SDL_OnApplicationDidReceiveMemoryWarning(void) { return 0; }
void *SDL_OnApplicationWillEnterBackground(void) { return 0; }
void *SDL_OnApplicationWillEnterForeground(void) { return 0; }
void *SDL_OnApplicationWillTerminate(void) { return 0; }
void *SDL_OpenAudioDevice(void) { return 0; }
void *SDL_OpenAudioDeviceStream(void) { return 0; }
void *SDL_OpenCamera(void) { return 0; }
void *SDL_OpenFileStorage(void) { return 0; }
void *SDL_OpenGamepad(void) { return 0; }
void *SDL_OpenHaptic(void) { return 0; }
void *SDL_OpenHapticFromJoystick(void) { return 0; }
void *SDL_OpenHapticFromMouse(void) { return 0; }
void *SDL_OpenIO(void) { return 0; }
void *SDL_OpenJoystick(void) { return 0; }
void *SDL_OpenSensor(void) { return 0; }
void *SDL_OpenStorage(void) { return 0; }
void *SDL_OpenTitleStorage(void) { return 0; }
void *SDL_OpenURL(void) { return 0; }
void *SDL_OpenUserStorage(void) { return 0; }
void *SDL_OutOfMemory(void) { return 0; }
void *SDL_PauseAudioDevice(void) { return 0; }
void *SDL_PauseAudioStreamDevice(void) { return 0; }
void *SDL_PauseHaptic(void) { return 0; }
void *SDL_PeepEvents(void) { return 0; }
void *SDL_PlayHapticRumble(void) { return 0; }
void *SDL_PollEvent(void) { return 0; }
void *SDL_PopGPUDebugGroup(void) { return 0; }
void *SDL_pow(void) { return 0; }
void *SDL_powf(void) { return 0; }
void *SDL_PremultiplyAlpha(void) { return 0; }
void *SDL_PremultiplySurfaceAlpha(void) { return 0; }
void *SDL_PumpEvents(void) { return 0; }
void *SDL_PushEvent(void) { return 0; }
void *SDL_PushGPUComputeUniformData(void) { return 0; }
void *SDL_PushGPUDebugGroup(void) { return 0; }
void *SDL_PushGPUFragmentUniformData(void) { return 0; }
void *SDL_PushGPUVertexUniformData(void) { return 0; }
void *SDL_PutAudioStreamData(void) { return 0; }
void *SDL_PutAudioStreamDataNoCopy(void) { return 0; }
void *SDL_PutAudioStreamPlanarData(void) { return 0; }
void *SDL_qsort(void) { return 0; }
void *SDL_qsort_r(void) { return 0; }
void *SDL_QueryGPUFence(void) { return 0; }
void *SDL_Quit(void) { return 0; }
void *SDL_QuitSubSystem(void) { return 0; }
int SDL_RaiseWindow(void *window) { return 1; }
void *SDL_rand(void) { return 0; }
void *SDL_rand_bits(void) { return 0; }
void *SDL_rand_bits_r(void) { return 0; }
void *SDL_randf(void) { return 0; }
void *SDL_randf_r(void) { return 0; }
void *SDL_rand_r(void) { return 0; }
void *SDL_ReadAsyncIO(void) { return 0; }
void *SDL_ReadIO(void) { return 0; }
void *SDL_ReadProcess(void) { return 0; }
void *SDL_ReadS16BE(void) { return 0; }
void *SDL_ReadS16LE(void) { return 0; }
void *SDL_ReadS32BE(void) { return 0; }
void *SDL_ReadS32LE(void) { return 0; }
void *SDL_ReadS64BE(void) { return 0; }
void *SDL_ReadS64LE(void) { return 0; }
void *SDL_ReadS8(void) { return 0; }
void *SDL_ReadStorageFile(void) { return 0; }
void *SDL_ReadSurfacePixel(void) { return 0; }
void *SDL_ReadSurfacePixelFloat(void) { return 0; }
void *SDL_ReadU16BE(void) { return 0; }
void *SDL_ReadU16LE(void) { return 0; }
void *SDL_ReadU32BE(void) { return 0; }
void *SDL_ReadU32LE(void) { return 0; }
void *SDL_ReadU64BE(void) { return 0; }
void *SDL_ReadU64LE(void) { return 0; }
void *SDL_ReadU8(void) { return 0; }
void *SDL_realloc(void) { return 0; }
void *SDL_RegisterApp(void) { return 0; }
void *SDL_RegisterEvents(void) { return 0; }
void *SDL_ReleaseCameraFrame(void) { return 0; }
void *SDL_ReleaseGPUBuffer(void) { return 0; }
void *SDL_ReleaseGPUComputePipeline(void) { return 0; }
void *SDL_ReleaseGPUFence(void) { return 0; }
void *SDL_ReleaseGPUGraphicsPipeline(void) { return 0; }
void *SDL_ReleaseGPUSampler(void) { return 0; }
void *SDL_ReleaseGPUShader(void) { return 0; }
void *SDL_ReleaseGPUTexture(void) { return 0; }
void *SDL_ReleaseGPUTransferBuffer(void) { return 0; }
void *SDL_ReleaseWindowFromGPUDevice(void) { return 0; }
void *SDL_ReloadGamepadMappings(void) { return 0; }
void *SDL_RemoveEventWatch(void) { return 0; }
void *SDL_RemoveHintCallback(void) { return 0; }
void *SDL_RemovePath(void) { return 0; }
void *SDL_RemoveStoragePath(void) { return 0; }
void *SDL_RemoveSurfaceAlternateImages(void) { return 0; }
void *SDL_RemoveTimer(void) { return 0; }
void *SDL_RemoveTrayEntry(void) { return 0; }
void *SDL_RenamePath(void) { return 0; }
void *SDL_RenameStoragePath(void) { return 0; }
int SDL_RenderClear(void *renderer) { return 1; }
void *SDL_RenderClipEnabled(void) { return 0; }
void *SDL_RenderCoordinatesFromWindow(void) { return 0; }
void *SDL_RenderCoordinatesToWindow(void) { return 0; }
void *SDL_RenderDebugText(void) { return 0; }
void *SDL_RenderDebugTextFormat(void) { return 0; }
void *SDL_RenderFillRect(void) { return 0; }
void *SDL_RenderFillRects(void) { return 0; }
void *SDL_RenderGeometry(void) { return 0; }
void *SDL_RenderGeometryRaw(void) { return 0; }
void *SDL_RenderLine(void) { return 0; }
void *SDL_RenderLines(void) { return 0; }
void *SDL_RenderPoint(void) { return 0; }
void *SDL_RenderPoints(void) { return 0; }
int SDL_RenderPresent(void *renderer) { return 1; }
void *SDL_RenderReadPixels(void) { return 0; }
void *SDL_RenderRect(void) { return 0; }
void *SDL_RenderRects(void) { return 0; }
int SDL_RenderTexture(void *renderer, void *texture, void *srcrect, void *dstrect) { return 1; }
void *SDL_RenderTexture9Grid(void) { return 0; }
void *SDL_RenderTexture9GridTiled(void) { return 0; }
void *SDL_RenderTextureAffine(void) { return 0; }
void *SDL_RenderTextureRotated(void) { return 0; }
void *SDL_RenderTextureTiled(void) { return 0; }
void *SDL_RenderViewportSet(void) { return 0; }
void *SDL_ReportAssertion(void) { return 0; }
void *SDL_RequestAndroidPermission(void) { return 0; }
void *SDL_ResetAssertionReport(void) { return 0; }
void *SDL_ResetHint(void) { return 0; }
void *SDL_ResetHints(void) { return 0; }
void *SDL_ResetKeyboard(void) { return 0; }
void *SDL_ResetLogPriorities(void) { return 0; }
void *SDL_RestoreWindow(void) { return 0; }
void *SDL_ResumeAudioDevice(void) { return 0; }
void *SDL_ResumeAudioStreamDevice(void) { return 0; }
void *SDL_ResumeHaptic(void) { return 0; }
void *SDL_RotateSurface(void) { return 0; }
void *SDL_round(void) { return 0; }
void *SDL_roundf(void) { return 0; }
void *SDL_RumbleGamepad(void) { return 0; }
void *SDL_RumbleGamepadTriggers(void) { return 0; }
void *SDL_RumbleJoystick(void) { return 0; }
void *SDL_RumbleJoystickTriggers(void) { return 0; }
void *SDL_RunApp(void) { return 0; }
void *SDL_RunHapticEffect(void) { return 0; }
void *SDL_RunOnMainThread(void) { return 0; }
void *SDL_SaveBMP(void) { return 0; }
void *SDL_SaveBMP_IO(void) { return 0; }
void *SDL_SaveFile(void) { return 0; }
void *SDL_SaveFile_IO(void) { return 0; }
void *SDL_SavePNG(void) { return 0; }
void *SDL_SavePNG_IO(void) { return 0; }
void *SDL_scalbn(void) { return 0; }
void *SDL_scalbnf(void) { return 0; }
void *SDL_ScaleSurface(void) { return 0; }
void *SDL_ScreenKeyboardShown(void) { return 0; }
void *SDL_ScreenSaverEnabled(void) { return 0; }
void *SDL_SeekIO(void) { return 0; }
void *SDL_SendAndroidBackButton(void) { return 0; }
void *SDL_SendAndroidMessage(void) { return 0; }
void *SDL_SendGamepadEffect(void) { return 0; }
void *SDL_SendJoystickEffect(void) { return 0; }
void *SDL_SendJoystickVirtualSensorData(void) { return 0; }
void *SDL_SetAppMetadata(void) { return 0; }
void *SDL_SetAppMetadataProperty(void) { return 0; }
void *SDL_SetAssertionHandler(void) { return 0; }
void *SDL_SetAtomicInt(void) { return 0; }
void *SDL_SetAtomicPointer(void) { return 0; }
void *SDL_SetAtomicU32(void) { return 0; }
void *SDL_SetAudioDeviceGain(void) { return 0; }
void *SDL_SetAudioPostmixCallback(void) { return 0; }
void *SDL_SetAudioStreamFormat(void) { return 0; }
void *SDL_SetAudioStreamFrequencyRatio(void) { return 0; }
void *SDL_SetAudioStreamGain(void) { return 0; }
void *SDL_SetAudioStreamGetCallback(void) { return 0; }
void *SDL_SetAudioStreamInputChannelMap(void) { return 0; }
void *SDL_SetAudioStreamOutputChannelMap(void) { return 0; }
void *SDL_SetAudioStreamPutCallback(void) { return 0; }
void *SDL_SetBooleanProperty(void) { return 0; }
void *SDL_SetClipboardData(void) { return 0; }
void *SDL_SetClipboardText(void) { return 0; }
void *SDL_SetCurrentThreadPriority(void) { return 0; }
void *SDL_SetCursor(void) { return 0; }
void *SDL_SetDefaultTextureScaleMode(void) { return 0; }
void *SDL_SetEnvironmentVariable(void) { return 0; }
void *SDL_setenv_unsafe(void) { return 0; }
void *SDL_SetError(void) { return 0; }
void *SDL_SetErrorV(void) { return 0; }
void *SDL_SetEventEnabled(void) { return 0; }
void *SDL_SetEventFilter(void) { return 0; }
void *SDL_SetFloatProperty(void) { return 0; }
void *SDL_SetGamepadEventsEnabled(void) { return 0; }
void *SDL_SetGamepadLED(void) { return 0; }
void *SDL_SetGamepadMapping(void) { return 0; }
void *SDL_SetGamepadPlayerIndex(void) { return 0; }
void *SDL_SetGamepadSensorEnabled(void) { return 0; }
void *SDL_SetGPUAllowedFramesInFlight(void) { return 0; }
void *SDL_SetGPUBlendConstants(void) { return 0; }
void *SDL_SetGPUBufferName(void) { return 0; }
void *SDL_SetGPURenderState(void) { return 0; }
void *SDL_SetGPURenderStateFragmentUniforms(void) { return 0; }
void *SDL_SetGPUScissor(void) { return 0; }
void *SDL_SetGPUStencilReference(void) { return 0; }
void *SDL_SetGPUSwapchainParameters(void) { return 0; }
void *SDL_SetGPUTextureName(void) { return 0; }
void *SDL_SetGPUViewport(void) { return 0; }
void *SDL_SetHapticAutocenter(void) { return 0; }
void *SDL_SetHapticGain(void) { return 0; }
int SDL_SetHint(const char *name, const char *value) { return 1; }
int SDL_SetHintWithPriority(const char *name, const char *value, int priority) { return 1; }
void *SDL_SetInitialized(void) { return 0; }
void *SDL_SetiOSAnimationCallback(void) { return 0; }
void *SDL_SetiOSEventPump(void) { return 0; }
void *SDL_SetJoystickEventsEnabled(void) { return 0; }
void *SDL_SetJoystickLED(void) { return 0; }
void *SDL_SetJoystickPlayerIndex(void) { return 0; }
void *SDL_SetJoystickVirtualAxis(void) { return 0; }
void *SDL_SetJoystickVirtualBall(void) { return 0; }
void *SDL_SetJoystickVirtualButton(void) { return 0; }
void *SDL_SetJoystickVirtualHat(void) { return 0; }
void *SDL_SetJoystickVirtualTouchpad(void) { return 0; }
void *SDL_SetLinuxThreadPriority(void) { return 0; }
void *SDL_SetLinuxThreadPriorityAndPolicy(void) { return 0; }
void *SDL_SetLogOutputFunction(void) { return 0; }
void *SDL_SetLogPriorities(void) { return 0; }
void *SDL_SetLogPriority(void) { return 0; }
void *SDL_SetLogPriorityPrefix(void) { return 0; }
void *SDL_SetMainReady(void) { return 0; }
void *SDL_SetMemoryFunctions(void) { return 0; }
void *SDL_SetModState(void) { return 0; }
int SDL_SetNumberProperty(unsigned int props, const char *name, long long value) {
    if (name) { sdl_log("SDL3-STUB: SDL_SetNumberProperty: "); sdl_log(name); sdl_log("\n"); }
    return 1; /* true = success */
}
void *SDL_SetPaletteColors(void) { return 0; }
int SDL_SetPointerProperty(unsigned int props, const char *name, void *value) {
    if (name) { sdl_log("SDL3-STUB: SDL_SetPointerProperty: "); sdl_log(name); sdl_log("\n"); }
    return 1;
}
int SDL_SetPointerPropertyWithCleanup(unsigned int props, const char *name, void *value, void (*cleanup)(void*, void*), void *userdata) {
    return 1;
}
void *SDL_SetPrimarySelectionText(void) { return 0; }
void *SDL_SetRelativeMouseTransform(void) { return 0; }
void *SDL_SetRenderClipRect(void) { return 0; }
void *SDL_SetRenderColorScale(void) { return 0; }
void *SDL_SetRenderDrawBlendMode(void) { return 0; }
int SDL_SetRenderDrawColor(void *renderer, unsigned char r, unsigned char g, unsigned char b, unsigned char a) { return 1; }
int SDL_SetRenderDrawColorFloat(void *renderer, float r, float g, float b, float a) { return 1; }
void *SDL_SetRenderLogicalPresentation(void) { return 0; }
void *SDL_SetRenderScale(void) { return 0; }
void *SDL_SetRenderTarget(void) { return 0; }
void *SDL_SetRenderTextureAddressMode(void) { return 0; }
void *SDL_SetRenderViewport(void) { return 0; }
void *SDL_SetRenderVSync(void) { return 0; }
void *SDL_SetScancodeName(void) { return 0; }
int SDL_SetStringProperty(unsigned int props, const char *name, const char *value) {
    if (name) { sdl_log("SDL3-STUB: SDL_SetStringProperty: "); sdl_log(name); sdl_log("\n"); }
    return 1; /* true = success */
}
void *SDL_SetSurfaceAlphaMod(void) { return 0; }
void *SDL_SetSurfaceBlendMode(void) { return 0; }
void *SDL_SetSurfaceClipRect(void) { return 0; }
void *SDL_SetSurfaceColorKey(void) { return 0; }
void *SDL_SetSurfaceColorMod(void) { return 0; }
void *SDL_SetSurfaceColorspace(void) { return 0; }
void *SDL_SetSurfacePalette(void) { return 0; }
void *SDL_SetSurfaceRLE(void) { return 0; }
void *SDL_SetTextInputArea(void) { return 0; }
void *SDL_SetTextureAlphaMod(void) { return 0; }
void *SDL_SetTextureAlphaModFloat(void) { return 0; }
void *SDL_SetTextureBlendMode(void) { return 0; }
void *SDL_SetTextureColorMod(void) { return 0; }
void *SDL_SetTextureColorModFloat(void) { return 0; }
void *SDL_SetTexturePalette(void) { return 0; }
void *SDL_SetTextureScaleMode(void) { return 0; }
void *SDL_SetTLS(void) { return 0; }
void *SDL_SetTrayEntryCallback(void) { return 0; }
void *SDL_SetTrayEntryChecked(void) { return 0; }
void *SDL_SetTrayEntryEnabled(void) { return 0; }
void *SDL_SetTrayEntryLabel(void) { return 0; }
void *SDL_SetTrayIcon(void) { return 0; }
void *SDL_SetTrayTooltip(void) { return 0; }
void *SDL_SetWindowAlwaysOnTop(void) { return 0; }
void *SDL_SetWindowAspectRatio(void) { return 0; }
void *SDL_SetWindowBordered(void) { return 0; }
void *SDL_SetWindowFillDocument(void) { return 0; }
void *SDL_SetWindowFocusable(void) { return 0; }
void *SDL_SetWindowFullscreen(void) { return 0; }
void *SDL_SetWindowFullscreenMode(void) { return 0; }
void *SDL_SetWindowHitTest(void) { return 0; }
void *SDL_SetWindowIcon(void) { return 0; }
void *SDL_SetWindowKeyboardGrab(void) { return 0; }
void *SDL_SetWindowMaximumSize(void) { return 0; }
void *SDL_SetWindowMinimumSize(void) { return 0; }
void *SDL_SetWindowModal(void) { return 0; }
void *SDL_SetWindowMouseGrab(void) { return 0; }
void *SDL_SetWindowMouseRect(void) { return 0; }
void *SDL_SetWindowOpacity(void) { return 0; }
void *SDL_SetWindowParent(void) { return 0; }
int SDL_SetWindowPosition(void *window, int x, int y) { return 1; }
void *SDL_SetWindowProgressState(void) { return 0; }
void *SDL_SetWindowProgressValue(void) { return 0; }
void *SDL_SetWindowRelativeMouseMode(void) { return 0; }
void *SDL_SetWindowResizable(void) { return 0; }
void *SDL_SetWindowShape(void) { return 0; }
int SDL_SetWindowSize(void *window, int w, int h) { return 1; }
void *SDL_SetWindowsMessageHook(void) { return 0; }
void *SDL_SetWindowSurfaceVSync(void) { return 0; }
int SDL_SetWindowTitle(void *window, const char *title) { sdl_log("SDL3-STUB: SDL_SetWindowTitle called\n"); return 1; }
void *SDL_SetX11EventHook(void) { return 0; }
void *SDL_ShouldInit(void) { return 0; }
void *SDL_ShouldQuit(void) { return 0; }
void *SDL_ShowAndroidToast(void) { return 0; }
void *SDL_ShowCursor(void) { return 0; }
void *SDL_ShowFileDialogWithProperties(void) { return 0; }
void *SDL_ShowMessageBox(void) { return 0; }
void *SDL_ShowOpenFileDialog(void) { return 0; }
void *SDL_ShowOpenFolderDialog(void) { return 0; }
void *SDL_ShowSaveFileDialog(void) { return 0; }
void *SDL_ShowSimpleMessageBox(void) { return 0; }
int SDL_ShowWindow(void *window) { sdl_log("SDL3-STUB: SDL_ShowWindow called\n"); return 1; }
void *SDL_ShowWindowSystemMenu(void) { return 0; }
void *SDL_SignalAsyncIOQueue(void) { return 0; }
void *SDL_SignalCondition(void) { return 0; }
void *SDL_SignalSemaphore(void) { return 0; }
void *SDL_sin(void) { return 0; }
void *SDL_sinf(void) { return 0; }
void *SDL_snprintf(void) { return 0; }
void *SDL_sqrt(void) { return 0; }
void *SDL_sqrtf(void) { return 0; }
void *SDL_srand(void) { return 0; }
void *SDL_sscanf(void) { return 0; }
void *SDL_StartTextInput(void) { return 0; }
void *SDL_StartTextInputWithProperties(void) { return 0; }
void *SDL_StepBackUTF8(void) { return 0; }
void *SDL_StepUTF8(void) { return 0; }
void *SDL_StopHapticEffect(void) { return 0; }
void *SDL_StopHapticEffects(void) { return 0; }
void *SDL_StopHapticRumble(void) { return 0; }
void *SDL_StopTextInput(void) { return 0; }
void *SDL_StorageReady(void) { return 0; }
void *SDL_strcasecmp(void) { return 0; }
void *SDL_strcasestr(void) { return 0; }
void *SDL_strchr(void) { return 0; }
void *SDL_strcmp(void) { return 0; }
void *SDL_strdup(void) { return 0; }
void *SDL_StretchSurface(void) { return 0; }
void *SDL_StringToGUID(void) { return 0; }
void *SDL_strlcat(void) { return 0; }
void *SDL_strlcpy(void) { return 0; }
void *SDL_strlen(void) { return 0; }
void *SDL_strlwr(void) { return 0; }
void *SDL_strncasecmp(void) { return 0; }
void *SDL_strncmp(void) { return 0; }
void *SDL_strndup(void) { return 0; }
void *SDL_strnlen(void) { return 0; }
void *SDL_strnstr(void) { return 0; }
void *SDL_strpbrk(void) { return 0; }
void *SDL_strrchr(void) { return 0; }
void *SDL_strrev(void) { return 0; }
void *SDL_strstr(void) { return 0; }
void *SDL_strtod(void) { return 0; }
void *SDL_strtok_r(void) { return 0; }
void *SDL_strtol(void) { return 0; }
void *SDL_strtoll(void) { return 0; }
void *SDL_strtoul(void) { return 0; }
void *SDL_strtoull(void) { return 0; }
void *SDL_strupr(void) { return 0; }
void *SDL_SubmitGPUCommandBuffer(void) { return 0; }
void *SDL_SubmitGPUCommandBufferAndAcquireFence(void) { return 0; }
void *SDL_SurfaceHasAlternateImages(void) { return 0; }
void *SDL_SurfaceHasColorKey(void) { return 0; }
void *SDL_SurfaceHasRLE(void) { return 0; }
void *SDL_swprintf(void) { return 0; }
void *SDL_SyncWindow(void) { return 0; }
void *SDL_tan(void) { return 0; }
void *SDL_tanf(void) { return 0; }
void *SDL_TellIO(void) { return 0; }
void *SDL_TextInputActive(void) { return 0; }
void *SDL_TimeFromWindows(void) { return 0; }
void *SDL_TimeToDateTime(void) { return 0; }
void *SDL_TimeToWindows(void) { return 0; }
void *SDL_tolower(void) { return 0; }
void *SDL_toupper(void) { return 0; }
void *SDL_trunc(void) { return 0; }
void *SDL_truncf(void) { return 0; }
void *SDL_TryLockMutex(void) { return 0; }
void *SDL_TryLockRWLockForReading(void) { return 0; }
void *SDL_TryLockRWLockForWriting(void) { return 0; }
void *SDL_TryLockSpinlock(void) { return 0; }
void *SDL_TryWaitSemaphore(void) { return 0; }
void *SDL_UCS4ToUTF8(void) { return 0; }
void *SDL_uitoa(void) { return 0; }
void *SDL_ulltoa(void) { return 0; }
void *SDL_ultoa(void) { return 0; }
void *SDL_UnbindAudioStream(void) { return 0; }
void *SDL_UnbindAudioStreams(void) { return 0; }
void *SDL_UnloadObject(void) { return 0; }
void *SDL_UnlockAudioStream(void) { return 0; }
void *SDL_UnlockJoysticks(void) { return 0; }
void *SDL_UnlockMutex(void) { return 0; }
void *SDL_UnlockProperties(void) { return 0; }
void *SDL_UnlockRWLock(void) { return 0; }
void *SDL_UnlockSpinlock(void) { return 0; }
void SDL_UnlockSurface(void *surface) { }
void *SDL_UnlockTexture(void) { return 0; }
void *SDL_UnmapGPUTransferBuffer(void) { return 0; }
void *SDL_UnregisterApp(void) { return 0; }
void *SDL_UnsetEnvironmentVariable(void) { return 0; }
void *SDL_unsetenv_unsafe(void) { return 0; }
void *SDL_UpdateGamepads(void) { return 0; }
void *SDL_UpdateHapticEffect(void) { return 0; }
void *SDL_UpdateJoysticks(void) { return 0; }
void *SDL_UpdateNVTexture(void) { return 0; }
void *SDL_UpdateSensors(void) { return 0; }
void *SDL_UpdateTexture(void) { return 0; }
void *SDL_UpdateTrays(void) { return 0; }
int SDL_UpdateWindowSurface(void *window) { return 1; } /* true = success */
int SDL_UpdateWindowSurfaceRects(void *window, void *rects, int numrects) { return 1; }
void *SDL_UpdateYUVTexture(void) { return 0; }
void *SDL_UploadToGPUBuffer(void) { return 0; }
void *SDL_UploadToGPUTexture(void) { return 0; }
void *SDL_utf8strlcpy(void) { return 0; }
void *SDL_utf8strlen(void) { return 0; }
void *SDL_utf8strnlen(void) { return 0; }
void *SDL_vasprintf(void) { return 0; }
void *SDL_vsnprintf(void) { return 0; }
void *SDL_vsscanf(void) { return 0; }
void *SDL_vswprintf(void) { return 0; }
void *SDL_Vulkan_CreateSurface(void) { return 0; }
void *SDL_Vulkan_DestroySurface(void) { return 0; }
void *SDL_Vulkan_GetInstanceExtensions(void) { return 0; }
void *SDL_Vulkan_GetPresentationSupport(void) { return 0; }
void *SDL_Vulkan_GetVkGetInstanceProcAddr(void) { return 0; }
void *SDL_Vulkan_LoadLibrary(void) { return 0; }
void *SDL_Vulkan_UnloadLibrary(void) { return 0; }
void *SDL_WaitAndAcquireGPUSwapchainTexture(void) { return 0; }
void *SDL_WaitAsyncIOResult(void) { return 0; }
void *SDL_WaitCondition(void) { return 0; }
void *SDL_WaitConditionTimeout(void) { return 0; }
void *SDL_WaitEvent(void) { return 0; }
void *SDL_WaitEventTimeout(void) { return 0; }
void *SDL_WaitForGPUFences(void) { return 0; }
void *SDL_WaitForGPUIdle(void) { return 0; }
void *SDL_WaitForGPUSwapchain(void) { return 0; }
void *SDL_WaitProcess(void) { return 0; }
void *SDL_WaitSemaphore(void) { return 0; }
void *SDL_WaitSemaphoreTimeout(void) { return 0; }
void *SDL_WaitThread(void) { return 0; }
void *SDL_WarpMouseGlobal(void) { return 0; }
void *SDL_WarpMouseInWindow(void) { return 0; }
unsigned int SDL_WasInit(unsigned int flags) { return flags; } /* pretend all inited */
void *SDL_wcscasecmp(void) { return 0; }
void *SDL_wcscmp(void) { return 0; }
void *SDL_wcsdup(void) { return 0; }
void *SDL_wcslcat(void) { return 0; }
void *SDL_wcslcpy(void) { return 0; }
void *SDL_wcslen(void) { return 0; }
void *SDL_wcsncasecmp(void) { return 0; }
void *SDL_wcsncmp(void) { return 0; }
void *SDL_wcsnlen(void) { return 0; }
void *SDL_wcsnstr(void) { return 0; }
void *SDL_wcsstr(void) { return 0; }
void *SDL_wcstol(void) { return 0; }
void *SDL_WindowHasSurface(void) { return 0; }
void *SDL_WindowSupportsGPUPresentMode(void) { return 0; }
void *SDL_WindowSupportsGPUSwapchainComposition(void) { return 0; }
void *SDL_WriteAsyncIO(void) { return 0; }
void *SDL_WriteIO(void) { return 0; }
void *SDL_WriteS16BE(void) { return 0; }
void *SDL_WriteS16LE(void) { return 0; }
void *SDL_WriteS32BE(void) { return 0; }
void *SDL_WriteS32LE(void) { return 0; }
void *SDL_WriteS64BE(void) { return 0; }
void *SDL_WriteS64LE(void) { return 0; }
void *SDL_WriteS8(void) { return 0; }
void *SDL_WriteStorageFile(void) { return 0; }
void *SDL_WriteSurfacePixel(void) { return 0; }
void *SDL_WriteSurfacePixelFloat(void) { return 0; }
void *SDL_WriteU16BE(void) { return 0; }
void *SDL_WriteU16LE(void) { return 0; }
void *SDL_WriteU32BE(void) { return 0; }
void *SDL_WriteU32LE(void) { return 0; }
void *SDL_WriteU64BE(void) { return 0; }
void *SDL_WriteU64LE(void) { return 0; }
void *SDL_WriteU8(void) { return 0; }

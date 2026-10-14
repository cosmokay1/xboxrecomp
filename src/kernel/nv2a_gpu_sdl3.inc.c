/* ── Cross-Platform SDL3 GPU Presenter & Renderer (`nv2a_gpu_sdl3.inc.c`) ──
 *
 * Included by nv2a_gpu_d3d11.inc.c on non-Windows platforms (Linux / macOS)
 * and provides the SDL3_GPU pipeline + all video settings / menu / HUD exports.
 */

#include <SDL3/SDL.h>
#include "../input/xinput_xbox.h"
#if defined(__linux__)
#include "../platform/guest_memory.h"
#endif

extern void *xbox_FramebufferWindowHwnd(void);
extern void  xbox_FramebufferSetGpuPresent(int on);
extern void  xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk));
extern void  xbox_FramebufferApplyMode(int mode, int w, int h);
extern char *xbox_user_path(const char *rel, char *out, size_t n);

int  xbox_ggl_release_all(void);
void xbox_ggl_reacquire(int depth);

/* ---- Texture override hooks (used by src/nfl2k5_controller_art.cpp) ---- */
int (*g_xbox_tex_override_wants)(uint32_t fmt, uint32_t w, uint32_t h) = NULL;
uint32_t (*g_xbox_tex_override_gen)(void) = NULL;
const uint32_t *(*g_xbox_tex_override)(uint32_t fmt, uint32_t w, uint32_t h, uint32_t content,
                                       const uint32_t *orig, uint32_t *out_w, uint32_t *out_h) = NULL;
volatile long g_xbox_tex_dump_wanted = 0;
int (*g_xbox_prompt_style)(void) = NULL;

/* ---- Video settings & F1 menu state (matches nv2a_gpu_present.inc.c) --- */
enum { VM_WINDOWED, VM_BORDERLESS, VM_FULLSCREEN };
enum { AR_ORIGINAL, AR_STRETCH, AR_16_9, AR_21_9, AR_32_9, AR_ZOOM };
enum { SF_BILINEAR, SF_SHARP, SF_NEAREST, SF_ENHANCED };

typedef struct {
    int mode;
    int res_w, res_h;
    int aspect;
    int scale;
    int filter;
    int vsync;
    int brightness;
    int gamma;
    int crop;
    int show_fps;
    int aniso;
    int fps_limit;
    int widescreen;
    int lighting;
} VideoSettings;

static VideoSettings s_vs = { VM_BORDERLESS, 1920, 1080, AR_ORIGINAL, 3, SF_ENHANCED, 1, 100, 100, 0, 0, 8, 0, 1, 0 };
static SDL_AtomicInt s_vs_dirty = { 1 };
static const char *k_vs_file = "nfl2k5_video.ini";

volatile LONG g_xbox_input_blocked = 0;
void (*g_xbox_fps_limit_changed)(void) = NULL;
int (*xbox_present_in_match_fn)(void) = NULL;
int xbox_hud_shared_failed = 1;

typedef struct { int bb_w, bb_h; float game_x, game_y, game_w, game_h; } XboxHudFrame;
typedef struct { const void *pixels; int w, h, stride, x, y, changed, dw, dh; void *shared; } XboxHudImage;
typedef int (*XboxHudCallback)(const XboxHudFrame *f, XboxHudImage *img);
static XboxHudCallback s_hud_cb = NULL;

void xbox_PresentSetHudCallback(XboxHudCallback cb) { s_hud_cb = cb; }
int  xbox_VideoFpsLimit(void) { return s_vs.fps_limit; }

static void vs_load(void)
{
    char vpath[512];
    FILE *f = fopen(xbox_user_path(k_vs_file, vpath, sizeof vpath), "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int v, v2;
        if (sscanf(line, " resolution = %dx%d", &v, &v2) == 2) { s_vs.res_w = v; s_vs.res_h = v2; continue; }
        if (sscanf(line, " %63[a-z_] = %d", key, &v) != 2) continue;
        if (!strcmp(key, "mode")) s_vs.mode = v;
        else if (!strcmp(key, "aspect")) s_vs.aspect = v;
        else if (!strcmp(key, "scale")) s_vs.scale = v;
        else if (!strcmp(key, "filter")) s_vs.filter = v;
        else if (!strcmp(key, "vsync")) s_vs.vsync = v;
        else if (!strcmp(key, "brightness")) s_vs.brightness = v;
        else if (!strcmp(key, "gamma")) s_vs.gamma = v;
        else if (!strcmp(key, "crop")) s_vs.crop = v;
        else if (!strcmp(key, "show_fps")) s_vs.show_fps = v;
        else if (!strcmp(key, "aniso")) s_vs.aniso = v;
        else if (!strcmp(key, "fps_limit")) s_vs.fps_limit = v;
        else if (!strcmp(key, "widescreen")) s_vs.widescreen = v != 0;
        else if (!strcmp(key, "lighting")) s_vs.lighting = v;
    }
    fclose(f);
}

static void vs_save(void)
{
    char vpath[512];
    FILE *f = fopen(xbox_user_path(k_vs_file, vpath, sizeof vpath), "w");
    if (!f) return;
    fprintf(f, "mode = %d\nresolution = %dx%d\naspect = %d\nscale = %d\nfilter = %d\n"
               "vsync = %d\nbrightness = %d\ngamma = %d\ncrop = %d\nshow_fps = %d\n"
               "aniso = %d\nfps_limit = %d\nwidescreen = %d\nlighting = %d\n",
            s_vs.mode, s_vs.res_w, s_vs.res_h, s_vs.aspect, s_vs.scale, s_vs.filter,
            s_vs.vsync, s_vs.brightness, s_vs.gamma, s_vs.crop, s_vs.show_fps,
            s_vs.aniso, s_vs.fps_limit, s_vs.widescreen, s_vs.lighting);
    fclose(f);
}

static int s_res[][2] = {
    { 960, 720 }, { 1280, 720 }, { 1440, 1080 }, { 1600, 900 }, { 1920, 1080 },
    { 2560, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 }
};
static const int s_nres = (int)(sizeof(s_res) / sizeof(s_res[0]));

void xbox_VideoSettingsLoad(void)
{
    static SDL_AtomicInt once;
    if (SDL_CompareAndSwapAtomicInt(&once, 0, 1))
        vs_load();
}

enum { MI_MODE, MI_RES, MI_WIDE, MI_ASPECT, MI_SCALE, MI_LIGHTING, MI_FILTER, MI_VSYNC, MI_BRIGHT, MI_GAMMA, MI_CROP,
       MI_ANISO, MI_FPS, MI_LIMIT, MI_RESET, MI_CLOSE, MI_COUNT };
static const int k_fps_limits[] = { 30, 60, 120, 0, -1 };
#define FPS_LIMITS 5
static const int k_vm_items[] = { MI_MODE, MI_RES, MI_WIDE, MI_ASPECT, MI_SCALE, MI_LIGHTING, MI_FILTER, MI_VSYNC,
                                  MI_BRIGHT, MI_GAMMA, MI_CROP, MI_ANISO, MI_FPS, MI_LIMIT };
#define VM_COUNT ((int)(sizeof k_vm_items / sizeof k_vm_items[0]))
static const int k_vm_aniso[] = { 1, 2, 4, 8, 16 };

typedef struct XboxMenuPage {
    const char *title;
    int (*count)(void);
    void (*label)(int row, char *name, char *val, size_t n);
    void (*change)(int row, int dir);
    void (*activate)(int row);
    int (*busy)(void);
} XboxMenuPage;

static SDL_AtomicInt s_menu_open;
int (*g_xbox_menu_pad)(XBOX_GAMEPAD *g) = NULL;
void xbox_MenuAddPage(const XboxMenuPage *p) { (void)p; }
void xbox_MenuInvalidate(void) {}
int  xbox_MenuIsOpen(void) { return SDL_GetAtomicInt(&s_menu_open) != 0; }
void xbox_PresentToast(const char *header, const char *line1, const char *line2, unsigned ms)
{
    (void)ms;
    fprintf(stderr, "[TOAST] %s: %s (%s)\n", header ? header : "", line1 ? line1 : "", line2 ? line2 : "");
}
void xbox_PresentRecord(int seconds) { (void)seconds; }

int xbox_VideoSettingCount(void) { return VM_COUNT; }
const char *xbox_VideoSettingName(int i)
{
    static const char *names[] = { "Display Mode", "Resolution", "Widescreen", "Aspect Ratio", "Render Scale", "Lighting & Materials",
                                   "Upscaling Filter", "VSync", "Brightness", "Gamma",
                                   "Overscan Crop", "Texture Filtering", "Show FPS", "Frame Rate Cap" };
    return (i >= 0 && i < VM_COUNT) ? names[i] : "";
}
int xbox_VideoSettingIsToggle(int i)
{
    return i >= 0 && i < VM_COUNT && (k_vm_items[i] == MI_VSYNC || k_vm_items[i] == MI_FPS || k_vm_items[i] == MI_WIDE);
}
int xbox_VideoSettingValues(int i)
{
    if (i < 0 || i >= VM_COUNT) return 1;
    switch (k_vm_items[i]) {
    case MI_MODE: return 3;
    case MI_FILTER: return 4;
    case MI_RES: return s_nres;
    case MI_ASPECT: return 6;
    case MI_SCALE: return 16;
    case MI_LIGHTING: return 3;
    case MI_VSYNC: case MI_FPS: case MI_WIDE: return 2;
    case MI_BRIGHT: case MI_GAMMA: return 21;
    case MI_CROP: case MI_ANISO: return 5;
    case MI_LIMIT: return FPS_LIMITS;
    }
    return 1;
}

static int vm_get(const VideoSettings *vs, int item)
{
    int k;
    switch (item) {
    case MI_MODE: return vs->mode;
    case MI_RES:
        for (k = 0; k < s_nres; k++)
            if (s_res[k][0] == vs->res_w && s_res[k][1] == vs->res_h) return k;
        return 0;
    case MI_ASPECT: return vs->aspect;
    case MI_SCALE: return vs->scale - 1;
    case MI_LIGHTING: return vs->lighting;
    case MI_FILTER: return vs->filter;
    case MI_VSYNC: return vs->vsync ? 1 : 0;
    case MI_BRIGHT: return (vs->brightness - 50) / 5;
    case MI_GAMMA: return (vs->gamma - 60) / 5;
    case MI_CROP: return vs->crop / 8;
    case MI_ANISO:
        for (k = 0; k < 5; k++) if (k_vm_aniso[k] == vs->aniso) return k;
        return 0;
    case MI_FPS: return vs->show_fps ? 1 : 0;
    case MI_WIDE: return vs->widescreen ? 1 : 0;
    case MI_LIMIT:
        for (k = 0; k < FPS_LIMITS; k++) if (k_fps_limits[k] == vs->fps_limit) return k;
        return 3;
    }
    return 0;
}

static void vm_set(VideoSettings *vs, int item, int idx)
{
    switch (item) {
    case MI_MODE: vs->mode = idx; break;
    case MI_RES: if (idx >= 0 && idx < s_nres) { vs->res_w = s_res[idx][0]; vs->res_h = s_res[idx][1]; } break;
    case MI_ASPECT: vs->aspect = idx; break;
    case MI_SCALE: vs->scale = idx + 1; break;
    case MI_LIGHTING: vs->lighting = idx; break;
    case MI_FILTER: vs->filter = idx; break;
    case MI_VSYNC: vs->vsync = idx; break;
    case MI_BRIGHT: vs->brightness = 50 + 5 * idx; break;
    case MI_GAMMA: vs->gamma = 60 + 5 * idx; break;
    case MI_CROP: vs->crop = 8 * idx; break;
    case MI_ANISO: vs->aniso = k_vm_aniso[idx % 5]; break;
    case MI_FPS: vs->show_fps = idx; break;
    case MI_WIDE: vs->widescreen = idx; break;
    case MI_LIMIT: vs->fps_limit = k_fps_limits[idx % FPS_LIMITS]; break;
    }
}

void xbox_VideoSettingValueName(int i, int idx, char *buf, size_t n)
{
    static const char *modes[] = { "Windowed", "Borderless", "Fullscreen" };
    static const char *aspects[] = { "Fill Screen", "Stretch", "16:9 Bars", "21:9 Wide", "32:9 Wide", "Zoom to Fill" };
    static const char *filters[] = { "Bilinear", "Sharp Bilinear", "Nearest", "Enhanced Sharp" };
    static const char *lighting[] = { "Classic Xbox", "Enhanced", "Maximum" };
    VideoSettings v = s_vs;
    int item;
    buf[0] = 0;
    if (i < 0 || i >= VM_COUNT) return;
    item = k_vm_items[i];
    vm_set(&v, item, idx);
    switch (item) {
    case MI_MODE: snprintf(buf, n, "%s", modes[v.mode % 3]); break;
    case MI_RES: snprintf(buf, n, "%d x %d", v.res_w, v.res_h); break;
    case MI_ASPECT: snprintf(buf, n, "%s", aspects[v.aspect % 6]); break;
    case MI_SCALE: snprintf(buf, n, "%dx (%d x %d)", v.scale, 720 * v.scale, 480 * v.scale); break;
    case MI_LIGHTING: snprintf(buf, n, "%s", lighting[v.lighting % 3]); break;
    case MI_FILTER: snprintf(buf, n, "%s", filters[v.filter % 4]); break;
    case MI_VSYNC: snprintf(buf, n, "%s", v.vsync ? "On" : "Off"); break;
    case MI_BRIGHT: snprintf(buf, n, "%d%%", v.brightness); break;
    case MI_GAMMA: snprintf(buf, n, "%d.%02d", v.gamma / 100, v.gamma % 100); break;
    case MI_CROP: if (v.crop) snprintf(buf, n, "%d px", v.crop); else snprintf(buf, n, "Off"); break;
    case MI_ANISO: if (v.aniso > 1) snprintf(buf, n, "%dx Anisotropic", v.aniso); else snprintf(buf, n, "Standard"); break;
    case MI_FPS: snprintf(buf, n, "%s", v.show_fps ? "On" : "Off"); break;
    case MI_WIDE: snprintf(buf, n, "%s", v.widescreen ? "On" : "Off"); break;
    case MI_LIMIT: if (v.fps_limit < 0) snprintf(buf, n, "Original (30/60)");
                   else if (v.fps_limit) snprintf(buf, n, "%d FPS", v.fps_limit); else snprintf(buf, n, "Unlimited"); break;
    }
}

int xbox_VideoSettingGet(int i)
{
    return (i >= 0 && i < VM_COUNT) ? vm_get(&s_vs, k_vm_items[i]) : 0;
}

void xbox_VideoSettingSet(int i, int idx)
{
    int item, n;
    if (i < 0 || i >= VM_COUNT) return;
    n = xbox_VideoSettingValues(i);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    item = k_vm_items[i];
    vm_set(&s_vs, item, idx);
    if (item == MI_LIMIT && g_xbox_fps_limit_changed) g_xbox_fps_limit_changed();
    if (item == MI_MODE || item == MI_RES) SDL_SetAtomicInt(&s_vs_dirty, 1);
    vs_save();
}

/* ---- SDL3 GPU Presenter for Guest Surface + Broadcast HUD -------------- */
static struct {
    int                   tried, ok;
    SDL_Window           *win;
    SDL_GPUDevice        *gpu;
    SDL_GPUTexture       *fb_tex;
    SDL_GPUTransferBuffer*fb_tbuf;
    uint32_t              fb_w, fb_h;
#if defined(__linux__)
    uint8_t              *fb_staging;
    size_t                fb_staging_capacity;
#endif
    SDL_GPUTexture       *hud_tex;
    SDL_GPUTransferBuffer*hud_tbuf;
    uint32_t              hud_w, hud_h;
} s_sdl_pr;

static int sdl_pr_init(void)
{
    if (s_sdl_pr.tried) return s_sdl_pr.ok;
    SDL_Window *win = (SDL_Window *)xbox_FramebufferWindowHwnd();
    if (!win) return 0; /* Wait until fb_present opens the window */

    s_sdl_pr.tried = 1;
    xbox_VideoSettingsLoad();

    s_sdl_pr.gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
        false, NULL);
    if (!s_sdl_pr.gpu) {
        fprintf(stderr, "  [SDL-GPU] SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
        return 0;
    }

    xbox_FramebufferSetGpuPresent(1);
    if (!SDL_ClaimWindowForGPUDevice(s_sdl_pr.gpu, win)) {
        fprintf(stderr, "  [SDL-GPU] SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(s_sdl_pr.gpu);
        s_sdl_pr.gpu = NULL;
        xbox_FramebufferSetGpuPresent(0);
        return 0;
    }

    s_sdl_pr.win = win;
    s_sdl_pr.ok = 1;
    fprintf(stderr, "  [SDL-GPU] Presenting via SDL3 GPU API (%s driver)\n",
            SDL_GetGPUDeviceDriver(s_sdl_pr.gpu));
    return 1;
}

static void sdl_pr_flip(void)
{
    if (!sdl_pr_init()) return;

    if (SDL_CompareAndSwapAtomicInt(&s_vs_dirty, 1, 0)) {
        xbox_FramebufferApplyMode(s_vs.mode, s_vs.res_w, s_vs.res_h);
    }

    uint64_t width = (uint64_t)s_gpu.clip_x + s_gpu.clip_w;
    uint64_t height = (uint64_t)s_gpu.clip_y + s_gpu.clip_h;
    if (!width || !height || width > UINT32_MAX || height > UINT32_MAX) return;
    uint32_t w = (uint32_t)width, h = (uint32_t)height;
    uint32_t pitch = s_gpu.pitch;
    uint32_t guest = dma_resolve(s_gpu.color_offset);
    if (!w || !h || !pitch || surface_bpp() != 4) return;
    uint64_t row_bytes = width * 4;
    /* SDL transfer-buffer sizes are Uint32. Validate before multiplication,
     * allocation or guest-address arithmetic can wrap. */
    if (row_bytes > pitch || height > UINT32_MAX / row_bytes) return;
    size_t frame_bytes = (size_t)(row_bytes * height);
#if defined(__linux__)
    if (s_sdl_pr.fb_staging_capacity < frame_bytes) {
        uint8_t *replacement = (uint8_t *)realloc(s_sdl_pr.fb_staging, frame_bytes);
        if (!replacement) return;
        s_sdl_pr.fb_staging = replacement;
        s_sdl_pr.fb_staging_capacity = frame_bytes;
    }
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    for (uint32_t y = 0; y < h; ++y) {
        /* A partial checked read is discarded: no SDL buffer is touched until
         * every guest row has been staged successfully in ordinary host RAM. */
        if (!xbox_guest_memory_read(base, (uint64_t)guest + (uint64_t)y * pitch,
                s_sdl_pr.fb_staging + (size_t)y * (size_t)row_bytes, (size_t)row_bytes))
            return;
    }
#endif

    if (!s_sdl_pr.fb_tex || s_sdl_pr.fb_w != w || s_sdl_pr.fb_h != h) {
        if (s_sdl_pr.fb_tex)  SDL_ReleaseGPUTexture(s_sdl_pr.gpu, s_sdl_pr.fb_tex);
        if (s_sdl_pr.fb_tbuf) SDL_ReleaseGPUTransferBuffer(s_sdl_pr.gpu, s_sdl_pr.fb_tbuf);

        SDL_GPUTextureCreateInfo tci;
        SDL_zero(tci);
        tci.type                 = SDL_GPU_TEXTURETYPE_2D;
        tci.format               = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tci.width                = w;
        tci.height               = h;
        tci.layer_count_or_depth = 1;
        tci.num_levels           = 1;
        s_sdl_pr.fb_tex = SDL_CreateGPUTexture(s_sdl_pr.gpu, &tci);

        SDL_GPUTransferBufferCreateInfo tbci;
        SDL_zero(tbci);
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size  = (Uint32)frame_bytes;
        s_sdl_pr.fb_tbuf = SDL_CreateGPUTransferBuffer(s_sdl_pr.gpu, &tbci);

        s_sdl_pr.fb_w = w;
        s_sdl_pr.fb_h = h;
    }

    if (!s_sdl_pr.fb_tex || !s_sdl_pr.fb_tbuf) return;

    uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(s_sdl_pr.gpu, s_sdl_pr.fb_tbuf, true);
    if (!mapped) return;
#if defined(__linux__)
    memcpy(mapped, s_sdl_pr.fb_staging, frame_bytes);
#else
    {
        const uint8_t *src = (const uint8_t *)xbox_GetMemoryOffset() + guest;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(mapped + (size_t)y * w * 4, src + (size_t)y * pitch, (size_t)w * 4);
        }
    }
#endif
    SDL_UnmapGPUTransferBuffer(s_sdl_pr.gpu, s_sdl_pr.fb_tbuf);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_sdl_pr.gpu);
    if (!cmd) return;

    SDL_GPUCopyPass *cpass = SDL_BeginGPUCopyPass(cmd);
    if (cpass) {
        SDL_GPUTextureTransferInfo tti;
        SDL_zero(tti);
        tti.transfer_buffer = s_sdl_pr.fb_tbuf;
        tti.pixels_per_row  = w;
        tti.rows_per_layer  = h;

        SDL_GPUTextureRegion reg;
        SDL_zero(reg);
        reg.texture = s_sdl_pr.fb_tex;
        reg.w       = w;
        reg.h       = h;
        reg.d       = 1;

        SDL_UploadToGPUTexture(cpass, &tti, &reg, true);
        SDL_EndGPUCopyPass(cpass);
    }

    SDL_GPUTexture *swap_tex = NULL;
    Uint32 sw_w = 0, sw_h = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_sdl_pr.win, &swap_tex, &sw_w, &sw_h) && swap_tex) {
        SDL_GPUBlitInfo blit;
        SDL_zero(blit);
        blit.source.texture      = s_sdl_pr.fb_tex;
        blit.source.w            = w;
        blit.source.h            = h;
        blit.destination.texture = swap_tex;
        blit.destination.w       = sw_w;
        blit.destination.h       = sw_h;
        blit.load_op             = SDL_GPU_LOADOP_CLEAR;
        blit.filter              = (s_vs.filter == SF_NEAREST) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &blit);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
}

/* On non-Windows builds, rasterisation runs through nv2a_pb_exec.c's CPU
 * pipeline and gpu_flip() presents the finished surface via SDL_GPU! */
static int  gpu_enabled(void)              { return 1; }
static void gpu_cpu_access(void)           {}
static int  gpu_draw_batch(int flat2d)     { (void)flat2d; return 0; }
static int  gpu_clear(uint32_t param)      { (void)param; return 0; }
static void gpu_flip(void)                 { sdl_pr_flip(); }

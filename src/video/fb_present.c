/**
 * fb_present.c -- Cross-platform SDL3 Game Window & Framebuffer Presenter
 * =============================================================================
 * Replaces legacy Win32/GDI window code with SDL3.
 * Compatible across Windows, Linux, and macOS.
 * 
 */

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define FB_TITLE          "ESPN NFL 2K5"
#define FB_GUEST_RAM_SIZE 0x04000000u

enum {
    FB_MODE_WINDOWED   = 0,
    FB_MODE_BORDERLESS = 1,
    FB_MODE_FULLSCREEN = 2
};

static struct {
    /* Lifecycle */
    SDL_AtomicInt    running;
    SDL_Thread      *thread;
    SDL_Window      *window;

    /* Fallback SDL renderer & texture */
    SDL_Renderer    *fallback_renderer;
    SDL_Texture     *fallback_texture;

    /* Guest buffer addresses & dimensions */
    SDL_AtomicU32    va;
    SDL_AtomicU32    pitch;
    SDL_AtomicU32    want_w, want_h;

    /* Thread-safe copy buffer */
    SDL_RWLock      *rgb_lock;
    uint32_t        *rgb;
    uint32_t         width, height;

    /* Presenter handoff & hooks */
    SDL_AtomicInt    gpu_present;
    void (* volatile key_hook)(unsigned vk);

    int              mode;
    int              client_w, client_h;
    int              ultrawide;
    Uint32           apply_mode_event;
} g_fb = {
    .width  = 640,
    .height = 480,
};

/* Maps SDL_Scancode to standard Windows Virtual Key codes for game key hooks */
static unsigned fb_scancode_to_vk(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 0x41u + (unsigned)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 0x31u + (unsigned)(sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0)
        return 0x30u;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70u + (unsigned)(sc - SDL_SCANCODE_F1);

    switch (sc) {
        case SDL_SCANCODE_ESCAPE:    return 0x1Bu;
        case SDL_SCANCODE_RETURN:    return 0x0Du;
        case SDL_SCANCODE_SPACE:     return 0x20u;
        case SDL_SCANCODE_BACKSPACE: return 0x08u;
        case SDL_SCANCODE_TAB:       return 0x09u;
        case SDL_SCANCODE_LEFT:      return 0x25u;
        case SDL_SCANCODE_UP:        return 0x26u;
        case SDL_SCANCODE_RIGHT:     return 0x27u;
        case SDL_SCANCODE_DOWN:      return 0x28u;
        default:                     return 0u;
    }
}

static void fb_convert(const uint8_t *src, uint32_t pitch, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < g_fb.height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = g_fb.rgb + (size_t)y * g_fb.width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)g_fb.width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < g_fb.width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)g_fb.width * 4);
        }
    }
}

static void fb_apply_mode_sdl(SDL_Window *win, int mode, int w, int h)
{
    g_fb.mode = mode;
    if (mode == FB_MODE_WINDOWED) {
        g_fb.client_w = w;
        g_fb.client_h = h;
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowBordered(win, true);
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_SetWindowFullscreenMode(win, NULL);
        SDL_SetWindowFullscreen(win, true);
    }
}

static void fb_apply_pending_size_sdl(SDL_Window *win)
{
    uint32_t ww = (uint32_t)SDL_GetAtomicU32(&g_fb.want_w);
    uint32_t wh = (uint32_t)SDL_GetAtomicU32(&g_fb.want_h);
    uint32_t pitch = (uint32_t)SDL_GetAtomicU32(&g_fb.pitch);
    uint32_t *nb;

    if (!pitch)
        pitch = ww * 4u;
    if (!ww || !wh || (ww == g_fb.width && wh == g_fb.height) || ww * 4u > pitch)
        return;

    nb = (uint32_t *)calloc((size_t)ww * wh, 4);
    if (!nb)
        return;

    SDL_LockRWLockForWriting(g_fb.rgb_lock);
    free(g_fb.rgb);
    g_fb.rgb    = nb;
    g_fb.width  = ww;
    g_fb.height = wh;
    SDL_UnlockRWLock(g_fb.rgb_lock);

    if (g_fb.fallback_texture && g_fb.fallback_renderer) {
        SDL_DestroyTexture(g_fb.fallback_texture);
        g_fb.fallback_texture = SDL_CreateTexture(
            g_fb.fallback_renderer,
            SDL_PIXELFORMAT_BGRA32,
            SDL_TEXTUREACCESS_STREAMING,
            (int)ww, (int)wh
        );
    }

    if (!g_fb.ultrawide && !SDL_GetAtomicInt(&g_fb.gpu_present) && g_fb.mode == FB_MODE_WINDOWED)
        SDL_SetWindowSize(win, (int)ww, (int)wh);
}

static void fb_draw_sdl_fallback(void)
{
    uint32_t va = (uint32_t)SDL_GetAtomicU32(&g_fb.va);
    uint32_t pitch = (uint32_t)SDL_GetAtomicU32(&g_fb.pitch);
    uint32_t base;
    uint64_t end;
    const uint8_t *src;

    if (!va || !pitch || !g_fb.rgb || !g_fb.width || pitch < g_fb.width)
        return;

    base = va & 0x7FFFFFFFu;
    end  = (uint64_t)base + (uint64_t)pitch * g_fb.height;
    if (!(va >= 0x80000000u || va < FB_GUEST_RAM_SIZE) || end > FB_GUEST_RAM_SIZE)
        return;

    src = (const uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());

    SDL_LockRWLockForWriting(g_fb.rgb_lock);
    fb_convert(src, pitch, pitch / g_fb.width);
    SDL_UpdateTexture(g_fb.fallback_texture, NULL, g_fb.rgb, (int)(g_fb.width * 4u));
    SDL_UnlockRWLock(g_fb.rgb_lock);

    SDL_SetRenderDrawColor(g_fb.fallback_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_fb.fallback_renderer);

    if (g_fb.ultrawide) {
        int out_w = 0, out_h = 0;
        SDL_GetRenderOutputSize(g_fb.fallback_renderer, &out_w, &out_h);
        if (out_w > 0 && out_h > 0) {
            float sx = (float)out_w / (float)g_fb.width;
            float sy = (float)out_h / (float)g_fb.height;
            float s  = sx < sy ? sx : sy;
            SDL_FRect dst = {
                .w = (float)g_fb.width * s,
                .h = (float)g_fb.height * s,
            };
            dst.x = ((float)out_w - dst.w) * 0.5f;
            dst.y = ((float)out_h - dst.h) * 0.5f;
            SDL_RenderTexture(g_fb.fallback_renderer, g_fb.fallback_texture, NULL, &dst);
        }
    } else {
        SDL_RenderTexture(g_fb.fallback_renderer, g_fb.fallback_texture, NULL, NULL);
    }

    SDL_RenderPresent(g_fb.fallback_renderer);
}

static int SDLCALL fb_thread_sdl(void *data)
{
    int dump_frames = 0;
    const char *dump_path = getenv("RECOMP_FB_DUMP");
    SDL_WindowFlags flags;
    SDL_Window *win;

    (void)data;

    g_fb.ultrawide = getenv("RECOMP_ULTRAWIDE") && *getenv("RECOMP_ULTRAWIDE") != '0';

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "  [FBWIN] SDL_Init failed: %s\n", SDL_GetError());
        SDL_SetAtomicInt(&g_fb.running, 0);
        return -1;
    }

    g_fb.apply_mode_event = SDL_RegisterEvents(1);

    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    g_fb.mode     = FB_MODE_WINDOWED;
    g_fb.client_w = (int)g_fb.width;
    g_fb.client_h = (int)g_fb.height;

    if (g_fb.ultrawide) {
        flags |= SDL_WINDOW_FULLSCREEN;
        g_fb.mode = FB_MODE_BORDERLESS;
    }

    win = SDL_CreateWindow(FB_TITLE, (int)g_fb.width, (int)g_fb.height, flags);
    if (!win) {
        fprintf(stderr, "  [FBWIN] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_SetAtomicInt(&g_fb.running, 0);
        return -1;
    }

    g_fb.rgb_lock = SDL_CreateRWLock();
    SDL_LockRWLockForWriting(g_fb.rgb_lock);
    g_fb.rgb = (uint32_t *)calloc((size_t)g_fb.width * g_fb.height, 4);
    SDL_UnlockRWLock(g_fb.rgb_lock);

    g_fb.window = win;

    fprintf(stderr, "  [FBWIN] Unified SDL3 window active (%ux%u surface, scale %.2f)\n",
            g_fb.width, g_fb.height, SDL_GetWindowDisplayScale(win));

    while (SDL_GetAtomicInt(&g_fb.running)) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                fprintf(stderr, "[WINDOW] SDL quit requested -- shutting down game\n");
                fflush(stderr);
                SDL_SetAtomicInt(&g_fb.running, 0);
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (g_fb.key_hook) {
                    unsigned vk = fb_scancode_to_vk(ev.key.scancode);
                    if (vk)
                        g_fb.key_hook(vk);
                }
            } else if (ev.type == g_fb.apply_mode_event) {
                fb_apply_mode_sdl(win, (int)ev.user.code,
                                  (int)(intptr_t)ev.user.data1,
                                  (int)(intptr_t)ev.user.data2);
            }
        }

        fb_apply_pending_size_sdl(win);

        if (!SDL_GetAtomicInt(&g_fb.gpu_present)) {
            if (!g_fb.fallback_renderer) {
                g_fb.fallback_renderer = SDL_CreateRenderer(win, NULL);
                g_fb.fallback_texture  = SDL_CreateTexture(
                    g_fb.fallback_renderer,
                    SDL_PIXELFORMAT_BGRA32,
                    SDL_TEXTUREACCESS_STREAMING,
                    (int)g_fb.width, (int)g_fb.height
                );
            }
            fb_draw_sdl_fallback();
        }

        if (dump_path && ++dump_frames == 60)
            xbox_FramebufferDumpBmp(dump_path);

        SDL_Delay(SDL_GetAtomicInt(&g_fb.gpu_present) ? 8 : 16);
    }

    g_fb.window = NULL;
    if (g_fb.fallback_texture)  SDL_DestroyTexture(g_fb.fallback_texture);
    if (g_fb.fallback_renderer) SDL_DestroyRenderer(g_fb.fallback_renderer);
    SDL_DestroyWindow(win);

    SDL_LockRWLockForWriting(g_fb.rgb_lock);
    free(g_fb.rgb);
    g_fb.rgb = NULL;
    SDL_UnlockRWLock(g_fb.rgb_lock);
    SDL_DestroyRWLock(g_fb.rgb_lock);

    exit(0);
    return 0;
}

static void fb_window_start(void)
{
    if (SDL_CompareAndSwapAtomicInt(&g_fb.running, 0, 1)) {
        g_fb.thread = SDL_CreateThread(fb_thread_sdl, "fb_thread", NULL);
        if (!g_fb.thread) {
            SDL_SetAtomicInt(&g_fb.running, 0);
        } else {
            SDL_DetachThread(g_fb.thread);
        }
    }
}

void xbox_FramebufferWindowStart(void)
{
    if (getenv("RECOMP_FB_WINDOW"))
        fb_window_start();
}

void xbox_FramebufferWindowForceStart(void)
{
    fb_window_start();
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");

    SDL_SetAtomicU32(&g_fb.va, pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va);
    if (pitch)
        SDL_SetAtomicU32(&g_fb.pitch, pitch);
}

void xbox_FramebufferWindowSize(uint32_t w, uint32_t h)
{
    if (w >= 64 && w <= 2048 && h >= 64 && h <= 2048) {
        SDL_SetAtomicU32(&g_fb.want_w, w);
        SDL_SetAtomicU32(&g_fb.want_h, h);
    }
}

void xbox_FramebufferApplyMode(int mode, int w, int h)
{
    SDL_Event ev;

    if (!g_fb.window || !g_fb.apply_mode_event)
        return;

    SDL_zero(ev);
    ev.type       = g_fb.apply_mode_event;
    ev.user.code  = mode;
    ev.user.data1 = (void *)(intptr_t)w;
    ev.user.data2 = (void *)(intptr_t)h;
    SDL_PushEvent(&ev);
}

void    *xbox_FramebufferWindowHwnd(void) { return (void *)g_fb.window; }
uint32_t xbox_FramebufferWindowVa(void)   { return (uint32_t)SDL_GetAtomicU32(&g_fb.va); }
void     xbox_FramebufferSetGpuPresent(int on) { SDL_SetAtomicInt(&g_fb.gpu_present, on ? 1 : 0); }
void     xbox_FramebufferSetKeyHook(void (*hook)(unsigned vk)) { g_fb.key_hook = hook; }

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint8_t hdr[54], *line;
    uint32_t row, img, total, x, y;
    int rc = -1;

    if (!g_fb.rgb_lock)
        return -1;

    SDL_LockRWLockForReading(g_fb.rgb_lock);
    if (!g_fb.rgb || !SDL_GetAtomicU32(&g_fb.va))
        goto out;

    row   = ((g_fb.width * 3u) + 3u) & ~3u;
    img   = row * g_fb.height;
    total = 54u + img;

    f = fopen(path, "wb");
    if (!f)
        goto out;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2,  &total, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &g_fb.width, 4);
    memcpy(hdr + 22, &g_fb.height, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);

    line = (uint8_t *)calloc(1, row);
    if (line) {
        for (y = 0; y < g_fb.height; y++) {
            const uint32_t *src = g_fb.rgb + (size_t)(g_fb.height - 1 - y) * g_fb.width;
            for (x = 0; x < g_fb.width; x++) {
                line[x * 3 + 0] = (uint8_t)( src[x]        & 0xFF);
                line[x * 3 + 1] = (uint8_t)((src[x] >>  8) & 0xFF);
                line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
            }
            fwrite(line, 1, row, f);
        }
        free(line);
        rc = 0;
    }
    fclose(f);
    if (rc == 0)
        fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
                path, g_fb.width, g_fb.height, (uint32_t)SDL_GetAtomicU32(&g_fb.va));
out:
    SDL_UnlockRWLock(g_fb.rgb_lock);
    return rc;
}

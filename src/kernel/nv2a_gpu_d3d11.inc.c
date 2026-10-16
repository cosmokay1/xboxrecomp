/* ── Hardware rendering (Direct3D 11) ─────────────────────────────────────
 *
 * Included into nv2a_pb_exec.c (it reads that file's state directly), after
 * the register combiners and the vertex-program block.
 *
 * Phase 1 of moving NV2A rendering off the CPU: vertex programs still run in
 * vp_run (vertex counts are small), but rasterisation and everything per
 * pixel -- texturing, the register combiners, alpha test, depth, stencil,
 * blending, colour mask -- runs on the host GPU.
 *
 *  - Every guest colour surface the title draws into gets a render target
 *    (B8G8R8A8, the byte order of the guest's A8R8G8B8) and a D24S8 depth-
 *    stencil buffer. Its contents start as the guest memory, and are copied
 *    back into guest memory at each FLIP_STALL (and before a texture that
 *    aliases the surface is decoded), so the window and anything the title
 *    reads back stay exactly as before.
 *  - Guest textures are decoded once through the existing software sampler
 *    (so every format it supports -- swizzled, linear, paletted, DXT -- works
 *    identically) into RGBA8 and cached, keyed by address/format/size/palette
 *    and a sparse content hash so movie frames and palette swaps refresh.
 *  - One generic pixel shader interprets the register-combiner state from a
 *    constant buffer, the same semantics as comb_eval.
 *
 * Opt-in with RECOMP_GPU=1 while it is verified against the software path.
 *
 * ponytail: depth/stencil clears with a partial rectangle clear the whole
 * buffer; fog is not applied; no mipmaps (level 0 only).
 */
#define COBJMACROS
#if defined(_WIN32)
#include <d3d11.h>
#include <d3d11_1.h>     /* ID3D11Device1: shared HUD textures */
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <intrin.h>        /* __rdtsc: draw-path profile */
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

typedef struct {
    float pos[4];
    float d0[4];
    float d1[4];
    float t[4][4];                       /* oT0..oT3 as written */
    float wz[4];                         /* x: oPos.w, for w-buffered depth */
    float rawscreen[4];                  /* guest XY/Z/W before host clip conversion */
} GpuVertex;

typedef struct {
    uint32_t guest;                      /* resolved guest address of the surface */
    uint32_t w, h, pitch;
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11Texture2D *ds;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D *staging;
    uint32_t scale;                      /* render-target size = guest size x scale */
    uint32_t requested_scale;            /* legal scale requested when this target was allocated */
    ID3D11Texture2D *down;               /* guest-sized target a scaled surface is read back through */
    ID3D11RenderTargetView *down_rtv;
    ID3D11Texture2D *copy;               /* GPU copy sampled when the target is a texture */
    ID3D11ShaderResourceView *copy_srv;
    int dirty;                           /* GPU holds newer pixels than guest RAM */
    int stale;                           /* guest RAM holds newer pixels than the GPU */
    uint32_t gen;                        /* bumped by every draw, clear and upload into tex */
    uint32_t copy_gen;                   /* gen the GPU copy was taken at (0: none yet) */
    uint32_t ox;                         /* widescreen: extra guest columns on each side of tex */
    ID3D11Texture2D *ctr;                /* ox > 0: the original-width centre, scaled (RT + SRV) */
    ID3D11RenderTargetView *ctr_rtv;
} GpuSurface;

/* Internal resolution (video settings): every render target is this many
 * times its guest size. Set by the presenter; surfaces follow on next use. */
static uint32_t g_render_scale = 1;
static uint32_t g_aniso = 1;
/* 0 preserves the original Xbox combiner output. Optional profiles are
 * applied after the emulated material/lighting pipeline. */
static uint32_t g_lighting_profile = 0;

/* Widescreen (Hor+). The game draws a 16:9 picture into its 720x480 surface and
 * never clips its geometry at the left and right edge (the NV2A has a guard
 * band), so a wider render target simply shows more of the same view at the
 * same scale. g_wide_ar is the display aspect to fill (0 = off, the plain
 * 16:9 surface); the presenter sets it. A screen-sized colour surface then
 * carries ox extra guest columns on each side, and every draw into it shifts
 * right by ox. */
static float g_wide_ar = 0.0f;
static uint32_t g_draw_ox = 0;           /* ox of the surface being drawn into */

static uint32_t gpu_wide_ox(uint32_t w, uint32_t h)
{
    float k;
    uint32_t total;
    if (g_wide_ar <= 16.0f / 9.0f + 0.02f)
        return 0;
    if (w < 640 || w > 720 || h != 480)
        return 0;
    k = g_wide_ar / (16.0f / 9.0f);
    if (k > 2.0f) k = 2.0f;
    total = (uint32_t)(w * k + 0.5f);
    return total > w ? (total - w + 1u) / 2u : 0u;
}

/* The scale a target of this guest size (with its widescreen columns) gets:
 * the setting, stepped down until it fits D3D11's 16384 texture limit (a 32:9
 * target at a higher multiplier can exceed the hardware dimension limit). */
static uint32_t gpu_fit_scale(uint32_t full_w, uint32_t h)
{
    uint32_t sc = g_render_scale;
    while (sc > 1 && (full_w * sc > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || h * sc > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))
        sc--;
    return sc;
}

/* What the draw path last bound, so an unchanged setting is not sent again.
 * In-game NFL 2K5 issues ~2,000-2,800 draws a frame and each set ~20 pieces
 * of state; most never change between draws (2026-10-02). Code that binds
 * its own state (the presenter, blits, fills, HUD) calls
 * gpu_state_invalidate() and the next draw sends everything. */
static struct {
    int valid;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *ib, *vs_cb1, *vs_cb2, *gs_cb, *ps_cb;
    ID3D11ShaderResourceView *vs_srv;
    UINT vb_stride;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11GeometryShader *gs;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp;
    UINT nsc;
    D3D11_RECT sc[8];
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *blend;
    float bf[4];
    ID3D11DepthStencilState *ds;
    UINT sref;
} s_bound;

static void gpu_state_invalidate(void) { s_bound.valid = 0; }
static int s_dp_on = -1;           /* RECOMP_GPU_PROFILE: draw-path timing */

/* nv2a_gpu_present.inc.c: scaled copies between a surface and guest-sized
 * data, and rectangle fills. */
static void gpu_blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, UINT dw, UINT dh, int point);
static void gpu_fill(ID3D11RenderTargetView *dst, UINT x, UINT y, UINT w, UINT h, const float col[4], UINT mask);

typedef struct {
    uint32_t addr, fmt, w, h, pitch, palette, hash;
    uint32_t cube;                       /* a TextureCube (6 faces) */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t last_use;
    uint32_t checked;                    /* frame+1 the contents were last hashed */
} GpuTexture;

#define GPU_MAX_SURFACES 8
#define GPU_MAX_TEXTURES 4096          /* power of two: hashed, probed */
#define GPU_TEX_PROBE    16
#define GPU_MAX_VERTS    (NV_MAX_INDICES * 3)

static struct {
    int tried, ok;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11GeometryShader *gs;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vb, *cb;
    ID3D11RasterizerState *rs;
    ID3D11RasterizerState *rs_cull[2][2];    /* [cull back][front counter-clockwise] */
    ID3D11Texture2D *white;
    ID3D11ShaderResourceView *white_srv;
    ID3D11Texture2D *white_cube;
    ID3D11ShaderResourceView *white_cube_srv;
    GpuSurface surf[GPU_MAX_SURFACES];
    int cur;                             /* index into surf, -1 none */
    GpuTexture texs[GPU_MAX_TEXTURES];
    uint32_t frame;
    uint32_t draws, tris, tex_uploads, readbacks, rt_copies, surf_creates;
    GpuVertex verts[GPU_MAX_VERTS];
} s_gpu11 = { .cur = -1 };

/* Constant buffer the shader reads; layout matches the HLSL cbuffer below. */
typedef struct {
    uint32_t stage[8][4];                /* color_icw, color_ocw, alpha_icw, alpha_ocw */
    float    f0[8][4], f1[8][4];         /* combiner factors, RGBA */
    uint32_t misc[4];                    /* control, stage_prog, final0, final1 */
    float    specfog0[4], specfog1[4];
    uint32_t alpha[4];                   /* test enable, func, ref, combiner active */
    uint32_t texon[4];                   /* per stage: 1 when a texture is bound */
    uint32_t mode[4];                    /* texture stage modes (PS_TEXTUREMODES_*) */
    uint32_t dotmap[4];                  /* SET_DOT_RGBMAPPING per stage */
    uint32_t input[4];                   /* SET_SHADER_OTHER_STAGE_INPUT per stage */
    uint32_t cube[4];                    /* stage texture is a cube map */
    uint32_t rect[4];                    /* stage coordinates are in texels */
    float    tsize[4][4];                /* stage texture width, height */
    uint32_t show[4];                    /* RECOMP_GPU_SHOW debug view */
    float    depthp[4];                  /* w-buffer, zmax, clip min, clip max */
    float    fogc[4];                    /* fog colour (RGB) */
    uint32_t depthfmt[4];                /* integer depth bits: 0 floating, 16 or 24 */
    float    depthofs[4];                /* polygon bias, slope factor, render scale */
    uint32_t lighting[4];                /* optional post-combiner material/lighting profile */
} GpuConsts;

#include "nv2a_gpu_d3d11_hlsl.inc"   /* k_gpu_hlsl, from nv2a_gpu_d3d11.hlsl */

enum {
    RENDERER_CPU     = 0,
    RENDERER_D3D11   = 1,
    RENDERER_SDL_GPU = 2
};

static int gpu_renderer_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *r = getenv("RECOMP_RENDERER");
        const char *g = getenv("RECOMP_GPU");
        if (r && (! _stricmp(r, "cpu") || ! _stricmp(r, "sw") || ! _stricmp(r, "0")))
            mode = RENDERER_CPU;
        else if (r && (! _stricmp(r, "sdl") || ! _stricmp(r, "sdl3") || ! _stricmp(r, "sdl_gpu")))
            mode = RENDERER_SDL_GPU;
        else if (r && (! _stricmp(r, "d3d11") || ! _stricmp(r, "dx11")))
            mode = RENDERER_D3D11;
        else if (g && strcmp(g, "0") == 0)
            mode = RENDERER_CPU;
        else
            mode = (g && *g) ? RENDERER_D3D11 : RENDERER_CPU;
        fprintf(stderr, "  [GPU] Runtime renderer mode: %s\n",
                mode == RENDERER_D3D11 ? "D3D11" :
                mode == RENDERER_SDL_GPU ? "SDL3_GPU" : "CPU Software");
    }
    return mode;
}

static int gpu_enabled(void)
{
    return gpu_renderer_mode() != RENDERER_CPU;
}

/* RECOMP_GPU_DEBUG=1: the D3D11 debug layer, with its messages printed after
 * each operation (first 300). Names the call a driver crash came from. */
static ID3D11InfoQueue *s_gpu_iq;
static void gpu_debug_drain(const char *where)
{
    static unsigned shown;
    UINT64 i, n;
    if (!s_gpu_iq)
        return;
    n = ID3D11InfoQueue_GetNumStoredMessages(s_gpu_iq);
    for (i = 0; i < n && shown < 300; i++) {
        SIZE_T len = 0;
        D3D11_MESSAGE *msg;
        if (FAILED(ID3D11InfoQueue_GetMessage(s_gpu_iq, i, NULL, &len)) || !len)
            continue;
        msg = (D3D11_MESSAGE *)malloc(len);
        if (msg && SUCCEEDED(ID3D11InfoQueue_GetMessage(s_gpu_iq, i, msg, &len))
                && msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            fprintf(stderr, "  [GPU11DBG] %s: %.*s\n", where, (int)msg->DescriptionByteLength, msg->pDescription);
            shown++;
        }
        free(msg);
    }
    ID3D11InfoQueue_ClearStoredMessages(s_gpu_iq);
    if (shown)
        fflush(stderr);
}

static int gpu_init(void)
{
    HRESULT hr;
    ID3DBlob *vsb = NULL, *psb = NULL, *gsb = NULL, *err = NULL;
    D3D_FEATURE_LEVEL fl;

    if (s_gpu11.tried)
        return s_gpu11.ok;
    s_gpu11.tried = 1;
    if (env_cached("RECOMP_GPU_DEBUG")) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_DEBUG, NULL, 0,
                               D3D11_SDK_VERSION, &s_gpu11.dev, &fl, &s_gpu11.ctx);
        if (SUCCEEDED(hr))
            ID3D11Device_QueryInterface(s_gpu11.dev, &IID_ID3D11InfoQueue, (void **)&s_gpu_iq);
        fprintf(stderr, "  [GPU11] debug layer %s\n", s_gpu_iq ? "on" : "unavailable");
    } else
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                           &s_gpu11.dev, &fl, &s_gpu11.ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] D3D11CreateDevice failed 0x%08lX; staying on the software rasteriser\n",
                (unsigned long)hr);
        return 0;
    }
    hr = D3DCompile(k_gpu_hlsl, strlen(k_gpu_hlsl), "nv2a", NULL, NULL, "vs_main", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] VS compile failed: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    hr = D3DCompile(k_gpu_hlsl, strlen(k_gpu_hlsl), "nv2a", NULL, NULL, "ps_main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] PS compile failed: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_gpu11.vs);
    ID3D11Device_CreatePixelShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_gpu11.ps);
    if (!env_cached("RECOMP_GPU_PLANE_DEPTH") || *env_cached("RECOMP_GPU_PLANE_DEPTH") != '0') {
        hr = D3DCompile(k_gpu_hlsl, strlen(k_gpu_hlsl), "nv2a", NULL, NULL, "gs_main", "gs_4_0",
                        D3DCOMPILE_IEEE_STRICTNESS, 0, &gsb, &err);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateGeometryShader(s_gpu11.dev, ID3D10Blob_GetBufferPointer(gsb),
                                                   ID3D10Blob_GetBufferSize(gsb), NULL, &s_gpu11.gs);
            ID3D10Blob_Release(gsb);
        }
        if (FAILED(hr)) {
            fprintf(stderr, "  [GPU11] original-plane depth shader failed: %s\n",
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "device creation");
            return 0;
        }
        fprintf(stderr, "  [GPU11] original-triangle depth reconstruction enabled\n");
    }
    {
        D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, d0), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, d1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, t[0]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, t[1]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, t[2]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, t[3]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, wz), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(GpuVertex, rawscreen), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(s_gpu11.dev, el, 9, ID3D10Blob_GetBufferPointer(vsb),
                                       ID3D10Blob_GetBufferSize(vsb), &s_gpu11.layout);
    }
    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(GpuVertex) * GPU_MAX_VERTS * 8;   /* ring, see gpu_draw_batch_unlocked */
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_gpu11.vb);
        bd.ByteWidth = (sizeof(GpuConsts) + 15) & ~15u;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device_CreateBuffer(s_gpu11.dev, &bd, NULL, &s_gpu11.cb);
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;      /* culling stays opt-in, see raster_triangle_xf */
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_gpu11.dev, &rd, &s_gpu11.rs);
        {
            int b, c;
            for (b = 0; b < 2; b++)
                for (c = 0; c < 2; c++) {
                    rd.CullMode = b ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
                    rd.FrontCounterClockwise = c ? TRUE : FALSE;
                    ID3D11Device_CreateRasterizerState(s_gpu11.dev, &rd, &s_gpu11.rs_cull[b][c]);
                }
        }
    }
    {
        uint32_t white = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd = { &white, 4, 4 };
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, &sd, &s_gpu11.white);
        ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_gpu11.white, NULL, &s_gpu11.white_srv);
        {
            D3D11_SUBRESOURCE_DATA faces[6];
            int f;
            for (f = 0; f < 6; f++) { faces[f].pSysMem = &white; faces[f].SysMemPitch = 4; faces[f].SysMemSlicePitch = 4; }
            td.ArraySize = 6;
            td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, faces, &s_gpu11.white_cube);
            ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s_gpu11.white_cube, NULL,
                                                  &s_gpu11.white_cube_srv);
        }
    }
    s_gpu11.ok = s_gpu11.vs && s_gpu11.ps && s_gpu11.layout && s_gpu11.vb && s_gpu11.cb;
    fprintf(stderr, "  [GPU11] Direct3D 11 renderer %s (feature level 0x%X)\n",
            s_gpu11.ok ? "ready" : "FAILED", (unsigned)fl);
    return s_gpu11.ok;
}

/* ---- surfaces -------------------------------------------------------- */

static void gpu_readback(GpuSurface *s)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t y;
    if (!s->tex || !s->dirty)
        return;
    if (s->ox && s->ctr && s->down_rtv) {
        /* The guest's surface is the original-width centre of the wide target. */
        ID3D11ShaderResourceView *srv = NULL;
        D3D11_BOX box = { s->ox * s->scale, 0, 0, (s->ox + s->w) * s->scale, s->h * s->scale, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(s_gpu11.ctx, (ID3D11Resource *)s->ctr, 0, 0, 0, 0,
                                                  (ID3D11Resource *)s->tex, 0, &box);
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s->ctr, NULL, &srv))) {
            gpu_blit(srv, s->down_rtv, s->w, s->h, 0);
            ID3D11ShaderResourceView_Release(srv);
        }
        ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)s->staging, (ID3D11Resource *)s->down);
    } else if (s->scale > 1 && s->down_rtv) {
        ID3D11ShaderResourceView *srv = NULL;
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s->tex, NULL, &srv))) {
            gpu_blit(srv, s->down_rtv, s->w, s->h, 0);
            ID3D11ShaderResourceView_Release(srv);
        }
        ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)s->staging, (ID3D11Resource *)s->down);
    } else
    ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)s->staging, (ID3D11Resource *)s->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s->staging, 0, D3D11_MAP_READ, 0, &m)))
        return;
    for (y = 0; y < s->h; y++)
        memcpy(mem + s->guest + (size_t)y * s->pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
               (size_t)s->w * 4);
    ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s->staging, 0);
    s->dirty = 0;
    s_gpu11.readbacks++;
}

/* The software rasteriser is about to read or write the current surface in
 * guest RAM: hand it the newest pixels, and take them back on the next GPU
 * draw. */
static void gpu_cpu_access_unlocked(void)
{
    uint32_t guest;
    int i;
    if (!s_gpu11.ok)
        return;
    guest = dma_resolve(s_gpu.color_offset);
    for (i = 0; i < GPU_MAX_SURFACES; i++) {
        GpuSurface *s = &s_gpu11.surf[i];
        if (s->tex && s->guest == guest) {
            gpu_readback(s);
            s->stale = 1;
        }
    }
}

static void gpu_release_surface(GpuSurface *s)
{
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->ds) ID3D11Texture2D_Release(s->ds);
    if (s->staging) ID3D11Texture2D_Release(s->staging);
    if (s->down_rtv) ID3D11RenderTargetView_Release(s->down_rtv);
    if (s->down) ID3D11Texture2D_Release(s->down);
    if (s->ctr_rtv) ID3D11RenderTargetView_Release(s->ctr_rtv);
    if (s->ctr) ID3D11Texture2D_Release(s->ctr);
    if (s->copy_srv) ID3D11ShaderResourceView_Release(s->copy_srv);
    if (s->copy) ID3D11Texture2D_Release(s->copy);
    memset(s, 0, sizeof *s);
}

/* Guest RAM -> render target. At scale 1 a straight copy; scaled, the
 * guest-sized pixels go into the readback target and are drawn up. */
static void gpu_surface_upload(GpuSurface *s)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    s->gen++;
    if (s->ox && s->down && s->ctr_rtv) {
        ID3D11ShaderResourceView *srv = NULL;
        D3D11_BOX src = { 0, 0, 0, s->w * s->scale, s->h * s->scale, 1 };
        ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s->down, 0, NULL, mem + s->guest, s->pitch, 0);
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s->down, NULL, &srv))) {
            gpu_blit(srv, s->ctr_rtv, s->w * s->scale, s->h * s->scale, 1);
            ID3D11ShaderResourceView_Release(srv);
        }
        ID3D11DeviceContext_CopySubresourceRegion(s_gpu11.ctx, (ID3D11Resource *)s->tex, 0, s->ox * s->scale, 0, 0,
                                                  (ID3D11Resource *)s->ctr, 0, &src);
        return;
    }
    if (s->scale <= 1 || !s->down) {
        ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s->tex, 0, NULL,
                                              mem + s->guest, s->pitch, 0);
        return;
    }
    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)s->down, 0, NULL, mem + s->guest, s->pitch, 0);
    {
        ID3D11ShaderResourceView *srv = NULL;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(s->down, &td);
        if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
            return;
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)s->down, NULL, &srv))) {
            gpu_blit(srv, s->rtv, s->w * s->scale, s->h * s->scale, 1);
            ID3D11ShaderResourceView_Release(srv);
        }
    }
}

/* The render target for the current NV2A colour surface, created (and
 * seeded from guest memory) on first use. -1 when it cannot be used. */
static int gpu_bind_surface(void)
{
    uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h;
    uint32_t guest = dma_resolve(s_gpu.color_offset);
    int i, free_i = -1;

    if (surface_bpp() != 4 || !w || !h || w > 4096 || h > 4096 || !s_gpu.pitch) {
        /* RECOMP_GPU_PROFILE: surfaces that fall back to the software
         * rasteriser (each such draw is CPU work, and a texture read from
         * one is decoded again). */
        if (s_dp_on > 0) {
            static uint32_t seen[16];
            static int nseen;
            int k;
            for (k = 0; k < nseen && seen[k] != guest; k++) {}
            if (k == nseen && nseen < 16) {
                seen[nseen++] = guest;
                fprintf(stderr, "  [GPU11] software surface %08X %ux%u bpp %u format %08X pitch %u\n",
                        guest, w, h, surface_bpp(), s_gpu.format, s_gpu.pitch);
            }
        }
        return -1;
    }
    if (surface_write_refused(guest, s_gpu.pitch * h, "gpu"))
        return -1;
    for (i = 0; i < GPU_MAX_SURFACES; i++) {
        GpuSurface *s = &s_gpu11.surf[i];
        if (s->tex && s->guest == guest) {
            if (s->w == w && s->h == h && s->pitch == s_gpu.pitch
                    /* Retain a prior lower allocation after an OOM step-down,
                     * but rebuild if the player changes the requested scale. */
                    && s->requested_scale == gpu_fit_scale(w + 2 * gpu_wide_ox(w, h), h)
                    && s->ox == gpu_wide_ox(w, h)) {
                if (s->stale) {             /* the CPU drew into it since */
                    gpu_surface_upload(s);
                    s->stale = 0;
                }
                return i;
            }
            gpu_readback(s);
            gpu_release_surface(s);
            free_i = i;
            break;
        }
        if (!s->tex && free_i < 0)
            free_i = i;
    }
    if (free_i < 0) {                       /* evict the first one */
        gpu_readback(&s_gpu11.surf[0]);
        gpu_release_surface(&s_gpu11.surf[0]);
        free_i = 0;
    }
    {
        GpuSurface *s = &s_gpu11.surf[free_i];
        D3D11_TEXTURE2D_DESC td;
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        D3D11_SUBRESOURCE_DATA sd;
        uint32_t ox = gpu_wide_ox(w, h), sc = gpu_fit_scale(w + 2 * ox, h);
        uint32_t requested_sc = sc;
retry_surface_create:
        memset(&td, 0, sizeof td);
        td.Width = (w + 2 * ox) * sc; td.Height = h * sc; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = mem + guest; sd.SysMemPitch = s_gpu.pitch; sd.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, sc == 1 && !ox ? &sd : NULL, &s->tex)) ||
                FAILED(ID3D11Device_CreateRenderTargetView(s_gpu11.dev, (ID3D11Resource *)s->tex, NULL, &s->rtv)))
            goto surface_create_failed;
        s->guest = guest; s->w = w; s->h = h; s->pitch = s_gpu.pitch;
        s->scale = sc; s->requested_scale = requested_sc; s->ox = ox;
        s_gpu11.surf_creates++;
        {
            static unsigned wide_logged;
            if (wide_logged++ < 24)
                fprintf(stderr, "  [GPU11] surface %08X %ux%u (clip %u,%u %ux%u) pitch %u scale %u widescreen +%u columns (target aspect %.3f)\n",
                        guest, w, h, s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h, s_gpu.pitch, sc, ox, g_wide_ar);
        }
        if (s_dp_on > 0) {
            static unsigned logged;
            if (logged++ < 40)
                fprintf(stderr, "  [GPU11] new target #%u slot %d %08X %ux%u pitch %u x%u (frame %u)\n",
                        logged, free_i, guest, w, h, s_gpu.pitch, sc, s_gpu11.frame);
        }
        td.Width = w; td.Height = h;
        if (ox) {                              /* the original-width centre (see gpu_readback) */
            td.Width = w * sc; td.Height = h * sc;
            if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s->ctr)) || !s->ctr ||
                    FAILED(ID3D11Device_CreateRenderTargetView(s_gpu11.dev, (ID3D11Resource *)s->ctr, NULL, &s->ctr_rtv)))
                goto surface_create_failed;
            td.Width = w; td.Height = h;
        }
        if (sc > 1 || ox) {                    /* guest-sized: readback goes through it */
            if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s->down)) || !s->down ||
                    FAILED(ID3D11Device_CreateRenderTargetView(s_gpu11.dev, (ID3D11Resource *)s->down, NULL, &s->down_rtv)))
                goto surface_create_failed;
            gpu_surface_upload(s);
        }
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s->staging)) || !s->staging)
            goto surface_create_failed;
        td.Width = (w + 2 * ox) * sc; td.Height = h * sc;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
        td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &s->ds)) || !s->ds ||
                FAILED(ID3D11Device_CreateDepthStencilView(s_gpu11.dev, (ID3D11Resource *)s->ds, NULL, &s->dsv)) || !s->dsv)
            goto surface_create_failed;
        ID3D11DeviceContext_ClearDepthStencilView(s_gpu11.ctx, s->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        s->dirty = 0;
        if (sc != requested_sc)
            fprintf(stderr, "  [GPU11] render target requested %ux, allocated %ux after memory fallback\n", requested_sc, sc);
        return free_i;
surface_create_failed:
        gpu_release_surface(s);
        if (sc > 1) { sc--; goto retry_surface_create; }
        fprintf(stderr, "  [GPU11] could not allocate %ux%u render target even at 1x\n", w + 2 * ox, h);
        return -1;
    }
}

/* Copy every surface that overlaps [addr, addr+bytes) back to guest RAM:
 * before a texture reading that memory is decoded, and at a flip. */
static void gpu_flush_range(uint32_t addr, uint32_t bytes)
{
    int i;
    for (i = 0; i < GPU_MAX_SURFACES; i++) {
        GpuSurface *s = &s_gpu11.surf[i];
        if (s->tex && s->dirty && addr < s->guest + s->pitch * s->h && s->guest < addr + bytes)
            gpu_readback(s);
    }
}

/* ---- textures -------------------------------------------------------- */

static uint32_t gpu_tex_bytes(const Texture *t)
{
    uint32_t b = d3d8_format_dxt_block_bytes(t->color);
    if (b)
        return (t->width / 4u) * (t->height / 4u) * b;
    if (t->pitch && !tex_size_from_format(t->color))
        return t->pitch * t->height;
    return t->width * t->height * (t->color == 0x0B || t->color == 0x00 || t->color == 0x13 ? 1u : 4u);
}

#include "nv2a_gpu_prompts.inc.c"   /* PlayStation button icons on the game's button sheet */

/* ── Texture overrides (2026-10-09) ─────────────────────────────────────────
 * The title can show its own image in place of one of the game's textures
 * (the Controller Setup pictures and button icons, src/nfl2k5_controller_art.cpp):
 *   wants(fmt, w, h)  -- the shapes it may replace (cheap; asked per lookup):
 *                        bit 0 = may replace, bit 1 = its image follows gen()
 *   gen()             -- changes whenever those images change, so the cached
 *                        texture is uploaded again (only shapes with bit 1)
 *   override(...)     -- given the texture's content hash (every byte, plus the
 *                        palette) and, for formats decoded here, its BGRA texels,
 *                        returns BGRA texels of any size (sampled with normalized
 *                        coordinates, so a larger image is just sharper), or NULL. */
int (*g_xbox_tex_override_wants)(uint32_t fmt, uint32_t w, uint32_t h);
uint32_t (*g_xbox_tex_override_gen)(void);
const uint32_t *(*g_xbox_tex_override)(uint32_t fmt, uint32_t w, uint32_t h, uint32_t content,
                                       const uint32_t *orig, uint32_t *out_w, uint32_t *out_h);

volatile long g_xbox_tex_dump_wanted;   /* the title dumps textures: DXT ones are decoded for it */

/* DXT1/3/5 (BC1/2/3) to BGRA, for a texture dump. */
static uint32_t *dxt_decode(const uint8_t *src, uint32_t w, uint32_t h, uint32_t fmt)
{
    uint32_t *out = (uint32_t *)calloc((size_t)w * h, 4), bx, by, i;
    uint32_t bb = fmt == 0x0C ? 8u : 16u;
    if (!out) return NULL;
    for (by = 0; by < h / 4; by++)
        for (bx = 0; bx < w / 4; bx++) {
            const uint8_t *b = src + (by * (w / 4) + bx) * bb, *c = fmt == 0x0C ? b : b + 8;
            uint32_t col[4], c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8, idx = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            uint32_t r0 = (c0 >> 11) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
            uint32_t r1 = (c1 >> 11) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
            uint8_t al[16];
            col[0] = 0xFF000000u | r0 << 16 | g0 << 8 | b0;
            col[1] = 0xFF000000u | r1 << 16 | g1 << 8 | b1;
            if (c0 > c1 || fmt != 0x0C) {
                col[2] = 0xFF000000u | ((2 * r0 + r1) / 3) << 16 | ((2 * g0 + g1) / 3) << 8 | (2 * b0 + b1) / 3;
                col[3] = 0xFF000000u | ((r0 + 2 * r1) / 3) << 16 | ((g0 + 2 * g1) / 3) << 8 | (b0 + 2 * b1) / 3;
            } else {
                col[2] = 0xFF000000u | ((r0 + r1) / 2) << 16 | ((g0 + g1) / 2) << 8 | (b0 + b1) / 2;
                col[3] = 0;
            }
            for (i = 0; i < 16; i++) al[i] = 255;
            if (fmt == 0x0E)
                for (i = 0; i < 16; i++) al[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
            else if (fmt == 0x0F) {
                uint32_t a0 = b[0], a1 = b[1], av[8], k;
                uint64_t bits = 0;
                for (k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                av[0] = a0; av[1] = a1;
                for (k = 2; k < 8; k++)
                    av[k] = a0 > a1 ? ((8 - k) * a0 + (k - 1) * a1) / 7 : k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : k == 6 ? 0 : 255;
                for (i = 0; i < 16; i++) al[i] = (uint8_t)av[(bits >> (3 * i)) & 7];
            }
            for (i = 0; i < 16; i++) {
                uint32_t px = col[(idx >> (2 * i)) & 3];
                if (fmt != 0x0C) px = (px & 0x00FFFFFFu) | (uint32_t)al[i] << 24;
                out[(by * 4 + i / 4) * w + bx * 4 + (i & 3)] = px;
            }
        }
    return out;
}

static uint32_t gpu_tex_bytes(const Texture *t);
static int tex_override_wants(const Texture *t)
{
    return g_xbox_tex_override && g_xbox_tex_override_wants &&
           g_xbox_tex_override_wants(t->color, t->width, t->height);
}

static uint32_t tex_content_hash(const Texture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = gpu_tex_bytes(t), h = 2166136261u, i;
    if (bytes > 4u * 1024u * 1024u) bytes = 4u * 1024u * 1024u;
    __try {
        for (i = 0; i + 4 <= bytes; i += 4)
            h = (h ^ *(const uint32_t *)(mem + t->offset + i)) * 16777619u;
        if (t->color == 0x0B && t->palette)
            for (i = 0; i < 1024; i += 4)
                h = (h ^ *(const uint32_t *)(mem + t->palette + i)) * 16777619u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h;
}

static uint32_t gpu_tex_hash(const Texture *t)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = gpu_tex_bytes(t), h = 2166136261u, i, step;
    if (bytes > 16u * 1024u * 1024u) bytes = 16u * 1024u * 1024u;
    /* 256 samples plus the last word: re-hashed once a frame for every
     * texture in use, 1,024 samples was ~2.5% of the game thread. Animated
     * textures change across their area, which this still sees. */
    step = (bytes / 256u) & ~3u; if (step < 4) step = 4;
    if (env_cached("RECOMP_TEX_FULLHASH")) step = 4;     /* diagnostics: every word */
    for (i = 0; i + 4 <= bytes; i += step)
        h = (h ^ *(const uint32_t *)(mem + t->offset + i)) * 16777619u;
    if (bytes >= 4)
        h = (h ^ *(const uint32_t *)(mem + t->offset + bytes - 4)) * 16777619u;
    if (t->color == 0x0B && t->palette)
        for (i = 0; i < 1024; i += env_cached("RECOMP_TEX_FULLHASH") ? 4 : 16)
            h = (h ^ *(const uint32_t *)(mem + t->palette + i)) * 16777619u;
    if (t->color == 0x0B && t->width == 128 && t->height == 128)
        h ^= (uint32_t)prompt_style() * 0x5BD1E995u;   /* the button sheet follows the prompt style */
    if ((tex_override_wants(t) & 2) && g_xbox_tex_override_gen)
        h ^= g_xbox_tex_override_gen() * 0x9E3779B1u;    /* its replacement changed */
    return h;
}

/* SET_TEXTURE_FORMAT of a stage says it is a cube map (bit 2). */
static int gpu_stage_is_cube(int st)
{
    return (s_tex_reg[(0x1B04 + 0x40u * (uint32_t)st - NV_TEX_FIRST) / 4] >> 2) & 1;
}

/* Bytes per texel of an NV097 colour format, for the size of a cube face's
 * mip chain (xemu kelvin_color_format_info). DXT formats are handled apart. */
static uint32_t gpu_fmt_bpp(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A:
    case 0x1C: case 0x1D: case 0x25: case 0x27: case 0x28: case 0x30: case 0x31: return 2;
    default: return 4;
    }
}

/* Offset from one cube face to the next: the face's whole mip chain, rounded
 * up to NV2A_CUBEMAP_FACE_ALIGNMENT (128), as xemu's pgraph texture length. */
static uint32_t gpu_cube_face_bytes(int st, const Texture *t)
{
    uint32_t fmtreg = s_tex_reg[(0x1B04 + 0x40u * (uint32_t)st - NV_TEX_FIRST) / 4];
    uint32_t levels = (fmtreg >> 16) & 0xF, w = t->width, h = t->height, len = 0, l;
    uint32_t block = d3d8_format_dxt_block_bytes(t->color);
    if (!levels) levels = 1;
    for (l = 0; l < levels; l++) {
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        len += block ? ((w + 3) / 4) * ((h + 3) / 4) * block : w * h * gpu_fmt_bpp(t->color);
        w /= 2; h /= 2;
    }
    return (len + 127u) & ~127u;
}

/* SRV for texture stage st, decoding it through the software sampler the
 * first time (and whenever its contents change). A cube map decodes all six
 * faces into a TextureCube. */
static int gpu_texdump_match(const Texture *t, uint32_t n);
/* RECOMP_GPU_PROFILE: which textures are decoded again and again (address,
 * format, size, count per five seconds) -- each upload unswizzles on the CPU
 * and creates a new GPU texture. */
static void gpu_upload_note(const Texture *t, int cube)
{
    static struct { uint32_t addr, fmt, w, h, n; } tab[64];
    static int used;
    static DWORD since;
    DWORD now = GetTickCount();
    int i;
    if (s_dp_on <= 0) return;
    for (i = 0; i < used; i++)
        if (tab[i].addr == t->offset && tab[i].fmt == t->color) break;
    if (i == used && used < 64) { tab[used].addr = t->offset; tab[used].fmt = t->color; tab[used].w = t->width; tab[used].h = t->height; tab[used].n = 0; used++; }
    if (i < 64) tab[i].n++;
    if (!since) since = now;
    if (now - since >= 5000) {
        int k, j;
        fprintf(stderr, "  [TEXUP] uploads by texture (5 s):");
        for (k = 0; k < 6; k++) {
            int best = -1;
            for (j = 0; j < used; j++) if (tab[j].n && (best < 0 || tab[j].n > tab[best].n)) best = j;
            if (best < 0) break;
            fprintf(stderr, " %08X/%02X/%ux%u%s x%u", tab[best].addr, tab[best].fmt, tab[best].w, tab[best].h, cube ? "c" : "", tab[best].n);
            tab[best].n = 0;
        }
        fprintf(stderr, "\n");
        used = 0;
        since = now;
    }
}

static LONGLONG g_tex_upload_qpc, g_tex_create_qpc;

/* Texture object pool (2026-10-09). Creating a texture and its view cost
 * ~0.15-0.2 ms each, ~2/3 of the 30-50 ms render-thread spikes at camera cuts
 * (100+ new textures; [HITCH] log). A replaced or evicted texture is kept here
 * and refilled with UpdateSubresource when one of the same description is
 * needed. Only entries pooled in an earlier frame are reused, so a texture a
 * draw of this frame may still bind is never overwritten; draws already
 * issued keep their contents (UpdateSubresource is ordered after them).
 * RECOMP_TEX_POOL=0 turns it off. */
#define TEX_POOL 384
static struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; D3D11_TEXTURE2D_DESC d; uint32_t frame; } s_tpool[TEX_POOL];
static int s_tpool_n;
static unsigned s_tpool_hits, s_tpool_misses;

static int tex_pool_on(void)
{
    static int on = -1;
    if (on < 0) on = !(env_cached("RECOMP_TEX_POOL") && env_cached("RECOMP_TEX_POOL")[0] == '0');
    return on;
}

static void tex_pool_put(ID3D11Texture2D *tex, ID3D11ShaderResourceView *srv)
{
    D3D11_TEXTURE2D_DESC d;
    if (!tex || !srv || !tex_pool_on()) {
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (tex) ID3D11Texture2D_Release(tex);
        return;
    }
    ID3D11Texture2D_GetDesc(tex, &d);
    if (d.Usage != D3D11_USAGE_DEFAULT) {          /* immutable ones cannot be refilled */
        ID3D11ShaderResourceView_Release(srv);
        ID3D11Texture2D_Release(tex);
        return;
    }
    if (s_tpool_n == TEX_POOL) {                   /* full: drop the oldest */
        ID3D11ShaderResourceView_Release(s_tpool[0].srv);
        ID3D11Texture2D_Release(s_tpool[0].tex);
        memmove(&s_tpool[0], &s_tpool[1], sizeof s_tpool[0] * (TEX_POOL - 1));
        s_tpool_n--;
    }
    s_tpool[s_tpool_n].tex = tex; s_tpool[s_tpool_n].srv = srv; s_tpool[s_tpool_n].d = d;
    s_tpool[s_tpool_n].frame = s_gpu11.frame;
    s_tpool_n++;
}

/* Create (or take from the pool and refill) a shader-resource texture. */
static int tex_make(D3D11_TEXTURE2D_DESC *td, const D3D11_SUBRESOURCE_DATA *sd,
                    ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv)
{
    int i;
    if (tex_pool_on()) {
        td->Usage = D3D11_USAGE_DEFAULT;
        for (i = s_tpool_n - 1; i >= 0; i--) {
            const D3D11_TEXTURE2D_DESC *p = &s_tpool[i].d;
            if (s_tpool[i].frame != s_gpu11.frame && p->Width == td->Width && p->Height == td->Height &&
                    p->MipLevels == td->MipLevels && p->ArraySize == td->ArraySize && p->Format == td->Format &&
                    p->MiscFlags == td->MiscFlags) {
                UINT k, subs = td->ArraySize * (td->MipLevels ? td->MipLevels : 1);
                *tex = s_tpool[i].tex; *srv = s_tpool[i].srv;
                memmove(&s_tpool[i], &s_tpool[i + 1], sizeof s_tpool[0] * (size_t)(s_tpool_n - i - 1));
                s_tpool_n--;
                for (k = 0; k < subs; k++)
                    ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)*tex, k, NULL,
                                                          sd[k].pSysMem, sd[k].SysMemPitch, sd[k].SysMemSlicePitch);
                s_tpool_hits++;
                return 1;
            }
        }
    }
    if (++s_tpool_misses % 2000 == 0)
        fprintf(stderr, "  [TEXPOOL] %u reused, %u created (pool holds %d)\n", s_tpool_hits, s_tpool_misses, s_tpool_n);
    if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, td, sd, tex)))
        return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)*tex, NULL, srv))) {
        ID3D11Texture2D_Release(*tex); *tex = NULL;
        return 0;
    }
    return 1;
}              /* decode + create + upload, all textures (hitch log) */
/* Upload a texture override's image into cache entry e (see g_xbox_tex_override). */
static int tex_upload_override(GpuTexture *e, const Texture *t, uint32_t tpal, uint32_t hash,
                               const uint32_t *img, uint32_t ow, uint32_t oh)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    if (!ow || !oh || ow > 16384 || oh > 16384) return 0;
    memset(&td, 0, sizeof td);
    td.Width = ow; td.Height = oh; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = img; sd.SysMemPitch = ow * 4; sd.SysMemSlicePitch = ow * oh * 4;
    if (ow > t->width || oh > t->height) {
        /* larger than the game's: a full mip chain, so it does not shimmer far away */
        td.MipLevels = 0; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &e->tex))) return 0;
        if (FAILED(ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)e->tex, NULL, &e->srv))) {
            ID3D11Texture2D_Release(e->tex); e->tex = NULL;
            return 0;
        }
        ID3D11DeviceContext_UpdateSubresource(s_gpu11.ctx, (ID3D11Resource *)e->tex, 0, NULL, img, ow * 4, ow * oh * 4);
        ID3D11DeviceContext_GenerateMips(s_gpu11.ctx, e->srv);
    } else if (!tex_make(&td, &sd, &e->tex, &e->srv)) return 0;
    e->addr = t->offset; e->fmt = t->color; e->w = t->width; e->h = t->height;
    e->pitch = t->pitch; e->palette = tpal; e->hash = hash; e->last_use = s_gpu11.frame;
    e->checked = s_gpu11.frame + 1;
    e->cube = 0;
    s_gpu11.tex_uploads++;
    return 1;
}

static ID3D11ShaderResourceView *gpu_texture(int st, int cube)
{
    Texture *t = st == 0 ? &s_gpu.tex : &s_stage[st];
    uint32_t faces = cube ? 6u : 1u, face_bytes = cube ? gpu_cube_face_bytes(st, t) : 0u, f;
    uint32_t hash, i, n, oldest = 0, oldest_use = 0xFFFFFFFFu;
    uint32_t *pix;
    GpuTexture *e = NULL;
    uint32_t tpal;

    if (!t->valid || t->width > 4096 || t->height > 4096)
        return NULL;
    if (env_cached("RECOMP_RTT_TRACE")) {
        /* Which textures read memory a render target covers. */
        static uint32_t seen[64][3];
        static int nseen;
        int k2, j2;
        for (k2 = 0; k2 < GPU_MAX_SURFACES; k2++) {
            GpuSurface *sf = &s_gpu11.surf[k2];
            if (!sf->tex || !(t->offset < sf->guest + sf->pitch * sf->h && sf->guest < t->offset + gpu_tex_bytes(t)))
                continue;
            for (j2 = 0; j2 < nseen; j2++)
                if (seen[j2][0] == t->offset && seen[j2][1] == t->color && seen[j2][2] == sf->guest) break;
            if (j2 == nseen && nseen < 64) {
                seen[nseen][0] = t->offset; seen[nseen][1] = t->color; seen[nseen][2] = sf->guest; nseen++;
                fprintf(stderr, "  [RTT] stage %d tex %08X fmt %02X %ux%u pitch %u cube %d <- surface %08X %ux%u pitch %u dirty %d\n",
                        st, t->offset, t->color, t->width, t->height, t->pitch, cube, sf->guest, sf->w, sf->h, sf->pitch, sf->dirty);
            }
        }
    }
    /* A texture that is exactly a render target (NFL 2K5 samples its front
     * and back buffers as 720x480 LIN_X8R8G8B8 for full-screen effects):
     * copy the target on the GPU and sample the copy. Reading it back to
     * guest RAM and decoding it again was ~22% of a frame, stalled on the
     * GPU, and lost the internal resolution (2026-09-26). The copy keeps
     * the draw from sampling the target it may be drawing into. */
    if (!cube && (t->color == 0x1E || t->color == 0x12)) {
        int k3;
        for (k3 = 0; k3 < GPU_MAX_SURFACES; k3++) {
            GpuSurface *sf = &s_gpu11.surf[k3];
            if (sf->tex && sf->guest == t->offset && sf->w == t->width && sf->h == t->height && sf->pitch == t->pitch) {
                if (!sf->copy) {
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc(sf->tex, &td);
                    if (sf->ox) td.Width = sf->w * sf->scale;        /* a copy of the original-width centre */
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (FAILED(ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &sf->copy)))
                        break;
                    ID3D11Device_CreateShaderResourceView(s_gpu11.dev, (ID3D11Resource *)sf->copy, NULL, &sf->copy_srv);
                }
                if (sf->copy_srv) {
                    /* A CPU access can invalidate the GPU copy without this
                     * surface being rebound as a render target. Sampling it
                     * must observe those guest RAM writes, just as rebinding
                     * it in gpu_bind_surface does. */
                    if (sf->stale) {
                        if (env_cached("RECOMP_RTT_TRACE")) {
                            static uint32_t stale_samples;
                            if (stale_samples < 32) {
                                fprintf(stderr, "  [RTT] stale sample #%u frame %u stage %d surface %08X %ux%u pitch %u dirty %d -> guest upload\n",
                                        ++stale_samples, s_gpu11.frame, st, sf->guest,
                                        sf->w, sf->h, sf->pitch, sf->dirty);
                            }
                        }
                        gpu_surface_upload(sf);
                        sf->stale = 0;
                    }
                    /* Copy only when the target changed since the last
                     * copy. Several draws a frame sample the same buffer;
                     * at 8x one copy is ~90 MB of GPU bandwidth, and
                     * copying for each of them made high internal
                     * resolutions GPU-bound (2026-10-03). */
                    if (sf->copy_gen != sf->gen || !sf->gen) {
                        if (sf->ox) {
                            D3D11_BOX cb = { sf->ox * sf->scale, 0, 0, (sf->ox + sf->w) * sf->scale, sf->h * sf->scale, 1 };
                            ID3D11DeviceContext_CopySubresourceRegion(s_gpu11.ctx, (ID3D11Resource *)sf->copy, 0, 0, 0, 0,
                                                                      (ID3D11Resource *)sf->tex, 0, &cb);
                        } else
                        ID3D11DeviceContext_CopyResource(s_gpu11.ctx, (ID3D11Resource *)sf->copy, (ID3D11Resource *)sf->tex);
                        sf->copy_gen = sf->gen;
                        s_gpu11.rt_copies++;
                    }
                    return sf->copy_srv;
                }
            }
        }
    }
    gpu_flush_range(t->offset, gpu_tex_bytes(t));     /* render-to-texture */
    /* Hashed by identity, probed over a short run; the oldest slot of the run
     * is the victim. In-game NFL 2K5 binds far more than the old 256-entry
     * linear cache held, and thrashing it re-decoded ~4 textures a frame on
     * the CPU. Contents are re-hashed once per frame, not per draw. */
    /* The palette address only identifies a paletted texture. Keyed for
     * every format, a stray palette register split one animated 256x256
     * texture into several cache entries, each decoded again every frame
     * (~4 uploads a frame, 2026-10-02). */
    tpal = t->color == 0x0B ? t->palette : 0;
    {
        uint32_t key = t->offset * 2654435761u ^ t->color * 40503u ^ (t->width << 16 | t->height)
                     ^ t->pitch * 97u ^ tpal * 31u ^ (uint32_t)cube * 0x9E3779B9u;
        uint32_t base = (key ^ (key >> 15)) & (GPU_MAX_TEXTURES - 1), k;
        hash = 0;
        for (k = 0; k < GPU_TEX_PROBE; k++) {
            GpuTexture *c = &s_gpu11.texs[(base + k) & (GPU_MAX_TEXTURES - 1)];
            if (c->srv && c->addr == t->offset && c->fmt == t->color && c->w == t->width
                    && c->h == t->height && c->pitch == t->pitch && c->palette == tpal
                    && c->cube == (uint32_t)cube) {
                if (c->checked == s_gpu11.frame + 1) {
                    c->last_use = s_gpu11.frame;
                    return c->srv;
                }
                hash = gpu_tex_hash(t);
                if (c->hash == hash) {
                    c->last_use = s_gpu11.frame;
                    c->checked = s_gpu11.frame + 1;
                    return c->srv;
                }
                e = c;                           /* same texture, new contents */
                break;
            }
            if (!c->srv) { e = c; break; }       /* free slot */
            if (c->last_use < oldest_use) { oldest_use = c->last_use; oldest = (base + k) & (GPU_MAX_TEXTURES - 1); }
        }
        if (!e)
            e = &s_gpu11.texs[oldest];
        if (!hash)
            hash = gpu_tex_hash(t);
    }
    LARGE_INTEGER tex_t0;                       /* frame statistics: texture decode + upload time */
    QueryPerformanceCounter(&tex_t0);
    tex_pool_put(e->tex, e->srv);               /* reused for a later texture of the same shape */
    e->tex = NULL; e->srv = NULL;
    if (!cube && tex_override_wants(t) && d3d8_format_dxt_block_bytes(t->color)) {
        uint32_t ow = 0, oh = 0, *dec = NULL;
        const uint32_t *img;
        if (g_xbox_tex_dump_wanted)                  /* only to dump it */
            __try { dec = dxt_decode((const uint8_t *)xbox_GetMemoryOffset() + t->offset, t->width, t->height, t->color); }
            __except (EXCEPTION_EXECUTE_HANDLER) { dec = NULL; }
        img = g_xbox_tex_override(t->color, t->width, t->height, tex_content_hash(t), dec, &ow, &oh);
        free(dec);
        if (img && tex_upload_override(e, t, tpal, hash, img, ow, oh))
            return e->srv;
    }
    /* DXT1/3/5 go to the GPU as BC1/2/3 as they are -- same block layout,
     * no swizzle -- instead of being decoded texel by texel on the CPU,
     * which re-decoded the whole 4x4 block for every texel (2026-09-26). */
    if (d3d8_format_dxt_block_bytes(t->color) && (t->width & 3) == 0 && (t->height & 3) == 0
            && (!cube || t->width == t->height)) {
        const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t bb = d3d8_format_dxt_block_bytes(t->color);
        uint32_t fbytes = (t->width / 4) * (t->height / 4) * bb;
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd[6];
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = faces;
        td.Format = t->color == 0x0C ? DXGI_FORMAT_BC1_UNORM
                  : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        for (f = 0; f < faces; f++) {
            sd[f].pSysMem = gm + t->offset + f * face_bytes;
            sd[f].SysMemPitch = (t->width / 4) * bb;
            sd[f].SysMemSlicePitch = fbytes;
        }
        if (env_cached("RECOMP_GPU_TEXDUMP") && gpu_texdump_match(t, t->width * t->height)) {
            /* the blocks as they are, in a .dds (any image viewer decodes it) */
            static unsigned dumped;
            char path[512];
            FILE *df;
            snprintf(path, sizeof path, "%sdxt%03u_%08X_fmt%02X_%ux%u.dds", env_cached("RECOMP_GPU_TEXDUMP"),
                     dumped++, t->offset, t->color, t->width, t->height);
            if (dumped < 400 && (df = fopen(path, "wb")) != NULL) {
                uint32_t hdr[32];
                memset(hdr, 0, sizeof hdr);
                hdr[0] = 0x20534444u; hdr[1] = 124; hdr[2] = 0x1007; hdr[3] = t->height; hdr[4] = t->width;
                hdr[5] = fbytes; hdr[19] = 32; hdr[20] = 4;
                hdr[21] = t->color == 0x0C ? 0x31545844u : t->color == 0x0E ? 0x33545844u : 0x35545844u;
                hdr[27] = 0x1000;
                fwrite(hdr, 4, 32, df);
                fwrite(gm + t->offset, 1, fbytes, df);
                fclose(df);
            }
        }
        if (tex_make(&td, sd, &e->tex, &e->srv)) {
            e->addr = t->offset; e->fmt = t->color; e->w = t->width; e->h = t->height;
            e->pitch = t->pitch; e->palette = tpal; e->hash = hash; e->last_use = s_gpu11.frame;
            e->checked = s_gpu11.frame + 1;
            e->cube = (uint32_t)cube;
            s_gpu11.tex_uploads++;
            gpu_upload_note(t, cube);
            return e->srv;
        }
    }
    n = t->width * t->height;
    pix = (uint32_t *)malloc((size_t)n * 4 * faces);
    if (!pix)
        return NULL;
    /* Fast paths for the formats that dominate (profile, 2026-09-25: the
     * per-texel sampler below was ~19% of the frame): unswizzle a whole face
     * at once, then expand. Same results as sample_stage for in-range
     * coordinates. */
    if (t->color == 0x00 || t->color == 0x19 || t->color == 0x02 || t->color == 0x03
            || t->color == 0x04 || t->color == 0x05) {
        /* 8- and 16-bit swizzled formats: unswizzle a face, then expand each
         * texel exactly as sample_texture's linear twins do. */
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        static uint8_t lin[1024 * 1024 * 2];
        int ok = 1, w = (int)t->width, h = (int)t->height;
        uint32_t bpp = (t->color == 0x00 || t->color == 0x19) ? 1u : 2u, k2;
        if ((w & (w - 1)) == 0 && (h & (h - 1)) == 0 && n * bpp <= sizeof lin) {
            __try {
                for (f = 0; f < faces; f++) {
                    uint32_t *dst = pix + (size_t)f * n;
                    xbox_unswizzle_rect(lin, mem + t->offset + f * face_bytes, t->width, t->height, bpp);
                    /* One loop per format (the switch per texel was the
                     * inner loop's main cost). */
                    const uint16_t *l16 = (const uint16_t *)lin;
                    static uint32_t t5[32], t6[64], t4[16];
                    if (!t6[63]) {          /* d3d8_expand_channel, tabulated */
                        uint32_t q;
                        for (q = 0; q < 32; q++) t5[q] = q * 255u / 31u;
                        for (q = 0; q < 64; q++) t6[q] = q * 255u / 63u;
                        for (q = 0; q < 16; q++) t4[q] = q * 255u / 15u;
                    }
#define EXP5(x) t5[x]
#define EXP6(x) t6[x]
#define EXP4(x) t4[x]
                    switch (t->color) {
                    case 0x00: for (k2 = 0; k2 < n; k2++) { uint32_t v = lin[k2]; dst[k2] = 0xFF000000u | (v << 16) | (v << 8) | v; } break;
                    case 0x19: for (k2 = 0; k2 < n; k2++) dst[k2] = ((uint32_t)lin[k2] << 24) | 0x00FFFFFFu; break;
                    case 0x02: for (k2 = 0; k2 < n; k2++) { uint32_t v = l16[k2];
                                   dst[k2] = ((v & 0x8000u) ? 0xFF000000u : 0u) | (EXP5((v >> 10) & 0x1F) << 16)
                                           | (EXP5((v >> 5) & 0x1F) << 8) | EXP5(v & 0x1F); } break;
                    case 0x03: for (k2 = 0; k2 < n; k2++) { uint32_t v = l16[k2];
                                   dst[k2] = 0xFF000000u | (EXP5((v >> 10) & 0x1F) << 16)
                                           | (EXP5((v >> 5) & 0x1F) << 8) | EXP5(v & 0x1F); } break;
                    case 0x04: for (k2 = 0; k2 < n; k2++) { uint32_t v = l16[k2];
                                   dst[k2] = (EXP4((v >> 12) & 0xF) << 24) | (EXP4((v >> 8) & 0xF) << 16)
                                           | (EXP4((v >> 4) & 0xF) << 8) | EXP4(v & 0xF); } break;
                    default:   for (k2 = 0; k2 < n; k2++) { uint32_t v = l16[k2];
                                   dst[k2] = 0xFF000000u | (EXP5((v >> 11) & 0x1F) << 16)
                                           | (EXP6((v >> 5) & 0x3F) << 8) | EXP5(v & 0x1F); } break;
                    }
#undef EXP5
#undef EXP6
#undef EXP4
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = 0;
            }
            if (ok)
                goto decoded;
        }
    }
    if ((t->color == 0x0B && t->palette) || t->color == 0x06 || t->color == 0x07) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int ok = 1, w = (int)t->width, h = (int)t->height;
        if ((w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
            __try {
                for (f = 0; f < faces; f++) {
                    uint32_t *dst = pix + (size_t)f * n;
                    const uint8_t *src = mem + t->offset + f * face_bytes;
                    if (t->color == 0x0B) {
                        /* Grows to the largest paletted texture seen; was a
                         * fixed 1024x1024, so a bigger one (the NV2A allows
                         * 4096x4096) failed to decode. */
                        static uint8_t *idx;
                        static size_t idx_cap;
                        const uint32_t *pal = (const uint32_t *)(mem + t->palette);
                        uint32_t k2;
                        if (n > idx_cap) {
                            uint8_t *g = (uint8_t *)realloc(idx, n);
                            if (!g) { ok = 0; break; }
                            idx = g; idx_cap = n;
                        }
                        xbox_unswizzle_rect(idx, src, t->width, t->height, 1);
                        for (k2 = 0; k2 < n; k2++)
                            dst[k2] = pal[idx[k2]];
                        if (t->width == 128 && t->height == 128 && f == 0)
                            if (!g_xbox_tex_override) prompt_patch(dst, t->offset);   /* button icons (nv2a_gpu_prompts.inc.c) */
                    } else {
                        xbox_unswizzle_rect(dst, src, t->width, t->height, 4);
                        if (t->color == 0x07) {
                            uint32_t k2;
                            for (k2 = 0; k2 < n; k2++)
                                dst[k2] |= 0xFF000000u;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = 0;
            }
            if (ok)
                goto decoded;
        }
    }
    {
        uint32_t u, v, base = t->offset;
        int ok = 1;
        __try {
            for (f = 0; f < faces; f++) {
                t->offset = base + f * face_bytes;       /* sample_stage reads the stage's Texture */
                for (v = 0; v < t->height; v++)
                    for (u = 0; u < t->width; u++) {
                        uint32_t c = 0;
                        if (!sample_stage(st, u, v, &c) && u == 0 && v == 0 && f == 0 &&
                            env_cached("RECOMP_TEX_FORMAT_TRACE")) {
                            static uint64_t seen[32];
                            static unsigned used;
                            uint64_t key = ((uint64_t)t->color << 32) | s_comb.stage_prog;
                            unsigned k;
                            for (k = 0; k < used && seen[k] != key; ++k) {}
                            if (k == used && used < 32) {
                                seen[used++] = key;
                                fprintf(stderr, "  [TEXFORMAT] unsupported stage=%u fmt=%02X addr=%08X %ux%u prog=%08X\n",
                                        st, t->color, base, t->width, t->height, s_comb.stage_prog);
                            }
                        }
                        pix[(size_t)f * n + (size_t)v * t->width + u] = c;
                    }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = 0;
        }
        t->offset = base;
        if (!ok) { free(pix); return NULL; }
    }
decoded:
    /* RECOMP_GPU_TEXDUMP=<prefix>: write what was decoded (face 0, BGRA over
     * mid-grey), textures of 128x128 and up, first 400. */
    if (env_cached("RECOMP_GPU_TEXDUMP") && gpu_texdump_match(t, n)) {   /* _ADDR: only that texture */
        static unsigned dumped;
        if (dumped < (env_cached("RECOMP_GPU_TEXDUMP_ADDR") && !strcmp(env_cached("RECOMP_GPU_TEXDUMP_ADDR"), "all") ? 3000u : 400u)) {
            char path[512];
            FILE *df;
            snprintf(path, sizeof path, "%s%03u_%08X_fmt%02X_pal%08X_%ux%u%s.bmp",
                     env_cached("RECOMP_GPU_TEXDUMP"), dumped++, t->offset, t->color, t->palette,
                     t->width, t->height, cube ? "_cube" : "");
            df = fopen(path, "wb");
            if (df) {
                uint32_t size = 54 + n * 3, u2, v2;
                uint8_t hdr[54] = { 'B', 'M' };
                memcpy(hdr + 2, &size, 4);
                hdr[10] = 54; hdr[14] = 40;
                memcpy(hdr + 18, &t->width, 4);
                { int32_t nh = -(int32_t)t->height; memcpy(hdr + 22, &nh, 4); }
                hdr[26] = 1; hdr[28] = 24;
                fwrite(hdr, 1, 54, df);
                for (v2 = 0; v2 < t->height; v2++)
                    for (u2 = 0; u2 < t->width; u2++) {
                        uint32_t px = pix[(size_t)v2 * t->width + u2], a = px >> 24;
                        uint8_t o3[3];
                        o3[0] = (uint8_t)(((px & 0xFF) * a + 128u * (255u - a)) / 255u);
                        o3[1] = (uint8_t)((((px >> 8) & 0xFF) * a + 128u * (255u - a)) / 255u);
                        o3[2] = (uint8_t)((((px >> 16) & 0xFF) * a + 128u * (255u - a)) / 255u);
                        fwrite(o3, 1, 3, df);
                    }
                fclose(df);
            }
        }
    }
    if (!cube && tex_override_wants(t)) {
        uint32_t ow = 0, oh = 0;
        const uint32_t *img = g_xbox_tex_override(t->color, t->width, t->height, tex_content_hash(t), pix, &ow, &oh);
        if (img && tex_upload_override(e, t, tpal, hash, img, ow, oh)) {
            free(pix);
            return e->srv;
        }
    }
    {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd[6];
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = 1; td.ArraySize = faces;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        for (f = 0; f < faces; f++) {
            sd[f].pSysMem = pix + (size_t)f * n; sd[f].SysMemPitch = t->width * 4; sd[f].SysMemSlicePitch = n * 4;
        }
        if (cube && t->width != t->height) {
            free(pix);
            return NULL;                                 /* a cube face must be square */
        }
        {
            LARGE_INTEGER c0, c1;
            QueryPerformanceCounter(&c0);
            tex_make(&td, sd, &e->tex, &e->srv);
            QueryPerformanceCounter(&c1);
            g_tex_create_qpc += c1.QuadPart - c0.QuadPart;
        }
    }
    free(pix);
    e->addr = t->offset; e->fmt = t->color; e->w = t->width; e->h = t->height;
    e->pitch = t->pitch; e->palette = tpal; e->hash = hash; e->last_use = s_gpu11.frame;
    e->checked = s_gpu11.frame + 1;
    e->cube = (uint32_t)cube;
    s_gpu11.tex_uploads++;
    gpu_upload_note(t, cube);
    if (env_cached("RECOMP_TEX_REPEAT_LOG")) {   /* how many uploads are contents seen before? */
        static uint64_t seen[1 << 16];
        static unsigned uploads, repeats;
        const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t bytes = gpu_tex_bytes(t), i2, h2 = 2166136261u;
        uint64_t key;
        unsigned slot;
        if (bytes > 4u * 1024u * 1024u) bytes = 4u * 1024u * 1024u;
        for (i2 = 0; i2 + 4 <= bytes; i2 += 4) h2 = (h2 ^ *(const uint32_t *)(gm + t->offset + i2)) * 16777619u;
        key = ((uint64_t)h2 << 32) ^ ((uint64_t)t->color << 24) ^ ((uint64_t)t->width << 12) ^ t->height ^ ((uint64_t)tpal << 1);
        slot = (unsigned)(key ^ (key >> 29)) & 0xFFFF;
        while (seen[slot] && seen[slot] != key) slot = (slot + 1) & 0xFFFF;
        uploads++;
        if (seen[slot] == key) repeats++; else seen[slot] = key;
        if ((uploads % 500) == 0)
            fprintf(stderr, "  [TEXREPEAT] %u uploads, %u of them contents uploaded before (%.0f%%)\n",
                    uploads, repeats, 100.0 * repeats / uploads);
    }
    {
        LARGE_INTEGER tex_t1;
        QueryPerformanceCounter(&tex_t1);
        g_tex_upload_qpc += tex_t1.QuadPart - tex_t0.QuadPart;
    }
    return e->srv;
}

static ID3D11SamplerState *gpu_sampler(int st)
{
    static ID3D11SamplerState *cache[64];
    static uint32_t keys[64];
    const Texture *t = st == 0 ? &s_gpu.tex : &s_stage[st];
    uint32_t filt = s_tex_reg[(0x1B14 + 0x40u * (uint32_t)st - NV_TEX_FIRST) / 4];
    uint32_t mag = (filt >> 24) & 0xF, key;
    D3D11_SAMPLER_DESC sd;
    int i;
    D3D11_TEXTURE_ADDRESS_MODE am[2];
    uint32_t modes[2];
    modes[0] = t->addr_u; modes[1] = t->addr_v;
    key = (mag == 1 ? 1u : 0u) | (t->addr_u << 4) | (t->addr_v << 8) | (g_aniso << 16);
    for (i = 0; i < 64; i++) {
        if (cache[i] && keys[i] == key)
            return cache[i];
        if (!cache[i])
            break;
    }
    if (i == 64)
        i = 0;
    for (int k = 0; k < 2; k++)
        am[k] = (modes[k] == 1 || env_cached("RECOMP_FORCE_WRAP")) ? D3D11_TEXTURE_ADDRESS_WRAP
              : modes[k] == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR
              : modes[k] == 4 ? D3D11_TEXTURE_ADDRESS_BORDER
              : D3D11_TEXTURE_ADDRESS_CLAMP;
    memset(&sd, 0, sizeof sd);
    sd.Filter = mag == 1 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (mag != 1 && g_aniso > 1) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = g_aniso;
    }
    sd.AddressU = am[0]; sd.AddressV = am[1]; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (cache[i]) ID3D11SamplerState_Release(cache[i]);
    cache[i] = NULL;
    ID3D11Device_CreateSamplerState(s_gpu11.dev, &sd, &cache[i]);
    keys[i] = key;
    return cache[i];
}

/* ---- output-merge state ---------------------------------------------- */

static D3D11_BLEND gpu_blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return D3D11_BLEND_SRC_COLOR;
    case 0x0301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D11_BLEND_DEST_COLOR;
    case 0x0307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND gpu_blend_alpha(D3D11_BLEND b)
{
    switch (b) {                     /* colour factors are not allowed for alpha */
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return b;
    }
}

static ID3D11BlendState *gpu_blend_state(void)
{
    static ID3D11BlendState *cache[64];
    static uint64_t keys[64];
    static int used;
    uint32_t cm = g_color_mask, mask = 0;
    uint64_t key;
    int i;
    D3D11_BLEND_DESC bd;
    if (cm & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (cm & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (cm & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (cm & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    key = ((uint64_t)(s_gpu.blend ? 1 : 0)) | ((uint64_t)(s_gpu.blend_sf & 0xFFFF) << 1)
        | ((uint64_t)(s_gpu.blend_df & 0xFFFF) << 17) | ((uint64_t)(s_gpu.blend_eq & 0xFFFF) << 33)
        | ((uint64_t)mask << 49);
    for (i = 0; i < used; i++)
        if (keys[i] == key)
            return cache[i];
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = s_gpu.blend ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = gpu_blend_factor(s_gpu.blend_sf);
    bd.RenderTarget[0].DestBlend = gpu_blend_factor(s_gpu.blend_df);
    bd.RenderTarget[0].SrcBlendAlpha = gpu_blend_alpha(bd.RenderTarget[0].SrcBlend);
    bd.RenderTarget[0].DestBlendAlpha = gpu_blend_alpha(bd.RenderTarget[0].DestBlend);
    switch (s_gpu.blend_eq) {
    case 0x800A: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
    case 0x800B: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case 0x8007: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN; break;
    case 0x8008: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX; break;
    default:     bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; break;
    }
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    if (used == 64) {
        ID3D11BlendState_Release(cache[0]);
        used = 0;
    }
    ID3D11Device_CreateBlendState(s_gpu11.dev, &bd, &cache[used]);
    keys[used] = key;
    return cache[used++];
}

static D3D11_COMPARISON_FUNC gpu_cmp(uint32_t f)
{
    switch (f) {
    case 0x0200: return D3D11_COMPARISON_NEVER;
    case 0x0201: return D3D11_COMPARISON_LESS;
    case 0x0202: return D3D11_COMPARISON_EQUAL;
    case 0x0203: return D3D11_COMPARISON_LESS_EQUAL;
    case 0x0204: return D3D11_COMPARISON_GREATER;
    case 0x0205: return D3D11_COMPARISON_NOT_EQUAL;
    case 0x0206: return D3D11_COMPARISON_GREATER_EQUAL;
    default:     return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP gpu_sop(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *gpu_depth_state(void)
{
    static ID3D11DepthStencilState *cache[64];
    static uint64_t keys[64];
    static int used;
    uint64_t key;
    int i;
    D3D11_DEPTH_STENCIL_DESC dd;
    key = (uint64_t)(s_vp.depth_test ? 1 : 0) | ((uint64_t)(s_vp.depth_mask ? 1 : 0) << 1)
        | ((uint64_t)(s_vp.depth_func & 0xF) << 2) | ((uint64_t)(s_vp.stencil_test ? 1 : 0) << 6)
        | ((uint64_t)(s_vp.stencil_func & 0xF) << 7) | ((uint64_t)(s_vp.stencil_fmask & 0xFF) << 11)
        | ((uint64_t)(s_vp.stencil_wmask & 0xFF) << 19) | ((uint64_t)gpu_sop(s_vp.op_fail) << 27)
        | ((uint64_t)gpu_sop(s_vp.op_zfail) << 31) | ((uint64_t)gpu_sop(s_vp.op_zpass) << 35);
    for (i = 0; i < used; i++)
        if (keys[i] == key)
            return cache[i];
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = s_vp.depth_test && !env_cached("RECOMP_GPU_NODEPTH") ? TRUE : FALSE;
    dd.DepthWriteMask = (s_vp.depth_test && s_vp.depth_mask) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = gpu_cmp(s_vp.depth_func);
    dd.StencilEnable = s_vp.stencil_test ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)s_vp.stencil_fmask;
    dd.StencilWriteMask = (UINT8)s_vp.stencil_wmask;
    dd.FrontFace.StencilFunc = gpu_cmp(s_vp.stencil_func);
    dd.FrontFace.StencilFailOp = gpu_sop(s_vp.op_fail);
    dd.FrontFace.StencilDepthFailOp = gpu_sop(s_vp.op_zfail);
    dd.FrontFace.StencilPassOp = gpu_sop(s_vp.op_zpass);
    dd.BackFace = dd.FrontFace;
    if (used == 64) {
        ID3D11DepthStencilState_Release(cache[0]);
        used = 0;
    }
    ID3D11Device_CreateDepthStencilState(s_gpu11.dev, &dd, &cache[used]);
    keys[used] = key;
    return cache[used++];
}

static ID3D11DepthStencilState *gpu_no_depth(void)
{
    static ID3D11DepthStencilState *st;
    if (!st) {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11Device_CreateDepthStencilState(s_gpu11.dev, &dd, &st);
    }
    return st;
}

/* The depth buffer's full-scale value (xemu pgraph_glsl_set_clip_range):
 * Z16 or Z24, integer or floating point (SET_CONTROL0 bit 12). */
static float gpu_zmax(void)
{
    int z16 = ((s_gpu.format >> 4) & 0xF) == 1;
    int zfloat = (s_vp.control0 >> 12) & 1;
    if (zfloat)
        return z16 ? 511.9375f : 1.0e30f;
    return z16 ? 65535.0f : 16777215.0f;
}

/* RECOMP_GPU_TEXDUMP_ADDR: a hex address, or FMT:WxH (e.g. 0B:128x64);
 * without it, every texture of 128x128 texels or more. */
static int gpu_texdump_match(const Texture *t, uint32_t n)
{
    const char *f = env_cached("RECOMP_GPU_TEXDUMP_ADDR");
    unsigned fmt, w, h;
    if (!f)
        return n >= 128u * 128u;
    if (!strcmp(f, "all"))                     /* every size (icons, glyph atlases) */
        return 1;
    if (sscanf(f, "%x:%ux%u", &fmt, &w, &h) == 3)
        return t->color == fmt && t->width == w && t->height == h;
    return strtoul(f, NULL, 16) == t->offset;
}

/* ---- drawing ---------------------------------------------------------- */

#include "nv2a_gpu_vp.inc.c"
#include "nv2a_gpu_ps.inc.c"

static void gpu_vertex(GpuVertex *o, const XfVert *x, uint32_t rw, uint32_t rh)
{
    /* oPos is in screen space, already divided by w; undo the divide so the
     * GPU clips and interpolates perspective-correctly (as xemu does). */
    float zfull = gpu_zmax(), zc = x->z;
    float nx = (vp_screen_coord(x->x) + (float)g_draw_ox) / (float)(rw + 2u * g_draw_ox) * 2.0f - 1.0f;
    float ny = 1.0f - vp_screen_coord(x->y) / (float)rh * 2.0f;
    float nz;
    /* No clamp to the clip range here: fragments outside it are discarded
     * (or clamped) per pixel, see ps_main. */
    nz = zc / zfull;
    float w = vp_clamp_w(x->w);
    int k;
    if (nz < 0.0f) nz = 0.0f;
    if (nz > 1.0f) nz = 1.0f;
    o->pos[0] = nx * w; o->pos[1] = ny * w; o->pos[2] = nz * w; o->pos[3] = w;
    o->rawscreen[0] = vp_screen_coord(x->x) + (float)g_draw_ox; o->rawscreen[1] = vp_screen_coord(x->y);
    o->rawscreen[2] = x->z; o->rawscreen[3] = w;
    memcpy(o->d0, x->col, sizeof o->d0);
    memcpy(o->d1, x->spec, sizeof o->d1);
    /* As the program wrote them; the shader applies each stage's convention. */
    memcpy(o->t, x->tc, sizeof o->t);
    o->wz[0] = x->w; o->wz[1] = x->fog; o->wz[2] = 0.0f; o->wz[3] = 1.0f;
    if (env_cached("RECOMP_GPU_NOPROJ"))
        for (k = 0; k < 4; k++)
            o->t[k][3] = 1.0f;
}

/* A pre-transformed (fixed-function mode) vertex, read the way the software
 * 2D path reads it: attribute 0 in pixels, no perspective, texel coordinates. */
static const XfVert *gpu_vertex_2d(uint32_t index)
{
    static XfVert ring[3];
    static unsigned next;
    XfVert *x = &ring[next++ % 3];
    float p[4], c[4];
    memset(x, 0, sizeof *x);
    x->ok = fetch_attr(&s_gpu.attr[0], index, p);
    x->x = p[0]; x->y = p[1]; x->z = 0.0f; x->w = 1.0f; x->fog = 1.0f;
    if (fetch_attr(color_attr(), index, c))
        memcpy(x->col, c, sizeof x->col);
    else
        x->col[0] = x->col[1] = x->col[2] = x->col[3] = 1.0f;
    if (!fetch_texcoord(index, x->uv))
        x->uv[0] = x->uv[1] = 0.0f;
    {
        float t[4];
        if (fetch_attr(texcoord_attr(), index, t)) {
            x->tc[0][0] = t[0]; x->tc[0][1] = t[1];
        }
        x->tc[0][3] = 1.0f;
    }
    return x;
}

#include "nv2a_gpu_capture.inc.c"
#include "nv2a_gpu_shadow_box.inc.c"

/* RECOMP_TRI_DUMP=<texture hex>,<ms>: after that many ms, write every CPU-
 * transformed triangle drawn with that texture during one frame to
 * logs\tridump.txt (x y z w per corner, then the draw's vp start). */
static void gpu_tri_dump(const XfVert *a, const XfVert *b, const XfVert *c, uint32_t ia, uint32_t ib, uint32_t ic)
{
    static int state = -1;          /* -1 unparsed, 0 off, 1 waiting, 2 dumping, 3 done */
    static uint32_t tex;
    static DWORD after, t0, frame;
    static FILE *f;
    if (state < 0) {
        const char *e = env_cached("RECOMP_TRI_DUMP");
        state = 0;
        if (e && sscanf(e, "%x,%lu", &tex, &after) == 2) state = 1;
    }
    if (state == 0 || state == 3) return;
    if (!t0) t0 = GetTickCount();
    if (state == 1) {
        if (GetTickCount() - t0 < after) return;
        state = 2;
        frame = s_gpu11.frame;
        f = fopen("logs\\tridump.txt", "w");
    }
    if (s_gpu11.frame != frame) { if (f) fclose(f); f = NULL; state = 3; return; }
    if (!f || (tex && (!s_gpu.tex.valid || s_gpu.tex.offset != tex))) return;   /* tex 0: every draw */
    {   /* once per draw: the constant rows the skinning program reads */
        static uint32_t last_draws = 0xFFFFFFFFu;
        if (s_gpu.draws != last_draws) {
            int q;
            last_draws = s_gpu.draws;
            fprintf(f, "DRAW %u start %u tex %08X\n", s_gpu.draws, s_vp.start, s_gpu.tex.valid ? s_gpu.tex.offset : 0);
            if (tex) for (q = 0; q < 192; q++)
                fprintf(f, "C %d %g %g %g %g\n", q, s_vp.c[q][0], s_vp.c[q][1], s_vp.c[q][2], s_vp.c[q][3]);
        }
    }
    {
        float ba[4], bb[4], bc[4], pa[4], pb[4], pc[4];
        fetch_attr(&s_gpu.attr[1], ia, ba); fetch_attr(&s_gpu.attr[1], ib, bb); fetch_attr(&s_gpu.attr[1], ic, bc);
        fetch_attr(&s_gpu.attr[0], ia, pa); fetch_attr(&s_gpu.attr[0], ib, pb); fetch_attr(&s_gpu.attr[0], ic, pc);
        fprintf(f, "%g %g %g %g %g %g %g %g %g %g %g %g %u %d%d%d bones %g %g %g idx %u %u %u in %g,%g,%g %g,%g,%g %g,%g,%g\n",
                a->x, a->y, a->z, a->w, b->x, b->y, b->z, b->w, c->x, c->y, c->z, c->w, s_vp.start, a->ok, b->ok, c->ok,
                ba[0], bb[0], bc[0], ia, ib, ic, pa[0], pa[1], pa[2], pb[0], pb[1], pb[2], pc[0], pc[1], pc[2]);
    }
}

/* RECOMP_VP_REJ_LOG=1: the first vertices whose transformed position is
 * unusable (triangle dropped): program start, output and every input attribute. */
static void gpu_log_rejected(uint32_t index, const XfVert *v)
{
    static int on = -1, n;
    if (on < 0) on = env_cached("RECOMP_VP_REJ_LOG") != NULL;
    static DWORD t0;
    if (!on || v->ok || n >= 60) return;
    if (!t0) t0 = GetTickCount();
    {
        const char *after = env_cached("RECOMP_VP_REJ_AFTER");   /* ms after the first rejection */
        if (after && GetTickCount() - t0 < (DWORD)atoi(after)) return;
    }
    n++;
    fprintf(stderr, "  [VPREJ] start %u prim %u idx %u out (%g,%g,%g,%g)\n",
            s_vp.start, s_gpu.prim, index, v->x, v->y, v->z, v->w);
    for (uint32_t k = 0; k < NV_VERTEX_ATTRS; k++) {
        float in[4];
        if (!s_gpu.attr[k].size) continue;
        fetch_attr(&s_gpu.attr[k], index, in);
        fprintf(stderr, "  [VPREJ]   attr%u off %08X type %u size %u stride %u = %g %g %g %g\n", k,
                s_gpu.attr[k].offset, s_gpu.attr[k].type, s_gpu.attr[k].size, s_gpu.attr[k].stride,
                in[0], in[1], in[2], in[3]);
    }
}

/* Draw the triangles of the current batch: transformed by the vertex
 * program, or (flat2d) pre-transformed screen-space vertices shaded like the
 * software 2D path -- texture x diffuse, no combiners, depth or stencil.
 * Returns 0 to fall back to the software rasteriser. */
/* RECOMP_GPU_PROFILE: where the draw path's time goes, in CPU cycles per
 * stage (vertex build, textures, buffer upload + binds, state + draw call,
 * the rest), printed every five seconds. */
static uint64_t s_dp_acc[6], s_dp_draws;
#define DP_NOW() (s_dp_on ? __rdtsc() : 0)
static void gpu_dp_report(void)
{
    static DWORD since;
    DWORD now = GetTickCount();
    if (!since) since = now;
    if (now - since < 5000 || !s_dp_draws) return;
    {
        uint64_t tot = s_dp_acc[5] ? s_dp_acc[5] : 1;
        fprintf(stderr, "  [DRAWPERF] %llu draws, %.1f Mcyc/s in draws: build %.0f%% tex %.0f%% upload+bind %.0f%% state+draw %.0f%% other %.0f%%, %u target copies, %u new targets\n",
                (unsigned long long)s_dp_draws, s_dp_acc[5] / 1e6 / ((now - since) / 1000.0),
                100.0 * s_dp_acc[0] / tot, 100.0 * s_dp_acc[1] / tot, 100.0 * s_dp_acc[2] / tot,
                100.0 * (s_dp_acc[3] - s_dp_acc[2]) / tot, 100.0 * (tot - s_dp_acc[0] - s_dp_acc[1] - s_dp_acc[3]) / tot, s_gpu11.rt_copies, s_gpu11.surf_creates);
        s_gpu11.surf_creates = 0;
        s_gpu11.rt_copies = 0;
    }
    memset(s_dp_acc, 0, sizeof s_dp_acc);
    s_dp_draws = 0;
    since = now;
}

static int gpu_draw_batch_unlocked_body(int flat2d);
static int gpu_draw_batch_unlocked(int flat2d)
{
    uint64_t t0;
    int r;
    if (s_dp_on < 0) s_dp_on = getenv("RECOMP_GPU_PROFILE") != NULL;
    t0 = DP_NOW();
    r = gpu_draw_batch_unlocked_body(flat2d);
    if (s_dp_on) { s_dp_acc[5] += __rdtsc() - t0; s_dp_draws++; gpu_dp_report(); }
    return r;
}

static int gpu_draw_batch_unlocked_body(int flat2d)
{
    const uint16_t *ix = s_gpu.idx;
    uint32_t n = s_gpu.idx_count, i, nv = 0, rw, rh;
    int si, st, lines;
    GpuSurface *surf;
    D3D11_MAPPED_SUBRESOURCE m;
    GpuConsts c;
    ID3D11ShaderResourceView *srv[4], *cubes[4];
    ID3D11SamplerState *smp[4];
    const XfVert *(*get)(uint32_t);
    ID3D11VertexShader *vpvs = NULL;     /* the program on the GPU (RECOMP_GPU_VP) */
    UINT first_vertex = 0;                /* where this batch sits in the vertex ring */
    uint32_t vp_nverts = 0;
    LARGE_INTEGER cpu_vp_t0 = { 0 }, cpu_vp_t1;
    int profile_cpu_vp = 0;
    int capture_cpu;

    if (!gpu_enabled() || !gpu_init())
        return 0;
    si = gpu_bind_surface();
    if (si < 0)
        return 0;
    surf = &s_gpu11.surf[si];
    rw = surf->w; rh = surf->h;
    g_draw_ox = surf->ox;
    capture_cpu = gpu_capture_begin(flat2d,surf);

#define GPU_TRI(a_, b_, c_) do { \
        const XfVert *A_ = get(ix[a_]), *B_ = get(ix[b_]), *C_ = get(ix[c_]); \
        gpu_tri_dump(A_, B_, C_, ix[a_], ix[b_], ix[c_]); \
        gpu_capture_triangle(A_, B_, C_, ix[a_], ix[b_], ix[c_]); \
        if (A_->ok && B_->ok && C_->ok && nv + 3 <= GPU_MAX_VERTS) { \
            gpu_vertex(&s_gpu11.verts[nv++], A_, rw, rh); \
            gpu_vertex(&s_gpu11.verts[nv++], B_, rw, rh); \
            gpu_vertex(&s_gpu11.verts[nv++], C_, rw, rh); \
        } else { s_vp.rej_bad++; gpu_log_rejected(ix[a_], A_); gpu_log_rejected(ix[b_], B_); gpu_log_rejected(ix[c_], C_); } } while (0)
    /* Lines (NV097 2 lines, 3 line loop, 4 line strip) go out as a D3D line
     * list: play art on the play-calling screen, field markings. */
#define GPU_LINE(a_, b_) do {         const XfVert *A_ = get(ix[a_]), *B_ = get(ix[b_]);         if (A_->ok && B_->ok && nv + 2 <= GPU_MAX_VERTS) {             gpu_vertex(&s_gpu11.verts[nv++], A_, rw, rh);             gpu_vertex(&s_gpu11.verts[nv++], B_, rw, rh);         } else s_vp.rej_bad++; } while (0)
    lines = s_gpu.prim >= NV_PRIM_LINES && s_gpu.prim <= NV_PRIM_LINE_STRIP;
    if (flat2d) {
        get = gpu_vertex_2d;
        if (n < (lines ? 2u : 3u))
            return 1;
        if (!batch_is_screen_space()) {
            s_gpu.batches_untransformed++;
            return 1;
        }
    } else {
        get = vp_vertex;
        vp_begin_batch();
        s_vp.batches++;
        if (!capture_cpu && gpu_vp_enabled())
            vpvs = gpu_vp_shader();
    }
    if (vpvs) {
        { uint64_t a = DP_NOW();
          nv = gpu_vp_raw_on() ? gpu_vp_build_raw() : gpu_vp_build(&vp_nverts);
          if (s_dp_on) s_dp_acc[0] += __rdtsc() - a; }
        s_vp.verts += vp_nverts;
    } else {
        profile_cpu_vp = !flat2d && env_cached("RECOMP_GPU_PROFILE") != NULL;
        if (profile_cpu_vp) QueryPerformanceCounter(&cpu_vp_t0);
        switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) GPU_TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) GPU_TRI(i + 1, i, i + 2); else GPU_TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) GPU_TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) { GPU_TRI(i, i + 1, i + 2); GPU_TRI(i, i + 2, i + 3); }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) { GPU_TRI(i, i + 1, i + 3); GPU_TRI(i, i + 3, i + 2); }
        break;
    case NV_PRIM_LINES:
        for (i = 0; i + 1 < n; i += 2) GPU_LINE(i, i + 1);
        break;
    case NV_PRIM_LINE_LOOP:
    case NV_PRIM_LINE_STRIP:
        for (i = 0; i + 1 < n; i++) GPU_LINE(i, i + 1);
        if (s_gpu.prim == NV_PRIM_LINE_LOOP && n > 2) GPU_LINE(n - 1, 0);
        break;
    default:
        break;
        }
        if (profile_cpu_vp) {
            static LARGE_INTEGER frequency;
            static LONGLONG start46_ticks, other_ticks;
            static uint64_t start46_verts, other_verts;
            static uint32_t start46_batches, other_batches;
            static DWORD since;
            DWORD now = GetTickCount();
            QueryPerformanceCounter(&cpu_vp_t1);
            if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
            if (s_vp.start == 46) {
                start46_ticks += cpu_vp_t1.QuadPart - cpu_vp_t0.QuadPart;
                start46_verts += nv; start46_batches++;
            } else {
                other_ticks += cpu_vp_t1.QuadPart - cpu_vp_t0.QuadPart;
                other_verts += nv; other_batches++;
            }
            if (!since) since = now;
            if (now - since >= 5000) {
                fprintf(stderr, "  [GPUVPPERF] cpu start46=%u batches/%llu verts/%.2fms other=%u/%llu/%.2fms over %lums\n",
                        start46_batches, (unsigned long long)start46_verts,
                        start46_ticks * 1000.0 / frequency.QuadPart,
                        other_batches, (unsigned long long)other_verts,
                        other_ticks * 1000.0 / frequency.QuadPart, now - since);
                start46_ticks = other_ticks = 0;
                start46_verts = other_verts = 0;
                start46_batches = other_batches = 0;
                since = now;
            }
        }
    }
#undef GPU_TRI
#undef GPU_LINE
    if (!nv)
        return 1;
    /* RECOMP_GPU_SKIP_BLEND=SSSS/DDDD (hex GL enums): drop blended draws
     * with that blend pair -- isolates an overlay pass while debugging. */
    if (env_cached("RECOMP_GPU_SKIP_BLEND") && s_gpu.blend) {
        unsigned sf = 0, df = 0;
        if (sscanf(env_cached("RECOMP_GPU_SKIP_BLEND"), "%x/%x", &sf, &df) == 2
                && sf == s_gpu.blend_sf && df == s_gpu.blend_df)
            return 1;
    }

    /* RECOMP_SKIP_DRAW=<vp start>,<fmt hex>,<w>,<h>[,<blend>]: drop those draws
     * (diagnostics: which draw paints a stray triangle). */
    if (env_cached("RECOMP_SKIP_DRAW")) {
        unsigned st = 0, fmt = 0, w = 0, h = 0; int bl = -1;
        if (sscanf(env_cached("RECOMP_SKIP_DRAW"), "%u,%x,%u,%u,%d", &st, &fmt, &w, &h, &bl) >= 4
                && st == s_vp.start && s_gpu.tex.valid && fmt == s_gpu.tex.color
                && w == s_gpu.tex.width && h == s_gpu.tex.height && (bl < 0 || bl == (int)s_gpu.blend))
            return 1;
    }

    /* RECOMP_FACE_TRACE=<vp start>: each distinct state the draws of that
     * vertex program use (texture, alpha test, blend, combiners), once. */
    if (env_cached("RECOMP_FACE_TRACE") && ((uint32_t)atoi(env_cached("RECOMP_FACE_TRACE")) == s_vp.start || atoi(env_cached("RECOMP_FACE_TRACE")) < 0)) {
        static uint32_t seen[256][5];
        static int nseen;
        uint32_t key[5] = { s_vp.start, s_gpu.tex.valid ? s_gpu.tex.offset : 0,
                            s_gpu.alpha_test << 24 | s_gpu.alpha_func << 8 | s_gpu.alpha_ref,
                            s_gpu.blend << 31 | s_gpu.blend_sf << 16 | s_gpu.blend_df, s_comb.final1 };
        int q, found = 0;
        for (q = 0; q < nseen; q++) if (!memcmp(seen[q], key, sizeof key)) { found = 1; break; }
        if (!found && nseen < 256) {
            memcpy(seen[nseen++], key, sizeof key);
            fprintf(stderr, "  [FACE] start=%u tex=%08X fmt=%02X %ux%u en=%d%d%d%d atest=%u/%04X/%u blend=%u %04X/%04X comb=%u "
                    "stages=%u ctl=%08X c0=%08X/%08X a0=%08X/%08X fin=%08X/%08X ztest=%u zfunc=%04X zmask=%u cull=%u\n",
                    key[0], key[1], s_gpu.tex.color, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.enabled, s_stage[1].enabled, s_stage[2].enabled, s_stage[3].enabled,
                    s_gpu.alpha_test, s_gpu.alpha_func, s_gpu.alpha_ref, s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df,
                    comb_active(), s_comb.stage_prog, s_comb.control, s_comb.color_icw[0], s_comb.color_ocw[0],
                    s_comb.alpha_icw[0], s_comb.alpha_ocw[0], s_comb.final0, s_comb.final1,
                    s_vp.depth_test, s_vp.depth_func, s_vp.depth_mask, s_vp.cull);
        }
    }

    /* RECOMP_GPU_TRACE=1: every draw of one frame in 300, with where its
     * first vertex lands and the state that decides whether it shows. */
    if (!vpvs && env_cached("RECOMP_GPU_TRACE") && (s_gpu11.frame % 300) == 0) {
        const GpuVertex *v = &s_gpu11.verts[0];
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        for (i = 0; i < nv; i++) {
            float iw = s_gpu11.verts[i].pos[3];
            float px = s_gpu11.verts[i].pos[0] / iw, py = s_gpu11.verts[i].pos[1] / iw;
            if (iw <= 0) continue;
            if (px < mnx) mnx = px; if (px > mxx) mxx = px;
            if (py < mny) mny = py; if (py > mxy) mxy = py;
        }
        fprintf(stderr, "  [GPUTRACE] f%u %s surf=%08X %ux%u tris=%u ndc x[%.2f,%.2f] y[%.2f,%.2f] "
                "v0 pos=(%.3g %.3g %.3g %.3g) d0=(%.2f %.2f %.2f %.2f) z=%u/%04X ztest=%u zmask=%u "
                "clip=[%g,%g] zclear=%08X sten=%u/%04X blend=%u %04X/%04X cmask=%08X atest=%u/%04X/%u "
                "comb=%u tex0=%08X fmt=%02X %ux%u en=%d%d%d%d prog=%08X ctl=%08X c0=%08X/%08X fin=%08X/%08X\n",
                s_gpu11.frame, flat2d ? "2D" : "VP", surf->guest, rw, rh, nv / 3, mnx, mxx, mny, mxy,
                v->pos[0], v->pos[1], v->pos[2], v->pos[3], v->d0[0], v->d0[1], v->d0[2], v->d0[3],
                s_vp.depth_test, s_vp.depth_func, s_vp.depth_test, s_vp.depth_mask,
                s_vp.clip_min, s_vp.clip_max, s_vp.zclear, s_vp.stencil_test, s_vp.stencil_func,
                s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df, g_color_mask,
                s_gpu.alpha_test, s_gpu.alpha_func, s_gpu.alpha_ref, comb_active(),
                s_gpu.tex.valid ? s_gpu.tex.offset : 0, s_gpu.tex.color, s_gpu.tex.width, s_gpu.tex.height,
                s_gpu.tex.enabled, s_stage[1].enabled, s_stage[2].enabled, s_stage[3].enabled,
                s_comb.stage_prog, s_comb.control, s_comb.color_icw[0], s_comb.color_ocw[0],
                s_comb.final0, s_comb.final1);
        {
            /* How much of each stage's first 16 KB is non-zero: an empty
             * texture (never loaded, or rendered on the host) shows as 0. */
            const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
            unsigned nzs[4] = {0, 0, 0, 0}, k2, b2;
            for (k2 = 0; k2 < 4; k2++) {
                uint32_t off = k2 ? s_stage[k2].offset : s_gpu.tex.offset;
                if (!off) continue;
                for (b2 = 0; b2 < 16384; b2++) nzs[k2] += gm[off + b2] != 0;
            }
            fprintf(stderr, "  [GPUTRACE]   nonzero/16K t0 %u t1 %u t2 %u t3 %u\n", nzs[0], nzs[1], nzs[2], nzs[3]);
        }
        fprintf(stderr, "  [GPUTRACE]   pal0 %08X first %08X %08X ctl0 %08X\n", s_gpu.tex.palette,
                s_gpu.tex.palette ? *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + s_gpu.tex.palette) : 0,
                s_gpu.tex.palette ? *(const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + s_gpu.tex.palette + 64) : 0,
                s_gpu.tex.enabled);
        fprintf(stderr, "  [GPUTRACE]   stages: t1 %08X fmt %02X %ux%u v%d | t2 %08X fmt %02X %ux%u v%d | t3 %08X fmt %02X %ux%u v%d"
                " | comb %08X/%08X %08X/%08X %08X/%08X %08X/%08X a %08X/%08X %08X/%08X\n",
                s_stage[1].offset, s_stage[1].color, s_stage[1].width, s_stage[1].height, s_stage[1].valid,
                s_stage[2].offset, s_stage[2].color, s_stage[2].width, s_stage[2].height, s_stage[2].valid,
                s_stage[3].offset, s_stage[3].color, s_stage[3].width, s_stage[3].height, s_stage[3].valid,
                s_comb.color_icw[0], s_comb.color_ocw[0], s_comb.color_icw[1], s_comb.color_ocw[1],
                s_comb.color_icw[2], s_comb.color_ocw[2], s_comb.color_icw[3], s_comb.color_ocw[3],
                s_comb.alpha_icw[0], s_comb.alpha_ocw[0], s_comb.alpha_icw[1], s_comb.alpha_ocw[1]);
        /* The inputs of the batch's first vertex: which attributes are
         * arrays (type/size/stride) and what they hold. Tells a colour read
         * wrongly from a colour the program computes wrongly. */
        if (!flat2d && nv >= 3 && s_gpu.idx_count) {
            uint32_t a;
            fprintf(stderr, "  [GPUTRACE]   in:");
            for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                float in[4];
                int arr = fetch_attr(&s_gpu.attr[a], s_gpu.idx[0], in);
                if (arr || in[0] != 0 || in[1] != 0 || in[2] != 0 || in[3] != 1)
                    fprintf(stderr, " v%u%s(t%u s%u)=%.3g,%.3g,%.3g,%.3g", a, arr ? "" : "i",
                            s_gpu.attr[a].type, s_gpu.attr[a].size, in[0], in[1], in[2], in[3]);
            }
            fprintf(stderr, " mode=%u start=%u\n", s_vp.mode, s_vp.start);
            {
                int st2;
                fprintf(stderr, "  [GPUTRACE]   stageprog=%08X ctl=%08X final=%08X/%08X",
                        s_comb.stage_prog, s_comb.control, s_comb.final0, s_comb.final1);
                for (st2 = 0; st2 < (int)(s_comb.control & 0xFF) && st2 < 8; st2++)
                    fprintf(stderr, " s%d[%08X %08X %08X %08X]", st2, s_comb.color_icw[st2],
                            s_comb.color_ocw[st2], s_comb.alpha_icw[st2], s_comb.alpha_ocw[st2]);
                for (st2 = 0; st2 < 4; st2++) {
                    const Texture *tx = st2 == 0 ? &s_gpu.tex : &s_stage[st2];
                    fprintf(stderr, " t%d=%08X/%02X/%ux%u/%s", st2, tx->offset, tx->color, tx->width,
                            tx->height, tx->valid ? (tx->enabled ? "on" : "off") : "-");
                }
                fprintf(stderr, "\n");
            }
            /* A lit batch whose diffuse came out black: trace its program
             * once per program start (first four). */
            if (s_gpu11.verts[0].d0[0] == 0 && s_gpu11.verts[0].d0[1] == 0 && s_gpu11.verts[0].d0[2] == 0
                    && nv / 3 >= 500) {
                static uint32_t traced[4];
                static int ntraced;
                int k, seen = 0;
                for (k = 0; k < ntraced; k++)
                    seen |= traced[k] == s_vp.start + 1;
                if (!seen && ntraced < 4) {
                    float V[16][4], O[16][4];
                    traced[ntraced++] = s_vp.start + 1;
                    for (a = 0; a < NV_VERTEX_ATTRS; a++)
                        fetch_attr(&s_gpu.attr[a], s_gpu.idx[0], V[a]);
                    fprintf(stderr, "  [VPI] program start=%u, vertex %u:\n", s_vp.start, s_gpu.idx[0]);
                    s_vp_trace_next = 1;
                    vp_run(V, O);
                    s_vp_trace_next = 0;
                    fprintf(stderr, "  [VPI] oD0=%g,%g,%g,%g oD1=%g,%g,%g,%g\n",
                            O[3][0], O[3][1], O[3][2], O[3][3], O[4][0], O[4][1], O[4][2], O[4][3]);
                }
            }
        }
    }

    /* Constants. */
    memset(&c, 0, sizeof c);
    for (i = 0; i < 8; i++) {
        c.stage[i][0] = s_comb.color_icw[i]; c.stage[i][1] = s_comb.color_ocw[i];
        c.stage[i][2] = s_comb.alpha_icw[i]; c.stage[i][3] = s_comb.alpha_ocw[i];
        comb_unpack(s_comb.factor0[i], c.f0[i]);
        comb_unpack(s_comb.factor1[i], c.f1[i]);
    }
    c.misc[0] = s_comb.control; c.misc[1] = s_comb.stage_prog;
    c.misc[2] = s_comb.final0; c.misc[3] = s_comb.final1;
    comb_unpack(s_comb.specfog[0], c.specfog0);
    comb_unpack(s_comb.specfog[1], c.specfog1);
    c.alpha[0] = s_gpu.alpha_test; c.alpha[1] = s_gpu.alpha_func;
    /* RECOMP_NO_ATEST=<vp start> (diagnostics): that program's draws skip the alpha test. */
    if (env_cached("RECOMP_NO_ATEST") && (uint32_t)atoi(env_cached("RECOMP_NO_ATEST")) == s_vp.start && vp_active())
        c.alpha[0] = 0;
    c.depthp[0] = (s_vp.control0 >> 16) & 1 ? 1.0f : 0.0f;
    c.depthp[1] = gpu_zmax();
    c.depthp[2] = s_vp.clip_max > s_vp.clip_min ? s_vp.clip_min : 0.0f;
    c.depthp[3] = s_vp.clip_max > s_vp.clip_min ? s_vp.clip_max : c.depthp[1];
    c.depthfmt[0] = (s_vp.control0 & 0x1000u) ? 0u :
                   (((s_gpu.format >> 4) & 0xFu) == 1u ? 16u : 24u);
    /* Offset applies to filled polygons, not line-list play art. Shader
     * derivatives are in scaled pixels; convert the slope to guest pixels. */
    if (!lines && s_gpu.prim >= NV_PRIM_TRIANGLES && s_vp.poly_offset_fill) {
        c.depthofs[0] = s_vp.poly_offset_bias;
        c.depthofs[1] = s_vp.poly_offset_scale;
    }
    c.depthofs[2] = (float)surf->scale;
    c.lighting[0] = g_lighting_profile;
    if (env_cached("RECOMP_GPU_DEPTH_TRACE")) {
        static uint32_t seen[64];
        static unsigned used;
        uint32_t biasbits, scalebits, key;
        unsigned k;
        memcpy(&biasbits, &c.depthofs[0], sizeof biasbits);
        memcpy(&scalebits, &c.depthofs[1], sizeof scalebits);
        key = s_vp.control0 ^ (s_gpu.format * 16777619u) ^
              (s_vp.depth_func << 12) ^ (s_vp.start << 20) ^
              (s_gpu.prim << 4) ^ biasbits ^ (scalebits * 31u);
        for (k = 0; k < used && seen[k] != key; ++k) {}
        if (k == used && used < 64) {
            seen[used++] = key;
            fprintf(stderr, "  [GPUDEPTH] frame=%u vp=%u prim=%u ctl=%08X fmt=%08X "
                    "integer=%u test=%u/%04X mask=%u clip=%g/%g offset=%g/%g "
                    "cull=%u/%04X front=%04X tex=%08X prog=%08X\n",
                    s_gpu11.frame, s_vp.start, s_gpu.prim, s_vp.control0, s_gpu.format,
                    c.depthfmt[0], s_vp.depth_test, s_vp.depth_func, s_vp.depth_mask,
                    c.depthp[2], c.depthp[3], c.depthofs[0], c.depthofs[1],
                    s_vp.cull, s_vp.cull_face, s_vp.front_face,
                    s_gpu.tex.offset, s_comb.stage_prog);
        }
    }
    /* Fragments outside [clip min, clip max] are discarded unless
     * SET_ZMIN_MAX_CONTROL selects clamping (xemu: ZCLAMP_EN == CULL, the
     * reset value). NFL 2K5 redraws the stadium with CLIP_MIN at half the
     * depth range after the players; clamping squashed its near half onto
     * the clip plane, over the players, which looked see-through. */
    c.show[1] = ((s_vp.zminmax >> 4) & 0xF) == 0 ? 1u : 0u;
    c.fogc[0] = (float)(s_fog.color & 0xFF) / 255.0f;
    c.fogc[1] = (float)((s_fog.color >> 8) & 0xFF) / 255.0f;
    c.fogc[2] = (float)((s_fog.color >> 16) & 0xFF) / 255.0f;
    c.fogc[3] = (float)(s_fog.color >> 24) / 255.0f;
    if (env_cached("RECOMP_GPU_SHOW"))
        c.show[0] = (uint32_t)atoi(env_cached("RECOMP_GPU_SHOW"));
    c.alpha[2] = s_gpu.alpha_ref; c.alpha[3] = !flat2d && comb_active() ? 1u : 0u;
    /* Texture stage modes (SET_SHADER_STAGE_PROGRAM, 5 bits a stage), the dot
     * mappings and stage inputs (NV_PGRAPH_SHADERCTL), as xemu's psh.c reads
     * them. The field and player models use cube maps and the dot-product
     * reflection modes; treated as plain 2D they lit to black (2026-09-25). */
    {
        uint32_t sp = s_comb.stage_prog, ctl = s_comb.shaderctl;
        for (st = 0; st < 4; st++)
            c.mode[st] = (sp >> (st * 5)) & 0x1F;
        if (flat2d) {
            c.mode[0] = 1; c.mode[1] = c.mode[2] = c.mode[3] = 0;
        } else if (!comb_active() && !c.mode[0]) {
            c.mode[0] = 1;                       /* texture x diffuse, as before */
        }
        c.dotmap[0] = 0; c.dotmap[1] = ctl & 0xF; c.dotmap[2] = (ctl >> 4) & 0xF; c.dotmap[3] = (ctl >> 8) & 0xF;
        c.input[0] = 0; c.input[1] = 0; c.input[2] = (ctl >> 16) & 0xF; c.input[3] = (ctl >> 20) & 0xF;
    }
    for (st = 0; st < 4; st++) {
        const Texture *t = st == 0 ? &s_gpu.tex : &s_stage[st];
        uint32_t md = c.mode[st];
        int want = t->valid && (flat2d ? st == 0
                                       : t->enabled && md != 0 && md != 4 && md != 0x11);
        ID3D11ShaderResourceView *view;
        c.cube[st] = !flat2d && gpu_stage_is_cube(st);
        c.rect[st] = !tex_size_from_format(t->color);
        c.tsize[st][0] = t->width ? (float)t->width : 1.0f;
        c.tsize[st][1] = t->height ? (float)t->height : 1.0f;
        { uint64_t a = DP_NOW(); view = want ? gpu_texture(st, c.cube[st]) : NULL; if (s_dp_on) s_dp_acc[1] += __rdtsc() - a; }
        c.texon[st] = view ? 1u : 0u;
        srv[st] = (view && !c.cube[st]) ? view : s_gpu11.white_srv;
        cubes[st] = (view && c.cube[st]) ? view : s_gpu11.white_cube_srv;
        /* Hold them until the draw is issued: fetching a later stage can
         * evict this stage's cache entry and release its view, and the driver
         * then read a freed view (debug layer: "PSSetShaderResources ... array
         * index 0 corrupt"; NVIDIA crashed at kickoff, 2026-09-25). */
        ID3D11ShaderResourceView_AddRef(srv[st]);
        ID3D11ShaderResourceView_AddRef(cubes[st]);
        smp[st] = gpu_sampler(st);
    }
    /* Constants only when they changed; vertices appended to a ring with
     * NO_OVERWRITE (a WRITE_DISCARD per batch made the driver stall). */
    {
        static GpuConsts last;
        static int last_valid;
        if (!last_valid || memcmp(&c, &last, sizeof c) != 0) {
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_gpu11.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, &c, sizeof c);
                ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_gpu11.cb, 0);
                last = c;
                last_valid = 1;
            }
        }
    }
    if (!vpvs) {
        static UINT ring_pos;
        const UINT cap = GPU_MAX_VERTS * 8;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (ring_pos + nv > cap) { ring_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)s_gpu11.vb, 0, how, 0, &m))) {
            memcpy((GpuVertex *)m.pData + ring_pos, s_gpu11.verts, sizeof(GpuVertex) * nv);
            ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)s_gpu11.vb, 0);
            first_vertex = ring_pos;
            ring_pos += nv;
        }
    }

    /* Pipeline. */
    {
        UINT stride = sizeof(GpuVertex), off = 0;
        D3D11_VIEWPORT vp = { 0, 0, (float)((rw + 2 * surf->ox) * surf->scale), (float)(rh * surf->scale), 0.0f, 1.0f };
        D3D11_RECT sc[8];
        UINT nsc = 0;
        float bf[4];
        comb_unpack(s_gpu.blend_color, bf);
        if (!s_gpu.window_clip_type) {
            UINT k;
            for (k = 0; k < 8; k++) {
                uint32_t wx = s_gpu.window_clip_x[k], wy = s_gpu.window_clip_y[k];
                /* NV2A stores [minimum, maximum) just like D3D11 scissor
                 * rectangles (xemu's fragment test rejects coord >= max). */
                LONG l = (LONG)(wx & 0xFFF), r = (LONG)((wx >> 16) & 0xFFF);
                LONG t = (LONG)(wy & 0xFFF), b = (LONG)((wy >> 16) & 0xFFF);
                LONG sl = (LONG)s_gpu.clip_x, sr = (LONG)(s_gpu.clip_x + s_gpu.clip_w);
                LONG st = (LONG)s_gpu.clip_y, sb = (LONG)(s_gpu.clip_y + s_gpu.clip_h);
                if (l < sl) l = sl; if (r > sr) r = sr;
                if (t < st) t = st; if (b > sb) b = sb;
                if (r <= l || b <= t) continue;
                /* A rectangle across the whole original width also covers the
                 * extra columns; a narrower one just moves with the picture. */
                if (surf->ox && l <= 0 && r >= (LONG)rw) { l = -(LONG)surf->ox; r = (LONG)(rw + surf->ox); }
                sc[nsc].left = (l + (LONG)surf->ox) * (LONG)surf->scale; sc[nsc].right = (r + (LONG)surf->ox) * (LONG)surf->scale;
                sc[nsc].top = t * (LONG)surf->scale; sc[nsc].bottom = b * (LONG)surf->scale;
                nsc++;
            }
        }
        /* Exclusive window clips require per-fragment subtraction. NFL 2K5's
         * play-call path uses inclusive clips; retain the surface clip for
         * exclusive state until that mode is observed in a title draw. */
        if (!nsc) {
            LONG l = (LONG)s_gpu.clip_x, r = (LONG)(s_gpu.clip_x + s_gpu.clip_w);
            if (surf->ox && l <= 0 && r >= (LONG)rw) { l = -(LONG)surf->ox; r = (LONG)(rw + surf->ox); }
            sc[0].left = (LONG)((l + (LONG)surf->ox) * (LONG)surf->scale);
            sc[0].top = (LONG)(s_gpu.clip_y * surf->scale);
            sc[0].right = (LONG)((r + (LONG)surf->ox) * (LONG)surf->scale);
            sc[0].bottom = (LONG)((s_gpu.clip_y + s_gpu.clip_h) * surf->scale);
            nsc = 1;
        }
        if (env_cached("RECOMP_GPU_CLIPLOG")) {
            static uint32_t seen[64][8];
            static unsigned nseen;
            uint32_t key[8] = { (uint32_t)sc[0].left, (uint32_t)sc[0].right, (uint32_t)sc[0].top, (uint32_t)sc[0].bottom,
                                (uint32_t)nsc, (uint32_t)flat2d, surf->guest, s_gpu.window_clip_type };
            unsigned q;
            for (q = 0; q < nseen && memcmp(seen[q], key, sizeof key); q++) {}
            if (q == nseen && nseen < 64) {
                memcpy(seen[nseen++], key, sizeof key);
                fprintf(stderr, "  [CLIP] surf %08X %ux%u+%u scale %u: scissor %d..%d x %d..%d (%u rects, %s) clip %u,%u %ux%u wtype %u wclip0 %08X/%08X vp.start %u\n",
                        surf->guest, rw, rh, surf->ox, surf->scale, (int)sc[0].left / (int)surf->scale, (int)sc[0].right / (int)surf->scale,
                        (int)sc[0].top / (int)surf->scale, (int)sc[0].bottom / (int)surf->scale, nsc, flat2d ? "2D" : "3D",
                        s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h, s_gpu.window_clip_type,
                        s_gpu.window_clip_x[0], s_gpu.window_clip_y[0], s_vp.start);
            }
        }
        uint64_t dp_state0 = DP_NOW();
        /* Each piece of state only when it differs from what is bound
         * (s_bound). A new render target is set first: D3D unbinds any of
         * its views that are bound as textures, so textures follow it. */
        {
            D3D11_PRIMITIVE_TOPOLOGY topo = lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                  : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            ID3D11GeometryShader *gs = !flat2d && !lines ? s_gpu11.gs : NULL;
            ID3D11BlendState *blend = gpu_blend_state();
            ID3D11DepthStencilState *dss = flat2d ? gpu_no_depth() : gpu_depth_state();
            int all = !s_bound.valid;
            if (all || s_bound.rtv != surf->rtv || s_bound.dsv != surf->dsv) {
                ID3D11DeviceContext_OMSetRenderTargets(s_gpu11.ctx, 1, &surf->rtv, surf->dsv);
                s_bound.rtv = surf->rtv; s_bound.dsv = surf->dsv;
                memset(s_bound.srv, 0, sizeof s_bound.srv);       /* resend textures */
                s_bound.srv[0] = (ID3D11ShaderResourceView *)(uintptr_t)1;
            }
            if (all || s_bound.topo != topo) {
                ID3D11DeviceContext_IASetPrimitiveTopology(s_gpu11.ctx, topo);
                s_bound.topo = topo;
            }
            if (all) {
                /* Bound once: these objects never change (constant buffers are
                 * refilled in place). */
                ID3D11DeviceContext_GSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_gpu11.cb);
                ID3D11DeviceContext_PSSetConstantBuffers(s_gpu11.ctx, 0, 1, &s_gpu11.cb);
                s_bound.rs = NULL;                  /* chosen per draw (culling) */
                s_bound.il = NULL; s_bound.vb = s_bound.ib = s_bound.vs_cb1 = s_bound.vs_cb2 = NULL;
                s_bound.vs_srv = NULL;
                s_bound.vs = NULL; s_bound.gs = (ID3D11GeometryShader *)(uintptr_t)1;
                s_bound.ps = NULL;
                s_bound.blend = NULL; s_bound.ds = NULL; s_bound.nsc = 0;
                memset(&s_bound.vp, 0, sizeof s_bound.vp);
                memset(s_bound.smp, 0, sizeof s_bound.smp);
                s_bound.valid = 1;
            }
            {
                ID3D11PixelShader *ps = gpu_ps_shader(&c);   /* specialised when compiled */
                if (s_bound.ps != ps) {
                    ID3D11DeviceContext_PSSetShader(s_gpu11.ctx, ps, NULL, 0);
                    s_bound.ps = ps;
                }
            }
            {
                /* Back-face culling as the title set it (SET_CULL_FACE_ENABLE,
                 * SET_CULL_FACE, SET_FRONT_FACE), for 3D draws. Without it the
                 * inside of a head -- mouth and eye interiors -- showed through
                 * the face as dark triangles. RECOMP_GPU_CULL=1 turns it on (until verified),
                 * RECOMP_GPU_CULL_FLIP=1 reverses the winding convention. */
                static int cull_on = -1, cull_flip = -1;
                ID3D11RasterizerState *rs = s_gpu11.rs;
                if (cull_on < 0) {
                    const char *e = env_cached("RECOMP_GPU_CULL");
                    cull_on = e && e[0] == '1';
                    cull_flip = env_cached("RECOMP_GPU_CULL_FLIP") != NULL;
                }
                if (cull_on && !flat2d && !lines && s_vp.cull
                        && (s_vp.cull_face == 0x0404 || s_vp.cull_face == 0x0405)) {
                    int ccw = (s_vp.front_face == 0x0901) ^ cull_flip;
                    if (s_gpu11.rs_cull[s_vp.cull_face == 0x0405][ccw])
                        rs = s_gpu11.rs_cull[s_vp.cull_face == 0x0405][ccw];
                }
                if (s_bound.rs != rs) {
                    ID3D11DeviceContext_RSSetState(s_gpu11.ctx, rs);
                    s_bound.rs = rs;
                }
            }
            { uint64_t a = DP_NOW();
              if (vpvs && !gpu_vp_bind(vp_nverts, nv, rw, rh, vpvs))
                  vpvs = NULL, nv = 0;
              if (s_dp_on) s_dp_acc[2] += __rdtsc() - a; }
            if (!vpvs) {
                if (s_bound.il != s_gpu11.layout) {
                    ID3D11DeviceContext_IASetInputLayout(s_gpu11.ctx, s_gpu11.layout);
                    s_bound.il = s_gpu11.layout;
                }
                if (s_bound.vb != s_gpu11.vb || s_bound.vb_stride != stride) {
                    ID3D11DeviceContext_IASetVertexBuffers(s_gpu11.ctx, 0, 1, &s_gpu11.vb, &stride, &off);
                    s_bound.vb = s_gpu11.vb; s_bound.vb_stride = stride;
                }
                if (s_bound.vs != s_gpu11.vs) {
                    ID3D11DeviceContext_VSSetShader(s_gpu11.ctx, s_gpu11.vs, NULL, 0);
                    s_bound.vs = s_gpu11.vs;
                }
            }
            if (s_bound.gs != gs) {
                ID3D11DeviceContext_GSSetShader(s_gpu11.ctx, gs, NULL, 0);
                s_bound.gs = gs;
            }
            if (memcmp(s_bound.srv, srv, 4 * sizeof srv[0]) || memcmp(s_bound.srv + 4, cubes, 4 * sizeof cubes[0])) {
                ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 0, 4, srv);
                ID3D11DeviceContext_PSSetShaderResources(s_gpu11.ctx, 4, 4, cubes);
                memcpy(s_bound.srv, srv, 4 * sizeof srv[0]);
                memcpy(s_bound.srv + 4, cubes, 4 * sizeof cubes[0]);
            }
            if (memcmp(s_bound.smp, smp, sizeof s_bound.smp)) {
                ID3D11DeviceContext_PSSetSamplers(s_gpu11.ctx, 0, 4, smp);
                memcpy(s_bound.smp, smp, sizeof s_bound.smp);
            }
            if (memcmp(&s_bound.vp, &vp, sizeof vp)) {
                ID3D11DeviceContext_RSSetViewports(s_gpu11.ctx, 1, &vp);
                s_bound.vp = vp;
            }
            if (s_bound.nsc != nsc || memcmp(s_bound.sc, sc, nsc * sizeof sc[0])) {
                ID3D11DeviceContext_RSSetScissorRects(s_gpu11.ctx, nsc, sc);
                s_bound.nsc = nsc;
                memcpy(s_bound.sc, sc, nsc * sizeof sc[0]);
            }
            if (s_bound.blend != blend || memcmp(s_bound.bf, bf, sizeof bf)) {
                ID3D11DeviceContext_OMSetBlendState(s_gpu11.ctx, blend, bf, 0xFFFFFFFFu);
                s_bound.blend = blend;
                memcpy(s_bound.bf, bf, sizeof bf);
            }
            if (s_bound.ds != dss || s_bound.sref != s_vp.stencil_ref) {
                ID3D11DeviceContext_OMSetDepthStencilState(s_gpu11.ctx, dss, s_vp.stencil_ref);
                s_bound.ds = dss;
                s_bound.sref = s_vp.stencil_ref;
            }
        }
        if (vpvs)
            ID3D11DeviceContext_DrawIndexed(s_gpu11.ctx, nv, s_vp_start_index, s_vp_base_vertex);
        else
            ID3D11DeviceContext_Draw(s_gpu11.ctx, nv, first_vertex);
        gpu_shadow_box_draw(flat2d,surf);
        if (s_dp_on) s_dp_acc[3] += __rdtsc() - dp_state0;   /* includes upload+bind; subtracted below */
        gpu_debug_drain(flat2d ? "draw2d" : "draw");
        /* RECOMP_GPU_PICK=x,y: in a traced frame (one in 300), read that
         * pixel back after every draw and log each draw that changes it --
         * the full history of one pixel, e.g. a patch of black field. */
        if (env_cached("RECOMP_GPU_PICK") &&
            (s_gpu11.frame % (env_cached("RECOMP_GPU_PICK_EVERY") ? (unsigned)atoi(env_cached("RECOMP_GPU_PICK_EVERY")) + 1 : 300)) == 0) {
            static ID3D11Texture2D *pick;
            static uint32_t last = 0xFFFFFFFFu, last_frame;
            unsigned px = 0, py = 0;
            if (sscanf(env_cached("RECOMP_GPU_PICK"), "%u,%u", &px, &py) == 2 && px < rw && py < rh) {
                D3D11_MAPPED_SUBRESOURCE pm;
                UINT sx = px * surf->scale, sy = py * surf->scale;
                D3D11_BOX box = { sx, sy, 0, sx + 1, sy + 1, 1 };
                if (!pick) {
                    D3D11_TEXTURE2D_DESC td;
                    memset(&td, 0, sizeof td);
                    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Device_CreateTexture2D(s_gpu11.dev, &td, NULL, &pick);
                }
                if (last_frame != s_gpu11.frame) { last = 0xFFFFFFFFu; last_frame = s_gpu11.frame; }
                if (pick) {
                    ID3D11DeviceContext_CopySubresourceRegion(s_gpu11.ctx, (ID3D11Resource *)pick, 0, 0, 0, 0,
                                                              (ID3D11Resource *)surf->tex, 0, &box);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(s_gpu11.ctx, (ID3D11Resource *)pick, 0, D3D11_MAP_READ, 0, &pm))) {
                        uint32_t v = *(const uint32_t *)pm.pData;
                        ID3D11DeviceContext_Unmap(s_gpu11.ctx, (ID3D11Resource *)pick, 0);
                        if (v != last)
                            fprintf(stderr, "  [PICK] f%u (%u,%u) %08X -> %08X by %s tris=%u vpstart=%u prog=%08X blend=%u %04X/%04X z=%04X/%u tex0=%08X fmt=%02X %ux%u c0=%08X/%08X fin=%08X/%08X d0=%.2f,%.2f,%.2f,%.2f\n",
                                    s_gpu11.frame, px, py, last, v, vpvs ? "gpuvp" : (flat2d ? "2d" : "vp"), nv / 3,
                                    s_vp.start, s_comb.stage_prog, s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df,
                                    s_vp.depth_func, s_vp.depth_mask, s_gpu.tex.offset, s_gpu.tex.color,
                                    s_gpu.tex.width, s_gpu.tex.height, s_comb.color_icw[0], s_comb.color_ocw[0],
                                    s_comb.final0, s_comb.final1,
                                    vpvs ? -1.0f : s_gpu11.verts[0].d0[0], vpvs ? -1.0f : s_gpu11.verts[0].d0[1],
                                    vpvs ? -1.0f : s_gpu11.verts[0].d0[2], vpvs ? -1.0f : s_gpu11.verts[0].d0[3]);
                        if (v != last && s_gpu.tex.offset) {
                            const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                            unsigned nzt = 0, b3, bytes = s_gpu.tex.width * s_gpu.tex.height;
                            if (bytes > 262144u) bytes = 262144u;
                            for (b3 = 0; b3 < bytes; b3++) nzt += gm[s_gpu.tex.offset + b3] != 0;
                            if (!vpvs) fprintf(stderr, "  [PICK]    t0 coords v0 %g %g %g %g  v1 %g %g %g %g  pos %g %g %g %g\n", s_gpu11.verts[0].t[0][0], s_gpu11.verts[0].t[0][1], s_gpu11.verts[0].t[0][2], s_gpu11.verts[0].t[0][3], s_gpu11.verts[1].t[0][0], s_gpu11.verts[1].t[0][1], s_gpu11.verts[1].t[0][2], s_gpu11.verts[1].t[0][3], s_gpu11.verts[0].pos[0], s_gpu11.verts[0].pos[1], s_gpu11.verts[0].pos[2], s_gpu11.verts[0].pos[3]);
                            fprintf(stderr, "  [PICK]    tex %08X: %u/%u nonzero, idx %02X %02X %02X %02X; pal %08X: %08X %08X %08X %08X\n",
                                    s_gpu.tex.offset, nzt, bytes, gm[s_gpu.tex.offset], gm[s_gpu.tex.offset + 1],
                                    gm[s_gpu.tex.offset + 1000], gm[s_gpu.tex.offset + 5000], s_gpu.tex.palette,
                                    s_gpu.tex.palette ? *(const uint32_t *)(gm + s_gpu.tex.palette) : 0,
                                    s_gpu.tex.palette ? *(const uint32_t *)(gm + s_gpu.tex.palette + 4) : 0,
                                    s_gpu.tex.palette ? *(const uint32_t *)(gm + s_gpu.tex.palette + 0x200) : 0,
                                    s_gpu.tex.palette ? *(const uint32_t *)(gm + s_gpu.tex.palette + 0x3FC) : 0);
                        }
                        if (v != last)
                            fprintf(stderr, "  [PICK]    t=%lu stages en %d%d%d%d | t1 %08X fmt %02X %ux%u | t2 %08X fmt %02X %ux%u | t3 %08X fmt %02X %ux%u | a0 %08X/%08X c1 %08X/%08X\n",
                                    GetTickCount(), s_gpu.tex.enabled, s_stage[1].enabled, s_stage[2].enabled, s_stage[3].enabled,
                                    s_stage[1].offset, s_stage[1].color, s_stage[1].width, s_stage[1].height,
                                    s_stage[2].offset, s_stage[2].color, s_stage[2].width, s_stage[2].height,
                                    s_stage[3].offset, s_stage[3].color, s_stage[3].width, s_stage[3].height,
                                    s_comb.alpha_icw[0], s_comb.alpha_ocw[0], s_comb.color_icw[1], s_comb.color_ocw[1]);
                        if (v != last)
                            fprintf(stderr, "  [PICK]    surf %08X %ux%u ctl %08X shaderprog %08X | c2 %08X/%08X c3 %08X/%08X a1 %08X/%08X k0 %08X k1 %08X\n",
                                    surf->guest, rw, rh, s_comb.control, s_comb.stage_prog,
                                    s_comb.color_icw[2], s_comb.color_ocw[2], s_comb.color_icw[3], s_comb.color_ocw[3],
                                    s_comb.alpha_icw[1], s_comb.alpha_ocw[1], s_comb.factor0[0], s_comb.factor1[0]);
                        last = v;
                    }
                }
            }
        }
    }
    for (st = 0; st < 4; st++) {
        ID3D11ShaderResourceView_Release(srv[st]);
        ID3D11ShaderResourceView_Release(cubes[st]);
    }
    surf->dirty = 1;
    surf->gen++;
    s_gpu11.draws++;
    s_gpu11.tris += nv / 3;
    s_vp.drawn += nv / 3;
    s_gpu.tris_drawn += nv / 3;
    s_gpu.drawn_offset = s_gpu.color_offset;
    return 1;
}

/* CLEAR_SURFACE on the GPU. Returns 0 to fall back to software. */
static int gpu_clear_unlocked(uint32_t param)
{
    int si;
    GpuSurface *s;
    if (!gpu_enabled() || !gpu_init())
        return 0;
    si = gpu_bind_surface();
    if (si < 0)
        return 0;
    s = &s_gpu11.surf[si];
    if (env_cached("RECOMP_GPU_TRACE") && (s_gpu11.frame % 300) == 0)
        fprintf(stderr, "  [GPUTRACE] f%u clear %02X surf=%08X color=%08X zclear=%08X rect=%08X/%08X\n",
                s_gpu11.frame, param, s->guest, s_gpu.clear_color, s_vp.zclear, g_clear_rx, g_clear_ry);
    if (param & 0xF0u) {
        float col[4];
        uint32_t x0 = g_clear_rx & 0xFFF, x1 = ((g_clear_rx >> 16) & 0xFFF) + 1;
        uint32_t y0 = g_clear_ry & 0xFFF, y1 = ((g_clear_ry >> 16) & 0xFFF) + 1;
        comb_unpack(s_gpu.clear_color, col);
        if ((param & 0xF0u) == 0xF0u && x0 == 0 && y0 == 0 && x1 >= s->w && y1 >= s->h) {
            ID3D11DeviceContext_ClearRenderTargetView(s_gpu11.ctx, s->rtv, col);
        } else {
            /* Partial rectangle or channel subset: a solid quad with the
             * channel write mask (it went through guest RAM before, a
             * readback and re-upload per clear, and lost resolution when
             * scaled). Param bits 4-7 are R G B A. */
            UINT mask = ((param & 0x10u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                      | ((param & 0x20u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                      | ((param & 0x40u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                      | ((param & 0x80u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
            if (x1 > s->w) x1 = s->w;
            if (y1 > s->h) y1 = s->h;
            if (x1 > x0 && y1 > y0) {
                uint32_t fx0 = x0 + s->ox, fx1 = x1 + s->ox;
                if (s->ox && x0 == 0 && x1 >= s->w) { fx0 = 0; fx1 = s->w + 2 * s->ox; }   /* full width: the extra columns too */
                gpu_fill(s->rtv, fx0 * s->scale, y0 * s->scale, (fx1 - fx0) * s->scale, (y1 - y0) * s->scale, col, mask);
            }
        }
    }
    if (param & 3u) {
        UINT flags = ((param & 1u) ? D3D11_CLEAR_DEPTH : 0) | ((param & 2u) ? D3D11_CLEAR_STENCIL : 0);
        /* xemu pgraph_get_clear_depth: the clear value in the buffer's own
         * format -- a 24-bit float is the top 24 bits of an IEEE single. */
        uint32_t zc24 = s_vp.zclear >> 8;
        float z;
        if ((s_vp.control0 >> 12) & 1) {
            uint32_t bits = zc24 << 7;
            float fz;
            memcpy(&fz, &bits, 4);
            z = zc24 ? fz / gpu_zmax() : 0.0f;
        } else {
            z = (float)zc24 / 16777215.0f;
        }
        if (z < 0.0f) z = 0.0f;
        if (z > 1.0f) z = 1.0f;
        ID3D11DeviceContext_ClearDepthStencilView(s_gpu11.ctx, s->dsv, flags, z, (UINT8)(s_vp.zclear & 0xFF));
    }
    s->dirty = 1;
    s->gen++;
    return 1;
}

/* A frame is finished: bring the surface back to guest RAM for the window. */
#include "nv2a_gpu_present.inc.c"

static void gpu_flip_unlocked(void)
{
    int i;
    if (!s_gpu11.ok)
        return;
    gpu_capture_finish();
    /* Present straight from the render target (nv2a_gpu_present.inc.c).
     * Then nothing needs the frame in guest RAM unless a screenshot or dump
     * reads it there; render-to-texture and the software paths fetch what
     * they read on demand (gpu_flush_range, gpu_cpu_access). */
    pr_hook_keys();
    if (pr_present(dma_resolve(s_gpu.color_offset))
            && !env_cached("RECOMP_SHOT_PREFIX") && !env_cached("RECOMP_FB_DUMP")) {
        /* presented */
    } else
    for (i = 0; i < GPU_MAX_SURFACES; i++)
        if (s_gpu11.surf[i].tex && s_gpu11.surf[i].dirty)
            gpu_readback(&s_gpu11.surf[i]);
    if (env_cached("RECOMP_GPU_TRACE") && (s_gpu11.frame % 300) == 0)
        fprintf(stderr, "  [GPUTRACE] f%u flip shows %08X\n", s_gpu11.frame, dma_resolve(s_gpu.color_offset));
    s_gpu11.frame++;
    if ((s_gpu11.frame % 600) == 0)
        fprintf(stderr, "  [GPU11] frames %u draws %u tris %u texture uploads %u readbacks %u\n",
                s_gpu11.frame, s_gpu11.draws, s_gpu11.tris, s_gpu11.tex_uploads, s_gpu11.readbacks);
}

/* ---- serialisation ------------------------------------------------------
 *
 * The D3D11 immediate context is not thread-safe, and the executor can run on
 * the game thread (synchronous KickOff) and on the fallback poller. Two
 * threads in it at once crashed NVIDIA's driver at kickoff (2026-09-25,
 * nvwgf2umx.dll reading 0x10). Every entry point takes this lock. */
static CRITICAL_SECTION s_gpu_lock;
static INIT_ONCE s_gpu_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK gpu_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_gpu_lock);
    return TRUE;
}

static void gpu_lock(void)
{
    InitOnceExecuteOnce(&s_gpu_lock_once, gpu_lock_init, NULL, NULL);
    EnterCriticalSection(&s_gpu_lock);
}

static void gpu_cpu_access(void)
{
    gpu_lock(); gpu_cpu_access_unlocked(); LeaveCriticalSection(&s_gpu_lock);
}

static int gpu_draw_batch(int flat2d)
{
    int r;
    gpu_lock(); r = gpu_draw_batch_unlocked(flat2d); LeaveCriticalSection(&s_gpu_lock);
    return r;
}

static int gpu_clear(uint32_t param)
{
    int r;
    gpu_lock(); r = gpu_clear_unlocked(param); LeaveCriticalSection(&s_gpu_lock);
    return r;
}

static void gpu_flip(void)
{
    gpu_lock(); gpu_flip_unlocked(); LeaveCriticalSection(&s_gpu_lock);
}
#else
#include "nv2a_gpu_sdl3.inc.c"
#endif

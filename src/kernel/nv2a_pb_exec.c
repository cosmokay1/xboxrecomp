/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, but only the part that can be drawn honestly:
 * batches whose attribute 0 is already in screen space, flat-shaded, straight
 * into the same guest framebuffer the clear writes. Titles draw their UI, HUD
 * and 2D overlays that way, so it is the first geometry to appear. Batches that
 * need a vertex program executed are counted and skipped rather than drawn
 * somewhere wrong -- see raster_batch(). Texturing, depth and vertex programs
 * are still a renderer, not a command decoder; the upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include "platform/xbox_winnt.h"
#if defined(__linux__)
#include "platform/guest_memory.h"
#endif
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "../d3d/d3d8_swizzle.h"

/* Environment switches are read on hot paths here -- per GPU method, per
 * vertex, even per pixel. CRT getenv takes the environment lock the title's
 * own threads also contend for, which slowed the whole game down enough to
 * change menu timing. The environment does not change while running, so
 * every lookup is cached after its first read. */
static const char *env_cached(const char *name)
{
    /* Hashed on the name's address (call sites pass literals), so a lookup
     * is one or two probes. The old linear scan over every debug switch ran
     * dozens of times per draw: ~3% of the game thread (2026-10-02). A miss
     * takes the lock, reads the environment once and publishes the value
     * before the name, so readers never see a name without its value. */
    static struct { PVOID volatile name; const char *value; } tab[1024];
    static SRWLOCK lock = SRWLOCK_INIT;
    uintptr_t k = (uintptr_t)name;
    unsigned h = (unsigned)(((k >> 3) * 2654435761u) >> 8) & 1023u, i;
    for (i = 0; i < 1024; i++, h = (h + 1) & 1023u) {
        const char *n = (const char *)InterlockedCompareExchangePointer(&tab[h].name, NULL, NULL);
        if (n == name) return tab[h].value;
        if (!n) break;
    }
    AcquireSRWLockExclusive(&lock);
    for (i = 0, h = (unsigned)(((k >> 3) * 2654435761u) >> 8) & 1023u; i < 1024; i++, h = (h + 1) & 1023u) {
        if (tab[h].name == (PVOID)name) break;
        if (!tab[h].name) {
            tab[h].value = getenv(name);
            /* Publish the value through the same atomic pointer readers use. */
            InterlockedCompareExchangePointer(&tab[h].name, (PVOID)name, NULL);
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return i < 1024 ? tab[h].value : getenv(name);
}
#define getenv(name) env_cached(name)


extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern void xbox_FramebufferWindowSize(uint32_t w, uint32_t h);
extern uint32_t g_nv2a_pb_calls;   /* nv2a_pb_scan.c */
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern uint32_t xbox_ContiguousAllocatedBytes(void);

    /* Did this runtime hand the offset out as contiguous memory? Then the
     * bytes live in the window, and that is not a guess: the arena is a bump
     * allocator from XBOX_CONTIG_BASE, so everything below its high-water
     * mark is memory some MmAllocateContiguousMemory call returned. The
     * title's own writes go through the window, so the executor's must too.
     *
     * Checking this BEFORE the image test is the whole point. The image test
     * only catches an offset that would land on the title's code, and whether
     * it does is an accident of where the image happens to end: Half-Life 2's
     * colour surface is physical 0x00A6C000, which clears the image by 700 KB.
     * So it looked like an ordinary VA, and the executor cleared 1.2 MB of
     * black straight through the guest heap -- which faulted the title three
     * frames later on a pointer that had been overwritten, while the real
     * framebuffer at 0x80A6C000 stayed untouched and the screen stayed black. */
    if (offset & XBOX_PHYS_LOW_TAG)     /* low memory, see MmGetPhysicalAddress */
        return offset & 0x03FFFFFFu;
    if (offset < xbox_ContiguousAllocatedBytes())
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    if ((uint64_t)offset < XBOX_CONTIG_SIZE)
        return XBOX_CONTIG_BASE + offset;
    return offset;                         /* nothing better to offer */
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

/* NV097 methods this executor acts on. */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_WINDOW_CLIP_TYPE        0x02B4
#define NV097_SET_WINDOW_CLIP_HORIZONTAL  0x02C0
#define NV097_SET_WINDOW_CLIP_VERTICAL    0x02E0
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_SURFACE_ZETA_OFFSET     0x0214
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_SET_ALPHA_TEST_ENABLE       0x0300
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_ALPHA_FUNC              0x033C
#define NV097_SET_ALPHA_REF               0x0340
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV097_SET_BLEND_COLOR             0x034C
#define NV097_SET_BLEND_EQUATION          0x0350
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_CONTROL0        0x1B0C   /* +i*0x40; bit 30 = stage enabled */
#define NV097_SET_TEXTURE_PALETTE         0x1B20   /* +i*0x40 */
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096

/* Indices that did not fit a batch (silently lost before 2026-10-02). */
static void idx_overflow_note(const char *how, uint32_t dropped)
{
    static unsigned long total, events;
    static DWORD last;
    total += dropped; events++;
    if (GetTickCount() - last > 2000) {
        fprintf(stderr, "  [IDXCAP] %lu batches overflowed, %lu indices dropped (last: %s)""\n", events, total, how);
        last = GetTickCount();
    }
}

/* Dwords of INLINE_ARRAY per batch. Was 4096 (an emulator cap, not the
 * NV2A's): longer inline draws lost their tail. xemu buffers 0x1FFFF. */
#define NV_MAX_INLINE   0x20000

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of A8R8G8B8 CLUT */
    int      enabled;                   /* CONTROL0 bit 30                */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* DRAW_ARRAYS method words are individual draw calls unless adjacent
     * ranges can be joined.  Keep that provenance so a non-contiguous range
     * can be flushed instead of turning two strips into one (which creates a
     * long bridge triangle across the field). */
    int        draw_arrays_active;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Current values of the immediate-mode attributes, and how many complete
     * vertices they have produced in this batch. */
    float      imm_pos[4];
    uint32_t   imm_diffuse;
    float      imm_tex[2];
    uint32_t   imm_count;
    int        inline_active;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    uint32_t zeta_offset, zeta_pitch;    /* independent NV097 depth/stencil surface */
    /* The surface the last batch actually drew into. A double-buffered title
     * has already pointed color_offset at the next buffer and cleared it by
     * the time the flip arrives, so dumping the current one dumps the frame
     * that has not been drawn yet -- which is how a correctly rendered
     * sequence came out as 12 black BMPs. */
    uint32_t drawn_offset;
    uint64_t pixels;
    uint32_t pixel_max;   /* brightest value any pixel write carried */
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t window_clip_type;
    uint32_t window_clip_x[8], window_clip_y[8];
    uint32_t clear_color;
    /* Fixed-function output merge: alpha test and blend, GL enums as the
     * NV2A takes them. Defaults are GL's (blend ONE/ZERO/ADD, test ALWAYS). */
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t blend, blend_sf, blend_df, blend_eq, blend_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    /* Why a batch came out flat. "Untextured" has two causes that look
     * identical on screen and want opposite fixes: the batch carried no
     * texture coordinates, or it did and the stage was not usable. */
    uint32_t batches_textured, batches_no_uv, batches_no_tex;
    Texture  tex;
} s_gpu;

/* What each vertex attribute reads when the batch does not supply it as an
 * array: the last SET_VERTEX_DATA* value, or the last element of the last
 * array draw that did supply it (xemu pgraph: inline_value,
 * pgraph_update_inline_value). A model whose colour is one value for the
 * whole mesh sends it this way; reading a fixed default instead gave NFL 2K5's
 * 3D characters -- Create Player, the pregame studio -- alpha 0, so they drew
 * fully transparent (2026-09-24). */
static float s_attr_inline[NV_VERTEX_ATTRS][4] = {
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
};

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    int was_valid = s_gpu.tex.valid;
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
    /* Opt-in: identify whether any bound texture matches the known
     * legal-screen asset (512x512 P8/indexed, archive record 0) without
     * changing behavior. See docs/GEMINI-HANDOFF.md for the verified
     * decode of that asset from the native rebuild track. */
    if (!was_valid && s_gpu.tex.valid && getenv("RECOMP_TEX_IDENTIFY")) {
        fprintf(stderr,
            "[TEX] bound: offset=0x%08X width=%u height=%u format=0x%02X pitch=%u%s\n",
            s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
            s_gpu.tex.color, s_gpu.tex.pitch,
            (s_gpu.tex.width == 512 && s_gpu.tex.height == 512) ?
                "  <-- matches legal-screen texture dimensions" : "");
    }
}

/* Every distinct texture a batch was drawn with, and how many batches used it.
 *
 * The per-draw verbose print shows the first few draws of the first frame,
 * which is enough to see that texturing works at all and not enough to answer
 * "is a font page ever bound". This is the same shape as the unhandled-method
 * table below it: a small set, ranked, printed with the rest of the report. */
#define PB_EXEC_MAX_TEXTURES 64
typedef struct {
    uint32_t offset, color, width, height, batches;
} PbTexUse;
static PbTexUse s_tex_use[PB_EXEC_MAX_TEXTURES];
static int s_tex_use_count;

/* Defined below, next to the sampler it goes through. */
static void dump_texture_bmp(uint32_t seq);

static void note_texture_use(void)
{
    int i;

    if (!s_gpu.tex.valid)
        return;
    for (i = 0; i < s_tex_use_count; i++) {
        if (s_tex_use[i].offset == s_gpu.tex.offset
         && s_tex_use[i].color  == s_gpu.tex.color) {
            s_tex_use[i].batches++;
            return;
        }
    }
    if (s_tex_use_count < PB_EXEC_MAX_TEXTURES) {
        s_tex_use[s_tex_use_count].offset  = s_gpu.tex.offset;
        s_tex_use[s_tex_use_count].color   = s_gpu.tex.color;
        s_tex_use[s_tex_use_count].width   = s_gpu.tex.width;
        s_tex_use[s_tex_use_count].height  = s_gpu.tex.height;
        s_tex_use[s_tex_use_count].batches = 1;
        s_tex_use_count++;
        dump_texture_bmp((uint32_t)s_tex_use_count - 1);
    }
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            s_unhandled[i].last_param = param;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        /* First sight of each unhandled method, with the draw it came before:
         * one line per distinct method, so a method the title waits on (a
         * report, a semaphore) is visible by when it first appears. */
        fprintf(stderr, "  [GPU] first unhandled method 0x%04X param 0x%08X (draw %u)\n",
                method, param, s_gpu.draws);
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;
    size_t bytes;
#if defined(__linux__)
    union { float floats[4]; uint8_t bytes[16]; } sampled;
#endif

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size) {
        ptrdiff_t k = a - s_gpu.attr;
        if (k >= 0 && k < NV_VERTEX_ATTRS)
            memcpy(out, s_attr_inline[k], sizeof s_attr_inline[k]);
        return 0;
    }
    switch (a->type) {
    case 0: case 6: bytes = 4; break;
    case 2: bytes = (a->size < 4 ? a->size : 4) * 4; break;
    case 1: case 5: bytes = (a->size < 4 ? a->size : 4) * 2; break;
    case 4: bytes = a->size < 4 ? a->size : 4; break;
    default: return 0;
    }
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        uint64_t at = (uint64_t)a->offset + (uint64_t)index * a->stride;
        uint64_t available = (uint64_t)s_gpu.inline_count * 4;
        if (at > available || bytes > available - at)
            return 0;
        p = (const uint8_t *)s_gpu.inline_buf + at;
    } else {
        if (!a->offset)
            return 0;
#if defined(__linux__)
        uint64_t address = (uint64_t)a->offset + (uint64_t)index * a->stride;
        if (!xbox_guest_memory_read((uintptr_t)mem,address,sampled.bytes,bytes))
            return 0;
        p = sampled.bytes;
#else
        p = mem + a->offset + (size_t)index * a->stride;
#endif
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        /* A zero-stride array supplies a constant inline value, decoded in
         * byte order just like SET_VERTEX_DATA4UB (xemu vertex.c and
         * gl/vertex.c). BGRA conversion applies only to array fetches. */
        out[0] = (float)p[a->stride ? 2 : 0] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[a->stride ? 0 : 2] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* Signed shorts. NFL 2K5's shaders take their matrix-palette index from
     * attribute 1 as raw shorts (type 5) through ARL; read as 0, every vertex
     * of a batch transformed by an unset matrix and collapsed to one point. */
    case 1:                                  /* short, normalised (S1) */
        for (i = 0; i < a->size && i < 4; i++) {
            float v = (float)((const int16_t *)p)[i] / 32767.0f;
            out[i] = v < -1.0f ? -1.0f : v;
        }
        return 1;
    case 5:                                  /* short, as is (S32K) */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 signed normal (CMP) */
        uint32_t w = *(const uint32_t *)p;
        int32_t x = (int32_t)(w << 21) >> 21;
        int32_t y = (int32_t)((w >> 11) << 21) >> 21;
        int32_t z = (int32_t)(w >> 22) << 22 >> 22;
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        if (!a->stride) {
            /* Constant arrays pass through pgraph_update_inline_value,
             * which clamps the negative endpoint of signed normal formats. */
            if (out[0] < -1.0f) out[0] = -1.0f;
            if (out[1] < -1.0f) out[1] = -1.0f;
            if (out[2] < -1.0f) out[2] = -1.0f;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static int pb_diagnostic_read(uint64_t address, void *output, size_t bytes)
{
#if defined(__linux__)
    return xbox_guest_memory_read((uintptr_t)xbox_GetMemoryOffset(), address, output, bytes);
#else
    memcpy(output, (const uint8_t *)xbox_GetMemoryOffset() + address, bytes);
    return 1;
#endif
}

static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;
#if defined(__linux__)
    uint8_t *staged_row;
    if ((uint64_t)w * 3 > UINT32_MAX - 3 ||
        (uint64_t)54 + (((uint64_t)w * 3 + 3) & ~UINT64_C(3)) * h > UINT32_MAX)
        return;
#endif

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;
#if defined(__linux__)
    staged_row = (uint8_t *)malloc((size_t)w * bpp);
    if (!staged_row) { fclose(f); remove(path); return; }
#endif

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
#if defined(__linux__)
        uint64_t row_index=(uint64_t)s_gpu.clip_y+y;
        uint64_t row_offset=(row_index<=UINT32_MAX ? row_index : 0)*s_gpu.pitch;
        uint64_t address = dma_resolve(s_gpu.drawn_offset ? s_gpu.drawn_offset : s_gpu.color_offset)
            + (row_offset<=UINT32_MAX ? row_offset : 0) + (uint64_t)s_gpu.clip_x * bpp;
        if (row_index>UINT32_MAX || row_offset>UINT32_MAX ||
                !pb_diagnostic_read(address, staged_row, (size_t)w * bpp)) {
            free(staged_row); fclose(f); remove(path);
            fprintf(stderr,"  [GPU] framebuffer dump unavailable: unreadable row %u\n",y);
            return;
        }
        const uint8_t *row = staged_row;
#else
        const uint8_t *row = mem + dma_resolve(s_gpu.drawn_offset
                                                ? s_gpu.drawn_offset
                                                : s_gpu.color_offset)
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
#endif
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[
#if defined(__linux__)
                    x
#else
                    s_gpu.clip_x + x
#endif
                ];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[
#if defined(__linux__)
                    x
#else
                    s_gpu.clip_x + x
#endif
                ];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
#if defined(__linux__)
    free(staged_row);
#endif
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2]);

/* Early copies of output-merge state the later vertex-program block owns
 * (s_vp), for the clear and pixel-write paths defined before it. */
static uint32_t g_clear_rx = 0x0FFF0000u, g_clear_ry = 0x0FFF0000u;
static uint32_t g_color_mask = 0x01010101u;
static uint8_t *s_surface;
static int surface_stage_begin(const uint8_t *mem);
static int surface_stage_flush(const uint8_t *mem);
static void surface_stage_reset(void);
static void surface_stage_dirty(uint32_t y, uint32_t first, uint32_t end);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    {
        uint32_t base = dma_resolve(s_gpu.color_offset);
        if (surface_write_refused(base,
                                  (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                                  "clear"))
            return;
        s_gpu.color_base = base;
    }
    if (!surface_stage_begin(mem)) return;

    {
        /* Only inside SET_CLEAR_RECT (inclusive) and only the channels the
         * clear names (bits 4..7 = R,G,B,A). Clearing the whole surface wiped
         * layers a title had already drawn when it cleared one region. */
        uint32_t rx0 = g_clear_rx & 0xFFF, rx1 = ((g_clear_rx >> 16) & 0xFFF) + 1;
        uint32_t ry0 = g_clear_ry & 0xFFF, ry1 = ((g_clear_ry >> 16) & 0xFFF) + 1;
        uint32_t keep = 0;
        if (!(param & 0x10)) keep |= 0x00FF0000u;
        if (!(param & 0x20)) keep |= 0x0000FF00u;
        if (!(param & 0x40)) keep |= 0x000000FFu;
        if (!(param & 0x80)) keep |= 0xFF000000u;
        if (rx0 < s_gpu.clip_x) rx0 = s_gpu.clip_x;
        if (ry0 < s_gpu.clip_y) ry0 = s_gpu.clip_y;
        if (rx1 > s_gpu.clip_x + s_gpu.clip_w) rx1 = s_gpu.clip_x + s_gpu.clip_w;
        if (ry1 > s_gpu.clip_y + s_gpu.clip_h) ry1 = s_gpu.clip_y + s_gpu.clip_h;
        if (bpp == 4) {
            for (y = ry0; y < ry1; y++) {
                uint32_t *p = (uint32_t *)(s_surface + (size_t)y * s_gpu.pitch);
                for (x = rx0; x < rx1; x++)
                    p[x] = (p[x] & keep) | (s_gpu.clear_color & ~keep);
                surface_stage_dirty(y,rx0,rx1);
            }
            goto cleared;
        }
    }
    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = s_surface + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
        surface_stage_dirty(s_gpu.clip_y+y,s_gpu.clip_x,s_gpu.clip_x+s_gpu.clip_w);
    }
cleared:
    if (!surface_stage_flush(mem)) { surface_stage_reset(); return; }
    surface_stage_reset();
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL);  /* magenta: never a clear colour */
            surface_stage_flush(mem);
            surface_stage_reset();
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    /* The window has to read where the pixels actually are, which is the
     * resolved address rather than the DMA-object offset. */
    /* Until the title flips, anyway: from the first FLIP_STALL on, the window
     * follows finished frames (see there), and following clears as well would
     * keep showing a buffer mid-redraw -- flicker between the clear colour
     * and the picture. */
    if (!s_gpu.flips)
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

static Texture s_stage[4];
#if defined(__linux__)
typedef struct {
    uint8_t *pixels;
    uint32_t palette[256];
    int ready;
} TextureSnapshot;
static TextureSnapshot s_texture_snapshot[4];
static int s_sampler_stage;

static void snapshot_texture(int stage, const Texture *texture)
{
    TextureSnapshot *snapshot = &s_texture_snapshot[stage];
    uint64_t bytes, bpp, blocks;
    uint32_t format = linear_twin(texture->color);
    free(snapshot->pixels); snapshot->pixels = NULL; snapshot->ready = 0;
    if (!texture->valid || !texture->width || !texture->height ||
        texture->width > 65535 || texture->height > 65535) return;
    blocks = d3d8_format_dxt_block_bytes(texture->color);
    if (blocks) {
        bytes = ((uint64_t)texture->width + 3) / 4 *
                (((uint64_t)texture->height + 3) / 4) * blocks;
    } else if (texture->color == 0x0B) {
        bytes = (uint64_t)swizzle_offset(texture->width - 1, texture->height - 1,
                                       texture->width, texture->height) + 1;
    } else {
        switch (format) {
        case 0x12: case 0x1E: case 0x3F: case 0x40: case 0x41: bpp = 4; break;
        case 0x10: case 0x1C: case 0x11: case 0x1D: bpp = 2; break;
        case 0x13: case 0x1F: bpp = 1; break;
        default: return;
        }
        if (d3d8_format_is_swizzled(texture->color)) {
            bytes = ((uint64_t)swizzle_offset(texture->width - 1, texture->height - 1,
                                            texture->width, texture->height) + 1) * bpp;
        } else {
            if ((uint64_t)texture->pitch < (uint64_t)texture->width * bpp) return;
            bytes = ((uint64_t)texture->height - 1) * texture->pitch + texture->width * bpp;
        }
    }
    if (!bytes || bytes > SIZE_MAX || bytes > UINT64_C(0x100000000) - texture->offset) return;
    snapshot->pixels = malloc((size_t)bytes);
    if (!snapshot->pixels) return;
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    if (!xbox_guest_memory_read(base,texture->offset,snapshot->pixels,(size_t)bytes) ||
        (texture->color == 0x0B && (!texture->palette ||
         !xbox_guest_memory_read(base,texture->palette,snapshot->palette,sizeof snapshot->palette)))) {
        free(snapshot->pixels); snapshot->pixels = NULL; return;
    }
    snapshot->ready = 1;
}

static int snapshot_textures(void)
{
    int valid = 1;
    snapshot_texture(0,&s_gpu.tex);
    for (int stage = 1; stage < 4; ++stage) snapshot_texture(stage,&s_stage[stage]);
    if (s_gpu.tex.enabled && s_gpu.tex.valid && !s_texture_snapshot[0].ready) valid = 0;
    for (int stage = 1; stage < 4; ++stage)
        if (s_stage[stage].enabled && s_stage[stage].valid && !s_texture_snapshot[stage].ready) valid = 0;
    s_sampler_stage = 0;
    return valid;
}
#endif

static int sample_texture(uint32_t u, uint32_t v, uint32_t *argb)
{
#if !defined(__linux__)
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
#endif
    const uint8_t *p;
    uint32_t fmt;

    if (!s_gpu.tex.valid)
        return 0;
#if defined(__linux__)
    if (!s_texture_snapshot[s_sampler_stage].ready) return 0;
    const uint8_t *image = s_texture_snapshot[s_sampler_stage].pixels;
#else
    const uint8_t *image = mem + s_gpu.tex.offset;
#endif
    u = wrap_coord(u, s_gpu.tex.width,  s_gpu.tex.addr_u);
    v = wrap_coord(v, s_gpu.tex.height, s_gpu.tex.addr_v);

    fmt = s_gpu.tex.color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(image, fmt, u, v,
                                     s_gpu.tex.width, argb);
    if (fmt == 0x0B) {
        /* SZ_I8_A8R8G8B8: swizzled 8-bit indices into a 256-entry A8R8G8B8
         * palette (SET_TEXTURE_PALETTE). NFL 2K5's legal screen and intro
         * movie frames are this format. */
        uint8_t idx;
        if (!s_gpu.tex.palette)
            return 0;
        idx = image[swizzle_offset(u, v, s_gpu.tex.width, s_gpu.tex.height)];
#if defined(__linux__)
        *argb = s_texture_snapshot[s_sampler_stage].palette[idx];
#else
        *argb = *(const uint32_t *)(mem + s_gpu.tex.palette + (size_t)idx * 4u);
#endif
        return 1;
    }
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = image;
        u = swizzle_offset(u, v, s_gpu.tex.width, s_gpu.tex.height);
    } else {
        p = image + (size_t)v * s_gpu.tex.pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    default:
        return 0;
    }
}

/* Write a bound texture out as a BMP, through the sampler rather than around it.
 *
 * "Which texture is this" is not answerable from an address and a format, and
 * it is the question behind most of the ones that matter -- is that a font
 * page or an icon atlas, did the swizzle decode, is the alpha inverted. Going
 * through sample_texture means the file shows exactly what the rasteriser
 * sees, so a decode bug appears here rather than only as a wrong-looking
 * triangle.
 *
 * ponytail: RGB only, alpha dropped. A glyph page is alpha and would come out
 * black, so alpha is composited onto mid-grey to stay legible; that is a
 * viewing choice, not a decode. One file per distinct texture, first use only.
 */
static void dump_texture_bmp(uint32_t seq)
{
    const char *prefix = getenv("RECOMP_TEX_DUMP");
    uint32_t w = s_gpu.tex.width, h = s_gpu.tex.height, x, y;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    if (!prefix || !w || !h || w > 4096 || h > 4096)
        return;
#if defined(__linux__)
    snapshot_texture(0,&s_gpu.tex);
    s_sampler_stage = 0;
    if (!s_texture_snapshot[0].ready) return;
#endif
    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%02u_%08X_fmt%02X_pal%08X_rp%08X.bmp",
             prefix, seq, s_gpu.tex.offset, s_gpu.tex.color, s_gpu.tex.palette,
             s_tex_reg[(0x1B20 - NV_TEX_FIRST) / 4]);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t argb = 0, a;
            uint8_t px[3];
            if (!sample_texture(x, h - 1 - y, &argb))
                argb = 0;
            a = (argb >> 24) & 0xFFu;
            /* over mid-grey, so an alpha-only page is visible either way */
            px[0] = (uint8_t)(((argb & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[1] = (uint8_t)((((argb >> 8) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            px[2] = (uint8_t)((((argb >> 16) & 0xFFu) * a + 128u * (255u - a)) / 255u);
            fwrite(px, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    fprintf(stderr, "  [TEXDUMP] %s (%ux%u fmt 0x%02X)\n",
            path, w, h, s_gpu.tex.color);
    fflush(stderr);
}

/* The surface, resolved once per batch.
 *
 * dma_resolve consults the contiguous arena's high-water mark and
 * surface_hits_image walks the image range; both were being done per pixel
 * -- dma_resolve twice -- which cost more than the rasterisation they
 * guarded. Neither answer can change inside a batch, because the colour
 * offset arrives as a method and a method cannot arrive mid-triangle.
 *
 * This is not a micro-optimisation for its own sake: the loader's video
 * paces on frames actually presented, so the rasteriser's throughput is the
 * playback rate. */
#if defined(__linux__)
static uint32_t *s_surface_first, *s_surface_end;
static uint32_t s_surface_base, s_surface_rows, s_surface_pitch, s_surface_bpp;
#endif

static void surface_stage_reset(void)
{
#if defined(__linux__)
    free(s_surface); free(s_surface_first); free(s_surface_end);
    s_surface_first=s_surface_end=NULL;
    s_surface_rows=0;
#endif
    s_surface=NULL;
}

static int surface_stage_begin(const uint8_t *mem)
{
    uint32_t base=dma_resolve(s_gpu.color_offset);
#if defined(__linux__)
    uint64_t right=(uint64_t)s_gpu.clip_x+s_gpu.clip_w;
    uint64_t bottom=(uint64_t)s_gpu.clip_y+s_gpu.clip_h;
    uint32_t bpp=surface_bpp();
    uint64_t bytes=bottom*s_gpu.pitch;
    uint32_t y;
    /* A bounded host allocation policy, not a guest hardware capacity claim. */
    if (!s_gpu.clip_w || !s_gpu.clip_h || (bpp!=2 && bpp!=4) ||
        right>65535 || bottom>65535 || right*bpp>s_gpu.pitch ||
        bytes>64u*1024u*1024u || (uint64_t)base+bytes>UINT64_C(0x100000000) ||
        surface_hits_image(base,(uint32_t)bytes)) return 0;
    if (s_surface) return base==s_surface_base && bottom==s_surface_rows &&
        s_gpu.pitch==s_surface_pitch && bpp==s_surface_bpp;
    s_surface=(uint8_t *)calloc(1,(size_t)bytes);
    s_surface_first=(uint32_t *)malloc((size_t)bottom*sizeof(uint32_t));
    s_surface_end=(uint32_t *)calloc((size_t)bottom,sizeof(uint32_t));
    if (!s_surface || !s_surface_first || !s_surface_end) { surface_stage_reset(); return 0; }
    s_surface_base=base; s_surface_rows=(uint32_t)bottom;
    s_surface_pitch=s_gpu.pitch; s_surface_bpp=bpp;
    for (y=0;y<s_surface_rows;y++) s_surface_first[y]=UINT32_MAX;
    for (y=s_gpu.clip_y;y<s_surface_rows;y++) {
        size_t at=(size_t)y*s_gpu.pitch+(size_t)s_gpu.clip_x*bpp;
        if (!xbox_guest_memory_read((uintptr_t)mem,(uint64_t)base+at,s_surface+at,
                                   (size_t)s_gpu.clip_w*bpp)) {
            surface_stage_reset(); return 0;
        }
    }
#else
    if (surface_hits_image(base,(s_gpu.clip_y+s_gpu.clip_h)*s_gpu.pitch)) return 0;
    s_surface=(uint8_t *)mem+base;
#endif
    return 1;
}

static void surface_stage_dirty(uint32_t y, uint32_t first, uint32_t end)
{
#if defined(__linux__)
    if (!s_surface || y>=s_surface_rows || first>=end) return;
    if (first<s_surface_first[y]) s_surface_first[y]=first;
    if (end>s_surface_end[y]) s_surface_end[y]=end;
#else
    (void)y; (void)first; (void)end;
#endif
}

static int surface_stage_flush(const uint8_t *mem)
{
#if defined(__linux__)
    uint32_t y;
    if (!s_surface) return 1;
    for (y=0;y<s_surface_rows;y++) if (s_surface_first[y]!=UINT32_MAX) {
        size_t at=(size_t)y*s_surface_pitch+(size_t)s_surface_first[y]*s_surface_bpp;
        size_t bytes=(size_t)(s_surface_end[y]-s_surface_first[y])*s_surface_bpp;
        /* Failed writes may have transferred a prefix; never claim atomicity. */
        if (!xbox_guest_memory_write((uintptr_t)mem,(uint64_t)s_surface_base+at,
                                    s_surface+at,bytes)) return 0;
    }
#else
    (void)mem;
#endif
    return 1;
}

static int surface_begin_batch(const uint8_t *mem)
{
    return surface_stage_begin(mem);
}

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    (void)mem;
    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    s_gpu.pixels++;
    if ((argb & 0x00FFFFFFu) > (s_gpu.pixel_max & 0x00FFFFFFu))
        s_gpu.pixel_max = argb;
    row = s_surface + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
    surface_stage_dirty((uint32_t)y,(uint32_t)x,(uint32_t)x+1);
}

/* One blend factor, per channel, for source s and destination d (0..255,
 * index 3 is alpha). GL enum values, which is what the NV2A takes. */
static uint32_t blend_factor(uint32_t f, const uint32_t s[4], const uint32_t d[4], int ch)
{
    uint32_t k[4];
    k[0] = (s_gpu.blend_color >> 16) & 0xFF; k[1] = (s_gpu.blend_color >> 8) & 0xFF;
    k[2] = s_gpu.blend_color & 0xFF;         k[3] = s_gpu.blend_color >> 24;
    switch (f) {
    case 0x0000: return 0;
    case 0x0001: return 255;
    case 0x0300: return s[ch];
    case 0x0301: return 255 - s[ch];
    case 0x0302: return s[3];
    case 0x0303: return 255 - s[3];
    case 0x0304: return d[3];
    case 0x0305: return 255 - d[3];
    case 0x0306: return d[ch];
    case 0x0307: return 255 - d[ch];
    case 0x0308: return ch == 3 ? 255 : (s[3] < 255 - d[3] ? s[3] : 255 - d[3]);
    case 0x8001: return k[ch];
    case 0x8002: return 255 - k[ch];
    case 0x8003: return k[3];
    case 0x8004: return 255 - k[3];
    default:     return 255;
    }
}

/* Write one fragment through alpha test and blending.
 *
 * The rasteriser used to store the texel as-is, so every translucent quad
 * painted solid: NFL 2K5 draws a 16x16 grey texture over its intro movie
 * (a fade, alpha from the vertex colour) and the movie vanished under flat
 * grey. The colour is texture MODULATE diffuse -- D3D's default stage 0 op,
 * and the identity for the white-vertex quads that were already right. */
static void shade_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t src)
{
    uint32_t a = src >> 24;

    if (s_gpu.alpha_test) {
        uint32_t r = s_gpu.alpha_ref;
        int pass;
        switch (s_gpu.alpha_func) {
        case 0x0200: pass = 0; break;              /* NEVER    */
        case 0x0201: pass = a <  r; break;          /* LESS     */
        case 0x0202: pass = a == r; break;          /* EQUAL    */
        case 0x0203: pass = a <= r; break;          /* LEQUAL   */
        case 0x0204: pass = a >  r; break;          /* GREATER  */
        case 0x0205: pass = a != r; break;          /* NOTEQUAL */
        case 0x0206: pass = a >= r; break;          /* GEQUAL   */
        default:     pass = 1; break;               /* ALWAYS   */
        }
        if (!pass)
            return;
    }
    if (s_gpu.blend && bpp == 4
            && x >= (int)s_gpu.clip_x && x < (int)(s_gpu.clip_x + s_gpu.clip_w)
            && y >= (int)s_gpu.clip_y && y < (int)(s_gpu.clip_y + s_gpu.clip_h)) {
        uint32_t dst = ((uint32_t *)(s_surface + (size_t)y * s_gpu.pitch))[x];
        uint32_t sv[4], dv[4], out = 0;
        int ch;
        sv[0] = (src >> 16) & 0xFF; sv[1] = (src >> 8) & 0xFF; sv[2] = src & 0xFF; sv[3] = a;
        dv[0] = (dst >> 16) & 0xFF; dv[1] = (dst >> 8) & 0xFF; dv[2] = dst & 0xFF; dv[3] = dst >> 24;
        for (ch = 0; ch < 4; ch++) {
            int32_t sp = (int32_t)(sv[ch] * blend_factor(s_gpu.blend_sf, sv, dv, ch) / 255);
            int32_t dp = (int32_t)(dv[ch] * blend_factor(s_gpu.blend_df, sv, dv, ch) / 255);
            int32_t v;
            switch (s_gpu.blend_eq) {
            case 0x800A: v = sp - dp; break;                     /* SUBTRACT     */
            case 0x800B: v = dp - sp; break;                     /* REV_SUBTRACT */
            case 0x8007: v = sv[ch] < dv[ch] ? sv[ch] : dv[ch]; break; /* MIN */
            case 0x8008: v = sv[ch] > dv[ch] ? sv[ch] : dv[ch]; break; /* MAX */
            default:     v = sp + dp; break;                     /* ADD          */
            }
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            out |= (uint32_t)v << (ch == 3 ? 24 : 16 - 8 * ch);
        }
        src = out;
    }
    /* SET_COLOR_MASK: channels with their write-enable cleared keep what the
     * surface already holds. */
    if (g_color_mask != 0x01010101u && bpp == 4
            && x >= (int)s_gpu.clip_x && x < (int)(s_gpu.clip_x + s_gpu.clip_w)
            && y >= (int)s_gpu.clip_y && y < (int)(s_gpu.clip_y + s_gpu.clip_h)) {
        uint32_t dst = ((uint32_t *)(s_surface + (size_t)y * s_gpu.pitch))[x];
        uint32_t keep = 0;
        if (!(g_color_mask & 0x00000001u)) keep |= 0x000000FFu;   /* blue  */
        if (!(g_color_mask & 0x00000100u)) keep |= 0x0000FF00u;   /* green */
        if (!(g_color_mask & 0x00010000u)) keep |= 0x00FF0000u;   /* red   */
        if (!(g_color_mask & 0x01000000u)) keep |= 0xFF000000u;   /* alpha */
        if (keep == 0xFFFFFFFFu)
            return;
        src = (dst & keep) | (src & ~keep);
    }
    put_pixel(mem, bpp, x, y, src);
}

static uint32_t modulate(uint32_t t, uint32_t c)
{
    uint32_t out = 0;
    int sh;
    for (sh = 0; sh < 32; sh += 8)
        out |= ((((t >> sh) & 0xFF) * ((c >> sh) & 0xFF) + 127) / 255) << sh;
    return out;
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float uv[3][2])
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;
    int textured = uv && s_gpu.tex.valid;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    /* Where this batch writes. The same check the per-pixel path made, made
     * once: a surface address landing on the title's own image is no safer
     * one pixel at a time than 4.9 MB at once. */
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0)
               || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            if (textured) {
                /* Barycentric, straight from the edge functions already
                 * computed: w1 is the area opposite a, w2 opposite b, w0
                 * opposite c, and the three sum to the whole triangle.
                 *
                 * No perspective divide. These are screen-space vertices with
                 * no w to divide by -- which is exactly the case a full-screen
                 * pass is, and the only case that reaches here. */
                uint32_t texel;
                float su = (w1 * uv[0][0] + w2 * uv[1][0] + w0 * uv[2][0]) / area;
                float sv = (w1 * uv[0][1] + w2 * uv[1][1] + w0 * uv[2][1]) / area;
                if (su < 0.0f) su = 0.0f;
                if (sv < 0.0f) sv = 0.0f;
                if (sample_texture((uint32_t)su, (uint32_t)sv, &texel)) {
                    shade_pixel(mem, bpp, x, y, modulate(texel, argb));
                    continue;
                }
            }
            shade_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
/* Which attribute carries the colour.
 *
 * Slot 3 is diffuse by convention and titles that follow it are read straight
 * from there. Half-Life 2 does not: its vertex is position, colour, texcoord
 * at stride 24, and the colour arrives in slot 5. So fall back to the format
 * rather than the slot number -- D3DCOLOR is the one attribute type that is
 * only ever a colour, which makes it a stronger signal than the convention. */
static const VertexAttr *color_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[3].offset && s_gpu.attr[3].stride)
        return &s_gpu.attr[3];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].size == 4
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[3];
}

/* Attribute 9 is texture coordinate 0 in the NV2A vertex layout, the same way
 * 0 is position and 3 is diffuse -- for a title that follows the convention.
 *
 * Half-Life 2 does not, in either place. Its menu and HUD vertex is position,
 * colour, texcoord at stride 24, with the colour in slot 5 and the texcoords
 * in slot 7, so reading slot 9 found nothing and every batch drew untextured.
 * That is invisible rather than wrong-looking: the menu paints a full-screen
 * quad and then draws its text over it, and with no sampling both come out
 * white, so the screen is blank white and nothing suggests the text was ever
 * drawn.
 *
 * Falling back to the format works because the three attributes of such a
 * vertex are distinguishable: position is float3, colour is D3DCOLOR, and a
 * float2 is a texture coordinate and nothing else.
 *
 * ponytail: takes the first float2 it finds, so a title with two texcoord sets
 * gets stage 0's -- which is what this single-texture rasteriser samples
 * anyway. Multi-texture wants the D3D11 translator, not another heuristic. */
static const VertexAttr *texcoord_attr(void)
{
    uint32_t a;

    if (s_gpu.attr[9].offset && s_gpu.attr[9].stride)
        return &s_gpu.attr[9];
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].type == 2 && s_gpu.attr[a].size == 2
                && s_gpu.attr[a].offset && s_gpu.attr[a].stride)
            return &s_gpu.attr[a];
    return &s_gpu.attr[9];
}

/* Texel coordinates, whichever convention the title used.
 *
 * The two are not interchangeable and the format decides which is in force: a
 * swizzled texture is addressed in [0,1], a linear one in texels. Both are
 * scaled to texels here so that everything downstream -- the barycentric
 * interpolation and the sampler -- works in one unit.
 *
 * This mattered the moment swizzled formats became samplable. Normalised
 * coordinates truncated to a texel index land on texel 0 for any coordinate
 * below 1.0, so a whole quad sampled a single texel and came out flat: the
 * background painted one near-black colour, which looks like a texture that
 * decoded wrong rather than one that was never indexed. */
static int fetch_texcoord(uint32_t index, float out[2])
{
    float t[4];

    if (!fetch_attr(texcoord_attr(), index, t))
        return 0;
    out[0] = t[0];
    out[1] = t[1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        out[0] *= (float)s_gpu.tex.width;
        out[1] *= (float)s_gpu.tex.height;
    }
    return 1;
}

static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(color_attr(), index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

static int batch_is_screen_space(void)
{
    float p[4], lo_x, hi_x, lo_y, hi_y;
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h || !s_gpu.idx_count)
        return 0;
    if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p))
        return 0;
    lo_x = hi_x = p[0];
    lo_y = hi_y = p[1];
    for (i = 1; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < lo_x) lo_x = p[0];
        if (p[0] > hi_x) hi_x = p[0];
        if (p[1] < lo_y) lo_y = p[1];
        if (p[1] > hi_y) hi_y = p[1];
    }

    /* Entirely off the surface: nothing to draw under either reading. */
    if (hi_x < (float)s_gpu.clip_x
     || lo_x > (float)(s_gpu.clip_x + s_gpu.clip_w)
     || hi_y < (float)s_gpu.clip_y
     || lo_y > (float)(s_gpu.clip_y + s_gpu.clip_h))
        return 0;

    /* Small enough to be model units rather than pixels. */
    if (hi_x - lo_x < OBJECT_SPACE_SPAN && hi_y - lo_y < OBJECT_SPACE_SPAN)
        return 0;

    return 1;
}

/* NV097 primitive types that are triangles under some winding. */
/* NV097_SET_BEGIN_END operands (0 = end, 1 points, 2 lines, 3 line loop,
 * 4 line strip). These were one lower -- a strip drew as a fan, which left a
 * triangle-shaped hole in NFL 2K5's full-screen movie quad. */
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

/* How many post-draw captures to keep: enough to see whether the geometry
 * is stable from frame to frame, few enough not to fill a directory. */
#define FB_DUMP_AFTER_DRAW 8
static int s_drawn_dumps;

static void dump_surface_bmp(void);

/* One triangle by vertex index: gather position and, if the batch has one,
 * texture coordinate 0. A vertex whose position cannot be read is not drawn;
 * a batch whose texcoords cannot be read is drawn untextured rather than not
 * at all, so a missing coordinate stream costs the colour and not the shape.
 */
static void raster_indexed(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t argb)
{
    float p[3][4], uv[3][2];
    int textured;

    if (!fetch_attr(&s_gpu.attr[0], i0, p[0])
     || !fetch_attr(&s_gpu.attr[0], i1, p[1])
     || !fetch_attr(&s_gpu.attr[0], i2, p[2]))
        return;

    textured = fetch_texcoord(i0, uv[0])
            && fetch_texcoord(i1, uv[1])
            && fetch_texcoord(i2, uv[2]);

    raster_triangle(p[0], p[1], p[2], argb,
                    textured ? (const float (*)[2])uv : NULL);
}

static void raster_batch(void)
{
    uint32_t i;
    uint32_t before = s_gpu.tris_drawn;

    if (s_gpu.idx_count < 3)
        return;
    if (!batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        return;
    }

    /* Count why, once per batch: the texture stage cannot change inside one. */
    {
        const VertexAttr *tc = texcoord_attr();

        if (!(tc->offset && tc->stride))
            s_gpu.batches_no_uv++;
        else if (!s_gpu.tex.valid)
            s_gpu.batches_no_tex++;
        else {
            s_gpu.batches_textured++;
            note_texture_use();
        }
    }

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indexed(s_gpu.idx[0], s_gpu.idx[i], s_gpu.idx[i+1],
                           vertex_color(s_gpu.idx[0]));
        break;
    case NV_PRIM_QUADS:
        /* Each group of four is its own quad: two triangles around its first
         * vertex. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+2], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        /* Vertices pair up; each new pair closes a quad with the previous. */
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+1], s_gpu.idx[i+3],
                           vertex_color(s_gpu.idx[i]));
            raster_indexed(s_gpu.idx[i], s_gpu.idx[i+3], s_gpu.idx[i+2],
                           vertex_color(s_gpu.idx[i]));
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);

    /* Capture the surface while the geometry is still on it.
     *
     * The periodic report dumps too, but a title clears every frame and draws
     * in only some of them, so a report almost always lands on a surface that
     * was wiped a moment ago -- which reads as "nothing was drawn" when the
     * triangles went down correctly just before it. A few frames that actually
     * contain geometry are worth more than any number of clears. */
    if (s_gpu.tris_drawn != before && s_drawn_dumps < FB_DUMP_AFTER_DRAW) {
        s_drawn_dumps++;
        dump_surface_bmp();
    }
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* Texture stages 1..3. Stage 0 stays in s_gpu.tex, which the 2D path and the
 * diagnostics already use; the other three are what multi-texturing needs
 * (lightmaps, detail and environment maps on 3D scenes). Same registers
 * as stage 0, 0x40 apart. */

static void stage_method(uint32_t method, uint32_t param)
{
    uint32_t st = (method - NV097_SET_TEXTURE_OFFSET) >> 6;
    uint32_t reg = (method - NV097_SET_TEXTURE_OFFSET) & 0x3F;
    Texture *t;

    if (st < 1 || st > 3)
        return;
    t = &s_stage[st];
    switch (reg) {
    case 0x00: t->offset = dma_resolve(param); break;
    case 0x04:
        t->color = (param >> 8) & 0xFF;
        if (tex_size_from_format(t->color)) {
            t->width  = 1u << ((param >> 20) & 0xF);
            t->height = 1u << ((param >> 24) & 0xF);
        }
        break;
    case 0x08: t->addr_u = param & 0xF; t->addr_v = (param >> 8) & 0xF; break;
    case 0x0C: t->enabled = (param >> 30) & 1; break;
    case 0x10: t->pitch = param >> 16; break;
    case 0x1C: t->width = param >> 16; t->height = param & 0xFFFF; break;
    case 0x20: t->palette = dma_resolve(param & ~0x3Fu); break;
    default: break;
    }
    t->valid = t->offset && t->width && t->height
            && (tex_size_from_format(t->color) || t->pitch);
}

/* Sample another stage through the stage-0 sampler. The sampler reads
 * s_gpu.tex throughout, so swap it in for the call. */
static int sample_stage(int st, uint32_t u, uint32_t v, uint32_t *argb)
{
    Texture saved;
    int r;
    if (st == 0)
        return sample_texture(u, v, argb);
    saved = s_gpu.tex;
    s_gpu.tex = s_stage[st];
#if defined(__linux__)
    int saved_stage = s_sampler_stage;
    s_sampler_stage = st;
#endif
    r = sample_texture(u, v, argb);
#if defined(__linux__)
    s_sampler_stage = saved_stage;
#endif
    s_gpu.tex = saved;
    return r;
}

/* ── Register combiners ──────────────────────────────────────────────────
 *
 * The NV2A has no fixed-function pixel pipeline: D3D turns its texture-stage
 * states into register-combiner programs, and every pixel's colour comes out
 * of them. Without this the executor used texture x diffuse, which is right
 * only when the title happens to set up exactly that -- NFL 2K5's movie quad
 * sets its diffuse to 0.5 and doubles it in the combiner (output mapping
 * SHIFTLEFT_1), so it drew at half brightness.
 *
 * Follows xemu's psh.c: up to 8 general stages, each computing AB, CD and
 * AB+CD (or a mux on r0.a) for RGB and alpha separately with per-input
 * mappings and per-output scale/bias, then the final combiner
 * D + mix(C, B, A) / G. Registers: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0 (diffuse),
 * 5 v1 (specular), 8..11 t0..t3, 12 r0, 13 r1, 14 v1+r0, 15 E*F.
 *
 * ponytail: only texture stage 0 is sampled; t1..t3 read as white when their
 * stage is enabled (so a lightmap multiply leaves the base texture alone)
 * and zero when it is not. Specular and fog read as zero.
 */
#define NV097_SET_COMBINER_ALPHA_ICW        0x0260   /* +i*4, 8 */
#define NV097_SET_COMBINER_SPECULAR_FOG_CW0 0x0288
#define NV097_SET_COMBINER_SPECULAR_FOG_CW1 0x028C
#define NV097_SET_COMBINER_FACTOR0          0x0A60   /* +i*4, 8 */
#define NV097_SET_COMBINER_FACTOR1          0x0A80   /* +i*4, 8 */
#define NV097_SET_COMBINER_ALPHA_OCW        0x0AA0   /* +i*4, 8 */
#define NV097_SET_COMBINER_COLOR_ICW        0x0AC0   /* +i*4, 8 */
#define NV097_SET_SPECULAR_FOG_FACTOR       0x1E20   /* +i*4, 2 */
#define NV097_SET_COMBINER_COLOR_OCW        0x1E40   /* +i*4, 8 */
#define NV097_SET_COMBINER_CONTROL          0x1E60
#define NV097_SET_SHADER_STAGE_PROGRAM      0x1E70

static struct {
    uint32_t color_icw[8], color_ocw[8], alpha_icw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8], specfog[2];
    uint32_t final0, final1, control, stage_prog, shaderctl;
    int      set;
} s_comb;

static int comb_method(uint32_t method, uint32_t param)
{
    uint32_t *arr = NULL, base = 0;

    if (method >= NV097_SET_COMBINER_ALPHA_ICW && method < NV097_SET_COMBINER_ALPHA_ICW + 32) {
        arr = s_comb.alpha_icw; base = NV097_SET_COMBINER_ALPHA_ICW;
    } else if (method >= NV097_SET_COMBINER_FACTOR0 && method < NV097_SET_COMBINER_FACTOR0 + 32) {
        arr = s_comb.factor0; base = NV097_SET_COMBINER_FACTOR0;
    } else if (method >= NV097_SET_COMBINER_FACTOR1 && method < NV097_SET_COMBINER_FACTOR1 + 32) {
        arr = s_comb.factor1; base = NV097_SET_COMBINER_FACTOR1;
    } else if (method >= NV097_SET_COMBINER_ALPHA_OCW && method < NV097_SET_COMBINER_ALPHA_OCW + 32) {
        arr = s_comb.alpha_ocw; base = NV097_SET_COMBINER_ALPHA_OCW;
    } else if (method >= NV097_SET_COMBINER_COLOR_ICW && method < NV097_SET_COMBINER_COLOR_ICW + 32) {
        arr = s_comb.color_icw; base = NV097_SET_COMBINER_COLOR_ICW;
    } else if (method >= NV097_SET_COMBINER_COLOR_OCW && method < NV097_SET_COMBINER_COLOR_OCW + 32) {
        arr = s_comb.color_ocw; base = NV097_SET_COMBINER_COLOR_OCW;
    }
    if (arr) {
        arr[(method - base) / 4] = param;
        s_comb.set = 1;
        return 1;
    }
    switch (method) {
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0: s_comb.final0 = param; return 1;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1: s_comb.final1 = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR:       s_comb.specfog[0] = param; return 1;
    case NV097_SET_SPECULAR_FOG_FACTOR + 4:   s_comb.specfog[1] = param; return 1;
    case NV097_SET_COMBINER_CONTROL:          s_comb.control = param; s_comb.set = 1; return 1;
    case NV097_SET_SHADER_STAGE_PROGRAM:      s_comb.stage_prog = param; return 1;
    /* NV_PGRAPH_SHADERCTL: dot mappings (bits 0-11) and each stage's input
     * stage (bits 16-27), for the dot-product texture modes (xemu). */
    case 0x1E74: s_comb.shaderctl = (s_comb.shaderctl & ~0xFFFu) | (param & 0xFFFu); return 1;
    case 0x1E78: s_comb.shaderctl = (s_comb.shaderctl & ~0xFFFF000u) | (param & 0xFFFF000u); return 1;
    default: return 0;
    }
}

static int comb_active(void)
{
    return s_comb.set && (s_comb.control & 0xFF);
}

static void comb_unpack(uint32_t c, float v[4])
{
    v[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    v[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    v[2] = (float)(c & 0xFF) / 255.0f;
    v[3] = (float)(c >> 24) / 255.0f;
}

/* One mapped input. `in` is the 8-bit input byte (reg | chan | mapping).
 * For RGB (is_alpha 0) fills 3 components; for alpha fills out[0]. */
static void comb_input(float regs[16][4], uint32_t in, int is_alpha, float out[3])
{
    const float *r = regs[in & 0xF];
    int chan_alpha = (in & 0x10) != 0;
    uint32_t mod = in & 0xE0;
    int i, n = is_alpha ? 1 : 3;

    for (i = 0; i < n; i++) {
        float x = is_alpha ? (chan_alpha ? r[3] : r[2]) : (chan_alpha ? r[3] : r[i]);
        float p = x > 0.0f ? x : 0.0f;
        switch (mod) {
        case 0x00: x = p; break;                                        /* unsigned identity */
        case 0x20: x = 1.0f - (p > 1.0f ? 1.0f : p); break;             /* unsigned invert   */
        case 0x40: x = 2.0f * p - 1.0f; break;                          /* expand normal     */
        case 0x60: x = -2.0f * p + 1.0f; break;                         /* expand negate     */
        case 0x80: x = p - 0.5f; break;                                 /* half-bias normal  */
        case 0xA0: x = -p + 0.5f; break;                                /* half-bias negate  */
        case 0xC0: break;                                               /* signed identity   */
        default:   x = -x; break;                                       /* signed negate     */
        }
        out[i] = x;
    }
}

static float comb_map(float x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: x -= 0.5f; break;
    case 0x10: x *= 2.0f; break;
    case 0x18: x = (x - 0.5f) * 2.0f; break;
    case 0x20: x *= 4.0f; break;
    case 0x30: x *= 0.5f; break;
    default: break;
    }
    return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}

/* One general-combiner stage for RGB or alpha. Reads `src`, writes `dst`. */
static void comb_stage(float src[16][4], float dst[16][4], uint32_t icw, uint32_t ocw,
                       int is_alpha)
{
    float A[3], B[3], C[3], D[3], ab[3], cd[3], ms[3];
    uint32_t flags = ocw >> 12, mapping = flags & 0x38;
    uint32_t ab_dst = (ocw >> 4) & 0xF, cd_dst = ocw & 0xF, ms_dst = (ocw >> 8) & 0xF;
    int n = is_alpha ? 1 : 3, i, mux_cd;

    comb_input(src, (icw >> 24) & 0xFF, is_alpha, A);
    comb_input(src, (icw >> 16) & 0xFF, is_alpha, B);
    comb_input(src, (icw >> 8) & 0xFF, is_alpha, C);
    comb_input(src, icw & 0xFF, is_alpha, D);

    if (!is_alpha && (flags & 2)) {
        float d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        ab[0] = ab[1] = ab[2] = d;
    } else {
        for (i = 0; i < n; i++) ab[i] = A[i] * B[i];
    }
    if (!is_alpha && (flags & 1)) {
        float d = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
        cd[0] = cd[1] = cd[2] = d;
    } else {
        for (i = 0; i < n; i++) cd[i] = C[i] * D[i];
    }
    if (s_comb.control & 0x100)           /* PS_COMBINERCOUNT_MUX_MSB */
        mux_cd = src[12][3] >= 0.5f;
    else
        mux_cd = ((uint32_t)(src[12][3] * 255.0f + 0.5f) & 1u) != 0;
    for (i = 0; i < n; i++)
        ms[i] = (flags & 4) ? (mux_cd ? cd[i] : ab[i]) : ab[i] + cd[i];

    for (i = 0; i < n; i++) {
        ab[i] = comb_map(ab[i], mapping);
        cd[i] = comb_map(cd[i], mapping);
        ms[i] = comb_map(ms[i], mapping);
    }
    if (is_alpha) {
        if (ab_dst) dst[ab_dst][3] = ab[0];
        if (cd_dst) dst[cd_dst][3] = cd[0];
        if (ms_dst) dst[ms_dst][3] = ms[0];
    } else {
        if (ab_dst) { for (i = 0; i < 3; i++) dst[ab_dst][i] = ab[i];
                      if (flags & 0x80) dst[ab_dst][3] = ab[2]; }
        if (cd_dst) { for (i = 0; i < 3; i++) dst[cd_dst][i] = cd[i];
                      if (flags & 0x40) dst[cd_dst][3] = cd[2]; }
        if (ms_dst) for (i = 0; i < 3; i++) dst[ms_dst][i] = ms[i];
    }
}

/* Final colour for one fragment. t0: sampled stage-0 texel (RGBA 0..1),
 * v0: interpolated diffuse. Returns ARGB8888. */
static uint32_t comb_eval(const float t0[4], const float v0[4], const float v1[4],
                          const float tn[3][4])
{
    float regs[16][4], next[16][4], out[4];
    int stages = (int)(s_comb.control & 0xFF), s, i;

    if (stages > 8) stages = 8;
    memset(regs, 0, sizeof regs);
    for (i = 0; i < 4; i++) {
        regs[4][i] = v0[i];
        regs[5][i] = v1[i];
        regs[8][i] = t0[i];
    }
    for (s = 1; s < 4; s++)
        if ((s_comb.stage_prog >> (s * 5)) & 0x1F)
            for (i = 0; i < 4; i++)
                regs[8 + s][i] = tn ? tn[s - 1][i] : 1.0f;
    regs[12][3] = (s_comb.stage_prog & 0x1F) ? t0[3] : 1.0f;   /* r0.a = t0.a */

    for (s = 0; s < stages; s++) {
        int c0 = (s_comb.control & 0x1000) ? s : 0;   /* UNIQUE_C0 */
        int c1 = (s_comb.control & 0x10000) ? s : 0;  /* UNIQUE_C1 */
        comb_unpack(s_comb.factor0[c0], regs[1]);
        comb_unpack(s_comb.factor1[c1], regs[2]);
        memcpy(next, regs, sizeof next);
        comb_stage(regs, next, s_comb.color_icw[s], s_comb.color_ocw[s], 0);
        comb_stage(regs, next, s_comb.alpha_icw[s], s_comb.alpha_ocw[s], 1);
        memcpy(regs, next, sizeof regs);
    }

    if (s_comb.final0 || s_comb.final1) {
        float A[3], B[3], C[3], D[3], E[3], F[3], G[3];
        uint32_t fl = s_comb.final1 & 0xFF;
        comb_unpack(s_comb.specfog[0], regs[1]);
        comb_unpack(s_comb.specfog[1], regs[2]);
        /* E and F first: EF_PROD (15) can feed A..D. */
        comb_input(regs, (s_comb.final1 >> 24) & 0xFF, 0, E);
        comb_input(regs, (s_comb.final1 >> 16) & 0xFF, 0, F);
        for (i = 0; i < 3; i++) {
            float v1 = (fl & 0x40) ? 1.0f - regs[5][i] : regs[5][i];
            float r0 = (fl & 0x20) ? 1.0f - regs[12][i] : regs[12][i];
            float sum = v1 + r0;
            if (fl & 0x80) sum = sum < 0 ? 0 : (sum > 1 ? 1 : sum);
            regs[14][i] = sum;
            regs[15][i] = E[i] * F[i];
        }
        regs[14][3] = regs[15][3] = 0.0f;
        comb_input(regs, (s_comb.final0 >> 24) & 0xFF, 0, A);
        comb_input(regs, (s_comb.final0 >> 16) & 0xFF, 0, B);
        comb_input(regs, (s_comb.final0 >> 8) & 0xFF, 0, C);
        comb_input(regs, s_comb.final0 & 0xFF, 0, D);
        comb_input(regs, (s_comb.final1 >> 8) & 0xFF, 1, G);
        for (i = 0; i < 3; i++)
            out[i] = D[i] + C[i] * (1.0f - A[i]) + B[i] * A[i];
        out[3] = G[0];
    } else {
        memcpy(out, regs[12], sizeof out);
    }
    for (i = 0; i < 4; i++)
        out[i] = out[i] < 0 ? 0 : (out[i] > 1 ? 1 : out[i]);
    return ((uint32_t)(out[3] * 255.0f + 0.5f) << 24)
         | ((uint32_t)(out[0] * 255.0f + 0.5f) << 16)
         | ((uint32_t)(out[1] * 255.0f + 0.5f) << 8)
         |  (uint32_t)(out[2] * 255.0f + 0.5f);
}

/* ── Vertex programs ─────────────────────────────────────────────────────
 *
 * Every 3D batch a title draws goes through an NV2A vertex program: D3D
 * uploads the microcode (SET_TRANSFORM_PROGRAM), the constants -- matrices,
 * lights, and the viewport D3D folds into every shader
 * (SET_TRANSFORM_CONSTANT, SET_VIEWPORT_*) -- and switches the transform
 * engine to program mode. Until this existed the executor could only take
 * attribute 0 at face value, which is right for pre-transformed 2D (menus,
 * movie frames) and meaningless for anything 3D: those batches were counted
 * as "not screen-space" and skipped. NFL 2K5's attract-mode demo and every
 * game scene are this kind.
 *
 * The program's oPos is already in screen space -- D3D appends the viewport
 * scale and offset and the divide by w itself -- so its x and y are pixels
 * on the surface, z is depth scaled to the depth buffer's range, and w is
 * kept for perspective-correct interpolation (see xemu vsh-prog.c, whose
 * decoding this follows field for field).
 *
 * ponytail: one interpreter call per vertex, no JIT; output registers other
 * than oPos, oD0 and oT0 are computed and dropped. Fixed-function transform
 * mode is not implemented -- batches in that mode keep the old path.
 */
#define NV097_SET_VIEWPORT_OFFSET           0x0A20
#define NV097_SET_VIEWPORT_SCALE            0x0AF0
#define NV097_SET_TRANSFORM_PROGRAM         0x0B00
#define NV097_SET_TRANSFORM_CONSTANT        0x0B80
#define NV097_SET_TRANSFORM_EXECUTION_MODE  0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_LOAD    0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START   0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD   0x1EA4
#define NV097_SET_CULL_FACE_ENABLE          0x0308
#define NV097_SET_DEPTH_TEST_ENABLE         0x030C
#define NV097_SET_DEPTH_FUNC                0x0354
#define NV097_SET_DEPTH_MASK                0x035C
#define NV097_SET_CULL_FACE                 0x039C
#define NV097_SET_FRONT_FACE                0x03A0
#define NV097_SET_ZSTENCIL_CLEAR_VALUE      0x1D8C

/* Fog (2026-09-26). The final combiner of nearly every NFL 2K5 draw blends
 * toward the fog register by a factor built from the fog alpha; with fog
 * unimplemented that register was 0 -- full fog, black -- and the field
 * came out black. As xemu: the factor is computed per vertex from the
 * program's oFog.x by SET_FOG_MODE/PARAMS (1 when fog is off), and the
 * pixel side sees (fog colour, factor). */
static struct {
    uint32_t enable, mode, gen, color;
    float p[3];
} s_fog;

static float fog_factor(float dist)
{
    float f, exceptional = 0.0f;
    if (!s_fog.enable)
        return 1.0f;
    switch (s_fog.mode) {
    case 0x0804:                      /* LINEAR_ABS */
    case 0x2601:                      /* LINEAR */
        exceptional = 1.0f;
        f = s_fog.p[0] + dist * s_fog.p[1] - 1.0f;
        break;
    case 0x0800:                      /* EXP */
        exceptional = 1.0f;
        /* fall through */
    case 0x0802:                      /* EXP_ABS */
        f = s_fog.p[0] + exp2f(dist * s_fog.p[1] * 16.0f) - 1.5f;
        break;
    case 0x0803:                      /* EXP2_ABS */
    case 0x0801:                      /* EXP2 */
        f = s_fog.p[0] + exp2f(-dist * dist * s_fog.p[1] * s_fog.p[1] * 32.0f) - 1.5f;
        break;
    default:     f = 1.0f; break;
    }
    /* xemu vsh.c: ABS modes apply to the factor, not the distance. Keep
     * out-of-range factors for interpolation; the combiner clamps per pixel. */
    if (s_fog.mode == 0x0804 || s_fog.mode == 0x0802 || s_fog.mode == 0x0803)
        f = fabsf(f);
    if (isinf(dist) || f != f)
        return exceptional;
    return f < -FLT_MAX ? -FLT_MAX : (f > FLT_MAX ? FLT_MAX : f);
}

#define VP_PROG_LEN   136
#define VP_CONSTS     192
/* Constant slots the title has written (RECOMP_VP_CONST_LOG): a matrix
 * palette entry read through a0 that was never uploaded shows up here. */
static uint8_t s_const_written[VP_CONSTS];
/* Bumped whenever s_vp.c changes: the GPU path re-sends the 3 KB of
 * constants only then (gpu_vp_bind compared all of it every draw). */
static uint32_t s_vp_cgen;
static void const_unwritten_note(int c, int a0);
#define VP_VPSCL      0x3A     /* constant slots SET_VIEWPORT_* write */
#define VP_VPOFF      0x3B

static struct {
    uint32_t prog[VP_PROG_LEN][4];
    float    c[VP_CONSTS][4];
    uint32_t prog_load, const_load, start;
    uint32_t mode;                        /* 0 fixed, 2 program */
    int      loaded;                      /* any program uploaded yet */
    /* output merge state the 3D path needs */
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t cull, cull_face, front_face;
    uint32_t specular_enable, light_control;
    uint32_t poly_offset_point, poly_offset_line, poly_offset_fill;
    float    poly_offset_scale, poly_offset_bias;
    uint32_t zclear;
    /* Stencil and colour write mask. NFL 2K5's menus mask their skewed
     * panels with the stencil: a colour-masked pass writes the panel shape,
     * then the studio is drawn only where the stencil matches. Without it
     * the studio filled the whole screen and the mask passes drew as
     * geometry. GL enums, as the NV2A takes them. */
    uint32_t stencil_test, stencil_wmask, stencil_func, stencil_ref, stencil_fmask;
    uint32_t op_fail, op_zfail, op_zpass;
    uint32_t color_mask;                  /* NV097_SET_COLOR_MASK */
    uint32_t clear_rx, clear_ry;          /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL */
    float    clip_min, clip_max;          /* SET_CLIP_MIN/MAX: depth range oPos.z is in */
    uint32_t control0;                    /* SET_CONTROL0: bit 16 Z perspective (w-buffer), bit 12 float Z */
    uint32_t zminmax;                     /* SET_ZMIN_MAX_CONTROL: bits 4-7 ZCLAMP_EN (0 cull, 1 clamp) */
    uint32_t batches, verts;              /* counters for the report */
    uint32_t rej_bad, rej_area, rej_cull, rej_clip, drawn;
} s_vp = { .depth_func = 0x0201, .depth_mask = 1, .cull_face = 0x0405,
           .front_face = 0x0901,
           .stencil_wmask = 0xFF, .stencil_func = 0x0207, .stencil_fmask = 0xFF,
           .op_fail = 0x1E00, .op_zfail = 0x1E00, .op_zpass = 0x1E00,
           .color_mask = 0x01010101u,
           .clear_rx = 0x0FFF0000u, .clear_ry = 0x0FFF0000u };

static void const_unwritten_note(int c, int a0)
{
    static int on = -1, n;
    static uint32_t seen[64];
    uint32_t key = ((uint32_t)s_vp.start << 16) | (uint32_t)c;
    int k;
    if (on < 0) on = getenv("RECOMP_VP_CONST_LOG") != NULL;
    if (!on || n >= 64) return;
    for (k = 0; k < n; k++) if (seen[k] == key) return;
    seen[n++] = key;
    fprintf(stderr, "  [VPCONST] program start %u reads unwritten c[%d] (a0 %d, const_load now %u)\n",
            s_vp.start, c, a0, s_vp.const_load);
}

/* Returns 1 when the method was a transform-engine method. */
static int vp_method(uint32_t method, uint32_t param)
{
    float f;
    memcpy(&f, &param, 4);
    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (s_vp.prog_load < VP_PROG_LEN)
            s_vp.prog[s_vp.prog_load][slot % 4] = param;
        if (slot % 4 == 3)
            s_vp.prog_load++;
        s_vp.loaded = 1;
        return 1;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (s_vp.const_load < VP_CONSTS) {
            s_vp.c[s_vp.const_load][slot % 4] = f;
            s_const_written[s_vp.const_load] = 1;
            s_vp_cgen++;
        }
        if (slot % 4 == 3)
            s_vp.const_load++;
        return 1;
    }
    if (method >= NV097_SET_VIEWPORT_OFFSET && method < NV097_SET_VIEWPORT_OFFSET + 16) {
        s_vp.c[VP_VPOFF][(method - NV097_SET_VIEWPORT_OFFSET) / 4] = f;
        s_vp_cgen++;
        return 1;
    }
    if (method >= NV097_SET_VIEWPORT_SCALE && method < NV097_SET_VIEWPORT_SCALE + 16) {
        s_vp.c[VP_VPSCL][(method - NV097_SET_VIEWPORT_SCALE) / 4] = f;
        s_vp_cgen++;
        return 1;
    }
    switch (method) {
    case NV097_SET_TRANSFORM_EXECUTION_MODE: s_vp.mode = param & 3; return 1;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:   s_vp.prog_load = param; return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START:  s_vp.start = param; return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:  s_vp.const_load = param; return 1;
    case NV097_SET_DEPTH_TEST_ENABLE:        s_vp.depth_test = param; return 1;
    case NV097_SET_DEPTH_FUNC:               s_vp.depth_func = param; return 1;
    case NV097_SET_DEPTH_MASK:               s_vp.depth_mask = param; return 1;
    case NV097_SET_CULL_FACE_ENABLE:         s_vp.cull = param; return 1;
    case NV097_SET_CULL_FACE:                s_vp.cull_face = param; return 1;
    case NV097_SET_FRONT_FACE:               s_vp.front_face = param; return 1;
    case 0x0330: s_vp.poly_offset_point = param; return 1; /* SET_POLY_OFFSET_POINT_ENABLE */
    case 0x0334: s_vp.poly_offset_line  = param; return 1; /* SET_POLY_OFFSET_LINE_ENABLE */
    case 0x0338: s_vp.poly_offset_fill  = param; return 1; /* SET_POLY_OFFSET_FILL_ENABLE */
    case 0x0384: s_vp.poly_offset_scale = f; return 1;     /* SET_POLYGON_OFFSET_SCALE_FACTOR */
    case 0x0388: s_vp.poly_offset_bias  = f; return 1;     /* SET_POLYGON_OFFSET_BIAS */
    case NV097_SET_ZSTENCIL_CLEAR_VALUE:     s_vp.zclear = param; return 1;
    case 0x032C: s_vp.stencil_test  = param; return 1;   /* SET_STENCIL_TEST_ENABLE */
    case 0x0358: s_vp.color_mask = g_color_mask = param; return 1;   /* SET_COLOR_MASK */
    case 0x0360: s_vp.stencil_wmask = param & 0xFF; return 1;
    case 0x0364: s_vp.stencil_func  = param; return 1;
    case 0x0368: s_vp.stencil_ref   = param & 0xFF; return 1;
    case 0x036C: s_vp.stencil_fmask = param & 0xFF; return 1;
    case 0x0370: s_vp.op_fail       = param; return 1;
    case 0x0374: s_vp.op_zfail      = param; return 1;
    case 0x0378: s_vp.op_zpass      = param; return 1;
    case 0x0290: s_vp.control0 = param; return 1;  /* SET_CONTROL0 */
    case 0x0294: s_vp.light_control = param; return 1; /* SET_LIGHT_CONTROL */
    case 0x03B8: s_vp.specular_enable = param; return 1; /* SET_SPECULAR_ENABLE */
    case 0x0394: s_vp.clip_min = f; return 1;   /* SET_CLIP_MIN */
    case 0x0398: s_vp.clip_max = f; return 1;   /* SET_CLIP_MAX */
    case 0x1D78: s_vp.zminmax = param; return 1; /* SET_ZMIN_MAX_CONTROL */
    case 0x029C: s_fog.mode = param; return 1;     /* SET_FOG_MODE */
    case 0x02A0: s_fog.gen = param; return 1;      /* SET_FOG_GEN_MODE */
    case 0x02A4: s_fog.enable = param; return 1;   /* SET_FOG_ENABLE */
    case 0x02A8: s_fog.color = param; return 1;    /* SET_FOG_COLOR: A B G R from the top */
    case 0x09C0: case 0x09C4: case 0x09C8:         /* SET_FOG_PARAMS */
        s_fog.p[(method - 0x09C0) / 4] = f; return 1;
    case 0x1D98: s_vp.clear_rx = g_clear_rx = param; return 1;   /* SET_CLEAR_RECT_HORIZONTAL */
    case 0x1D9C: s_vp.clear_ry = g_clear_ry = param; return 1;   /* SET_CLEAR_RECT_VERTICAL */
    default: return 0;
    }
}

static int vp_active(void)
{
    return s_vp.mode == 2 && s_vp.loaded;
}

static uint32_t vp_field(const uint32_t *t, int word, int bit, int len)
{
    return (t[word] >> bit) & ((1u << len) - 1u);
}

/* One source operand: register file by mux (1 R, 2 v, 3 c), swizzle, negate.
 * `scalar` replicates the first swizzle component, which is how the ILU
 * reads its (C) operand for the scalar ops. */
static void vp_src(const uint32_t *t, int mux, int reg, int neg, const int swz[4],
                   float R[13][4], float O[16][4], const float V[16][4], int a0,
                   int scalar, float out[4])
{
    const float *s;
    int i;
    float tmp[4];

    switch (mux) {
    case 1:  s = reg == 12 ? O[0] : R[reg < 13 ? reg : 0]; break;
    case 2:  s = V[vp_field(t, 1, 9, 4)]; break;
    case 3: {
        int c = (int)vp_field(t, 1, 13, 8);
        if (vp_field(t, 3, 1, 1)) {
            c += a0;
            if (c >= 0 && c < VP_CONSTS && !s_const_written[c]) const_unwritten_note(c, a0);
        }
        if (c < 0) c = 0;
        if (c >= VP_CONSTS) c = VP_CONSTS - 1;
        s = s_vp.c[c];
        break;
    }
    default: s = R[0]; break;
    }
    for (i = 0; i < 4; i++)
        tmp[i] = s[scalar ? swz[0] : swz[i]];
    for (i = 0; i < 4; i++)
        out[i] = neg ? -tmp[i] : tmp[i];
}

static void vp_write(float *dst, const float v[4], uint32_t mask)
{
    /* mask bit 3 = x ... bit 0 = w */
    if (mask & 8) dst[0] = v[0];
    if (mask & 4) dst[1] = v[1];
    if (mask & 2) dst[2] = v[2];
    if (mask & 1) dst[3] = v[3];
}

static uint32_t vp_fog_write_mask(uint32_t mask)
{
    /* xemu fog_mask_str: every n-component oFog write maps to the first n
     * components, making x the scalar fog output even for an oFog.w write. */
    static const uint8_t masks[16] = {
        0, 8, 8, 12, 8, 12, 12, 14, 8, 12, 12, 14, 12, 14, 14, 15
    };
    return masks[mask & 15u];
}

/* NV2A raster setup parity with xemu vsh.c: screen coordinates have four
 * fractional bits and truncate toward zero, including negative positions. */
static float vp_screen_coord(float value)
{
    return truncf(value * 16.0f) / 16.0f;
}

/* xemu clampAwayZeroInf: preserve signed zero while clamping W to the exact
 * signed [2^-64, 2^64] interval. Exceptional NaN values remain NaN. */
static float vp_clamp_w(float value)
{
    const float minimum = 5.421010862427522e-20f;
    const float maximum = 1.8446744073709552e19f;
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    if (value > 0.0f || bits == 0) {
        if (value < minimum) return minimum;
        if (value > maximum) return maximum;
    } else {
        if (value > -minimum) return -minimum;
        if (value < -maximum) return -maximum;
    }
    return value;
}

/* Run the loaded program for one vertex. V: inputs v0..v15.
 * O: outputs (0 oPos, 3 oD0, 4 oD1, 5 oFog, 6 oPts, 7/8 oB0/oB1, 9..12 oT0..3). */
/* Set to trace every instruction of the next vp_run (RECOMP_GPU_TRACE). */
static int s_vp_trace_next;

static float s_vp_dbg_r[13][4];             /* last vp_run's temporaries (RECOMP_IDX_DUMP) */
static int s_vp_dbg_a0;
static void vp_run(const float V[16][4], float O[16][4])
{
    float R[13][4];
    int a0 = 0, slot, i;

    memset(R, 0, sizeof R);
    for (i = 0; i < 16; i++) {
        O[i][0] = O[i][1] = O[i][2] = 0.0f;
        O[i][3] = 1.0f;
    }

    for (slot = (int)s_vp.start; slot < VP_PROG_LEN; slot++) {
        const uint32_t *t = s_vp.prog[slot];
        uint32_t ilu = vp_field(t, 1, 25, 3), mac = vp_field(t, 1, 21, 4);
        float A[4], B[4], C[4], mres[4] = {0, 0, 0, 0}, ires[4] = {0, 0, 0, 0};
        int sa[4], sb[4], sc[4];
        int mux_a = (int)vp_field(t, 2, 26, 2), mux_b = (int)vp_field(t, 2, 11, 2);
        int mux_c = (int)vp_field(t, 3, 28, 2);
        int reg_a = (int)vp_field(t, 2, 28, 4), reg_b = (int)vp_field(t, 2, 13, 4);
        int reg_c = (int)((vp_field(t, 2, 0, 2) << 2) | vp_field(t, 3, 30, 2));
        uint32_t mac_mask = vp_field(t, 3, 24, 4), ilu_mask = vp_field(t, 3, 16, 4);
        uint32_t o_mask = vp_field(t, 3, 12, 4), out_r = vp_field(t, 3, 20, 4);
        uint32_t orb = vp_field(t, 3, 11, 1), out_addr = vp_field(t, 3, 3, 8);
        uint32_t out_mux = vp_field(t, 3, 2, 1);
        int new_a0 = a0, a0_written = 0;

        sa[0] = (int)vp_field(t, 1, 6, 2); sa[1] = (int)vp_field(t, 1, 4, 2);
        sa[2] = (int)vp_field(t, 1, 2, 2); sa[3] = (int)vp_field(t, 1, 0, 2);
        sb[0] = (int)vp_field(t, 2, 23, 2); sb[1] = (int)vp_field(t, 2, 21, 2);
        sb[2] = (int)vp_field(t, 2, 19, 2); sb[3] = (int)vp_field(t, 2, 17, 2);
        sc[0] = (int)vp_field(t, 2, 8, 2); sc[1] = (int)vp_field(t, 2, 6, 2);
        sc[2] = (int)vp_field(t, 2, 4, 2); sc[3] = (int)vp_field(t, 2, 2, 2);

        vp_src(t, mux_a, reg_a, (int)vp_field(t, 1, 8, 1), sa, R, O, V, a0, 0, A);
        vp_src(t, mux_b, reg_b, (int)vp_field(t, 2, 25, 1), sb, R, O, V, a0, 0, B);
        vp_src(t, mux_c, reg_c, (int)vp_field(t, 2, 10, 1), sc, R, O, V, a0,
               ilu >= 2 && ilu <= 6, C);

        switch (mac) {
        case 1: memcpy(mres, A, sizeof mres); break;                                  /* MOV */
        case 2: for (i = 0; i < 4; i++) mres[i] = A[i] * B[i]; break;                /* MUL */
        case 3: for (i = 0; i < 4; i++) mres[i] = A[i] + C[i]; break;                /* ADD */
        case 4: for (i = 0; i < 4; i++) mres[i] = A[i] * B[i] + C[i]; break;         /* MAD */
        case 5: mres[0] = mres[1] = mres[2] = mres[3] =                                /* DP3 */
                    A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; break;
        case 6: mres[0] = mres[1] = mres[2] = mres[3] =                                /* DPH */
                    A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + B[3]; break;
        case 7: mres[0] = mres[1] = mres[2] = mres[3] =                                /* DP4 */
                    A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]; break;
        case 8: mres[0] = 1.0f; mres[1] = A[1] * B[1]; mres[2] = A[2]; mres[3] = B[3]; break; /* DST */
        case 9: for (i = 0; i < 4; i++) mres[i] = A[i] < B[i] ? A[i] : B[i]; break;  /* MIN */
        case 10: for (i = 0; i < 4; i++) mres[i] = A[i] > B[i] ? A[i] : B[i]; break; /* MAX */
        case 11: for (i = 0; i < 4; i++) mres[i] = A[i] < B[i] ? 1.0f : 0.0f; break; /* SLT */
        case 12: for (i = 0; i < 4; i++) mres[i] = A[i] >= B[i] ? 1.0f : 0.0f; break;/* SGE */
        case 13: new_a0 = (int)floorf(A[0] + 0.001f); a0_written = 1;                  /* ARL */
            {   /* RECOMP_VP_A0_LOG: the address range each program start uses */
                static int on = -1;
                static int lo[256], hi[256], seen[256];
                static DWORD last;
                if (on < 0) on = getenv("RECOMP_VP_A0_LOG") != NULL;
                if (on && s_vp.start < 256) {
                    int st = (int)s_vp.start;
                    if (!seen[st] || new_a0 < lo[st]) lo[st] = new_a0;
                    if (!seen[st] || new_a0 > hi[st]) hi[st] = new_a0;
                    seen[st] = 1;
                    if (GetTickCount() - last > 3000) {
                        int q;
                        last = GetTickCount();
                        for (q = 0; q < 256; q++)
                            if (seen[q]) fprintf(stderr, "  [VPA0] start %d a0 %d..%d (input %g)\n", q, lo[q], hi[q], A[0]);
                    }
                }
            }
            break;
        default: break;
        }

        switch (ilu) {
        case 1: memcpy(ires, C, sizeof ires); break;                                  /* MOV */
        case 2: { float x = C[0];                                                     /* RCP */
                  float r = x == 1.0f ? 1.0f : (x == 0.0f ? INFINITY : 1.0f / x);
                  ires[0] = ires[1] = ires[2] = ires[3] = r; break; }
        case 3: { float x = C[0], r;                                                  /* RCC */
                  r = x == 0.0f ? 1.8446743e19f : 1.0f / x;
                  if (r > 0) { if (r < 5.42101e-20f) r = 5.42101e-20f; if (r > 1.8446743e19f) r = 1.8446743e19f; }
                  else { if (r > -5.42101e-20f) r = -5.42101e-20f; if (r < -1.8446743e19f) r = -1.8446743e19f; }
                  ires[0] = ires[1] = ires[2] = ires[3] = r; break; }
        case 4: { float x = fabsf(C[0]);                                              /* RSQ */
                  float r = x == 1.0f ? 1.0f : (x == 0.0f ? INFINITY : 1.0f / sqrtf(x));
                  ires[0] = ires[1] = ires[2] = ires[3] = r; break; }
        case 5: { float x = C[0], fl = floorf(x);                                     /* EXP */
                  ires[0] = exp2f(fl); ires[1] = x - fl; ires[2] = exp2f(x); ires[3] = 1.0f; break; }
        case 6: { float x = fabsf(C[0]);                                              /* LOG */
                  if (x == 0.0f) { ires[0] = ires[2] = -INFINITY; ires[1] = 1.0f; }
                  else { float e = floorf(log2f(x)); ires[0] = e; ires[1] = x / exp2f(e); ires[2] = log2f(x); }
                  ires[3] = 1.0f; break; }
        case 7: { float sx = C[0] > 0 ? C[0] : 0, sy = C[1] > 0 ? C[1] : 0;          /* LIT */
                  float sw = C[3];
                  if (sw < -127.996f) sw = -127.996f;
                  if (sw > 127.996f) sw = 127.996f;
                  ires[0] = 1.0f; ires[1] = sx;
                  ires[2] = sx > 0 ? exp2f(sw * log2f(sy)) : 0.0f; ires[3] = 1.0f; break; }
        default: break;
        }

        /* Register writes. A MAC paired with an ILU op writes its own
         * register (except R1, which the pair gives to the ILU); the ILU
         * writes R1 when paired, its own register otherwise. */
        /* R12 is oPos, for writes as for reads. */
#define VP_REG(r) ((r) == 12 ? O[0] : R[(r) < 13 ? (r) : 0])
        if (mac && mac != 13 && mac_mask) {
            if (!(ilu && out_r == 1))
                vp_write(VP_REG(out_r), mres, mac_mask);
        }
        if (ilu && ilu_mask)
            vp_write(mac ? R[1] : VP_REG(out_r), ires, ilu_mask);
#undef VP_REG

        /* Output register (or writable constant). */
        if (o_mask) {
            const float *v = out_mux ? ires : mres;
            if ((out_mux == 0 && mac && mac != 13) || (out_mux == 1 && ilu)) {
                if (orb) {
                    uint32_t reg = out_addr & 15;
                    vp_write(O[reg], v, reg == 5 ? vp_fog_write_mask(o_mask) : o_mask);
                } else if (out_addr < VP_CONSTS) {
                    vp_write(s_vp.c[out_addr], v, o_mask);
                    s_vp_cgen++;
                }
            }
        }
        if (s_vp_trace_next)
            fprintf(stderr, "  [VPI] %3d mac=%2u ilu=%u A(m%d r%d)=%.4g,%.4g,%.4g,%.4g B(m%d r%d)=%.4g,%.4g,%.4g,%.4g"
                    " C(m%d r%d)=%.4g,%.4g,%.4g,%.4g -> mres=%.4g,%.4g,%.4g,%.4g ires=%.4g"
                    " r%u m%X i%X o%s%u m%X cidx=%u\n", slot, mac, ilu,
                    mux_a, reg_a, A[0], A[1], A[2], A[3], mux_b, reg_b, B[0], B[1], B[2], B[3],
                    mux_c, reg_c, C[0], C[1], C[2], C[3], mres[0], mres[1], mres[2], mres[3], ires[0],
                    out_r, mac_mask, ilu_mask, orb ? "O" : "c", out_addr, o_mask,
                    vp_field(t, 1, 13, 8));
        if (a0_written)
            a0 = new_a0;
        if (vp_field(t, 3, 0, 1))
            break;
    }
    memcpy(s_vp_dbg_r, R, sizeof R);
    s_vp_dbg_a0 = a0;
}

/* One transformed vertex, as the rasteriser wants it. */
typedef struct {
    float x, y, z, w;       /* screen x, y; depth; w for perspective */
    float col[4];           /* oD0, 0..1 RGBA */
    float spec[4];          /* oD1, 0..1 RGBA -- where 3D lighting lands */
    float uv[2];            /* oT0 in texels (scaled like fetch_texcoord) */
    float uvn[3][2];        /* oT1..oT3, scaled for stages 1..3 */
    float tc[4][4];         /* oT0..oT3 as the program wrote them (GPU renderer) */
    float fog;              /* fog factor from oFog (fog_factor) */
    int   ok;
} XfVert;

#define VP_CACHE 65536
static XfVert   s_xf[VP_CACHE];
static uint32_t s_xf_stamp[VP_CACHE];
static uint32_t s_xf_gen = 1;

static void vp_begin_batch(void)
{
    if (++s_xf_gen == 0) {
        memset(s_xf_stamp, 0, sizeof s_xf_stamp);
        s_xf_gen = 1;
    }
}

static const XfVert *vp_vertex(uint32_t index)
{
    XfVert *x = &s_xf[index & (VP_CACHE - 1)];
    float V[16][4], O[16][4];
    uint32_t a;
    int k;

    if (s_xf_stamp[index & (VP_CACHE - 1)] == s_xf_gen)
        return x;
    s_xf_stamp[index & (VP_CACHE - 1)] = s_xf_gen;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        fetch_attr(&s_gpu.attr[a], index, V[a]);
    vp_run(V, O);
    s_vp.verts++;
    if (getenv("RECOMP_VP_TRACE")) {
        static unsigned shown;
        if (shown < 12 || (s_vp.verts % 40000) == 0) {
            if (shown == 0) {
                int sl;
                fprintf(stderr, "  [VP] program from slot %u:\n", s_vp.start);
                for (sl = (int)s_vp.start; sl < VP_PROG_LEN; sl++) {
                    fprintf(stderr, "  [VP]   %3d: %08X %08X %08X %08X\n", sl,
                            s_vp.prog[sl][0], s_vp.prog[sl][1], s_vp.prog[sl][2], s_vp.prog[sl][3]);
                    if (s_vp.prog[sl][3] & 1) break;
                }
                fprintf(stderr, "  [VP]   c58 vpscale %g %g %g %g  c59 vpoff %g %g %g %g\n",
                        s_vp.c[VP_VPSCL][0], s_vp.c[VP_VPSCL][1], s_vp.c[VP_VPSCL][2], s_vp.c[VP_VPSCL][3],
                        s_vp.c[VP_VPOFF][0], s_vp.c[VP_VPOFF][1], s_vp.c[VP_VPOFF][2], s_vp.c[VP_VPOFF][3]);
            }
            shown++;
            fprintf(stderr, "  [VP] v0 %g %g %g %g  v3 %g %g %g %g  v9 %g %g -> oPos %g %g %g %g  oD0 %g %g %g %g  oT0 %g %g\n",
                    V[0][0], V[0][1], V[0][2], V[0][3], V[3][0], V[3][1], V[3][2], V[3][3],
                    V[9][0], V[9][1], O[0][0], O[0][1], O[0][2], O[0][3],
                    O[3][0], O[3][1], O[3][2], O[3][3], O[9][0], O[9][1]);
        }
    }
    x->x = O[0][0]; x->y = O[0][1]; x->z = O[0][2]; x->w = O[0][3];
    x->w = vp_clamp_w(x->w);
    for (k = 0; k < 4; k++) {
        float c = O[3][k];
        if (c != c) c = 1.0f;                    /* NaN reads as 1, like xemu */
        x->col[k] = c < 0 ? 0 : (c > 1 ? 1 : c);
        c = s_vp.specular_enable ? O[4][k] : (k == 3 ? 1.0f : 0.0f);
        if (c != c) c = 1.0f;
        x->spec[k] = c < 0 ? 0 : (c > 1 ? 1 : c);
    }
    if (!(s_vp.light_control & (1u << 17)))
        x->spec[3] = 1.0f;
    memcpy(x->tc, O[9], sizeof x->tc);
    x->fog = fog_factor(O[5][0]);
    x->uv[0] = O[9][0];
    x->uv[1] = O[9][1];
    if (tex_size_from_format(s_gpu.tex.color)) {
        x->uv[0] *= (float)s_gpu.tex.width;
        x->uv[1] *= (float)s_gpu.tex.height;
    }
    for (k = 0; k < 3; k++) {
        const Texture *st = &s_stage[k + 1];
        x->uvn[k][0] = O[10 + k][0];
        x->uvn[k][1] = O[10 + k][1];
        if (tex_size_from_format(st->color)) {
            x->uvn[k][0] *= (float)st->width;
            x->uvn[k][1] *= (float)st->height;
        }
    }
    x->ok = (x->x == x->x) && (x->y == x->y) && fabsf(x->x) < 1e6f && fabsf(x->y) < 1e6f;
    return x;
}

/* Depth buffer: host-side, one float per pixel of the current surface.
 * Cleared by CLEAR_SURFACE's Z bit. */
#define ZB_W 1024
#define ZB_H 1024
static float s_zbuf[ZB_W * ZB_H];

static uint8_t s_sbuf[ZB_W * ZB_H];      /* stencil, one byte per pixel */

/* The rectangle SET_CLEAR_RECT_* names (inclusive), clipped to the buffers. */
static void clear_rect(uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1)
{
    *x0 = s_vp.clear_rx & 0xFFF;
    *x1 = ((s_vp.clear_rx >> 16) & 0xFFF) + 1;
    *y0 = s_vp.clear_ry & 0xFFF;
    *y1 = ((s_vp.clear_ry >> 16) & 0xFFF) + 1;
    if (*x1 > ZB_W) *x1 = ZB_W;
    if (*y1 > ZB_H) *y1 = ZB_H;
}

/* Depth (CLEAR_SURFACE bit 0) and/or stencil (bit 1), inside the clear
 * rectangle only. The clear value is Z24S8: depth in the top 24 bits, in the
 * units the shaders scale oPos.z to; stencil in the low 8. */
static void zbuf_clear_bits(uint32_t param)
{
    float z = (float)(s_vp.zclear >> 8);
    uint8_t st = (uint8_t)(s_vp.zclear & 0xFF);
    uint32_t x0, y0, x1, y1, x, y;
    clear_rect(&x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            if (param & 1u) s_zbuf[(size_t)y * ZB_W + x] = z;
            if (param & 2u) s_sbuf[(size_t)y * ZB_W + x] = st;
        }
}

static void zbuf_clear(void)
{
    zbuf_clear_bits(1u);
}

static int stencil_cmp(uint32_t func, uint32_t ref, uint32_t val)
{
    switch (func) {
    case 0x0200: return 0;
    case 0x0201: return ref <  val;
    case 0x0202: return ref == val;
    case 0x0203: return ref <= val;
    case 0x0204: return ref >  val;
    case 0x0205: return ref != val;
    case 0x0206: return ref >= val;
    default:     return 1;
    }
}

static void stencil_apply(uint8_t *s, uint32_t op)
{
    uint32_t v = *s, n;
    switch (op) {
    case 0x0000: n = 0; break;                                   /* ZERO    */
    case 0x1E01: n = s_vp.stencil_ref; break;                    /* REPLACE */
    case 0x1E02: n = v < 255 ? v + 1 : 255; break;               /* INCRSAT */
    case 0x1E03: n = v > 0 ? v - 1 : 0; break;                   /* DECRSAT */
    case 0x150A: n = ~v & 0xFF; break;                           /* INVERT  */
    case 0x8507: n = (v + 1) & 0xFF; break;                      /* INCR    */
    case 0x8508: n = (v - 1) & 0xFF; break;                      /* DECR    */
    default:     return;                                         /* KEEP    */
    }
    *s = (uint8_t)((v & ~s_vp.stencil_wmask) | (n & s_vp.stencil_wmask));
}

static int depth_pass(float z, float d)
{
    switch (s_vp.depth_func) {
    case 0x0200: return 0;
    case 0x0201: return z <  d;
    case 0x0202: return z == d;
    case 0x0203: return z <= d;
    case 0x0204: return z >  d;
    case 0x0205: return z != d;
    case 0x0206: return z >= d;
    default:     return 1;
    }
}

/* Perspective-correct, Gouraud-shaded, depth-tested triangle. */
static void raster_triangle_xf(const XfVert *a, const XfVert *b, const XfVert *c)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area, iwa, iwb, iwc;
    int minx, maxx, miny, maxy, x, y;
    int textured = s_gpu.tex.valid && s_gpu.tex.enabled;

    if (!a->ok || !b->ok || !c->ok || (bpp != 4 && bpp != 2)) {
        s_vp.rej_bad++;
        return;
    }
    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.0f) {
        s_vp.rej_area++;
        if (getenv("RECOMP_VP_TRACE") && (s_vp.rej_area % 2000000) == 1) {
            uint32_t k;
            fprintf(stderr, "  [VPZERO] prim %u idx %u: (%g,%g,%g,%g) (%g,%g,%g,%g) (%g,%g,%g,%g) inline=%d\n",
                    s_gpu.prim, s_gpu.idx_count, a->x, a->y, a->z, a->w, b->x, b->y, b->z, b->w,
                    c->x, c->y, c->z, c->w, s_gpu.inline_active);
            for (k = 0; k < NV_VERTEX_ATTRS; k++)
                if (s_gpu.attr[k].size)
                    fprintf(stderr, "  [VPZERO]   attr%u off %08X type %u size %u stride %u\n", k,
                            s_gpu.attr[k].offset, s_gpu.attr[k].type, s_gpu.attr[k].size, s_gpu.attr[k].stride);
            for (k = 0; k < 3 && k < s_gpu.idx_count; k++) {
                float v[4];
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[k], v);
                fprintf(stderr, "  [VPZERO]   idx[%u]=%u v0=%g %g %g %g\n", k, s_gpu.idx[k], v[0], v[1], v[2], v[3]);
            }
            {
                int sl;
                fprintf(stderr, "  [VPZERO]   program start %u:\n", s_vp.start);
                for (sl = (int)s_vp.start; sl < VP_PROG_LEN; sl++) {
                    fprintf(stderr, "  [VPZERO]   prog %3d: %08X %08X %08X %08X\n", sl,
                            s_vp.prog[sl][0], s_vp.prog[sl][1], s_vp.prog[sl][2], s_vp.prog[sl][3]);
                    if (s_vp.prog[sl][3] & 1) break;
                }
                for (k = 0; k < VP_CONSTS; k++)
                    if (s_vp.c[k][0] != 0 || s_vp.c[k][1] != 0 || s_vp.c[k][2] != 0 || s_vp.c[k][3] != 0)
                        fprintf(stderr, "  [VPZERO]   c%u = %g %g %g %g\n", k,
                                s_vp.c[k][0], s_vp.c[k][1], s_vp.c[k][2], s_vp.c[k][3]);
            }
        }
        return;
    }
    /* Culling is opt-in (RECOMP_CULL=1) until its winding convention is
     * checked against xemu: with one sign NFL 2K5's movie quad was culled,
     * with the other its main-menu frame, panel and logos were (2026-09-24).
     * Drawing back faces costs little -- the depth test hides most. */
    static int no_cull = -1;
    if (no_cull < 0)
        no_cull = getenv("RECOMP_CULL") == NULL;
    if (s_vp.cull && !no_cull) {
        /* Winding as the NV2A sees it: its window space has y growing
         * upward, so in these y-down screen coordinates a positive area is
         * counter-clockwise. (Taking it as clockwise culled NFL 2K5's movie
         * quad as a back face and left only its fade overlay.) */
        int cw = area < 0;
        int front = (s_vp.front_face == 0x0900) ? cw : !cw;
        if ((s_vp.cull_face == 0x0404 && front) || (s_vp.cull_face == 0x0405 && !front)
                || s_vp.cull_face == 0x0408) {
            s_vp.rej_cull++;
            return;
        }
    }
    if (!surface_begin_batch(mem))
        return;

    minx = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    maxx = (int)ceilf (fmaxf(a->x, fmaxf(b->x, c->x)));
    miny = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    maxy = (int)ceilf (fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (maxx > ZB_W) maxx = ZB_W;
    if (maxy > ZB_H) maxy = ZB_H;
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        s_vp.rej_clip++;
        return;
    }
    s_vp.drawn++;
    iwa = 1.0f / a->w; iwb = 1.0f / b->w; iwc = 1.0f / c->w;

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            float w1 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            float w2 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            float la, lb, lc, pa, pb, pc, iw, z;
            uint32_t argb, k;
            float col[4], spec[4];
            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)))
                continue;
            /* Barycentrics: w1 is opposite a, w2 opposite b, w0 opposite c. */
            la = w1 / area; lb = w2 / area; lc = w0 / area;
            z = la * a->z + lb * b->z + lc * c->z;
            {
                /* Stencil test, then depth test, then the matching stencil
                 * op -- the fixed order of the NV2A (and GL). */
                size_t zi = (size_t)y * ZB_W + (size_t)x;
                float *zp = &s_zbuf[zi];
                uint8_t *sp = &s_sbuf[zi];
                if (s_vp.stencil_test
                        && !stencil_cmp(s_vp.stencil_func, s_vp.stencil_ref & s_vp.stencil_fmask,
                                        *sp & s_vp.stencil_fmask)) {
                    stencil_apply(sp, s_vp.op_fail);
                    continue;
                }
                if (s_vp.depth_test && !depth_pass(z, *zp)) {
                    if (s_vp.stencil_test)
                        stencil_apply(sp, s_vp.op_zfail);
                    continue;
                }
                if (s_vp.stencil_test)
                    stencil_apply(sp, s_vp.op_zpass);
                if (s_vp.depth_test && s_vp.depth_mask)
                    *zp = z;
                /* Nothing to write when every colour channel is masked:
                 * a stencil- or depth-only pass. */
                if ((s_vp.color_mask & 0x01010101u) == 0)
                    continue;
            }
            pa = la * iwa; pb = lb * iwb; pc = lc * iwc;
            iw = pa + pb + pc;
            if (iw == 0.0f) continue;
            pa /= iw; pb /= iw; pc /= iw;
            for (k = 0; k < 4; k++) {
                col[k] = pa * a->col[k] + pb * b->col[k] + pc * c->col[k];
                spec[k] = pa * a->spec[k] + pb * b->spec[k] + pc * c->spec[k];
            }
            argb = ((uint32_t)(col[3] * 255.0f + 0.5f) << 24)
                 | ((uint32_t)(col[0] * 255.0f + 0.5f) << 16)
                 | ((uint32_t)(col[1] * 255.0f + 0.5f) << 8)
                 |  (uint32_t)(col[2] * 255.0f + 0.5f);
            {
                float t0[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                int sampled = 0;
                uint32_t texel = 0;
                if (textured) {
                float su = pa * a->uv[0] + pb * b->uv[0] + pc * c->uv[0];
                float sv = pa * a->uv[1] + pb * b->uv[1] + pc * c->uv[1];
                /* Negative coordinates wrap like any other: shift into range
                 * before the unsigned conversion wrap_coord expects. */
                if (su < 0.0f) su += (float)s_gpu.tex.width * (floorf(-su / (float)s_gpu.tex.width) + 1.0f);
                if (sv < 0.0f) sv += (float)s_gpu.tex.height * (floorf(-sv / (float)s_gpu.tex.height) + 1.0f);
                sampled = sample_texture((uint32_t)su, (uint32_t)sv, &texel);
                if (sampled)
                    comb_unpack(texel, t0);
                }
                if (comb_active())
                {
                    float tn[3][4];
                    int st;
                    for (st = 1; st < 4; st++) {
                        const Texture *tx = &s_stage[st];
                        float *o = tn[st - 1];
                        o[0] = o[1] = o[2] = o[3] = 1.0f;
                        if (((s_comb.stage_prog >> (st * 5)) & 0x1F) && tx->valid && tx->enabled) {
                            uint32_t tex2;
                            float su2 = pa * a->uvn[st - 1][0] + pb * b->uvn[st - 1][0] + pc * c->uvn[st - 1][0];
                            float sv2 = pa * a->uvn[st - 1][1] + pb * b->uvn[st - 1][1] + pc * c->uvn[st - 1][1];
                            if (su2 < 0.0f) su2 += (float)tx->width * (floorf(-su2 / (float)tx->width) + 1.0f);
                            if (sv2 < 0.0f) sv2 += (float)tx->height * (floorf(-sv2 / (float)tx->height) + 1.0f);
                            if (sample_stage(st, (uint32_t)su2, (uint32_t)sv2, &tex2))
                                comb_unpack(tex2, o);
                        }
                    }
                    argb = comb_eval(t0, col, spec, (const float (*)[4])tn);
                }
                else if (sampled)
                    argb = modulate(texel, argb);
                if (getenv("RECOMP_COMB_TRACE")) {
                    static unsigned shown_fmt[256];
                    unsigned shown = shown_fmt[s_gpu.tex.color & 0xFF];
                    if (shown < 3 && x == minx + (maxx - minx) / 2 && y == miny + (maxy - miny) / 2) {
                        shown_fmt[s_gpu.tex.color & 0xFF]++;
                        int k2;
                        shown++;
                        fprintf(stderr, "  [COMB] active=%d control=%08X stageprog=%08X final0=%08X final1=%08X"
                                        " t0=%.2f %.2f %.2f %.2f v0=%.2f %.2f %.2f %.2f -> %08X"
                                        " blend=%u sf=%04X df=%04X atest=%u afunc=%04X aref=%u texfmt=%02X\n",
                                comb_active(), s_comb.control, s_comb.stage_prog, s_comb.final0, s_comb.final1,
                                t0[0], t0[1], t0[2], t0[3], col[0], col[1], col[2], col[3], argb,
                                s_gpu.blend, s_gpu.blend_sf, s_gpu.blend_df, s_gpu.alpha_test,
                                s_gpu.alpha_func, s_gpu.alpha_ref, s_gpu.tex.color);
                        for (k2 = 0; k2 < (int)(s_comb.control & 0xFF) && k2 < 8; k2++)
                            fprintf(stderr, "  [COMB]   stage %d: cI %08X cO %08X aI %08X aO %08X f0 %08X f1 %08X\n",
                                    k2, s_comb.color_icw[k2], s_comb.color_ocw[k2], s_comb.alpha_icw[k2],
                                    s_comb.alpha_ocw[k2], s_comb.factor0[k2], s_comb.factor1[k2]);
                    }
                }
            }
            shade_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.drawn_offset = s_gpu.color_offset;
}

static void xf_lerp(XfVert *o, const XfVert *p, const XfVert *q, float t)
{
    const float *a = (const float *)p, *b2 = (const float *)q;
    float *d = (float *)o;
    size_t k, n = offsetof(XfVert, ok) / sizeof(float);
    for (k = 0; k < n; k++)
        d[k] = a[k] + (b2[k] - a[k]) * t;
    o->ok = 1;
}

/* Near-plane clipping. The program's oPos is already divided by w, so a
 * vertex at or behind the eye lands at a huge or non-finite screen position,
 * and the rasteriser used to drop the whole triangle -- floors, walls and
 * anything close to the camera vanished (1.5 million rejected vertices in
 * NFL 2K5's stadium). Undo the divide (x*w is the clip-space coordinate, the
 * same reconstruction xemu does), clip the polygon against w >= eps, divide
 * again and fan out the pieces. */
static void raster_tri_clip(const XfVert *a, const XfVert *b, const XfVert *c)
{
    const float eps = 1e-3f;
    XfVert in[3], out[6];
    int i, m = 0;

    /* Only triangles with an unusable vertex are clipped. A usable screen
     * position is drawn as-is whatever its w: 2D shaders (NFL 2K5's coach
     * matchup pie charts) leave w at 0 or below with valid x/y, and clipping
     * those on w turned them into screen-filling spikes. */
    if (a->ok && b->ok && c->ok) {
        raster_triangle_xf(a, b, c);
        return;
    }
    /* Opt-in until checked against xemu: on NFL 2K5's coach-matchup pie
     * charts the reconstructed triangles (vertices with w ~ 0) came out as
     * screen-filling slivers, where dropping them matched the real screen. */
    {
        static int clip_on = -1;
        if (clip_on < 0)
            clip_on = getenv("RECOMP_CLIP") != NULL;
        if (!clip_on) {
            s_vp.rej_bad++;
            if (getenv("RECOMP_VP_TRACE") && (s_vp.rej_bad % 400000) == 1) {
                const XfVert *vv[3];
                int q, sl;
                uint32_t k;
                vv[0] = a; vv[1] = b; vv[2] = c;
                fprintf(stderr, "  [VPBAD] prim %u idx %u start %u const_load %u\n",
                        s_gpu.prim, s_gpu.idx_count, s_vp.start, s_vp.const_load);
                for (q = 0; q < 3; q++) {
                    float v[4];
                    fetch_attr(&s_gpu.attr[0], s_gpu.idx[q], v);
                    fprintf(stderr, "  [VPBAD]   v%d out (%g,%g,%g,%g) ok=%d  in v0 (%g,%g,%g,%g)\n", q,
                            vv[q]->x, vv[q]->y, vv[q]->z, vv[q]->w, vv[q]->ok, v[0], v[1], v[2], v[3]);
                }
                for (k = 0; k < NV_VERTEX_ATTRS; k++)
                    if (s_gpu.attr[k].size)
                        fprintf(stderr, "  [VPBAD]   attr%u type %u size %u stride %u\n", k,
                                s_gpu.attr[k].type, s_gpu.attr[k].size, s_gpu.attr[k].stride);
                for (sl = (int)s_vp.start; sl < VP_PROG_LEN; sl++) {
                    fprintf(stderr, "  [VPBAD]   prog %3d: %08X %08X %08X %08X\n", sl,
                            s_vp.prog[sl][0], s_vp.prog[sl][1], s_vp.prog[sl][2], s_vp.prog[sl][3]);
                    if (s_vp.prog[sl][3] & 1) break;
                }
                for (k = 0; k < VP_CONSTS; k++)
                    if (s_vp.c[k][0] != 0 || s_vp.c[k][1] != 0 || s_vp.c[k][2] != 0 || s_vp.c[k][3] != 0)
                        fprintf(stderr, "  [VPBAD]   c%u = %g %g %g %g\n", k,
                                s_vp.c[k][0], s_vp.c[k][1], s_vp.c[k][2], s_vp.c[k][3]);
            }
            return;
        }
    }
    in[0] = *a; in[1] = *b; in[2] = *c;
    for (i = 0; i < 3; i++) {
        XfVert *v = &in[i];
        if (!(v->w == v->w) || !(v->x == v->x) || !(v->y == v->y) || !(v->z == v->z)) {
            s_vp.rej_bad++;
            return;
        }
        v->x *= v->w; v->y *= v->w; v->z *= v->w;
    }
    for (i = 0; i < 3; i++) {
        const XfVert *p = &in[i], *q = &in[(i + 1) % 3];
        int pin = p->w >= eps, qin = q->w >= eps;
        if (pin)
            out[m++] = *p;
        if (pin != qin)
            xf_lerp(&out[m++], p, q, (eps - p->w) / (q->w - p->w));
    }
    if (m < 3) {
        s_vp.rej_clip++;
        return;
    }
    for (i = 0; i < m; i++) {
        XfVert *v = &out[i];
        v->x /= v->w; v->y /= v->w; v->z /= v->w;
        v->ok = (v->x == v->x) && (v->y == v->y);
    }
    for (i = 1; i + 1 < m; i++)
        raster_triangle_xf(&out[0], &out[i], &out[i + 1]);
}

/* Batches drawn through a vertex program (3D) since the last present: a
 * frame without any is a full-screen 2D card (nv2a_gpu_present.inc.c). */
static volatile LONG s_frame_vp_draws;
static unsigned s_begin_seq;               /* BEGINs seen (RECOMP_IDX_DUMP: which draws share one) */
static int s_elements_seen;                 /* this BEGIN has had ARRAY_ELEMENT indices */
#include "nv2a_gpu_d3d11.inc.c"

/* Draw the current batch through the vertex program. */
static void raster_batch_xf(void)
{
    uint32_t i, n = s_gpu.idx_count;
    const uint16_t *ix = s_gpu.idx;

    if (gpu_enabled()) {
        if (gpu_draw_batch(0))
            return;
        gpu_cpu_access();
    }
    vp_begin_batch();
    s_vp.batches++;
    if (s_gpu.tex.valid)
        note_texture_use();
#define XV(k) vp_vertex(ix[k])
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3)
            raster_tri_clip(XV(i), XV(i + 1), XV(i + 2));
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) raster_tri_clip(XV(i + 1), XV(i), XV(i + 2));
            else       raster_tri_clip(XV(i), XV(i + 1), XV(i + 2));
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++)
            raster_tri_clip(XV(0), XV(i), XV(i + 1));
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            raster_tri_clip(XV(i), XV(i + 1), XV(i + 2));
            raster_tri_clip(XV(i), XV(i + 2), XV(i + 3));
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            raster_tri_clip(XV(i), XV(i + 1), XV(i + 3));
            raster_tri_clip(XV(i), XV(i + 3), XV(i + 2));
        }
        break;
    default:
        break;
    }
#undef XV
}

/* Count constant arrays once per batch, never from the per-vertex fetch path.
 * This is deliberately opt-in and bounded so input diagnostics do not change
 * normal match pacing. */
static void trace_constant_arrays(void)
{
    static uint32_t batches, counts[NV_VERTEX_ATTRS], reports;
    uint32_t a, mask = 0;
    if (!env_cached("RECOMP_ATTR_TRACE"))
        return;
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        if (s_gpu.attr[a].size && !s_gpu.attr[a].stride) {
            mask |= 1u << a;
            counts[a]++;
        }
    }
    if (!mask)
        return;
    batches++;
    if (reports >= 12 || (batches > 4 && (batches % 2000) != 0))
        return;
    reports++;
    fprintf(stderr, "[ATTR] stride0 batches=%u draw=%u mask=%04X first=%u last=%u",
            batches, s_gpu.draws, mask, s_gpu.idx[0], s_gpu.idx[s_gpu.idx_count - 1]);
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (counts[a])
            fprintf(stderr, " a%u=%u(off=%08X fmt=%u/%u)", a, counts[a],
                    s_gpu.attr[a].offset, s_gpu.attr[a].type, s_gpu.attr[a].size);
    fprintf(stderr, "\n");
}

static void trace_zeta_surface(void)
{
    static uint32_t seen[32][9], count;
    uint32_t i;
    uint32_t key[9] = {
        s_gpu.color_offset, s_gpu.zeta_offset, s_gpu.pitch, s_gpu.zeta_pitch,
        s_gpu.format, s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h
    };
    if (!env_cached("RECOMP_ZETA_TRACE") || count == 32)
        return;
    for (i = 0; i < count; i++)
        if (memcmp(seen[i], key, sizeof key) == 0)
            return;
    memcpy(seen[count++], key, sizeof key);
    fprintf(stderr,
            "[ZETA] pair=%u draw=%u prim=%u color=%08X zeta=%08X pitch=%u/%u "
            "clip=%ux%u+%u+%u fmt=%08X depth=%u/%04X/%u control0=%08X\n",
            count, s_gpu.draws, s_gpu.prim, s_gpu.color_offset, s_gpu.zeta_offset,
            s_gpu.pitch, s_gpu.zeta_pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.format,
            s_vp.depth_test, s_vp.depth_func, s_vp.depth_mask, s_vp.control0);
}

/* RECOMP_VB_WATCH=1: does the title rewrite a skinned batch's vertex data
 * after the draw was executed? Each such draw records a hash of the vertex
 * range it read; every later kick rehashes it. */
static struct { uint32_t addr, len, hash, kick; } s_vbw[64];
static uint32_t s_vbw_kick;
static int vbw_hash(uint32_t addr, uint32_t len, uint32_t *result)
{
    uint32_t h = 2166136261u, i, done = 0;
    uint8_t bytes[4096];
    while (done < len) {
        size_t count = len - done;
        if (count > sizeof(bytes)) count = sizeof(bytes);
        if (!pb_diagnostic_read((uint64_t)addr + done,bytes,count)) return 0;
        for (i=0;i<count;i++) h=(h ^ bytes[i])*16777619u;
        done += (uint32_t)count;
    }
    *result=h;
    return 1;
}
static void vbw_record(void)
{
    static int on = -1;
    uint32_t i, maxi = 0, len, k;
    if (on < 0) on = getenv("RECOMP_VB_WATCH") != NULL;
    if (!on || s_gpu.attr[1].type != 5 || !s_gpu.attr[0].stride || !s_gpu.idx_count) return;
    for (i = 0; i < s_gpu.idx_count; i++) if (s_gpu.idx[i] > maxi) maxi = s_gpu.idx[i];
    uint64_t length=((uint64_t)maxi+1)*s_gpu.attr[0].stride;
    if (length > (1u << 20)) return;
    len=(uint32_t)length;
    {   /* static or rewritten between draws? per address, summary every 5 s */
        static struct { uint32_t addr, hash, draws, changes; } t[256];
        static DWORD last;
        uint32_t h, q;
        if (!vbw_hash(s_gpu.attr[0].offset,len,&h)) {
            fprintf(stderr,"  [VBWATCH] vertex hash unavailable at %08X\n",s_gpu.attr[0].offset);
            return;
        }
        for (q = 0; q < 256; q++)
            if (!t[q].addr || t[q].addr == s_gpu.attr[0].offset) {
                if (t[q].addr && t[q].hash != h) t[q].changes++;
                t[q].addr = s_gpu.attr[0].offset; t[q].hash = h; t[q].draws++;
                break;
            }
        if (GetTickCount() - last > 5000) {
            uint32_t stat = 0, dyn = 0;
            last = GetTickCount();
            for (q = 0; q < 256 && t[q].addr; q++) {
                if (t[q].changes) {
                    dyn++;
                    if (dyn <= 12) fprintf(stderr, "  [VBDYN] %08X changed %u of %u draws\n", t[q].addr, t[q].changes, t[q].draws);
                } else stat++;
            }
            fprintf(stderr, "  [VBDYN] skinned buffers: %u static, %u rewritten\n", stat, dyn);
        }
    }
    for (k = 0; k < 64; k++)
        if (!s_vbw[k].len || s_vbw[k].addr == s_gpu.attr[0].offset) {
            s_vbw[k].addr = s_gpu.attr[0].offset; s_vbw[k].len = len;
            if (!vbw_hash(s_vbw[k].addr,len,&s_vbw[k].hash)) { s_vbw[k].len=0; return; }
            s_vbw[k].kick = s_vbw_kick;
            return;
        }
}
void nv2a_vb_watch_kick(void)
{
    static int shown;
    uint32_t k;
    s_vbw_kick++;
    for (k = 0; k < 64; k++) {
        uint32_t d, hash;
        if (!s_vbw[k].len || s_vbw[k].kick == s_vbw_kick) continue;
        d = s_vbw_kick - s_vbw[k].kick;
        if (!vbw_hash(s_vbw[k].addr,s_vbw[k].len,&hash)) {
            fprintf(stderr,"  [VBWATCH] vertex hash unavailable at %08X\n",s_vbw[k].addr);
            s_vbw[k].len=0;
        } else if (hash != s_vbw[k].hash) {
            if (shown++ < 40)
                fprintf(stderr, "  [VBWATCH] skinned vertex data %08X (+%u) changed %u kick(s) after its draw\n",
                        s_vbw[k].addr, s_vbw[k].len, d);
            s_vbw[k].len = 0;
        } else if (d > 8) {
            s_vbw[k].len = 0;
        }
    }
}

/* RECOMP_DRAW_LOG=<ms>: list 600 draws starting that long after the first
 * draw (one scene's worth): layout, program, texture. */
static void draw_list_log(void)
{
    static int on = -1;
    static DWORD t0;
    static unsigned n;
    unsigned a;
    if (on < 0) on = getenv("RECOMP_DRAW_LOG") != NULL;
    if (!on || n >= 600) return;
    if (!t0) t0 = GetTickCount();
    if (GetTickCount() - t0 < (DWORD)atoi(getenv("RECOMP_DRAW_LOG"))) return;
    n++;
    fprintf(stderr, "  [DRAWLOG] #%u prim %u idx %u vpstart %u mode %u tex %08X fmt %02X %ux%u |",
            n, s_gpu.prim, s_gpu.idx_count, s_vp.start, s_vp.mode, s_gpu.tex.valid ? s_gpu.tex.offset : 0,
            s_gpu.tex.color, s_gpu.tex.width, s_gpu.tex.height);
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            fprintf(stderr, " a%u:%08X t%u s%u st%u", a, s_gpu.attr[a].offset, s_gpu.attr[a].type,
                    s_gpu.attr[a].size, s_gpu.attr[a].stride);
    fputc('\n', stderr);
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
#if defined(__linux__)
    surface_stage_reset();
    if (!snapshot_textures()) {
        static unsigned failures;
        if (failures++ < 8) fprintf(stderr,"  [GPU] unreadable texture batch skipped\n");
        return;
    }
#endif
    s_gpu.draws++;
    vbw_record();
    draw_list_log();
    trace_constant_arrays();
    trace_zeta_surface();
    /* RECOMP_PRIM_LOG: how many batches of each NV097 primitive type (points
     * and lines are not rasterised yet), every 2000 draws. */
    if (getenv("RECOMP_PRIM_LOG")) {
        static unsigned counts[16], lastn[16], lastvp[16];
        counts[s_gpu.prim & 15]++;
        lastn[s_gpu.prim & 15] = s_gpu.idx_count;
        lastvp[s_gpu.prim & 15] = vp_active();
        if ((s_gpu.draws % 2000) == 0) {
            int k;
            fprintf(stderr, "[PRIM] t=%lu", (unsigned long)GetTickCount());
            for (k = 1; k < 11; k++)
                if (counts[k]) fprintf(stderr, " %d:%u(n%u%s)", k, counts[k], lastn[k], lastvp[k] ? ",vp" : "");
            fprintf(stderr, "\n");
            memset(counts, 0, sizeof counts);
        }
    }
    if ((s_gpu.draws % 200) == 0 && xbox_verbose())
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    /* RECOMP_IDX_DUMP=<ms>[,<vp start>]: for 30 ms that long after the first
     * draw, every batch of that vertex program (13 = skinned heads) to
     * logs\idxdump.txt -- primitive, indices, attribute 0/1 per vertex -- so
     * a hole can be told apart as missing in the title's data or lost here. */
    if (env_cached("RECOMP_IDX_DUMP") && vp_active()) {
        static DWORD t0;
        static unsigned at;
        static unsigned want = 13, wfmt = 0xFFFFFFFFu;
        static FILE *df;
        static int state;                       /* 0 waiting, 1 dumping, 2 done */
        if (!t0) { t0 = GetTickCount(); sscanf(env_cached("RECOMP_IDX_DUMP"), "%u,%u,%x", &at, &want, &wfmt); }
        #define IDX_MATCH ((want == 999 || s_vp.start == want) && (wfmt == 0xFFFFFFFFu || (s_gpu.tex.valid && s_gpu.tex.color == wfmt)))
        if (state == 0 && GetTickCount() - t0 >= at && IDX_MATCH) { state = 1; df = fopen("logs\\idxdump.txt", "w"); at = GetTickCount(); }
        if (state == 1 && GetTickCount() - at > 120) { state = 2; if (df) fclose(df); df = NULL; }
        if (state == 1 && df && IDX_MATCH) {
            uint32_t k;
            fprintf(df, "D %u prim %u n %u tex %08X %ux%u a0 %u/%u/%u a1 %u/%u/%u begin %u\n", s_gpu.draws, s_gpu.prim, s_gpu.idx_count,
                    s_gpu.tex.valid ? s_gpu.tex.offset : 0, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride,
                    s_gpu.attr[1].type, s_gpu.attr[1].size, s_gpu.attr[1].stride, s_begin_seq);
            for (k = 0; k < VP_CONSTS; k++)
                fprintf(df, "C %u %g %g %g %g\n", k, s_vp.c[k][0], s_vp.c[k][1], s_vp.c[k][2], s_vp.c[k][3]);
            {   /* the program, decoded elsewhere: raw words */
                uint32_t q;
                fprintf(df, "P");
                for (q = s_vp.start; q < s_vp.start + 40 && q < VP_PROG_LEN; q++)
                    fprintf(df, " %08X%08X%08X%08X", s_vp.prog[q][0], s_vp.prog[q][1], s_vp.prog[q][2], s_vp.prog[q][3]);
                fprintf(df, "\n");
            }
            fprintf(df, "I");
            for (k = 0; k < s_gpu.idx_count; k++) fprintf(df, " %u", s_gpu.idx[k]);
            fprintf(df, "\n");
            for (k = 0; k < s_gpu.idx_count; k++) {
                float p[4] = {0}, b[4] = {0};
                fetch_attr(&s_gpu.attr[0], s_gpu.idx[k], p);
                fetch_attr(&s_gpu.attr[1], s_gpu.idx[k], b);
                fprintf(df, "V %u %g %g %g %g %g\n", s_gpu.idx[k], p[0], p[1], p[2], b[0], b[1]);
                {
                    float V[16][4], O[16][4];
                    uint32_t a; int r;
                    for (a = 0; a < NV_VERTEX_ATTRS; a++) fetch_attr(&s_gpu.attr[a], s_gpu.idx[k], V[a]);
                    vp_run(V, O);
                    fprintf(df, "O %g %g %g %g a0 %d D %g %g %g %g T %g %g", O[0][0], O[0][1], O[0][2], O[0][3], s_vp_dbg_a0, O[3][0], O[3][1], O[3][2], O[3][3], O[9][0], O[9][1]);
                    for (r = 0; r < 12; r++)
                        fprintf(df, " R%d %g %g %g %g", r, s_vp_dbg_r[r][0], s_vp_dbg_r[r][1], s_vp_dbg_r[r][2], s_vp_dbg_r[r][3]);
                    fprintf(df, "\n");
                }
            }
        }
    }
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    /* A batch drawn through a vertex program is transformed here; one in
     * fixed-function mode keeps the screen-space path (pre-transformed 2D). */
    /* A batch whose texture, palette or vertex pointers name memory that
     * does not exist would fault here. The GPU would draw garbage for it;
     * the executor skips it rather than taking the title down with it
     * (2026-09-24: a menu transition read 0xC2D7597E and crashed the game
     * a second after the main menu appeared). */
#if defined(_WIN32)
    __try {
#else
    {
#endif
        if (vp_active()) {
            s_frame_vp_draws++;
            raster_batch_xf();
        }
        else if (!(gpu_enabled() && gpu_draw_batch(1))) {
            if (gpu_enabled())
                gpu_cpu_access();
            raster_batch();
        }
#if defined(_WIN32)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static unsigned faults;
        if (faults++ < 8)
            fprintf(stderr, "  [GPU] batch %u faulted (exception 0x%08lX); skipped\n",
                    s_gpu.draws, (unsigned long)GetExceptionCode());
    }
#else
    }
#endif
    if (!surface_stage_flush((const uint8_t *)xbox_GetMemoryOffset()))
        fprintf(stderr,"  [GPU] framebuffer copy failed; output may be partial\n");
    surface_stage_reset();

    /* An array attribute leaves its last element behind as the inline value
     * a later batch reads if it disables the array (xemu
     * pgraph_update_inline_value, which keeps D3DCOLOR's raw byte order). */
#if defined(_WIN32)
    __try {
#else
    {
#endif
        uint32_t a, last = s_gpu.idx[s_gpu.idx_count - 1];
        for (a = 0; a < NV_VERTEX_ATTRS; a++) {
            float v[4];
            if (s_gpu.attr[a].size && fetch_attr(&s_gpu.attr[a], last, v)) {
                if (s_gpu.attr[a].type == 0 && s_gpu.attr[a].stride) {
                    float t = v[0]; v[0] = v[2]; v[2] = t;
                }
                if (s_gpu.attr[a].type == 6) {
                    uint32_t c;
                    for (c = 0; c < 3; c++)
                        if (v[c] < -1.0f) v[c] = -1.0f;
                }
                memcpy(s_attr_inline[a], v, sizeof v);
            }
        }
#if defined(_WIN32)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    }
#endif

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            /* The texture stage, for either kind of batch. This used to print
             * only for inline batches, which meant a title drawing through
             * vertex arrays -- Half-Life 2's menu, for one -- showed no
             * texture state at all, and the reason a quad sampled flat was
             * invisible. */
            fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                            " colour 0x%02X swizzled %d valid %d\n",
                    s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                    s_gpu.tex.pitch, s_gpu.tex.color,
                    d3d8_format_is_swizzled(s_gpu.tex.color), s_gpu.tex.valid);
            {
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 3; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
            }
            /* An inline batch has no guest buffer to go and look at -- the
             * vertices are the payload -- so print the payload too. */
            if (s_gpu.inline_active) {
                uint32_t k;
                fprintf(stderr, "  [GPU]   inline %u dwords:", s_gpu.inline_count);
                for (k = 0; k < s_gpu.inline_count && k < 16; k++)
                    fprintf(stderr, " %08X", s_gpu.inline_buf[k]);
                fprintf(stderr, "\n");
                for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
                    float t[2];
                    if (fetch_texcoord(s_gpu.idx[k], t))
                        fprintf(stderr, "  [GPU]   uv[%u] = %.3f %.3f\n",
                                k, t[0], t[1]);
                }
                fprintf(stderr, "  [GPU]   tex: off 0x%08X %ux%u pitch %u"
                                " colour 0x%02X valid %d\n",
                        s_gpu.tex.offset, s_gpu.tex.width, s_gpu.tex.height,
                        s_gpu.tex.pitch, s_gpu.tex.color, s_gpu.tex.valid);
            }
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
#if defined(__linux__)
                    uint8_t bytes[64];
                    if (!pb_diagnostic_read(at->offset,bytes,sizeof bytes)) {
                        fprintf(stderr,"  [GPU] attr%u bytes unavailable\n",a);
                        continue;
                    }
                    for (k = 0; k < sizeof bytes; ++k) if (bytes[k]) ++nz;
#else
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
#endif
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
#if defined(__linux__)
                uint8_t bytes[32];
                if (pb_diagnostic_read(s_gpu.attr[0].offset,bytes,sizeof bytes)) {
                    for (k = 0; k < sizeof bytes; ++k) fprintf(stderr," %02X",bytes[k]);
                } else fprintf(stderr," unavailable");
#else
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
#endif
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* A batch with more indices than s_gpu.idx holds is drawn in pieces: draw
 * what is there and carry the indices the next piece needs to continue the
 * primitive (a list's incomplete tail, a strip's last two -- three to keep
 * the strip's winding alternation -- a fan's centre and last vertex). The
 * rest used to be dropped, which removed the end of every large mesh: holes
 * in faces and missing pieces of players (2026-10-02). */
static void idx_make_room(uint32_t need)
{
    uint32_t n = s_gpu.idx_count, keep = 0, k;
    uint16_t carry[4];
    if (n + need <= NV_MAX_INDICES)
        return;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:      keep = n % 3; break;
    case NV_PRIM_QUADS:          keep = n % 4; break;
    case NV_PRIM_LINES:          keep = n % 2; break;
    case NV_PRIM_TRIANGLE_STRIP:
    case NV_PRIM_QUAD_STRIP:     keep = (n % 2 == 0) ? 2 : 3; break;
    case NV_PRIM_LINE_STRIP:
    case NV_PRIM_LINE_LOOP:      keep = 1; break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:        keep = 2; break;
    default:                     keep = 0; break;
    }
    if (keep > n) keep = n;
    if (s_gpu.prim == NV_PRIM_TRIANGLE_FAN || s_gpu.prim == NV_PRIM_POLYGON) {
        carry[0] = s_gpu.idx[0];
        carry[1] = s_gpu.idx[n - 1];
    } else {
        for (k = 0; k < keep; k++)
            carry[k] = s_gpu.idx[n - keep + k];
    }
    draw_primitive();
    for (k = 0; k < keep; k++)
        s_gpu.idx[k] = carry[k];
    s_gpu.idx_count = keep;
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        /* Shorts are two bytes each. Sizing them as floats pushed every later
         * attribute 4 bytes too far: NFL 2K5's movie quad (float3, short2,
         * D3DCOLOR, float2 = 28 bytes) decoded at stride 32 with x = 0. */
        case 1:                                            /* short norm */
        case 5:  bytes = 2 * s_gpu.attr[a].size;   break;  /* short      */
        case 6:  bytes = 4;                        break;  /* packed CMP */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count > NV_MAX_INDICES)
        idx_overflow_note("inline array (whole batch)", count);
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = count;

    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Draw the vertices SET_VERTEX3F/4F completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as an ordinary vertex
 * array and hand it to the existing path. The layout is ours and fixed, so
 * the attribute table is written out here rather than derived from the
 * title's format registers.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 *
 * ponytail: position, diffuse and texcoord0 only. That is what a 2D quad
 * carries and what this rasteriser samples; a second texcoord set or a normal
 * would need the D3D11 translator, not more slots here.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t i;

    if (s_gpu.imm_count < 3)
        return;

    memcpy(saved, s_gpu.attr, sizeof saved);
    memset(s_gpu.attr, 0, sizeof s_gpu.attr);
    /* Offsets are byte offsets into inline_buf here, not guest addresses --
     * fetch_attr reads them that way while inline_active is set, which is
     * also why 0 is a legal offset for position. */
    s_gpu.attr[0].type = 2; s_gpu.attr[0].size = 4;   /* position float4  */
    s_gpu.attr[0].offset = 0;
    s_gpu.attr[3].type = 0; s_gpu.attr[3].size = 4;   /* diffuse D3DCOLOR */
    s_gpu.attr[3].offset = 16;
    s_gpu.attr[9].type = 2; s_gpu.attr[9].size = 2;   /* texcoord0 float2 */
    s_gpu.attr[9].offset = 20;
    s_gpu.attr[0].stride = s_gpu.attr[3].stride = s_gpu.attr[9].stride =
        IMM_VERTEX_DWORDS * 4;

    for (i = 0; i < s_gpu.imm_count && i < NV_MAX_INDICES; i++)
        s_gpu.idx[i] = (uint16_t)i;
    s_gpu.idx_count = i;

    /* fetch_attr bounds-checks against inline_count dwords. */
    s_gpu.inline_count = s_gpu.imm_count * IMM_VERTEX_DWORDS;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
    s_gpu.inline_count = 0;
}

/* A vertex is complete: append it in the layout draw_immediate describes. */
static void imm_emit_vertex(void)
{
    uint32_t at = s_gpu.imm_count * IMM_VERTEX_DWORDS;

    if (!s_gpu.prim || at + IMM_VERTEX_DWORDS > NV_MAX_INLINE)
        return;
    memcpy(&s_gpu.inline_buf[at],     s_gpu.imm_pos, 4 * sizeof(float));
    memcpy(&s_gpu.inline_buf[at + 4], &s_gpu.imm_diffuse, sizeof(uint32_t));
    memcpy(&s_gpu.inline_buf[at + 5], s_gpu.imm_tex, 2 * sizeof(float));
    s_gpu.imm_count++;
}

/* The immediate-mode writes. Returns 1 if `method` was one of them.
 *
 * Split out because it is a range test against five separate bases, and that
 * reads better than five more cases in an already long switch.
 */
static int imm_vertex_method(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    v.u = param;

    /* Every SET_VERTEX_DATA* write is also the attribute's inline value
     * (xemu pgraph.c), whatever else it does below. */
    if (method >= 0x1880 && method < 0x1880 + NV_VERTEX_ATTRS * 8) {        /* DATA2F_M */
        uint32_t a = (method - 0x1880) / 8, c = ((method - 0x1880) % 8) / 4;
        s_attr_inline[a][c] = v.f;
        s_attr_inline[a][2] = 0.0f;
        s_attr_inline[a][3] = 1.0f;
    } else if (method >= 0x1900 && method < 0x1900 + NV_VERTEX_ATTRS * 4) { /* DATA2S */
        uint32_t a = (method - 0x1900) / 4;
        s_attr_inline[a][0] = (float)(int16_t)(param & 0xFFFF);
        s_attr_inline[a][1] = (float)(int16_t)(param >> 16);
        s_attr_inline[a][2] = 0.0f;
        s_attr_inline[a][3] = 1.0f;
    } else if (method >= 0x1940 && method < 0x1940 + NV_VERTEX_ATTRS * 4) { /* DATA4UB */
        uint32_t a = (method - 0x1940) / 4;
        s_attr_inline[a][0] = (float)(param & 0xFF) / 255.0f;
        s_attr_inline[a][1] = (float)((param >> 8) & 0xFF) / 255.0f;
        s_attr_inline[a][2] = (float)((param >> 16) & 0xFF) / 255.0f;
        s_attr_inline[a][3] = (float)(param >> 24) / 255.0f;
    } else if (method >= 0x1980 && method < 0x1980 + NV_VERTEX_ATTRS * 8) { /* DATA4S_M */
        uint32_t a = (method - 0x1980) / 8, part = ((method - 0x1980) % 8) / 4;
        s_attr_inline[a][part * 2 + 0] = (float)(int16_t)(param & 0xFFFF);
        s_attr_inline[a][part * 2 + 1] = (float)(int16_t)(param >> 16);
    } else if (method >= 0x1A00 && method < 0x1A00 + NV_VERTEX_ATTRS * 16) { /* DATA4F_M */
        s_attr_inline[(method - 0x1A00) / 16][((method - 0x1A00) % 16) / 4] = v.f;
    }
    if (method >= 0x1900 && method < 0x1900 + NV_VERTEX_ATTRS * 4)
        return 1;
    if (method >= 0x1980 && method < 0x1980 + NV_VERTEX_ATTRS * 8)
        return 1;

    if (method >= NV097_SET_VERTEX4F && method < NV097_SET_VERTEX4F + 16) {
        uint32_t c = (method - NV097_SET_VERTEX4F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 3)                      /* w completes the vertex */
            imm_emit_vertex();
        return 1;
    }
    if (method >= NV097_SET_VERTEX3F && method < NV097_SET_VERTEX3F + 12) {
        uint32_t c = (method - NV097_SET_VERTEX3F) / 4;
        s_gpu.imm_pos[c] = v.f;
        if (c == 2) {                    /* z completes it, w is implicitly 1 */
            s_gpu.imm_pos[3] = 1.0f;
            imm_emit_vertex();
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA2F_M + NV_VERTEX_ATTRS * 8) {
        uint32_t off = method - NV097_SET_VERTEX_DATA2F_M;
        if (off / 8 == 9)                /* attribute 9 is texture coord 0 */
            s_gpu.imm_tex[(off % 8) / 4] = v.f;
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        uint32_t off = method - NV097_SET_VERTEX_DATA4F_M;
        uint32_t attr = off / 16, c = (off % 16) / 4;
        if (attr == 0) {
            s_gpu.imm_pos[c] = v.f;
            if (c == 3)
                imm_emit_vertex();
        } else if (attr == 9 && c < 2) {
            s_gpu.imm_tex[c] = v.f;
        }
        return 1;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB
            && method < NV097_SET_VERTEX_DATA4UB + NV_VERTEX_ATTRS * 4) {
        if ((method - NV097_SET_VERTEX_DATA4UB) / 4 == 3)   /* diffuse */
            s_gpu.imm_diffuse = param;
        return 1;
    }
    return 0;
}
/* A run of method words from one header (nv2a_pb_scan), for the long runs
 * that are data rather than state: transform constants and program words,
 * INLINE_ARRAY vertices and ARRAY_ELEMENT16 indices. The same effect as one
 * nv2a_pb_exec_method call per word, without the per-word trip through the
 * dispatcher's checks: those were ~9% of the game thread in play
 * (2026-10-03). Returns the words consumed; 0 = take the per-word path. */
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
uint32_t nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc, const uint32_t *w, uint32_t n)
{
    static int traced = -1;
    uint32_t i;
    if (traced < 0)
        traced = getenv("RECOMP_PB_DRAW_TRACE") || getenv("RECOMP_PB_EXEC_VERBOSE") || getenv("RECOMP_TEX_TRACE");
    if (subch != 0 || n < 2 || traced)
        return 0;
    if (!noninc && method >= NV097_SET_TRANSFORM_CONSTANT && method + (n - 1) * 4 < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
        for (i = 0; i < n; i++, slot++) {
            if (s_vp.const_load < VP_CONSTS) {
                memcpy(&s_vp.c[s_vp.const_load][slot % 4], &w[i], 4);
                s_const_written[s_vp.const_load] = 1;
            }
            if (slot % 4 == 3)
                s_vp.const_load++;
        }
        s_vp_cgen++;
        return n;
    }
    if (!noninc && method >= NV097_SET_TRANSFORM_PROGRAM && method + (n - 1) * 4 < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        uint32_t slot = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
        for (i = 0; i < n; i++, slot++) {
            if (s_vp.prog_load < VP_PROG_LEN)
                s_vp.prog[s_vp.prog_load][slot % 4] = w[i];
            if (slot % 4 == 3)
                s_vp.prog_load++;
        }
        s_vp.loaded = 1;
        return n;
    }
    if (noninc && method == NV097_INLINE_ARRAY) {
        uint32_t room;
        nv2a_pb_exec_method(subch, method, w[0]);       /* flushes a pending DRAW_ARRAYS batch */
        if (!s_gpu.prim)
            return n;                                   /* outside BEGIN/END: dropped, as per word */
        room = NV_MAX_INLINE - s_gpu.inline_count;
        if (room > n - 1) room = n - 1;
        else if (room < n - 1) {
            static int warned;
            if (!warned++) fprintf(stderr, "  [GPU] INLINE_ARRAY batch over %u dwords: tail dropped\n", NV_MAX_INLINE);
        }
        memcpy(&s_gpu.inline_buf[s_gpu.inline_count], w + 1, room * 4);
        {   /* evidence for docs/XBOX-LIMITATION-AUDIT.md: did the old 4096 cap ever bite? */
            static int noted;
            if (s_gpu.inline_count <= 4096 && s_gpu.inline_count + room > 4096 && !noted++)
                fprintf(stderr, "  [GPU] INLINE_ARRAY batch past 4096 dwords (%u): the old cap would have cut it\n",
                        s_gpu.inline_count + room);
        }
        s_gpu.inline_count += room;
        return n;
    }
    if (noninc && method == NV097_ARRAY_ELEMENT16) {
        nv2a_pb_exec_method(subch, method, w[0]);       /* flushes a pending DRAW_ARRAYS batch */
        if (!s_gpu.prim)
            return n;
        for (i = 1; i < n; i++) {
            idx_make_room(2);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(w[i] & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(w[i] >> 16);
        }
        return n;
    }
    return 0;
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* BACK_END_WRITE_SEMAPHORE_RELEASE: D3D's fences (InsertFence puts the
     * fence number here; BlockOnFence spins until the semaphore reaches it).
     * Released when the command is reached, as on hardware -- what lets
     * asynchronous processing (RECOMP_GPU_ASYNC) keep D3D's waits honest. */
    if (method == 0x1D70) {
        extern void xbox_Nv2aSemaphoreRelease(uint32_t value);
        xbox_Nv2aSemaphoreRelease(param);
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    /* Read once: this runs for every method word (millions a second). */
    static int f_verbose = -1, f_textrace = -1;
    if (f_verbose < 0) {
        f_verbose = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
        f_textrace = getenv("RECOMP_TEX_TRACE") != NULL;
    }
    if (f_verbose) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    /* RECOMP_TEX_TRACE=1 (2026-09-25, black field): each distinct raw
     * texture offset / format word per stage, and the context-DMA methods.
     * Format bits 0-1 pick the DMA object (1 = A, 2 = B) the offset is in. */
    if (subch == 0 && f_textrace
            && ((method >= 0x1B00 && method < 0x1C00 && ((method & 0x3F) == 0 || (method & 0x3F) == 4))
                || (method >= 0x0180 && method < 0x01A0))) {
        static uint32_t seen[512][2];
        static unsigned nseen;
        unsigned k;
        for (k = 0; k < nseen; k++)
            if (seen[k][0] == method && seen[k][1] == param) break;
        /* Texture offset 0: the FORMAT word that follows is the header's
         * Format field ({Common, Data, Lock, Format, Size}); find every
         * header in guest memory with that Format and Data == 0. */
#if defined(__linux__)
        {
            static int pending,dumps;
            static uint32_t zva[8],zdraw[8],nz,zdone;
            extern volatile uint32_t g_nv2a_pb_last_word_va;
            if (method==0x1B00 && param==0 && nz<8) {
                zva[nz]=g_nv2a_pb_last_word_va;zdraw[nz]=s_gpu.draws;nz++;
                if (nz<=4) {
                    uint32_t words[8];
                    if (pb_diagnostic_read(0xA6AA14u,words,sizeof(words)))
                        fprintf(stderr,"  [TEXZERO] D3D buffers: A6AA14=%08X A6AA18=%08X A6AA20=%08X %08X %08X A6AA2C=%08X A6AA30=%08X\n",
                            words[0],words[1],words[3],words[4],words[5],words[6],words[7]);
                    else fprintf(stderr,"  [TEXZERO] D3D buffers unavailable (unreadable)\n");
                }
                if (nz==1) {
                    uint32_t words[0x4400/4],nonzero=0;
                    if (pb_diagnostic_read(0x80000000u,words,sizeof(words))) {
                        for (uint32_t i=0;i<0x4000;i++) nonzero+=((const uint8_t *)words)[i]!=0;
                        fprintf(stderr,"  [TEXZERO] phys 0..0x4000: %u nonzero bytes; first: %08X %08X %08X %08X; palette@0x4000: %08X %08X %08X %08X\n",
                            nonzero,words[0],words[1],words[0x100/4],words[0x2000/4],words[0x4000/4],words[0x4004/4],words[0x4100/4],words[0x43FC/4]);
                    } else fprintf(stderr,"  [TEXZERO] physical diagnostic unavailable (unreadable)\n");
                }
                if (nz<=3) {
                    uint32_t words[96];
                    if (pb_diagnostic_read((uint64_t)zva[nz-1]-16,words,sizeof(words))) {
                        fprintf(stderr,"  [TEXZERO] stream from %08X:",zva[nz-1]-16);
                        for(uint32_t i=0;i<96;i++) {
                            if (!(i&7)) fprintf(stderr,"\n  [TEXZERO]  ");
                            fprintf(stderr," %08X",words[i]);
                        }
                        fprintf(stderr,"\n");
                    } else fprintf(stderr,"  [TEXZERO] stream unavailable (unreadable)\n");
                }
            }
            if (nz && zdone<nz && s_gpu.draws>zdraw[zdone]+20000u) {
                uint32_t words[3];
                if (pb_diagnostic_read(zva[zdone],words,sizeof(words)))
                    fprintf(stderr,"  [TEXZERO] offset word at %08X (read 0 at draw %u) now %08X, next %08X %08X\n",zva[zdone],zdraw[zdone],words[0],words[1],words[2]);
                else fprintf(stderr,"  [TEXZERO] offset word at %08X unavailable (unreadable)\n",zva[zdone]);
                zdone++;
            }
            if (method==0x1B00) pending=param==0;
            else if (method==0x1B04 && pending && dumps<4) {
                pending=0;dumps++;
                uint32_t found=0;
                fprintf(stderr,"  [TEXZERO] format %08X with offset 0; headers:\n",param);
                for(uint32_t address=0x10000;address<0x04000000 && found<24;address+=4096) {
                    uint32_t words[1024];
                    if (!pb_diagnostic_read(address,words,sizeof(words))) {
                        fprintf(stderr,"  [TEXZERO] scan stopped at %08X (unreadable)\n",address);break;
                    }
                    for(uint32_t i=0;i<1024 && found<24;i++) {
                        if ((words[i]&0xFFF0FFFCu)!=(param&0xFFF0FFFCu)) continue;
                        uint32_t header[8],at=address+i*4;
                        if (!pb_diagnostic_read((uint64_t)at-12,header,sizeof(header))) {
                            fprintf(stderr,"  [TEXZERO] header at %08X unavailable (unreadable)\n",at);continue;
                        }
                        found++;
                        fprintf(stderr,"  [TEXZERO]   %08X: %08X %08X %08X [%08X] %08X %08X %08X %08X\n",at-12,
                            header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7]);
                    }
                }
            }
        }
#else
        {
            static int pending, dumps;
            /* Where the zero was read, and what that word says later: a
             * recorded pushbuffer whose fixups land after we ran it. */
            static uint32_t zva[8], zdraw[8], nz, zdone;
            {
                extern volatile uint32_t g_nv2a_pb_last_word_va;
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                if (method == 0x1B00 && param == 0 && nz < 8) {
                    zva[nz] = g_nv2a_pb_last_word_va; zdraw[nz] = s_gpu.draws; nz++;
                    if (nz <= 4)
                        fprintf(stderr, "  [TEXZERO] D3D buffers: A6AA14=%08X A6AA18=%08X A6AA20=%08X %08X %08X A6AA2C=%08X A6AA30=%08X\n",
                                *(const uint32_t *)(gm + 0xA6AA14u), *(const uint32_t *)(gm + 0xA6AA18u),
                                *(const uint32_t *)(gm + 0xA6AA20u), *(const uint32_t *)(gm + 0xA6AA24u),
                                *(const uint32_t *)(gm + 0xA6AA28u), *(const uint32_t *)(gm + 0xA6AA2Cu),
                                *(const uint32_t *)(gm + 0xA6AA30u));
                    if (nz == 1) {
                        uint32_t w, nzb = 0;
                        for (w = 0; w < 0x4000; w++) nzb += gm[0x80000000u + w] != 0;
                        fprintf(stderr, "  [TEXZERO] phys 0..0x4000: %u nonzero bytes; first: %08X %08X %08X %08X; palette@0x4000: %08X %08X %08X %08X\n",
                                nzb, *(const uint32_t *)(gm + 0x80000000u), *(const uint32_t *)(gm + 0x80000004u),
                                *(const uint32_t *)(gm + 0x80000100u), *(const uint32_t *)(gm + 0x80002000u),
                                *(const uint32_t *)(gm + 0x80004000u), *(const uint32_t *)(gm + 0x80004004u),
                                *(const uint32_t *)(gm + 0x80004100u), *(const uint32_t *)(gm + 0x800043FCu));
                    }
                    if (nz <= 3) {
                        uint32_t w;
                        fprintf(stderr, "  [TEXZERO] stream from %08X:", zva[nz - 1] - 16);
                        for (w = 0; w < 96; w++) {
                            if ((w & 7) == 0) fprintf(stderr, "\n  [TEXZERO]  ");
                            fprintf(stderr, " %08X", *(const uint32_t *)(gm + zva[nz - 1] - 16 + w * 4));
                        }
                        fprintf(stderr, "\n");
                    }
                }
                if (nz && zdone < nz && s_gpu.draws > zdraw[zdone] + 20000u) {
                    fprintf(stderr, "  [TEXZERO] offset word at %08X (read 0 at draw %u) now %08X, next %08X %08X\n",
                            zva[zdone], zdraw[zdone], *(const uint32_t *)(gm + zva[zdone]),
                            *(const uint32_t *)(gm + zva[zdone] + 4), *(const uint32_t *)(gm + zva[zdone] + 8));
                    zdone++;
                }
            }
            if (method == 0x1B00)
                pending = (param == 0);
            else if (method == 0x1B04 && pending && dumps < 4) {
                const uint8_t *gm = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, found = 0;
                pending = 0;
                dumps++;
                fprintf(stderr, "  [TEXZERO] format %08X with offset 0; headers:\n", param);
#if defined(_WIN32)
                __try {
#endif
                    for (a = 0x10000u; a < 0x04000000u && found < 24; a += 4) {
                        if ((*(const uint32_t *)(gm + a) & 0xFFF0FFFCu) != (param & 0xFFF0FFFCu))
                            continue;
                        found++;
                        fprintf(stderr, "  [TEXZERO]   %08X: %08X %08X %08X [%08X] %08X %08X %08X %08X\n", a - 12,
                                *(const uint32_t *)(gm + a - 12), *(const uint32_t *)(gm + a - 8),
                                *(const uint32_t *)(gm + a - 4), *(const uint32_t *)(gm + a),
                                *(const uint32_t *)(gm + a + 4), *(const uint32_t *)(gm + a + 8),
                                *(const uint32_t *)(gm + a + 12), *(const uint32_t *)(gm + a + 16));
                    }
#if defined(_WIN32)
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    fprintf(stderr, "  [TEXZERO]   (scan stopped at %08X)\n", a);
                }
#endif
            }
        }
#endif
        if (k == nseen && nseen < 512) {
            seen[nseen][0] = method; seen[nseen][1] = param; nseen++;
            fprintf(stderr, "  [TEXTRACE] method %04X = %08X (stage %u %s) draws %u\n", method, param,
                    method >= 0x1B00 ? (method - 0x1B00) >> 6 : 0,
                    method < 0x1B00 ? "ctxdma" : (method & 0x3F) ? "format" : "offset", s_gpu.draws);
        }
    }
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method, param);
        return;
    }
    if (vp_method(method, param) || comb_method(method, param))
        return;
    if (method >= NV097_SET_TEXTURE_OFFSET + 0x40 && method < NV097_SET_TEXTURE_OFFSET + 0x100) {
        stage_method(method, param);
        record_tex_reg(method, param);
        return;
    }
    if (method >= NV097_SET_WINDOW_CLIP_HORIZONTAL && method < NV097_SET_WINDOW_CLIP_HORIZONTAL + 32) {
        uint32_t slot = (method - NV097_SET_WINDOW_CLIP_HORIZONTAL) / 4, k;
        /* NV097's increasing method fills this slot and every following slot
         * with the same value (xemu pgraph SET_WINDOW_CLIP_HORIZONTAL). */
        for (k = slot; k < 8; k++) s_gpu.window_clip_x[k] = param;
        return;
    }
    if (method >= NV097_SET_WINDOW_CLIP_VERTICAL && method < NV097_SET_WINDOW_CLIP_VERTICAL + 32) {
        uint32_t slot = (method - NV097_SET_WINDOW_CLIP_VERTICAL) / 4, k;
        for (k = slot; k < 8; k++) s_gpu.window_clip_y[k] = param;
        return;
    }
    switch (method) {
    case NV097_SET_ALPHA_TEST_ENABLE: s_gpu.alpha_test = param; break;
    case NV097_SET_ALPHA_FUNC:        s_gpu.alpha_func = param; break;
    case NV097_SET_ALPHA_REF:         s_gpu.alpha_ref  = param & 0xFF; break;
    case NV097_SET_BLEND_ENABLE:      s_gpu.blend      = param; break;
    case NV097_SET_BLEND_FUNC_SFACTOR: s_gpu.blend_sf  = param; break;
    case NV097_SET_BLEND_FUNC_DFACTOR: s_gpu.blend_df  = param; break;
    case NV097_SET_BLEND_EQUATION:    s_gpu.blend_eq   = param; break;
    case NV097_SET_BLEND_COLOR:       s_gpu.blend_color = param; break;
    case NV097_SET_WINDOW_CLIP_TYPE:  s_gpu.window_clip_type = param & 1; break;
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        s_gpu.zeta_pitch = param >> 16;
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        if (getenv("RECOMP_SURF_TRACE") && param != s_gpu.color_offset) {
            static unsigned shown;
            if (shown++ < 400)
                fprintf(stderr, "  [SURF] color %08X -> %08X (pitch %u clip %ux%u fmt %08X) flips %u draws %u\n",
                        s_gpu.color_offset, param, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
                        s_gpu.format, s_gpu.flips, s_gpu.draws);
        }
        s_gpu.color_offset = param;
        break;
    case NV097_SET_SURFACE_ZETA_OFFSET:
        s_gpu.zeta_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        if (gpu_enabled() && gpu_clear(param))
            break;
        if (gpu_enabled())
            gpu_cpu_access();
        clear_surface(param);
        if (param & 3u)                    /* NV097_CLEAR_SURFACE_Z / _STENCIL */
            zbuf_clear_bits(param);
        break;

    case NV097_SET_BEGIN_END:
        if (getenv("RECOMP_PB_DRAW_TRACE"))
            fprintf(stderr, "  [GPU] BEGIN_END param=%u prior-prim=%u inline=%u indices=%u\n",
                    param, s_gpu.prim, s_gpu.inline_count, s_gpu.idx_count);
        s_elements_seen = 0;
        if (param) {
            s_begin_seq++;
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.draw_arrays_active = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.draw_arrays_active = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE) {
            if (s_gpu.draw_arrays_active && s_gpu.idx_count) {
                draw_primitive();
                s_gpu.idx_count = 0;
                s_gpu.draw_arrays_active = 0;
            }
            if (getenv("RECOMP_PB_DRAW_TRACE") && s_gpu.inline_count < 8) {
                extern volatile uint32_t g_nv2a_pb_last_word_va;
                fprintf(stderr, "  [GPU] INLINE[%u]=%08X prim=%u src_va=0x%08X\n",
                        s_gpu.inline_count, param, s_gpu.prim,
                        g_nv2a_pb_last_word_va);
            }
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        }
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        /* xemu keeps one (start,count) entry per DRAW_ARRAYS and submits each
         * as a separate draw.  It only joins immediately adjacent ranges.
         * Concatenating discontinuous triangle strips manufactured triangles
         * between the last vertex of one field tile and the first vertex of
         * the next, producing the large pale/white wedges seen in game. */
        /* Once this BEGIN has had ARRAY_ELEMENT indices, a run joins them as
         * more indices of the same primitive (xemu DRAW_ARRAYS with
         * inline_elements_length): no flush, the strip carries on. */
        if (s_elements_seen) {
            for (i = 0; i < count; i++) {
                idx_make_room(1);
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
            }
            break;
        }
        if (s_gpu.draw_arrays_active && s_gpu.idx_count
                && start != (uint32_t)s_gpu.idx[s_gpu.idx_count - 1] + 1u) {
            draw_primitive();
            s_gpu.idx_count = 0;
        }
        s_gpu.draw_arrays_active = 1;
        for (i = 0; i < count; i++) {
            idx_make_room(1);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
        }
        break;
    }

    case 0x1808:                           /* NV097_ARRAY_ELEMENT32 */
        /* One index per word. D3D sends the last index of an odd-length
         * 16-bit list this way, so without it every such draw lost its final
         * vertex. */
        /* A DRAW_ARRAYS run before it in this BEGIN is the start of the same
         * primitive (xemu pgraph_expand_draw_arrays): its indices are already
         * in the list, so the strip simply continues. Flushing here cut every
         * mixed strip in two and lost the two triangles across the cut --
         * the holes in player faces and bodies (2026-10-09). */
        s_gpu.draw_arrays_active = 0;
        s_elements_seen = 1;
        if (s_gpu.prim) {
            idx_make_room(1);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)param;
        }
        break;
    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. Continues a preceding
         * DRAW_ARRAYS run of this BEGIN, as above. */
        s_gpu.draw_arrays_active = 0;
        s_elements_seen = 1;
        if (s_gpu.prim) {
            idx_make_room(2);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL0:
        /* Stage 0 on or off. A batch drawn with the stage disabled must not
         * sample whatever texture happens to still be bound -- that painted
         * untextured shapes with a stale image. */
        s_gpu.tex.enabled = (param >> 30) & 1;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_PALETTE:
        /* Bit 0 picks the DMA context, 3:2 the length; the palette itself
         * is 64-byte aligned, so the rest of the word is its offset. This
         * was decoded from 0x1B0C (CONTROL0) until 2026-09-24, so every
         * paletted texture read a palette from a meaningless address. */
        s_gpu.tex.palette = dma_resolve(param & ~0x3Fu);
        record_tex_reg(method, param);
        break;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        return;

    case NV097_FLIP_STALL:
        /* The stall ends when the buffer being read is the one just finished.
         * There is no scanout here to wait for, so that is now. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* The surface drawn into up to here is a finished frame: show it. */
        if (gpu_enabled())
            gpu_flip();
        /* RECOMP_SHOT_PREFIX=<path prefix>: every RECOMP_SHOT_SECONDS (default
         * 30) write the finished frame as <prefix>-<n>.bmp, straight from the
         * surface -- works when the window is minimised or the screen is
         * locked, which window capture does not (2026-09-25). */
        if (getenv("RECOMP_SHOT_PREFIX")) {
            static DWORD next_shot;
            static unsigned shot_n;
            DWORD now = GetTickCount();
            if (!next_shot)
                next_shot = now;
            if (now >= next_shot && surface_bpp() == 4) {
                const char *secs = getenv("RECOMP_SHOT_SECONDS");
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t base = dma_resolve(s_gpu.color_offset);
                uint32_t w = s_gpu.clip_x + s_gpu.clip_w, h = s_gpu.clip_y + s_gpu.clip_h, y;
                char path[512];
                FILE *f;
                next_shot = now + 1000u * (secs ? (unsigned)atoi(secs) : 30u);
                snprintf(path, sizeof path, "%s-%03u.bmp", getenv("RECOMP_SHOT_PREFIX"), shot_n++);
                f = fopen(path, "wb");
                fprintf(stderr, "  [SHOT] %s t=%lu\n", path, (unsigned long)now);
                if (f && w && h && w <= 4096 && h <= 4096) {
                    uint32_t size = 54 + w * h * 4;
                    uint8_t hdr[54] = { 'B', 'M' };
                    memcpy(hdr + 2, &size, 4);
                    hdr[10] = 54; hdr[14] = 40;
                    memcpy(hdr + 18, &w, 4);
                    { int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4); }   /* top-down */
                    hdr[26] = 1; hdr[28] = 32;
                    fwrite(hdr, 1, 54, f);
                    for (y = 0; y < h; y++) {
#if defined(__linux__)
                        uint32_t row[4096];
                        if (!pb_diagnostic_read((uint64_t)base+(uint64_t)y*s_gpu.pitch,row,(size_t)w*4)) {
                            fclose(f); f=NULL; remove(path);
                            fprintf(stderr,"  [SHOT] unavailable: unreadable row %u\n",y);
                            break;
                        }
                        fwrite(row,4,w,f);
#else
                        fwrite(mem + base + (size_t)y * s_gpu.pitch, 4, w, f);
#endif
                    }
                }
                if (f)
                    fclose(f);
            }
        }
        xbox_FramebufferWindowSet(dma_resolve(s_gpu.color_offset), s_gpu.pitch);
        xbox_FramebufferWindowSize(s_gpu.clip_x + s_gpu.clip_w, s_gpu.clip_y + s_gpu.clip_h);
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. */
        xbox_Nv2aFrameCounterFlip();
        if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        if (getenv("RECOMP_TEX_IDENTIFY"))
            fprintf(stderr, "[TEX] FORMAT param=0x%08X color=0x%02X%s\n",
                    param, (param >> 8) & 0xFF,
                    ((param >> 8) & 0xFF) == 0x0B ?
                        "  <-- matches legal-screen P8 format byte" : "");
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        if (getenv("RECOMP_TEX_IDENTIFY"))
            fprintf(stderr, "[TEX] IMAGE_RECT width=%u height=%u offset=0x%08X\n",
                    s_gpu.tex.width, s_gpu.tex.height, s_gpu.tex.offset);
        record_tex_reg(method, param);
        break;

    default:
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_vertex_method(method, param)) {
            note_unhandled(method, param);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;
#if defined(__linux__)
    uint32_t staged[1024+8],begin=UINT32_MAX;
    ram=staged;
#endif

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        uint32_t at=i;
#if defined(__linux__)
        if (begin==UINT32_MAX || i>=begin+1024) {
            begin=i & ~1023u;
            size_t count=0x04000000u/4-begin;
            if(count>1024+8) count=1024+8;
            if(!pb_diagnostic_read((uint64_t)begin*4,staged,count*4)) {
                fprintf(stderr,"  [GPU] quad scan incomplete at %08X (unreadable)\n",begin*4);
                return;
            }
        }
        at=i-begin;
#endif
        if (ram[at] != W && ram[at] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[at + k] == W) has_w = 1;
                if (ram[at + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[at + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;
#if defined(__linux__)
    uint32_t staged[1024+20],begin=UINT32_MAX;
    ram=staged;
#endif

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        uint32_t at=i;
#if defined(__linux__)
        if(begin==UINT32_MAX || i>=begin+1024) {
            begin=i & ~1023u;
            size_t count=0x04000000u/4-begin;
            if(count>1024+20) count=1024+20;
            if(!pb_diagnostic_read((uint64_t)begin*4,staged,count*4)) {
                fprintf(stderr,"  [GPU] NaN scan incomplete at %08X (unreadable)\n",begin*4);
                return;
            }
        }
        at=i-begin;
#endif
        if (ram[at] != NAN_NEG || ram[at + 1] != NAN_NEG || ram[at + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[at + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        if (!pb_diagnostic_read(va,&v,sizeof(v))) {
            fprintf(stderr,"  [PEEK] 0x%08X = unavailable (unreadable)\n",va);
            continue;
        }
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            uint32_t value;
            if (!pb_diagnostic_read(cur,&value,sizeof(value))) {
                fprintf(stderr,"  [CHAIN] step %d: 0x%08X unreadable -- chain ends\n",step,cur);
                return;
            }
            cur = value + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur <= 0x04000000u - 4) {
        uint32_t value;
        if (pb_diagnostic_read(cur,&value,sizeof(value)))
            fprintf(stderr,"  [CHAIN] final value at 0x%08X = 0x%08X\n",cur,value);
        else fprintf(stderr,"  [CHAIN] final value at 0x%08X unavailable (unreadable)\n",cur);
    }
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] brightest pixel written 0x%08X\n", s_gpu.pixel_max);
    fprintf(stderr, "[GPU] %llu pixels written; draw surface 0x%08X"
                    " -> 0x%08X, clear surface 0x%08X -> 0x%08X\n",
            (unsigned long long)s_gpu.pixels, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset,
            dma_resolve(s_gpu.color_offset));
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface; %u pushbuffer CALLs followed;"
                    " %u batches / %u vertices through vertex programs\n"
                    "[GPU] vertex-program triangles: %u drawn, rejected %u bad position, %u zero area, %u culled, %u off surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen, g_nv2a_pb_calls, s_vp.batches, s_vp.verts,
            s_vp.drawn, s_vp.rej_bad, s_vp.rej_area, s_vp.rej_cull, s_vp.rej_clip);

    /* And of the batches that did rasterise, how many sampled anything. A menu
     * that draws its background from one texture and its text from another
     * shows both as flat colour if either half is missing, so the split is
     * what says which half. */
    fprintf(stderr, "[GPU] batches: %u textured, %u with no texcoords,"
                    " %u with texcoords but no usable stage\n",
            s_gpu.batches_textured, s_gpu.batches_no_uv, s_gpu.batches_no_tex);
    for (i = 0; i < s_tex_use_count; i++)
        fprintf(stderr, "  [TEXUSE] 0x%08X %ux%u fmt 0x%02X%s: %u batches\n",
                s_tex_use[i].offset, s_tex_use[i].width, s_tex_use[i].height,
                s_tex_use[i].color,
                d3d8_format_dxt_block_bytes(s_tex_use[i].color) ? " dxt"
                    : d3d8_format_is_swizzled(s_tex_use[i].color) ? " swz" : " lin",
                s_tex_use[i].batches);

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }    fflush(stderr);
}

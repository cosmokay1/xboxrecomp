/*
 * NV2A MMIO Hook - VEH / POSIX Signal instruction decoder for GPU register access
 * Decodes x86-64 MOV/CMP/TEST/OR/AND instructions that access NV2A MMIO registers,
 * routes them through the NV2A register handlers, and advances RIP.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include <stdio.h>
#include <string.h>

/* NV2A MMIO base in Xbox VA space */
#define NV2A_MMIO_BASE  0xFD000000u
#define NV2A_MMIO_SIZE  0x01000000u  /* 16MB */
#define NV2A_VRAM_BASE  0xF0000000u
#define NV2A_VRAM_SIZE  (64 * 1024 * 1024)  /* 64MB Xbox VRAM */
#define NV2A_RAMIN_SIZE (1 * 1024 * 1024)   /* 1MB RAMIN */

static ptrdiff_t g_mem_offset = 0;
static uint8_t *g_nv2a_vram = NULL;

/* Statistics */
static int g_mmio_read_count = 0;
static int g_mmio_write_count = 0;
static int g_mmio_decode_fail = 0;

#if defined(_WIN32)
/* ============================================================
 * Windows x86-64 register access helpers
 * ============================================================ */
typedef PCONTEXT Nv2aCpuContext;

static uint64_t *ctx_reg64(Nv2aCpuContext ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&ctx->Rax;
    case 1:  return (uint64_t *)&ctx->Rcx;
    case 2:  return (uint64_t *)&ctx->Rdx;
    case 3:  return (uint64_t *)&ctx->Rbx;
    case 4:  return (uint64_t *)&ctx->Rsp;
    case 5:  return (uint64_t *)&ctx->Rbp;
    case 6:  return (uint64_t *)&ctx->Rsi;
    case 7:  return (uint64_t *)&ctx->Rdi;
    case 8:  return (uint64_t *)&ctx->R8;
    case 9:  return (uint64_t *)&ctx->R9;
    case 10: return (uint64_t *)&ctx->R10;
    case 11: return (uint64_t *)&ctx->R11;
    case 12: return (uint64_t *)&ctx->R12;
    case 13: return (uint64_t *)&ctx->R13;
    case 14: return (uint64_t *)&ctx->R14;
    case 15: return (uint64_t *)&ctx->R15;
    default: return NULL;
    }
}
static uint64_t *ctx_rip(Nv2aCpuContext ctx)    { return (uint64_t *)&ctx->Rip; }
static uint32_t *ctx_eflags(Nv2aCpuContext ctx) { return (uint32_t *)&ctx->EFlags; }

#elif defined(__linux__) && defined(__x86_64__)
/* ============================================================
 * Linux POSIX ucontext_t x86-64 register access helpers
 * ============================================================ */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ucontext.h>
#include <sys/mman.h>
typedef ucontext_t *Nv2aCpuContext;

static uint64_t *ctx_reg64(Nv2aCpuContext ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RAX];
    case 1:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RCX];
    case 2:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RDX];
    case 3:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RBX];
    case 4:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RSP];
    case 5:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RBP];
    case 6:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RSI];
    case 7:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RDI];
    case 8:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R8];
    case 9:  return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R9];
    case 10: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R10];
    case 11: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R11];
    case 12: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R12];
    case 13: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R13];
    case 14: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R14];
    case 15: return (uint64_t *)&ctx->uc_mcontext.gregs[REG_R15];
    default: return NULL;
    }
}
static uint64_t *ctx_rip(Nv2aCpuContext ctx)    { return (uint64_t *)&ctx->uc_mcontext.gregs[REG_RIP]; }
static uint32_t *ctx_eflags(Nv2aCpuContext ctx) { return (uint32_t *)&ctx->uc_mcontext.gregs[REG_EFL]; }
#endif

#if defined(_WIN32) || (defined(__linux__) && defined(__x86_64__))

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3)
        return len;

    if ((rm & 7) == 4)
        len++; /* SIB byte */

    if (mod == 0) {
        if ((rm & 7) == 5) len += 4;
    } else if (mod == 1) {
        len += 1;
    } else if (mod == 2) {
        len += 4;
    }

    return len;
}

static bool decode_and_handle(Nv2aCpuContext ctx, uint32_t mmio_offset, int is_write)
{
    uint64_t *rip_ptr = ctx_rip(ctx);
    uint32_t *eflags_ptr = ctx_eflags(ctx);
    const uint8_t *ip = (const uint8_t *)(*rip_ptr);
    NV2AState *nv2a = nv2a_get_state();
    if (!nv2a) return false;
    (void)is_write;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0;
    int has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) {
            has_66 = 1;
            prefix_len++;
        } else if (b == 0xF2 || b == 0xF3) {
            prefix_len++;
        } else if (b >= 0x40 && b <= 0x4F) {
            rex = b;
            has_rex = 1;
            prefix_len++;
        } else {
            break;
        }
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);

        if (access_size == 1) val &= 0xFF;
        else if (access_size == 2) val &= 0xFFFF;
        else if (access_size == 4) val &= 0xFFFFFFFF;

        nv2a_mmio_write(nv2a, mmio_offset, val, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t *dest = ctx_reg64(ctx, reg);

        if (access_size == 1) {
            *dest = (*dest & ~0xFFULL) | (val & 0xFF);
        } else if (access_size == 2) {
            *dest = (*dest & ~0xFFFFULL) | (val & 0xFFFF);
        } else if (access_size == 4) {
            *dest = val & 0xFFFFFFFF;
        } else {
            *dest = val;
        }

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* MOV r/m32, imm32 (C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        const uint8_t *imm_ptr = opcode + 1 + modrm_len;
        uint32_t imm = *(const uint32_t *)imm_ptr;
        int imm_len = 4;

        nv2a_mmio_write(nv2a, mmio_offset, imm, access_size);
        *rip_ptr += prefix_len + 1 + modrm_len + imm_len;
        g_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (C6 /0) */
    if (opcode[0] == 0xC6) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);

        nv2a_mmio_write(nv2a, mmio_offset, imm, 1);
        *rip_ptr += prefix_len + 1 + modrm_len + 1;
        g_mmio_write_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 1) & 0xFF;
        *ctx_reg64(ctx, reg) = val;

        *rip_ptr += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t val = nv2a_mmio_read(nv2a, mmio_offset, 2) & 0xFFFF;
        *ctx_reg64(ctx, reg) = val;

        *rip_ptr += prefix_len + 2 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);

        if (access_size == 1) { mem_val &= 0xFF; reg_val &= 0xFF; }
        else if (access_size == 2) { mem_val &= 0xFFFF; reg_val &= 0xFFFF; }
        else if (access_size == 4) { mem_val &= 0xFFFFFFFF; reg_val &= 0xFFFFFFFF; }

        uint64_t result = mem_val & reg_val;
        *eflags_ptr &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
        if (result == 0) *eflags_ptr |= 0x0040u;
        if (result & (1ULL << (access_size * 8 - 1))) *eflags_ptr |= 0x0080u;

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        uint64_t result = mem_val - reg_val;
        *eflags_ptr &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
        if (result == 0) *eflags_ptr |= 0x0040u;
        if (mem_val < reg_val) *eflags_ptr |= 0x0001u;
        if (result & (1ULL << (access_size * 8 - 1))) *eflags_ptr |= 0x0080u;

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* CMP r, r/m (3A/3B) */
    if (opcode[0] == 0x3B || opcode[0] == 0x3A) {
        if (opcode[0] == 0x3A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }

        uint64_t result = reg_val - mem_val;
        *eflags_ptr &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
        if (result == 0) *eflags_ptr |= 0x0040u;
        if (reg_val < mem_val) *eflags_ptr |= 0x0001u;
        if (result & (1ULL << (access_size * 8 - 1))) *eflags_ptr |= 0x0080u;

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        nv2a_mmio_write(nv2a, mmio_offset, mem_val | reg_val, access_size);

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);

        uint64_t mem_val = nv2a_mmio_read(nv2a, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        nv2a_mmio_write(nv2a, mmio_offset, mem_val & reg_val, access_size);

        *rip_ptr += prefix_len + 1 + modrm_len;
        g_mmio_write_count++;
        return true;
    }

    g_mmio_decode_fail++;
    if (g_mmio_decode_fail <= 20) {
        fprintf(stderr, "[NV2A] MMIO decode fail at RIP=%p: %02X %02X %02X %02X %02X %02X\n",
                (void *)(*rip_ptr), ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}

void nv2a_hook_init(ptrdiff_t xbox_mem_offset)
{
    g_mem_offset = xbox_mem_offset;

#if defined(_WIN32)
    g_nv2a_vram = (uint8_t *)VirtualAlloc(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *mapped = mmap(NULL, NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_nv2a_vram = (mapped != MAP_FAILED) ? (uint8_t *)mapped : NULL;
#endif
    if (!g_nv2a_vram) {
        fprintf(stderr, "[NV2A] Failed to allocate %u MB for VRAM!\n",
                (NV2A_VRAM_SIZE + NV2A_RAMIN_SIZE) / (1024 * 1024));
        return;
    }

    uint8_t *ramin_ptr = g_nv2a_vram + NV2A_VRAM_SIZE;
    nv2a_init_standalone(g_nv2a_vram, NV2A_VRAM_SIZE, ramin_ptr, NV2A_RAMIN_SIZE);

    fprintf(stderr, "[NV2A] MMIO hook initialized: VRAM=%p RAMIN=%p\n",
            (void *)g_nv2a_vram, (void *)ramin_ptr);
}

bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{
    (void)fault_addr;
    uint32_t mmio_offset = fault_xbox_va - NV2A_MMIO_BASE;
    return decode_and_handle((Nv2aCpuContext)ctx, mmio_offset, is_write);
}

bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{
    (void)fault_xbox_va;
    uintptr_t alloc_base = fault_addr & ~(uintptr_t)0xFFFF;
#if defined(_WIN32)
    LPVOID result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!result) {
        result = VirtualAlloc((LPVOID)alloc_base, 0x10000,
                              MEM_COMMIT, PAGE_READWRITE);
    }
    if (result) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
#else
    void *result = mmap((void *)alloc_base, 0x10000,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (result != MAP_FAILED) {
        memset(result, 0, 0x10000);
        return true;
    }
    return false;
#endif
}

#else
void nv2a_hook_init(ptrdiff_t xbox_mem_offset) { (void)xbox_mem_offset; }
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write)
{ (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write; return false; }
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va)
{ (void)fault_addr; (void)fault_xbox_va; return false; }
#endif

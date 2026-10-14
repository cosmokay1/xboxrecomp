#define _GNU_SOURCE
/* Include the real implementation to exercise its private arming boundary.
 * Link-time section collection discards unrelated XBE/runtime entry points. */
#include "../../src/kernel/xbox_memory_layout.c"
#include <assert.h>

static volatile sig_atomic_t writes, steps;
static void dispatch(int number, siginfo_t *info, void *context)
{
    if (!xbox_ac97_handle_posix_signal(number,info->si_code,(uintptr_t)info->si_addr,context))
        _exit(90);
    if (number==SIGSEGV) writes++; else if(number==SIGTRAP) steps++;
}

int main(void)
{
    struct sigaction action={0};action.sa_sigaction=dispatch;action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGSEGV,&action,NULL)==0);assert(sigaction(SIGTRAP,&action,NULL)==0);
    size_t length=AC97_NABM_OFFSET+AC97_TRAP_BYTES;
    g_mcpx_memory=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(g_mcpx_memory!=MAP_FAILED);
    assert(!ac97_arm_write_trap()); /* Both dispatchers must be registered explicitly. */
    xbox_ac97_set_posix_handler_ready(TRUE);assert(ac97_arm_write_trap());
    volatile uint8_t *registers=(volatile uint8_t *)g_ac97_page;
    for(unsigned i=0;i<100;i++) {
        registers[0x10B]=0x03;
        assert(registers[0x10B]==0x01); /* Reset clears; run/pause is retained. */
        registers[0x16B]=0x07;
        assert(registers[0x16B]==0x05); /* Every modeled channel, including DDS9's. */
        registers[0x108]=0xA5;
        assert(registers[0x108]==0xA5); /* Unrelated control bytes retain exact writes. */
    }
    *(volatile uint32_t *)(registers+0x108)=0x03020100u;
    assert(*(volatile uint32_t *)(registers+0x108)==0x01020100u);
    assert(writes==301 && steps==301);assert(!s_ac97_stepping);
    ucontext_t context={0};
    assert(!xbox_ac97_handle_posix_signal(SIGSEGV,SEGV_ACCERR,1,&context));
    context.uc_mcontext.gregs[REG_ERR]=2;
    context.uc_mcontext.gregs[REG_EFL]=0x100u;
    assert(!xbox_ac97_handle_posix_signal(SIGSEGV,SEGV_ACCERR,(uintptr_t)g_ac97_page,&context));
    assert(!s_ac97_stepping && (context.uc_mcontext.gregs[REG_EFL]&0x100u));
    s_ac97_stepping=1;
    assert(!xbox_ac97_handle_posix_signal(SIGTRAP,SI_USER,0,&context));
    assert(s_ac97_stepping==1);s_ac97_stepping=0;
    g_ac97_page=NULL;assert(munmap(g_mcpx_memory,length)==0);g_mcpx_memory=NULL;
    assert(!xbox_ac97_handle_posix_signal(SIGTRAP,TRAP_TRACE,0,&context));
    puts("Real Linux AC97 fault/write/single-step/reset/protection cycles verified");
    return 0;
}

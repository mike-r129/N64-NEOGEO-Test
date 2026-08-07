#include "m64k.h"
#include "m64k_internal.h"
#include "cycles.h"
#include <libdragon.h>
#include <string.h>
#include "tlb.h"

#if M64K_CONFIG_LOG_EXCEPTIONS
#define logexcf(...)    debugf(__VA_ARGS__)
#else
#define logexcf(...)    ({ })
#endif

#define SR_T0   0x8000  // Trace 0
#define SR_T1   0x4000  // Trace 1
#define SR_S    0x2000  // Supervisor mode
#define SR_M    0x1000  // Master/interrupt state
#define SR_INT  0x0700  // Interrupt mask

// Convert a m68k address (24 bit) to a pointer in the N64 memory space
#define M64K_PTR(a)   ((void*)(((a) & 0x00FFFFFF) + M64K_CONFIG_MEMORY_BASE))

#define RM16(a)     (*(  uint16_t*)M64K_PTR(a))
#define RM32(a)     (*(u_uint32_t*)M64K_PTR(a))
#define WM32(a, v)  (*(u_uint32_t*)M64K_PTR(a) = (v))
#define WM16(a, v)  (*(  uint16_t*)M64K_PTR(a) = (v))

extern int _m64k_asmrun(m64k_t *m64k, int ncycles);

// Live context pointer for the application's TLB/MMIO exception handler
// (hw_n64.S), which maintains ts_cur and implements forced slice exits now
// that the interpreter's main loop no longer polls per-instruction.
m64k_t *__m64k_live;

void __m64k_assert_invalid_opcode(uint16_t opcode, uint32_t pc) {
    assertf(0, "Invalid opcode: %04x @ %08lx", opcode, pc);
}

void __m64k_assert_invalid_opmode(uint16_t opcode, uint32_t pc) {
    assertf(0, "Invalid opmode: %04x @ %08lx", opcode, pc);
}

void __m64k_assert_invalid_ea(uint16_t opcode, uint32_t pc) {
    assertf(0, "Invalid EA: %04x @ %08lx", opcode, pc);
}

void __m64k_assert_privilege_violation(uint16_t opcode, uint32_t pc) {
    assertf(0, "unimplemented: privilege violation error: %04x @ %08lx", opcode, pc);
}

static inline void exc_push32(m64k_t *m64k, uint32_t v)
{
    m64k->ssp -= 4;
    WM32(m64k->ssp, v);
}

static inline void exc_push16(m64k_t *m64k, uint16_t v)
{
    m64k->ssp -= 2;
    WM16(m64k->ssp, v);
}

#ifdef M64K_PREDECODE
/* Phase-1a predecode scaffold (see PLAN-OPTIMIZATION.md, 60fps campaign):
 * a 4096-entry L1 table maps every 4KB guest page to a record block of one
 * 4-byte record per guest word {s16 handler offset from main_loop, u16
 * operand}. Entries are PRE-BIASED so the asm dispatch computes the record
 * address as L1[page] + (m_pc << 1) in 32-bit arithmetic — m_pc carries the
 * 0xFF000000 memory-map base, and the bias cancels it mod 2^32. In phase 1a
 * no page is ever promoted: every entry points (with its own bias) at ONE
 * shared trampoline block whose every record targets classic_dispatch_body,
 * so behavior is provably identical to the classic optable dispatch. */
uint32_t __m64k_pd_l1[4096] __attribute__((aligned(16)));
static uint16_t pd_trampoline[2048 * 2] __attribute__((aligned(16)));
extern char main_loop[], classic_dispatch_body[];

static void __m64k_predecode_init(void)
{
    int32_t off = (int32_t)((uint32_t)(uintptr_t)classic_dispatch_body
                - (uint32_t)(uintptr_t)main_loop);
    assertf(off >= -32768 && off <= 32767,
            "predecode handler offset out of s16 range: %ld", (long)off);
    for (int i = 0; i < 2048; i++) {
        pd_trampoline[i * 2 + 0] = (uint16_t)(int16_t)off;
        pd_trampoline[i * 2 + 1] = 0;
    }
    for (uint32_t page = 0; page < 4096; page++) {
        uint32_t gbase = (uint32_t)M64K_CONFIG_MEMORY_BASE | (page << 12);
        __m64k_pd_l1[page] =
            (uint32_t)(uintptr_t)pd_trampoline - (gbase << 1);
    }
}
#endif

void m64k_init(m64k_t *m64k)
{
    memset(m64k, 0, sizeof(*m64k));
    m64k->sr = 0x2700;
    __m64k_tlb_reset(); // FIXME: this clears all TLB entries
    #ifdef M64K_PREDECODE
    __m64k_predecode_init();
    #endif
}

void m64k_pulse_reset(m64k_t *m64k)
{
    m64k->sr  = 0x2700;
    m64k->ssp = RM32(0);
    m64k->pc  = RM32(4);
}

void m64k_exception_address(m64k_t *m64k, uint32_t address, uint16_t fc)
{
    uint32_t oldsr = m64k->sr;

    m64k->sr &= ~(SR_T0 | SR_T1);
    m64k->sr |= SR_S;

    exc_push32(m64k, m64k->pc);  // cdef
    exc_push16(m64k, oldsr);     // ab
    exc_push16(m64k, m64k->ir);  // 89
    exc_push32(m64k, address);   // 4567
    exc_push16(m64k, fc);        // 23

    m64k->pc = RM32(m64k->vbr + 0x3*4);
    m64k->cycles += __m64k_exception_cycle_table[0x3];
}

void m64k_exception_divbyzero(m64k_t *m64k)
{
    uint32_t oldsr = m64k->sr;

    m64k->sr &= ~(SR_T0 | SR_T1);
    m64k->sr |= SR_S;

    exc_push32(m64k, m64k->pc);
    exc_push16(m64k, oldsr);

    m64k->pc = RM32(m64k->vbr + 0x5*4);
    m64k->cycles += __m64k_exception_cycle_table[0x5];
}

// Generic group-2 exception (short-format frame: push PC then old SR).
// Used for illegal instruction (vec 4), CHK (vec 6) and privilege
// violation (vec 8). The asm side sets m64k->pc to the value the 68000
// stacks for that exception (faulting instruction for illegal/privilege,
// next instruction for CHK).
void m64k_exception_group2(m64k_t *m64k, int vector)
{
    uint32_t oldsr = m64k->sr;

    m64k->sr &= ~(SR_T0 | SR_T1);
    m64k->sr |= SR_S;

    exc_push32(m64k, m64k->pc);
    exc_push16(m64k, oldsr);

    m64k->pc = RM32(m64k->vbr + vector*4);
    m64k->cycles += __m64k_exception_cycle_table[vector];
}

void m64k_exception_interrupt(m64k_t *m64k, int level)
{
    if (m64k->hook_irqack) {
        m64k->hook_irqack(m64k->hook_irqack_ctx, level);
    } else {
        // Auto-ack the interrupt for simpler cases
        if (m64k->virq & (1<<(level-1)))
            m64k_set_virq(m64k, level, false);
        else
            m64k_set_irq(m64k, 0);
    }

    uint32_t oldsr = m64k->sr;

    m64k->sr &= ~(SR_T0 | SR_T1 | SR_INT);
    m64k->sr |= SR_S | (level << 8);

    uint32_t pc = RM32(m64k->vbr + (24 + level)*4);
    if (pc == 0)
        pc = RM32(m64k->vbr + 15*4);

    exc_push32(m64k, m64k->pc);
    exc_push16(m64k, oldsr);

    m64k->pc = pc;
    m64k->cycles += __m64k_exception_cycle_table[24 + level];
}

int64_t m64k_run(m64k_t *m64k, int64_t until)
{
    __m64k_live = m64k;
    while (until > m64k->cycles) {
        int timeslice = until - m64k->cycles;
        int remaining = _m64k_asmrun(m64k, timeslice);
        if (__builtin_expect(m64k->forced_remaining != 0, 0)) {
            // A forced slice exit (slice_break / reload_sr clamp) banked the
            // cycle counter here; add it back so the guest clock stays exact.
            remaining += m64k->forced_remaining;
            m64k->forced_remaining = 0;
        }
        m64k->cycles += timeslice - remaining;

        if (__builtin_expect(m64k->pending_exc[0] != 0, 0)) {
            switch (m64k->pending_exc[0]) {
            #if M64K_CONFIG_ADDRERR
            case M64K_PENDINGEXC_ADDRERR:
                logexcf("[m64k] address error\n");
                m64k_exception_address(m64k, m64k->pending_exc[1], m64k->pending_exc[2]);
                break;
            #endif
            case M64K_PENDINGEXC_RSTO:
                logexcf("[m64k] RSTO asserted\n");
                break;
            case M64K_PENDINGEXC_DIVBYZERO:
                logexcf("[m64k] division by zero\n");
                m64k_exception_divbyzero(m64k);
                break;
            case M64K_PENDINGEXC_IRQ:
                m64k_exception_interrupt(m64k, m64k->pending_exc[1]);
                break;
            case M64K_PENDINGEXC_ILLEGAL:
                logexcf("[m64k] ILLEGAL instruction %04x @ pc=%06lx\n",
                        (unsigned)m64k->pending_exc[1], (unsigned long)(m64k->pc & 0xFFFFFF));
                m64k_exception_group2(m64k, 4);
                break;
            case M64K_PENDINGEXC_CHK:
                logexcf("[m64k] CHK trap @ pc=%06lx\n", (unsigned long)(m64k->pc & 0xFFFFFF));
                m64k_exception_group2(m64k, 6);
                break;
            case M64K_PENDINGEXC_PRIVERR:
                logexcf("[m64k] PRIVERR @ pc=%06lx\n", (unsigned long)(m64k->pc & 0xFFFFFF));
                m64k_exception_group2(m64k, 8);
                break;
            default:
                assertf(0, "Unhandled pending exception: %ld", m64k->pending_exc[0]);
            }
            m64k->pending_exc[0] = 0;
        }
    }

    return m64k->cycles;
}

void m64k_set_irq(m64k_t *m64k, int level)
{
    if (level == 7 && m64k->ipl < 7) {
        m64k->nmi_pending = 1;
    }
    m64k->ipl = level;
    m64k->slice_break = 1;
}

void m64k_set_virq(m64k_t *m64k, int irq, bool on)
{
    assertf(irq > 0 && irq <= 7, "Invalid IRQ: %d", irq);
    if (on)
        m64k->virq |= 1 << (irq-1);
    else
        m64k->virq &= ~(1 << (irq-1));

    int i;
    for (i=7; i>0; i--) {
        if (m64k->virq & (1 << (i-1)))
            break;
    }
    m64k_set_irq(m64k, i);
}

uint32_t m64k_get_pc(m64k_t *m64k)
{
    // FIXME: fix while m68k is running
    return m64k->pc;
}

int64_t m64k_get_clock(m64k_t *m64k)
{
    if (!m64k->ts_start)
        return m64k->cycles;
    return m64k->cycles + (m64k->ts_start - m64k->ts_cur);
}

void m64k_run_stop(m64k_t *m64k)
{
    m64k->slice_break = 1;
}

void m64k_set_hook_irqack(m64k_t *m64k, int (*hook)(void *ctx, int level), void *ctx)
{
    m64k->hook_irqack = hook;
    m64k->hook_irqack_ctx = ctx;
}

m64k_mapping_t m64k_map_memory(m64k_t *m64k, uint32_t address, uint32_t size, void *ptr, bool writable)
{
    assertf(address < 0x1000000, "address must be in the 24-bit range");
    assertf((size & (size-1)) == 0, "size must be a power of 2");
    assertf(size >= 0x1000, "size must be at least 4 KiB");

    int flags = TLBF_OVERWRITE;
    if (!writable)
        flags |= TLBF_READONLY;

    void *virt = (void*)(M64K_CONFIG_MEMORY_BASE | address);
    uint32_t phys = PhysicalAddr(ptr);
    return __m64k_tlb_add(virt, size-1, phys, flags);
}

void m64k_map_memory_change(m64k_t *m64k, m64k_mapping_t mapping, void *ptr, bool writable)
{
    __m64k_tlb_change(mapping, PhysicalAddr(ptr), writable ? 0 : TLBF_READONLY);
}

void m64k_unmap_memory(m64k_t *m64k, m64k_mapping_t mapping)
{
    __m64k_tlb_rem(mapping);
}

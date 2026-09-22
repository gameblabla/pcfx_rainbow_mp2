#include <pcfx/types.h>
#include <pcfx/v810.h>
#include <pcfx/timer.h>
#include "psg_sample.h"

#define TIMER_PORT_CONTROL    0xF00
#define TIMER_PORT_PERIOD     0xF80

kjmp2v_psg10_sample_t g_mp2psg10_ring[PSG10MP2_RING_SIZE] __attribute__((aligned(4)));
volatile uint32_t g_mp2psg10_read_pos;
volatile uint32_t g_mp2psg10_write_pos;
volatile uint32_t g_mp2psg10_playing;
volatile uint32_t g_mp2psg10_stereo;
volatile uint32_t g_mp2psg10_decode_done;
volatile uint32_t g_mp2psg10_irq_samples_emitted;
volatile uint32_t g_mp2psg10_underflows;
volatile uint32_t g_mp2psg10_timer_period_base;
volatile uint32_t g_mp2psg10_timer_period_frac;
volatile uint32_t g_mp2psg10_timer_period_accum;

static uint8_t g_psg10_high_volume = PSG10_DEFAULT_HIGH_VOL;
static uint8_t g_psg10_low_volume  = PSG10_DEFAULT_LOW_VOL;

static inline void io_out16(uint32_t port, uint32_t data)
{
    __asm__ volatile ("out.h %0, 0[%1]" :: "r"(data), "r"(port) : "memory");
}
static inline uint32_t io_in16(uint32_t port)
{
    uint32_t v;
    __asm__ volatile ("in.h 0[%1], %0" : "=r"(v) : "r"(port) : "memory");
    return v;
}

void PSG10MP2_SetPairVolumes(uint8_t high_volume, uint8_t low_volume)
{
    if (high_volume > 31u) high_volume = 31u;
    if (low_volume > 31u) low_volume = 31u;
    g_psg10_high_volume = high_volume;
    g_psg10_low_volume = low_volume;
}

void PSG10MP2_RingReset(void)
{
    g_mp2psg10_read_pos = 0;
    g_mp2psg10_write_pos = 0;
    g_mp2psg10_playing = 0;
    g_mp2psg10_stereo = 0;
    g_mp2psg10_decode_done = 0;
    g_mp2psg10_irq_samples_emitted = 0;
    g_mp2psg10_underflows = 0;
    g_mp2psg10_timer_period_accum = 0;
}

uint32_t PSG10MP2_RingUsed(void) { return g_mp2psg10_write_pos - g_mp2psg10_read_pos; }
uint32_t PSG10MP2_RingFree(void) { return (PSG10MP2_RING_SIZE - 1u) - PSG10MP2_RingUsed(); }

/* 1152 u32 PSG ticks.  Optimized for frame-sized producer: compute
 * wrap once and copy one or two contiguous spans.  Uses only caller-scratch
 * r10..r15 plus r6 argument; do not clobber r16+.
 */
__asm__(
".section .text.PSG10MP2_RingPushFrame, \"ax\"\n"
".align 4\n"
".global _PSG10MP2_RingPushFrame\n"
".type _PSG10MP2_RingPushFrame, @function\n"
"_PSG10MP2_RingPushFrame:\n"
"    movhi hi(_g_mp2psg10_write_pos), r0, r10\n"
"    ld.w lo(_g_mp2psg10_write_pos)[r10], r11\n"
"    movhi hi(_g_mp2psg10_ring), r0, r12\n"
"    movea lo(_g_mp2psg10_ring), r12, r12\n"
"    mov r11, r13\n"
"    andi 0x7FFF, r13, r13\n"
"    mov r13, r14\n"
"    shl 2, r14\n"
"    add r12, r14\n"
"    movea 31615, r0, r15\n"
"    cmp r15, r13\n"
"    bnh .Lring_no_wrap2\n"
"    movhi 1, r0, r13\n"
"    shr 1, r13\n"
"    mov r11, r15\n"
"    andi 0x7FFF, r15, r15\n"
"    sub r15, r13\n"
"    br .Lring_count_ready2\n"
".Lring_no_wrap2:\n"
"    movea 1152, r0, r13\n"
".Lring_count_ready2:\n"
"    mov r13, r15\n"
".Lring_copy_first2:\n"
"    ld.w 0[r6], r10\n"
"    st.w r10, 0[r14]\n"
"    addi 4, r6, r6\n"
"    addi 4, r14, r14\n"
"    add 1, r11\n"
"    add -1, r15\n"
"    bne .Lring_copy_first2\n"
"    movea 1152, r0, r15\n"
"    sub r13, r15\n"
"    cmp 0, r15\n"
"    be .Lring_store_pos2\n"
"    mov r12, r14\n"
".Lring_copy_second2:\n"
"    ld.w 0[r6], r10\n"
"    st.w r10, 0[r14]\n"
"    addi 4, r6, r6\n"
"    addi 4, r14, r14\n"
"    add 1, r11\n"
"    add -1, r15\n"
"    bne .Lring_copy_second2\n"
".Lring_store_pos2:\n"
"    movhi hi(_g_mp2psg10_write_pos), r0, r10\n"
"    st.w r11, lo(_g_mp2psg10_write_pos)[r10]\n"
"    jmp lp\n"
".size _PSG10MP2_RingPushFrame, . - _PSG10MP2_RingPushFrame\n"
".previous\n"
);

void PSG10MP2_SetDecodeDone(void) { g_mp2psg10_decode_done = 1; }

static inline void psg_setup_pair(uint32_t high_ch, uint32_t low_ch, uint32_t balance)
{
    const uint32_t hv = g_psg10_high_volume;
    const uint32_t lv = g_psg10_low_volume;
    __asm__ volatile (
        "out.b %0, 0x100[r0]\n"
        "ori 192, %3, r11\n"
        "out.b r11, 0x108[r0]\n"
        "out.b %2, 0x10A[r0]\n"
        "mov 0, r11\n"
        "out.b r11, 0x104[r0]\n"
        "out.b r11, 0x106[r0]\n"
        "out.b r11, 0x10E[r0]\n"
        "out.b %1, 0x100[r0]\n"
        "ori 192, %4, r11\n"
        "out.b r11, 0x108[r0]\n"
        "out.b %2, 0x10A[r0]\n"
        "mov 0, r11\n"
        "out.b r11, 0x104[r0]\n"
        "out.b r11, 0x106[r0]\n"
        "out.b r11, 0x10E[r0]\n"
        : : "r"(high_ch), "r"(low_ch), "r"(balance), "r"(hv), "r"(lv) : "r11", "memory"
    );
}


static inline void timer_start_direct(void)
{
    timer_start(1);
}

void PSG10MP2_StartMono(void)
{
    __asm__ volatile ("movea 255, r0, r10\n out.b r10, 0x102[r0]" ::: "r10", "memory");
    psg_setup_pair(PSG10_MONO_HIGH_CHANNEL, PSG10_MONO_LOW_CHANNEL, 0xFFu);
    g_mp2psg10_stereo = 0;
    g_mp2psg10_playing = 1;
    timer_start_direct();
}

void PSG10MP2_StartStereo(void)
{
    __asm__ volatile ("movea 255, r0, r10\n out.b r10, 0x102[r0]" ::: "r10", "memory");
    psg_setup_pair(PSG10_ST_L_HIGH_CHANNEL, PSG10_ST_L_LOW_CHANNEL, 0xF0u);
    psg_setup_pair(PSG10_ST_R_HIGH_CHANNEL, PSG10_ST_R_LOW_CHANNEL, 0x0Fu);
    g_mp2psg10_stereo = 1;
    g_mp2psg10_playing = 1;
    timer_start_direct();
}

void PSG10MP2_Stop(void) { g_mp2psg10_playing = 0; }

__asm__(
".section .text.samplepsg_timer_irq, \"ax\"\n"
".align 4\n"
".global _samplepsg_timer_irq\n"
".type _samplepsg_timer_irq, @function\n"
"_samplepsg_timer_irq:\n"
"    addi -28, sp, sp\n"
"    st.w r10, 0[sp]\n"
"    st.w r11, 4[sp]\n"
"    st.w r12, 8[sp]\n"
"    st.w r13, 12[sp]\n"
"    st.w r14, 16[sp]\n"
"    st.w r15, 20[sp]\n"
"    st.w r16, 24[sp]\n"
"    in.h 0xF00[r0], r10\n"
"    andi 0xFFFB, r10, r10\n"
"    out.h r10, 0xF00[r0]\n"
"    movhi hi(_g_mp2psg10_timer_period_frac), r0, r15\n"
"    ld.w lo(_g_mp2psg10_timer_period_frac)[r15], r14\n"
"    cmp 0, r14\n"
"    be .Ltimer_frac_done\n"
"    movhi hi(_g_mp2psg10_timer_period_accum), r0, r15\n"
"    ld.w lo(_g_mp2psg10_timer_period_accum)[r15], r13\n"
"    add r14, r13\n"
"    mov r13, r16\n"
"    shr 16, r16\n"
"    andi 0xFFFF, r13, r13\n"
"    st.w r13, lo(_g_mp2psg10_timer_period_accum)[r15]\n"
"    movhi hi(_g_mp2psg10_timer_period_base), r0, r15\n"
"    ld.w lo(_g_mp2psg10_timer_period_base)[r15], r14\n"
"    add r16, r14\n"
"    out.h r14, 0xF80[r0]\n"
".Ltimer_frac_done:\n"
"    movhi hi(_g_mp2psg10_playing), r0, r11\n"
"    ld.w lo(_g_mp2psg10_playing)[r11], r10\n"
"    cmp 0, r10\n"
"    be .Lirq_done\n"
"    movhi hi(_g_mp2psg10_read_pos), r0, r12\n"
"    movhi hi(_g_mp2psg10_write_pos), r0, r13\n"
"    ld.w lo(_g_mp2psg10_read_pos)[r12], r10\n"
"    ld.w lo(_g_mp2psg10_write_pos)[r13], r14\n"
"    cmp r14, r10\n"
"    bne .Lhave_sample\n"
"    movhi hi(_g_mp2psg10_decode_done), r0, r15\n"
"    ld.w lo(_g_mp2psg10_decode_done)[r15], r14\n"
"    cmp 0, r14\n"
"    be .Lunderflow\n"
"    mov 0, r14\n"
"    st.w r14, lo(_g_mp2psg10_playing)[r11]\n"
"    br .Lirq_done\n"
".Lunderflow:\n"
"    movhi hi(_g_mp2psg10_underflows), r0, r15\n"
"    ld.w lo(_g_mp2psg10_underflows)[r15], r14\n"
"    add 1, r14\n"
"    st.w r14, lo(_g_mp2psg10_underflows)[r15]\n"
"    mov 0, r14\n"
"    st.w r14, lo(_g_mp2psg10_playing)[r11]\n"
"    br .Lirq_done\n"
".Lhave_sample:\n"
"    movhi hi(_g_mp2psg10_ring), r0, r13\n"
"    movea lo(_g_mp2psg10_ring), r13, r13\n"
"    mov r10, r14\n"
"    andi 0x7FFF, r14, r14\n"
"    shl 2, r14\n"
"    add r14, r13\n"
"    ld.w 0[r13], r14\n"
"    add 1, r10\n"
"    st.w r10, lo(_g_mp2psg10_read_pos)[r12]\n"
"    mov 0, r15\n"
"    out.b r15, 0x100[r0]\n"
"    mov r14, r13\n"
"    shr 5, r13\n"
"    andi 31, r13, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    mov 1, r15\n"
"    out.b r15, 0x100[r0]\n"
"    andi 31, r14, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    movhi hi(_g_mp2psg10_stereo), r0, r16\n"
"    ld.w lo(_g_mp2psg10_stereo)[r16], r16\n"
"    cmp 0, r16\n"
"    be .Lcount_sample\n"
"    shr 16, r14\n"
"    mov 2, r15\n"
"    out.b r15, 0x100[r0]\n"
"    mov r14, r13\n"
"    shr 5, r13\n"
"    andi 31, r13, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    mov 3, r15\n"
"    out.b r15, 0x100[r0]\n"
"    andi 31, r14, r13\n"
"    out.b r13, 0x10C[r0]\n"
".Lcount_sample:\n"
".Lirq_done:\n"
"    ld.w 0[sp], r10\n"
"    ld.w 4[sp], r11\n"
"    ld.w 8[sp], r12\n"
"    ld.w 12[sp], r13\n"
"    ld.w 16[sp], r14\n"
"    ld.w 20[sp], r15\n"
"    ld.w 24[sp], r16\n"
"    addi 28, sp, sp\n"
"    reti\n"
".size _samplepsg_timer_irq, . - _samplepsg_timer_irq\n"
".previous\n"
);


__asm__(
".section .text.samplepsg_timer_irq_stereo, \"ax\"\n"
".align 4\n"
".global _samplepsg_timer_irq_stereo\n"
".type _samplepsg_timer_irq_stereo, @function\n"
"_samplepsg_timer_irq_stereo:\n"
"    addi -24, sp, sp\n"
"    st.w r10, 0[sp]\n"
"    st.w r11, 4[sp]\n"
"    st.w r12, 8[sp]\n"
"    st.w r13, 12[sp]\n"
"    st.w r14, 16[sp]\n"
"    st.w r15, 20[sp]\n"
"    in.h 0xF00[r0], r10\n"
"    andi 0xFFFB, r10, r10\n"
"    out.h r10, 0xF00[r0]\n"
"    movhi hi(_g_mp2psg10_timer_period_frac), r0, r15\n"
"    ld.w lo(_g_mp2psg10_timer_period_frac)[r15], r14\n"
"    cmp 0, r14\n"
"    be .Lst_timer_frac_done\n"
"    movhi hi(_g_mp2psg10_timer_period_accum), r0, r15\n"
"    ld.w lo(_g_mp2psg10_timer_period_accum)[r15], r13\n"
"    add r14, r13\n"
"    mov r13, r15\n"
"    shr 16, r15\n"
"    andi 0xFFFF, r13, r13\n"
"    movhi hi(_g_mp2psg10_timer_period_accum), r0, r14\n"
"    st.w r13, lo(_g_mp2psg10_timer_period_accum)[r14]\n"
"    movhi hi(_g_mp2psg10_timer_period_base), r0, r14\n"
"    ld.w lo(_g_mp2psg10_timer_period_base)[r14], r13\n"
"    add r15, r13\n"
"    out.h r13, 0xF80[r0]\n"
".Lst_timer_frac_done:\n"
"    movhi hi(_g_mp2psg10_playing), r0, r11\n"
"    ld.w lo(_g_mp2psg10_playing)[r11], r10\n"
"    cmp 0, r10\n"
"    be .Lst_done\n"
"    movhi hi(_g_mp2psg10_read_pos), r0, r12\n"
"    movhi hi(_g_mp2psg10_write_pos), r0, r13\n"
"    ld.w lo(_g_mp2psg10_read_pos)[r12], r10\n"
"    ld.w lo(_g_mp2psg10_write_pos)[r13], r14\n"
"    cmp r14, r10\n"
"    bne .Lst_have\n"
"    movhi hi(_g_mp2psg10_decode_done), r0, r15\n"
"    ld.w lo(_g_mp2psg10_decode_done)[r15], r14\n"
"    cmp 0, r14\n"
"    be .Lst_under\n"
"    mov 0, r14\n"
"    st.w r14, lo(_g_mp2psg10_playing)[r11]\n"
"    br .Lst_done\n"
".Lst_under:\n"
"    movhi hi(_g_mp2psg10_underflows), r0, r15\n"
"    ld.w lo(_g_mp2psg10_underflows)[r15], r14\n"
"    add 1, r14\n"
"    st.w r14, lo(_g_mp2psg10_underflows)[r15]\n"
"    mov 0, r14\n"
"    st.w r14, lo(_g_mp2psg10_playing)[r11]\n"
"    br .Lst_done\n"
".Lst_have:\n"
"    movhi hi(_g_mp2psg10_ring), r0, r13\n"
"    movea lo(_g_mp2psg10_ring), r13, r13\n"
"    mov r10, r14\n"
"    andi 0x7FFF, r14, r14\n"
"    shl 2, r14\n"
"    add r14, r13\n"
"    ld.w 0[r13], r14\n"
"    add 1, r10\n"
"    st.w r10, lo(_g_mp2psg10_read_pos)[r12]\n"
"    mov 0, r15\n"
"    out.b r15, 0x100[r0]\n"
"    mov r14, r13\n"
"    shr 5, r13\n"
"    andi 31, r13, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    mov 1, r15\n"
"    out.b r15, 0x100[r0]\n"
"    andi 31, r14, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    shr 16, r14\n"
"    mov 2, r15\n"
"    out.b r15, 0x100[r0]\n"
"    mov r14, r13\n"
"    shr 5, r13\n"
"    andi 31, r13, r13\n"
"    out.b r13, 0x10C[r0]\n"
"    mov 3, r15\n"
"    out.b r15, 0x100[r0]\n"
"    andi 31, r14, r13\n"
"    out.b r13, 0x10C[r0]\n"
".Lst_done:\n"
"    ld.w 0[sp], r10\n"
"    ld.w 4[sp], r11\n"
"    ld.w 8[sp], r12\n"
"    ld.w 12[sp], r13\n"
"    ld.w 16[sp], r14\n"
"    ld.w 20[sp], r15\n"
"    addi 24, sp, sp\n"
"    reti\n"
".size _samplepsg_timer_irq_stereo, . - _samplepsg_timer_irq_stereo\n"
".previous\n"
);


static inline void psg_write_dda(uint32_t ch, uint32_t val)
{
    __asm__ volatile(
        "out.b %0, 0x100[r0]\n"
        "out.b %1, 0x10C[r0]\n"
        : : "r"(ch), "r"(val) : "memory");
}

uint32_t PSG10MP2_ServiceTimerOnce(void)
{
    uint32_t cr = io_in16(TIMER_PORT_CONTROL);
    uint32_t rp, wp, s, c;
    if ((cr & 4u) == 0u) return 0;
    io_out16(TIMER_PORT_CONTROL, cr & ~4u);
    if (!g_mp2psg10_playing) return 1;
    rp = g_mp2psg10_read_pos;
    wp = g_mp2psg10_write_pos;
    if (rp == wp) {
        if (g_mp2psg10_decode_done) {
            g_mp2psg10_playing = 0;
            return 1;
        }
        g_mp2psg10_underflows++;
        psg_write_dda(0, 16u);
        psg_write_dda(1, 0u);
        if (g_mp2psg10_stereo) {
            psg_write_dda(2, 16u);
            psg_write_dda(3, 0u);
        }
        return 1;
    }
    s = g_mp2psg10_ring[rp & PSG10MP2_RING_MASK];
    g_mp2psg10_read_pos = rp + 1u;
    c = s & 0x03FFu;
    psg_write_dda(0, (c >> 5) & 31u);
    psg_write_dda(1, c & 31u);
    if (g_mp2psg10_stereo) {
        c = (s >> 16) & 0x03FFu;
        psg_write_dda(2, (c >> 5) & 31u);
        psg_write_dda(3, c & 31u);
    }
    return 1;
}

static inline void timer_stop_direct(void)
{
    uint32_t cr = io_in16(TIMER_PORT_CONTROL);
    cr &= ~1u;
    io_out16(TIMER_PORT_CONTROL, cr);
}

void PSG10MP2_StopTimer(void) { timer_stop_direct(); }

static void PSG10MP2_InitTimerWithHandler(int period, uint32_t frac_16_16, void (*handler)(void))
{
    irq_set_mask(0x7F);
    irq_set_raw_handler(0x9, handler);
    /* Unmask the timer only (mask bit 7 - source; timer = source 1).  0x37
       also unmasked VDC-A, whose pending IRQ ran a BIOS handler that left the
       RAINBOW black once interrupts were really enabled here. */
    irq_set_mask(0x3F);
    if (period <= 0) period = 90;
    g_mp2psg10_timer_period_base = (uint32_t)period;
    g_mp2psg10_timer_period_frac = frac_16_16;
    g_mp2psg10_timer_period_accum = 0;
    timer_init();
    timer_set_period(period);
    irq_set_level(8);
    irq_enable();
}

void PSG10MP2_InitTimer(int period)
{
    PSG10MP2_InitTimerWithHandler(period, 0u, samplepsg_timer_irq);
}

void PSG10MP2_InitTimerStereo(int period)
{
    PSG10MP2_InitTimerWithHandler(period, 0u, samplepsg_timer_irq_stereo);
}

void PSG10MP2_InitTimerFractional(int base_period, uint32_t frac_16_16)
{
    PSG10MP2_InitTimerWithHandler(base_period, frac_16_16, samplepsg_timer_irq);
}

void PSG10MP2_InitTimerFractionalStereo(int base_period, uint32_t frac_16_16)
{
    PSG10MP2_InitTimerWithHandler(base_period, frac_16_16, samplepsg_timer_irq_stereo);
}

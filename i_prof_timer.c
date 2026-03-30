/*
 * i_prof_timer.c -- High-resolution profiling timer using MFP Timer-A.
 *
 * Timer-A is documented as free for user programs on the X68000.
 * (Timer-B is used by Human68k for key repeat / cursor blink --
 * reprogramming it kills keyboard input.)
 *
 * Timer-A runs at 4 MHz / 4 = 1 MHz (1 us per tick).  The 8-bit down-
 * counter reloads from the data register on underflow.  We set the data
 * register to 0 (which the MFP treats as 256), so the counter counts
 * 256 -> 1 -> underflow -> reload 256, firing the ISR every 256 us.
 *
 * The ISR adds 256 to g_prof_overflow.  I_GetTimeUs() combines the
 * overflow accumulator with the current counter value for a full 32-bit
 * microsecond timestamp.
 *
 * Overhead: ~3900 ISRs/sec.  Each ISR is ~10 instructions on 68060 --
 * well under 0.5% of CPU time.  Acceptable for a profiling build.
 */

#ifdef DOOM_LOG  /* entire file is profiling-only */

#include "i_prof_timer.h"
#include "mfp.h"
#include "irq.h"
#include <stdint.h>

/* ---- MFP register accessors (byte-wide, directly addressed) ---- */
#define MFP_IERA_PTR   ((volatile uint8_t *)MFP_IERA)
#define MFP_IMRA_PTR   ((volatile uint8_t *)MFP_IMRA)
#define MFP_IPRA_PTR   ((volatile uint8_t *)MFP_IPRA)
#define MFP_ISRA_PTR   ((volatile uint8_t *)MFP_ISRA)
#define MFP_TACR_PTR   ((volatile uint8_t *)MFP_TACR)
#define MFP_TADR_PTR   ((volatile uint8_t *)MFP_TADR)

/* Timer-A interrupt is in IERA/IMRA bit 5. */
#define TA_IE_BIT      0x20

/* Timer-A prescaler values (TACR low 3 bits, same encoding as TBCR):
 *   0=stop  1=/4  2=/10  3=/16  4=/50  5=/64  6=/100  7=/200 */
#define TA_PRESCALE_4  0x01

/* Timer-A vector: IRQ_4D on X68000. */
#define TA_VEC_ADDR    ((volatile uint32_t *)IRQ_4D)

/* ---- State ---- */

/* Overflow accumulator -- incremented by 256 in the ISR. */
static volatile uint32_t g_prof_overflow;

/* Saved original Timer-A configuration for clean shutdown. */
static uint8_t  saved_tacr;
static uint8_t  saved_tadr;
static uint8_t  saved_iera_bit;
static uint8_t  saved_imra_bit;
static uint32_t saved_ta_vector;

/* ---- ISR ---- */

/* GCC interrupt_handler attribute generates RTE and saves/restores all
 * modified registers automatically. */
static void prof_timer_a_isr(void) __attribute__((interrupt_handler));
static void prof_timer_a_isr(void)
{
    g_prof_overflow += 256;
    /* Clear Timer-A in-service bit (ISRA bit 5) so lower-priority MFP
     * interrupts are not blocked. */
    *MFP_ISRA_PTR &= (uint8_t)~TA_IE_BIT;
}

/* ---- Public API ---- */

void I_ProfTimerInit(void)
{
    /* Save current state. */
    saved_tacr     = *MFP_TACR_PTR;
    saved_tadr     = *MFP_TADR_PTR;
    saved_iera_bit = *MFP_IERA_PTR & TA_IE_BIT;
    saved_imra_bit = *MFP_IMRA_PTR & TA_IE_BIT;
    saved_ta_vector = *TA_VEC_ADDR;

    /* Stop Timer-A while we reconfigure. */
    *MFP_TACR_PTR = 0x00;

    /* Install our ISR. */
    *TA_VEC_ADDR = (uint32_t)(uintptr_t)prof_timer_a_isr;

    /* Data register = 0 -> MFP treats as 256 (full 8-bit range). */
    *MFP_TADR_PTR = 0;

    /* Reset accumulator. */
    g_prof_overflow = 0;

    /* Clear any pending Timer-A interrupt. */
    *MFP_IPRA_PTR &= (uint8_t)~TA_IE_BIT;

    /* Enable Timer-A interrupt. */
    *MFP_IERA_PTR |= TA_IE_BIT;
    *MFP_IMRA_PTR |= TA_IE_BIT;

    /* Start Timer-A: prescaler /4 -> 4 MHz / 4 = 1 MHz (1 us/tick). */
    *MFP_TACR_PTR = TA_PRESCALE_4;
}

void I_ProfTimerShutdown(void)
{
    /* Stop Timer-A. */
    *MFP_TACR_PTR = 0x00;

    /* Restore original IERA/IMRA bits (only touch bit 5). */
    *MFP_IERA_PTR = (*MFP_IERA_PTR & (uint8_t)~TA_IE_BIT) | saved_iera_bit;
    *MFP_IMRA_PTR = (*MFP_IMRA_PTR & (uint8_t)~TA_IE_BIT) | saved_imra_bit;

    /* Restore original vector, data register, and control. */
    *TA_VEC_ADDR  = saved_ta_vector;
    *MFP_TADR_PTR = saved_tadr;
    *MFP_TACR_PTR = saved_tacr;
}

uint32_t I_GetTimeUs(void)
{
    uint32_t overflow;
    uint8_t  counter;
    uint8_t  pending;

    /* Briefly mask interrupts for an atomic read of overflow + counter.
     * On 68060 in supervisor mode: ORI/ANDI to SR is ~1 cycle each. */
    __asm__ volatile(
        "ori.w   #0x0700,%%sr\n\t"  /* mask all interrupts */
        : : : "cc"
    );

    overflow = g_prof_overflow;
    counter  = *MFP_TADR_PTR;

    /* If Timer-A interrupt is pending, the counter has already wrapped
     * but the ISR hasn't run yet.  Account for the missed overflow. */
    pending = *MFP_IPRA_PTR & TA_IE_BIT;
    if (pending) {
        overflow += 256;
        counter = *MFP_TADR_PTR;  /* re-read after wrap */
    }

    __asm__ volatile(
        "andi.w  #0xF8FF,%%sr\n\t"  /* unmask interrupts */
        : : : "cc"
    );

    /* Counter counts DOWN from 256 (loaded as 0) to 1, then wraps.
     * Elapsed ticks within current period = 256 - counter.
     * But when counter reads 0, it means 256 (MFP quirk: 0 == 256). */
    if (counter == 0)
        return overflow;  /* 256 - 256 = 0 elapsed in current period */

    return overflow + (uint32_t)(256 - counter);
}

#endif /* DOOM_LOG */

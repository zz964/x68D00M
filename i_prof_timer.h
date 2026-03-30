#ifndef I_PROF_TIMER_H
#define I_PROF_TIMER_H

#include <stdint.h>

/*
 * High-resolution profiling timer using MFP Timer-A.
 *
 * Timer-A is free for user programs on X68000 (Timer-B is used by
 * Human68k for key repeat / cursor blink -- do not touch it).
 *
 * Timer-A is configured with /4 prescaler from the 4 MHz MFP clock,
 * giving a 1 MHz tick rate (1 us per tick).  The 8-bit counter overflows
 * every 256 us; a lightweight ISR increments a 32-bit accumulator.
 * ~3900 ISRs/sec = ~0.2% CPU overhead -- negligible for profiling.
 *
 * The timer is only active between I_ProfTimerInit() and
 * I_ProfTimerShutdown().  Guard all usage with #ifdef DOOM_LOG so
 * release builds have zero overhead.
 */

#ifdef DOOM_LOG

/* Initialise Timer-A for profiling.  Call once from D_DoomMain after
 * supervisor mode is established.  Saves and restores the previous
 * Timer-A configuration on shutdown. */
void I_ProfTimerInit(void);

/* Restore original Timer-A state.  Call before exit. */
void I_ProfTimerShutdown(void);

/* Return elapsed microseconds since I_ProfTimerInit().
 * Briefly masks interrupts (~4 instructions) to read atomically. */
uint32_t I_GetTimeUs(void);

/* Convenience: difference between two I_GetTimeUs() values. */
static inline uint32_t I_ProfElapsed(uint32_t start)
{
    return I_GetTimeUs() - start;
}

#else /* !DOOM_LOG */

#define I_ProfTimerInit()       ((void)0)
#define I_ProfTimerShutdown()   ((void)0)
#define I_GetTimeUs()           ((uint32_t)0)
#define I_ProfElapsed(s)        ((uint32_t)0)

#endif /* DOOM_LOG */

#endif /* I_PROF_TIMER_H */

/* mus_player.c -- MUS format music player for X68000 Doom.
 *
 * Parses Doom's MUS music format (a simplified MIDI) and plays it
 * through the YM2151 FM chip via opm_music.c.
 *
 * Timing: hybrid approach. MFP Timer-D at ~140 Hz provides smooth
 * sub-frame tick counting. gametic (35 Hz * 4 = 140 Hz) provides
 * the authoritative pace. Each MUS_Update call processes ticks up
 * to whichever source is further ahead, preventing both drift and
 * jitter. */

#include "mus_player.h"
#include "opm_music.h"
#include "midi_out.h"
#include "doomstat.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <iocslib.h>
#include <doslib.h>
#include "mfp.h"
#include "irq.h"

/* ---- MUS event types ---- */
#define MUS_EV_RELEASE    0
#define MUS_EV_PLAY       1
#define MUS_EV_PITCHBEND  2
#define MUS_EV_SYSTEM     3
#define MUS_EV_CONTROLLER 4
#define MUS_EV_END        6

#define MUS_CTRL_INSTRUMENT   0
#define MUS_CTRL_VOLUME       3

#define MUS_TICKS_PER_GAMETIC  4

/* Map MUS channel (0-15) to MIDI channel (0-15).
 * MUS channel 15 = percussion -> MIDI channel 9.
 * MUS channels 0-8 -> MIDI 0-8, 9-14 -> MIDI 10-15. */
static int mus_to_midi_ch(int mus_chan)
{
    if (mus_chan == 15) return 9;
    if (mus_chan >= 9)  return mus_chan + 1;
    return mus_chan;
}

/* Forward-declare ISRs for vector assignment. */
static void mus_timer_isr_opm(void);
static void mus_timer_isr_midi(void);

/* Output mode flags: which outputs are active.
 * Set by MUS_SetOutputMode or auto-detected in MUS_Start. */
static int use_opm = 1;
static int use_midi = 0;
static int output_mode_set = 0;  /* 1 if explicitly set by user */

void MUS_SetOutputMode(int opm, int midi)
{
    use_opm = opm;
    use_midi = midi;
    output_mode_set = 1;
    /* ISR vector is set in timer_start(), called from MUS_Start().
     * I_SetMusicMode already does MUS_Stop + MUS_Start on mode change. */
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

typedef struct {
    uint8_t instrument;
    uint8_t volume;
} mus_chan_t;

typedef struct {
    int      mus_chan;
    int      note;
    uint32_t age;
} opm_voice_t;

/* ---- Player state ---- */
static const uint8_t *mus_data;
static const uint8_t *score_ptr;
static const uint8_t *score_start;
static const uint8_t *score_end;
static int            mus_looping;
static int            mus_playing;
static int            mus_paused;
static uint32_t       mus_delay;
static uint32_t       mus_tick_count;

/* ---- Watchdog: detect main loop crash from ISR ---- */
volatile uint32_t mus_heartbeat;         /* incremented by main loop each frame */
static uint32_t   mus_heartbeat_last;    /* ISR's snapshot of heartbeat */
static uint32_t   mus_heartbeat_stale;   /* consecutive ISR ticks with no change */
#define MUS_WATCHDOG_TICKS 1400  /* 140Hz * 10s = 1400 ticks -- crash if no frame for 10s */

/* ---- Global: disable music entirely (ISR never installed) ---- */
int mus_disabled = 0;  /* set by -nomusic */

static mus_chan_t     mus_channels[16];
static opm_voice_t   opm_voices[OPM_NUM_CHANNELS];

/* Timing */
static volatile uint32_t timer_ticks;  /* incremented by ISR at ~140 Hz */
static int               start_gametic;
static uint32_t          ticks_consumed;

/* ---- OPM Timer-A state ----
 * Use the YM2151's built-in Timer-A for music timing, like ZMusic3.
 * The OPM timer interrupt is IRQ_43 ("FM sound source"), routed
 * through MFP IERA/IMRA/ISRA bit 3.
 *
 * OPM Timer-A: 10-bit counter, period = (1024 - value) * 64 / 4MHz.
 * For 140 Hz: (1024 - value) = 4000000 / (64 * 140) = 446
 *             value = 1024 - 446 = 578 (0x242)
 * Register $10 = low 8 bits (0x42), Register $11 = high 2 bits (0x02)
 * Register $14 = timer control: bit 0=Timer-A load, bit 2=Timer-A enable,
 *                                bit 4=Timer-A IRQ enable */

#define OPM_ADDR    ((volatile uint8_t *)0xE90001)
#define OPM_DATA    ((volatile uint8_t *)0xE90003)
#define FM_VEC_ADDR ((volatile uint32_t *)IRQ_43)
#define FM_IE_BIT   0x08  /* IERB/IMRB bit 3 = FM sound source (IRQ_43 is in MFP group B) */

#define MFP_IERB_PTR   ((volatile uint8_t *)MFP_IERB)
#define MFP_IMRB_PTR   ((volatile uint8_t *)MFP_IMRB)
#define MFP_IPRB_PTR   ((volatile uint8_t *)MFP_IPRB)
#define MFP_ISRB_PTR   ((volatile uint8_t *)MFP_ISRB)

static uint32_t saved_fm_vector;
static uint8_t  saved_ierb_bit;
static uint8_t  saved_imrb_bit;
static int      timer_running = 0;
static int      atexit_registered = 0;

/* Direct OPM register write -- NO IOCS call.
 * IOCS OPMSET uses TRAP #15 which is NOT reentrant. If the main
 * thread is inside an IOCS call (BITSNS, MS_GETDT, ONTIME, etc.)
 * when the timer ISR fires, calling OPMSET corrupts IOCS state.
 * Direct writes avoid this entirely (like ZMusic3's opmset macro).
 * Busy-wait polls $E90003 bit 7 (from ZMusic3 data_io.s opmwait). */
static void opm_write(uint8_t reg, uint8_t val)
{
    while (*OPM_DATA & 0x80) ;  /* wait for OPM not busy */
    *OPM_ADDR = reg;
    while (*OPM_DATA & 0x80) ;
    *OPM_DATA = val;
}

/* Emergency cleanup: restore OPM timer and interrupt vector. */
static void mus_atexit_cleanup(void)
{
    if (timer_running) {
        opm_write(0x14, 0x00);
        *MFP_IERB_PTR = (*MFP_IERB_PTR & ~FM_IE_BIT) | saved_ierb_bit;
        *MFP_IMRB_PTR = (*MFP_IMRB_PTR & ~FM_IE_BIT) | saved_imrb_bit;
        *FM_VEC_ADDR = saved_fm_vector;
        timer_running = 0;
    }
}

/* ---- Voice allocation ---- */

static int alloc_voice(int mus_chan)
{
    int i, oldest_idx = 0;
    uint32_t oldest_age = 0xFFFFFFFF;

    for (i = 0; i < OPM_NUM_CHANNELS; i++) {
        if (opm_voices[i].mus_chan < 0)
            return i;
    }

    for (i = 0; i < OPM_NUM_CHANNELS; i++) {
        if (opm_voices[i].age < oldest_age) {
            oldest_age = opm_voices[i].age;
            oldest_idx = i;
        }
    }

    OPM_NoteOff(oldest_idx);
    return oldest_idx;
}

static void release_voice_by_note(int mus_chan, int note)
{
    int i;
    for (i = 0; i < OPM_NUM_CHANNELS; i++) {
        if (opm_voices[i].mus_chan == mus_chan &&
            opm_voices[i].note == note) {
            OPM_NoteOff(i);
            opm_voices[i].mus_chan = -1;
            return;
        }
    }
}

/* ---- MUS event processing ---- */

/* Safe read: returns 0 if past end of score */
#define SREAD() (score_ptr < score_end ? *score_ptr++ : 0)

/* OPM-only event processing (default FM path -- no MIDI overhead). */
static void process_events_opm(void)
{
    while (score_ptr < score_end)
    {
        uint8_t event_byte = *score_ptr++;
        int has_delay = event_byte & 0x80;
        int event_type = (event_byte >> 4) & 0x07;
        int channel = event_byte & 0x0F;

        switch (event_type)
        {
        case MUS_EV_RELEASE: {
            int note = SREAD() & 0x7F;
            release_voice_by_note(channel, note);
            break;
        }

        case MUS_EV_PLAY: {
            int note_byte = SREAD();
            int note = note_byte & 0x7F;
            int vel = mus_channels[channel].volume;
            int voice, patch;
            int opm_note;
            const opm_patch_t *p;

            if (note_byte & 0x80) {
                vel = SREAD() & 0x7F;
                mus_channels[channel].volume = vel;
            }

            voice = alloc_voice(channel);
            patch = mus_channels[channel].instrument;

            if (channel == 15) {
                int perc_idx = 128 + (note - 35);
                if (perc_idx >= 128 && perc_idx < OPM_NUM_PATCHES)
                    patch = perc_idx;
                else
                    patch = 128;
            }

            OPM_LoadPatch(voice, patch);

            p = &opm_patches[patch];
            opm_note = note;
            if (p->fixed_pitch)
                opm_note = p->fixed_note;
            else
                opm_note += p->note_offset;

            if (opm_note < 0) opm_note = 0;
            if (opm_note > 107) opm_note = 107;

            OPM_NoteOn(voice, opm_note, vel);

            opm_voices[voice].mus_chan = channel;
            opm_voices[voice].note = note_byte & 0x7F;
            opm_voices[voice].age = mus_tick_count;
            break;
        }

        case MUS_EV_PITCHBEND: {
            int bend = SREAD();
            int v;
            for (v = 0; v < OPM_NUM_CHANNELS; v++) {
                if (opm_voices[v].mus_chan == channel)
                    OPM_PitchBend(v, bend);
            }
            break;
        }

        case MUS_EV_SYSTEM: {
            int sys_ctrl = SREAD();
            if (sys_ctrl == 11) {
                int v; for (v=0; v<8; v++) {
                    if (opm_voices[v].mus_chan == channel) {
                        OPM_NoteOff(v);
                        opm_voices[v].mus_chan = -1;
                    }
                }
            }
            break;
        }

        case MUS_EV_CONTROLLER: {
            int ctrl = SREAD();
            int value = SREAD();
            if (ctrl == MUS_CTRL_INSTRUMENT) {
                if (value < 128)
                    mus_channels[channel].instrument = value;
            } else if (ctrl == MUS_CTRL_VOLUME) {
                mus_channels[channel].volume = value & 0x7F;
            }
            break;
        }

        case MUS_EV_END:
            if (mus_looping) {
                score_ptr = score_start;
            } else {
                mus_playing = 0;
                OPM_AllNotesOff();
            }
            return;

        default:
            break;
        }

        if (has_delay) {
            uint32_t delay = 0;
            uint8_t b;
            do {
                if (score_ptr >= score_end) break;
                b = *score_ptr++;
                delay = (delay << 7) | (b & 0x7F);
            } while (b & 0x80);
            mus_delay = delay;
            return;
        }
    }
}

/* MIDI event processing (external MIDI output path). */
static void process_events_midi(void)
{
    static const uint8_t mus_cc_to_midi[10] = {
        0, 0, 1, 7, 10, 11, 91, 93, 64, 67
    };

    while (score_ptr < score_end)
    {
        uint8_t event_byte = *score_ptr++;
        int has_delay = event_byte & 0x80;
        int event_type = (event_byte >> 4) & 0x07;
        int channel = event_byte & 0x0F;
        int midi_ch = mus_to_midi_ch(channel);

        switch (event_type)
        {
        case MUS_EV_RELEASE: {
            int note = SREAD() & 0x7F;
            MIDI_NoteOff(midi_ch, note);
            break;
        }

        case MUS_EV_PLAY: {
            int note_byte = SREAD();
            int note = note_byte & 0x7F;
            int vel = mus_channels[channel].volume;
            if (note_byte & 0x80) {
                vel = SREAD() & 0x7F;
                mus_channels[channel].volume = vel;
            }
            MIDI_NoteOn(midi_ch, note, vel);
            break;
        }

        case MUS_EV_PITCHBEND: {
            int bend = SREAD();
            MIDI_PitchBend(midi_ch, bend * 64);
            break;
        }

        case MUS_EV_SYSTEM: {
            int sys_ctrl = SREAD();
            switch (sys_ctrl) {
            case 10: MIDI_ControlChange(midi_ch, 120, 0); break;
            case 11: MIDI_ControlChange(midi_ch, 123, 0); break;
            case 14: MIDI_ControlChange(midi_ch, 121, 0); break;
            }
            break;
        }

        case MUS_EV_CONTROLLER: {
            int ctrl = SREAD();
            int value = SREAD();
            if (ctrl == MUS_CTRL_INSTRUMENT) {
                if (value < 128) {
                    mus_channels[channel].instrument = value;
                    MIDI_ProgramChange(midi_ch, value);
                }
            } else if (ctrl >= 1 && ctrl <= 9) {
                if (ctrl == MUS_CTRL_VOLUME)
                    mus_channels[channel].volume = value & 0x7F;
                MIDI_ControlChange(midi_ch, mus_cc_to_midi[ctrl], value & 0x7F);
            }
            break;
        }

        case MUS_EV_END:
            if (mus_looping) {
                score_ptr = score_start;
            } else {
                mus_playing = 0;
                MIDI_AllNotesOff();
            }
            return;

        default:
            break;
        }

        if (has_delay) {
            uint32_t delay = 0;
            uint8_t b;
            do {
                if (score_ptr >= score_end) break;
                b = *score_ptr++;
                delay = (delay << 7) | (b & 0x7F);
            } while (b & 0x80);
            mus_delay = delay;
            return;
        }
    }
}

#undef SREAD

/* ---- Watchdog: crash dump from ISR ----
 * When the main loop stops incrementing mus_heartbeat for 10 seconds,
 * assume a crash has occurred. Stop the timer, build a diagnostic
 * dump in a static buffer, write it to CRASH.TXT, and halt.
 *
 * File I/O uses raw Human68k DOS calls (TRAP #10 = DOS entry, but
 * xdev68k doslib uses the ff-prefix convention via inline functions).
 * Since the game is dead and we've disabled the timer interrupt,
 * reentrancy is not a concern. */

/* Static buffer for crash dump -- avoid any dynamic allocation in ISR. */
static char crash_buf[1024];
static int  crash_pos;

static void crash_append(const char *s)
{
    while (*s && crash_pos < (int)sizeof(crash_buf) - 1)
        crash_buf[crash_pos++] = *s++;
}

/* Convert unsigned long to hex string (8 chars + NUL). */
static void u32_to_hex(uint32_t v, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 7; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[8] = 0;
}

static void crash_hex32(const char *label, uint32_t v)
{
    char hexbuf[9];
    crash_append(label);
    u32_to_hex(v, hexbuf);
    crash_append(hexbuf);
}

/* Write the crash buffer to CRASH.TXT using Human68k DOS calls.
 * Uses doslib CREATE/WRITE/CLOSE which go through TRAP #10.
 * Safe here because the timer is stopped and the game is dead. */
static void crash_write_file(void)
{
    int handle;
    handle = CREATE((unsigned char *)"CRASH.TXT", 0x20);
    if (handle < 0)
        return;
    WRITE(handle, (unsigned char *)crash_buf, crash_pos);
    CLOSE(handle);
}

/* Crash dump: build diagnostic info and write to CRASH.TXT.
 * Called from ISR context after watchdog triggers. */
static void watchdog_crash_dump(void)
{
    uint32_t sp_val;
    uint16_t *sp16;
    char hexbuf[9];
    int i;

    /* Stop the timer so we don't re-enter. */
    opm_write(0x14, 0x00);
    *MFP_IERB_PTR &= ~FM_IE_BIT;

    crash_pos = 0;
    crash_append("*** WATCHDOG: main loop stopped ***\r\n");

    crash_hex32("heartbeat=", mus_heartbeat);
    crash_hex32("  timer_ticks=", timer_ticks);
    crash_append("\r\n");

    crash_hex32("score_ptr=", (uint32_t)score_ptr);
    crash_hex32("  mus_data=", (uint32_t)mus_data);
    crash_hex32("  score_end=", (uint32_t)score_end);
    crash_append("\r\n");

    crash_hex32("mus_playing=", (uint32_t)mus_playing);
    crash_hex32("  mus_paused=", (uint32_t)mus_paused);
    crash_append("\r\n");

    /* ISR stack pointer */
    __asm__ volatile ("move.l %%sp,%0" : "=d"(sp_val));
    crash_hex32("ISR_sp=", sp_val);
    crash_append("\r\n");

    /* Dump 64 words (128 bytes) from the ISR stack frame.
     * The interrupt exception frame is above our locals on the stack.
     * For 68030: frame = SR(2) + PC(4) = 6 bytes.
     * For 68060: frame = SR(2) + PC(4) + format/vector(2) = 8 bytes.
     * Somewhere in this dump is the interrupted PC (crash address). */
    sp16 = (uint16_t *)sp_val;
    crash_append("Stack dump (64 words from SP):\r\n");
    for (i = 0; i < 64; i++) {
        u32_to_hex(sp16[i], hexbuf);
        crash_append(hexbuf + 4);  /* low 4 hex chars for 16-bit word */
        crash_append((i & 15) == 15 ? "\r\n" : " ");
    }

    crash_append("*** End of crash dump ***\r\n");

    /* Write to file in current directory. */
    crash_write_file();

    /* Also try to print a short notice to the text console,
     * in case it's still visible. */
    {
        register long d0 __asm__("d0") = 0x0021;  /* IOCS _B_PRINT */
        register const char *a1 __asm__("a1") = "\r\nCrash dump written to CRASH.TXT\r\n";
        __asm__ volatile ("trap #15" : "+d"(d0), "+a"(a1) : : "a0", "cc");
    }

    /* Stop the music and disable the watchdog so the ISR
     * keeps running (for RTE) but doesn't re-trigger.
     * The OS stays responsive -- the user can reboot cleanly. */
    mus_playing = 0;
    mus_heartbeat_stale = 0;
}

/* Check heartbeat; call from ISR. */
static void watchdog_check(void)
{
    if (mus_heartbeat != mus_heartbeat_last) {
        mus_heartbeat_last = mus_heartbeat;
        mus_heartbeat_stale = 0;
    } else {
        mus_heartbeat_stale++;
        if (mus_heartbeat_stale >= MUS_WATCHDOG_TICKS)
            watchdog_crash_dump();
    }
}

/* ---- OPM Timer-A ISR (FM path) ----
 * Self-contained interrupt handler for YM2151 FM playback.
 * No MIDI code -- keeps I-cache footprint small. */

static void mus_timer_isr_opm(void) __attribute__((interrupt_handler));
static void mus_timer_isr_opm(void)
{
    timer_ticks++;
    opm_write(0x14, 0x15);

    if (mus_playing && !mus_paused)
    {
        mus_tick_count++;
        if (mus_delay > 0)
            mus_delay--;
        if (mus_delay == 0)
            process_events_opm();
    }

    /* Apply pending volume slider change (safe here in ISR context). */
    OPM_CheckVolumeDirty();

    watchdog_check();
    *MFP_ISRB_PTR &= (uint8_t)~FM_IE_BIT;
}

/* ---- OPM Timer-A ISR (MIDI path) ----
 * Self-contained interrupt handler for external MIDI playback. */

static void mus_timer_isr_midi(void) __attribute__((interrupt_handler));
static void mus_timer_isr_midi(void)
{
    timer_ticks++;
    opm_write(0x14, 0x15);

    midi_select_tx();

    if (mus_playing && !mus_paused)
    {
        mus_tick_count++;
        if (mus_delay > 0)
            mus_delay--;
        if (mus_delay == 0)
            process_events_midi();
    }

    MIDI_DrainBuffer();
    watchdog_check();
    *MFP_ISRB_PTR &= (uint8_t)~FM_IE_BIT;
}

static void timer_start(void)
{
    /* OPM Timer-A for 140 Hz:
     * Period = (1024 - value) * 64 / 4,000,000
     * For 140 Hz: value = 1024 - (4000000 / (64 * 140)) = 578 (0x242)
     * Register $10 = low 8 bits, $11 = high 2 bits. */
    int timer_val = 578;  /* 140.06 Hz */

    /* Reset watchdog so level-load time doesn't trigger it. */
    mus_heartbeat_last = mus_heartbeat;
    mus_heartbeat_stale = 0;

    if (!atexit_registered) {
        atexit(mus_atexit_cleanup);
        atexit_registered = 1;
    }

    /* Save current FM interrupt state. */
    saved_ierb_bit = *MFP_IERB_PTR & FM_IE_BIT;
    saved_imrb_bit = *MFP_IMRB_PTR & FM_IE_BIT;
    saved_fm_vector = *FM_VEC_ADDR;

    /* Disable FM interrupt while configuring. */
    *MFP_IERB_PTR &= ~FM_IE_BIT;

    /* Install the appropriate ISR for current output mode. */
    *FM_VEC_ADDR = (uint32_t)(uintptr_t)(use_midi ? mus_timer_isr_midi : mus_timer_isr_opm);

    /* Configure OPM Timer-A.
     * YM2151 Timer-A is 10-bit: reg $10 = upper 8 bits (value >> 2),
     * reg $11 = lower 2 bits (value & 3). */
    opm_write(0x10, (uint8_t)((timer_val >> 2) & 0xFF));  /* Timer-A bits 9-2 */
    opm_write(0x11, (uint8_t)(timer_val & 0x03));          /* Timer-A bits 1-0 */
    opm_write(0x14, 0x15);  /* bit 0=load Timer-A, bit 2=enable Timer-A IRQ, bit 4=reset flag */

    /* Enable FM interrupt in MFP. */
    *MFP_IPRB_PTR &= (uint8_t)~FM_IE_BIT;  /* clear any pending */
    *MFP_IERB_PTR |= FM_IE_BIT;
    *MFP_IMRB_PTR |= FM_IE_BIT;

    timer_running = 1;
}

static void timer_stop(void)
{
    /* Disable OPM Timer-A. */
    opm_write(0x14, 0x00);  /* disable all timer IRQs */

    /* Restore MFP FM interrupt state. */
    *MFP_IERB_PTR = (*MFP_IERB_PTR & ~FM_IE_BIT) | saved_ierb_bit;
    *MFP_IMRB_PTR = (*MFP_IMRB_PTR & ~FM_IE_BIT) | saved_imrb_bit;
    *FM_VEC_ADDR = saved_fm_vector;

    timer_running = 0;
}

/* ---- Public API ---- */

void MUS_Start(const void *data, int looping)
{
    const uint8_t *raw = (const uint8_t *)data;
    uint16_t score_offset;
    uint16_t score_len;
    int i;

    if (mus_disabled)
        return;

    MUS_Stop();

    /* Auto-detect output mode if not explicitly set by user. */
    if (!output_mode_set) {
        if (MIDI_IsAvailable()) {
            use_opm = 0;
            use_midi = 1;
        } else {
            use_opm = 1;
            use_midi = 0;
        }
    }

    if (raw[0] != 'M' || raw[1] != 'U' || raw[2] != 'S' || raw[3] != 0x1A)
        return;

    score_len = read_le16(&raw[4]);
    score_offset = read_le16(&raw[6]);

    mus_data = raw;
    score_start = raw + score_offset;
    score_ptr = score_start;
    /* Don't trust score_len for end boundary -- some MUS files have
     * inaccurate values. Use a generous limit and rely on MUS_EV_END
     * for actual termination (like Chocolate Doom does). */
    score_end = score_start + 65535;
    mus_looping = looping;
    mus_delay = 0;
    mus_tick_count = 0;

    for (i = 0; i < 16; i++) {
        mus_channels[i].instrument = 0;
        mus_channels[i].volume = 100;
    }

    for (i = 0; i < OPM_NUM_CHANNELS; i++) {
        opm_voices[i].mus_chan = -1;
        opm_voices[i].note = 0;
        opm_voices[i].age = 0;
    }

    mus_playing = 1;
    mus_paused = 0;
    start_gametic = gametic;
    timer_ticks = 0;
    ticks_consumed = 0;

    timer_start();
}

void MUS_Update(void)
{
    /* Event processing happens in the OPM Timer-A ISR.
     * Nothing to do here in the main thread. */
#if 0  /* main-thread fallback, retained for reference */
    uint32_t pending;
    if (!mus_playing || mus_paused) return;
    pending = timer_ticks - ticks_consumed;
    if (pending > 280) pending = 280;
    while (pending > 0) {
        pending--;
        ticks_consumed++;
        mus_tick_count++;
        if (mus_delay > 0) {
            mus_delay--;
        } else {
            process_events();
            if (!mus_playing) return;
        }
    }
#endif
}

void MUS_Stop(void)
{
    if (mus_playing) {
        timer_stop();
        mus_playing = 0;
        if (use_midi)
            MIDI_AllNotesOff();
        else
            OPM_AllNotesOff();
    }
}

void MUS_ShutdownTimer(void)
{
    if (timer_running)
        timer_stop();
}

void MUS_Pause(void)
{
    if (mus_playing && !mus_paused) {
        mus_paused = 1;
        if (use_midi)
            MIDI_AllNotesOff();
        else
            OPM_AllNotesOff();
    }
}

void MUS_Resume(void)
{
    if (mus_playing && mus_paused) {
        /* Resync both tick sources */
        start_gametic = gametic;
        timer_ticks = 0;
        ticks_consumed = 0;
        mus_paused = 0;
    }
}

int MUS_IsPlaying(void)
{
    return mus_playing;
}

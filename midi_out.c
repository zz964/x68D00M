/* midi_out.c -- External MIDI output via CZ-6BM1 board for X68000 Doom.
 *
 * The CZ-6BM1 uses a Yamaha YM3802 MIDI Communication Interface.
 * Register access is group-based: write group number to RGR, then
 * access registers at fixed offsets. Initialization sequence from
 * ZMusic3 source (zmsc3.s init_cz6bm1 / md_init_tbl_i).
 *
 * Detection uses DOS _BUS_ERR to safely probe without crashing. */

#include "midi_out.h"
#include <doslib.h>
#include <stdint.h>

/* YM3802 register addresses -- detected at runtime.
 * Board 0 (CZ-6BM1): $EAFA00-$EAFA0F (base $EAFA01)
 * Board 1:            $EAFA10-$EAFA1F (base $EAFA11) */
static uint32_t ym3802_base = 0;  /* set by MIDI_Init */

#define YM3802_REG(off) ((volatile uint8_t *)(ym3802_base + (off)))
#define YM3802_IVR   YM3802_REG(0)   /* +0 */
#define YM3802_RGR   YM3802_REG(2)   /* +2 */
#define YM3802_ISR   YM3802_REG(4)   /* +4 */
#define YM3802_ICR   YM3802_REG(6)   /* +6 */

/* Hardware delay: read OPM status register (harmless, ~1us). */
#define MIDI_WAIT() do { (void)(*(volatile uint8_t *)0xE9A001); } while(0)

/* Write to a YM3802 register using group/offset addressing.
 * group: register group (0-9), written to RGR
 * offset: byte offset from IVR (even numbers: 0,2,4,...14)
 * value: data byte to write */
static void ym3802_write(int group, int offset, uint8_t value)
{
    *YM3802_RGR = (uint8_t)group;
    MIDI_WAIT();
    *(volatile uint8_t *)(ym3802_base + offset) = value;
    MIDI_WAIT();
}

static int midi_available = 0;

/* ---- Ring buffer for non-blocking TX ---- */
#define MIDI_BUF_SIZE 1024  /* must be power of 2 */
#define MIDI_BUF_MASK (MIDI_BUF_SIZE - 1)
static volatile unsigned char midi_buf[MIDI_BUF_SIZE];
static volatile int midi_buf_head = 0;  /* write position */
static volatile int midi_buf_tail = 0;  /* read position */
static int midi_drops = 0;  /* debug: count dropped bytes */
static unsigned char midi_running_status = 0;  /* for running status optimization */

/* YM3802 TX requires RGR=5 selected. With RGR=5:
 *   base+8 (grp4) bit 6 = TX FIFO has space
 *   base+12 (grp6) = TX data register (TDR)
 * From ZMusic3 data_io.s m_out_m0 / TX ISR. */
#define YM3802_GRP4   YM3802_REG(8)

/* Select register group 5 for TX operations.
 * Called once before a batch of sends (ISR entry, drain, etc.). */
void midi_select_tx(void)
{
    *YM3802_RGR = 0x05;
    MIDI_WAIT();
}

/* Check if TX FIFO can accept a byte (RGR must be 5). */
static int midi_tx_ready(void)
{
    return (*YM3802_GRP4 & 0x40);
}

/* Write one byte to TX data register (RGR must be 5). */
static void midi_tx_byte(unsigned char byte)
{
    *(volatile uint8_t *)(ym3802_base + 12) = byte;
    MIDI_WAIT();
}

/* Drain as many bytes as the TX FIFO can accept right now.
 * Called from the timer ISR every tick to keep MIDI flowing.
 * Matches ZMusic3's approach: drain until FIFO full or buffer empty. */
void MIDI_DrainBuffer(void)
{
    if (!midi_available) return;
    if (midi_buf_tail == midi_buf_head) return;
    /* RGR=5 should be set by ISR caller, but ensure it for safety */
    midi_select_tx();
    while (midi_buf_tail != midi_buf_head) {
        if (!midi_tx_ready())
            break;  /* TX FIFO full, try again next tick */
        midi_tx_byte(midi_buf[midi_buf_tail]);
        midi_buf_tail = (midi_buf_tail + 1) & MIDI_BUF_MASK;
    }
}

/* Drain entire ring buffer, blocking until complete. */
static void midi_drain_all(void)
{
    int timeout;
    if (midi_buf_tail == midi_buf_head) return;
    midi_select_tx();
    while (midi_buf_tail != midi_buf_head) {
        timeout = 50000;
        while (!midi_tx_ready() && --timeout > 0)
            MIDI_WAIT();
        midi_tx_byte(midi_buf[midi_buf_tail]);
        midi_buf_tail = (midi_buf_tail + 1) & MIDI_BUF_MASK;
    }
}

/* Send a byte directly to hardware, bypassing the ring buffer. */
static void midi_send_direct(unsigned char byte)
{
    int timeout = 50000;
    midi_select_tx();
    while (!midi_tx_ready() && --timeout > 0)
        MIDI_WAIT();
    midi_tx_byte(byte);
}

/* ---- Hardware detection ---- */

int MIDI_Init(void)
{
    /* Probe MIDI board at both possible addresses.
     * Board 0: $EAFA01 (CZ-6BM1 standard)
     * Board 1: $EAFA11 (second slot, used by XM6G etc.) */
    if (BUS_ERR((UBYTE *)0xEAFA05, (UBYTE *)0xEAFA07, 1) == 0) {
        ym3802_base = 0xEAFA01;
    } else if (BUS_ERR((UBYTE *)0xEAFA15, (UBYTE *)0xEAFA17, 1) == 0) {
        ym3802_base = 0xEAFA11;
    } else {
        midi_available = 0;
        return 0;
    }

    midi_available = 1;
    printf("MIDI: YM3802 detected at $%06lX\n", (unsigned long)ym3802_base);

    /* Reset the YM3802 (from ZMusic3 init_cz6bm1). */
    *YM3802_RGR = 0x80;  /* set reset bit */
    MIDI_WAIT();
    MIDI_WAIT();
    MIDI_WAIT();
    MIDI_WAIT();
    *YM3802_RGR = 0x00;  /* clear reset */
    MIDI_WAIT();

    /* Write vector offset to GRP4. */
    *(volatile uint8_t *)(ym3802_base + 8) = 0x80;
    MIDI_WAIT();

    /* Exact init table from ZMusic3 md_init_tbl_i.
     * Format: ym3802_write(group, offset, value)
     * Decoded from the packed table entries. */
    ym3802_write(0, 12, 0x00);   /* $06: grp0 DCR = 0 */
    ym3802_write(6, 12, 0x02);   /* $66: grp6 DCR = Tx FIFO enable */
    ym3802_write(6, 14, 0x18);   /* $67: grp6 RRR = FIFO depth */
    ym3802_write(6, 10, 0x94);   /* $65: grp6 DMR = Tx config */
    ym3802_write(5, 10, 0x85);   /* $55: grp5 DMR = MIDI rate */
    ym3802_write(4,  8, 0x08);   /* $44: grp4 DSR = timer config */
    ym3802_write(3, 10, 0x90);   /* $35: grp3 DMR = Rx config */
    ym3802_write(2,  8, 0x08);   /* $24: grp2 DSR */
    ym3802_write(2, 10, 0x00);   /* $25: grp2 DMR */
    ym3802_write(0, 10, 0x02);   /* $05: grp0 DMR = general config */
    ym3802_write(0,  6, 0xFF);   /* $03: ICR = clear all interrupts */
    ym3802_write(1,  8, 0x2B);   /* $14: grp1 DSR = click config */
    ym3802_write(9,  8, 0x00);   /* $94: grp9 DSR */
    ym3802_write(3, 10, 0xD1);   /* $35: grp3 DMR = Tx enable */
    ym3802_write(5, 10, 0x81);   /* $55: grp5 DMR = final rate config */

    /* Send GM System On to put the module in a known state. */
    MIDI_SendGMReset();

    return 1;
}

void MIDI_Shutdown(void)
{
    if (!midi_available) return;
    MIDI_AllNotesOff();
    if (midi_drops > 0)
        printf("MIDI: %d bytes dropped (buffer overflow)\n", midi_drops);
}

int MIDI_IsAvailable(void)
{
    return midi_available;
}

/* ---- Byte-level transmission ---- */

void MIDI_SendByte(unsigned char byte)
{
    int next;
    if (!midi_available) return;

    /* Try direct send first (like ZMusic3 m_out_m0).
     * RGR=5 must already be set by the caller (ISR sets it once).
     * If FIFO has room, write immediately -- no buffer latency.
     * Only fall back to ring buffer if FIFO is full. */
    if (midi_buf_tail == midi_buf_head) {
        /* Buffer empty -- try direct (RGR=5 assumed set) */
        if (midi_tx_ready()) {
            midi_tx_byte(byte);
            return;
        }
    }

    /* FIFO full or buffer has pending bytes -- queue it. */
    next = (midi_buf_head + 1) & MIDI_BUF_MASK;
    if (next == midi_buf_tail) {
        midi_drops++;
        return;  /* buffer full, drop */
    }
    midi_buf[midi_buf_head] = byte;
    midi_buf_head = next;
}

/* ---- Message-level convenience functions ---- */

void MIDI_NoteOn(int channel, int note, int velocity)
{
    MIDI_SendByte((unsigned char)(0x90 | (channel & 0x0F)));
    MIDI_SendByte((unsigned char)(note & 0x7F));
    MIDI_SendByte((unsigned char)(velocity & 0x7F));
}

void MIDI_NoteOff(int channel, int note)
{
    MIDI_SendByte((unsigned char)(0x80 | (channel & 0x0F)));
    MIDI_SendByte((unsigned char)(note & 0x7F));
    MIDI_SendByte(0);
}

void MIDI_ProgramChange(int channel, int program)
{
    MIDI_SendByte((unsigned char)(0xC0 | (channel & 0x0F)));
    MIDI_SendByte((unsigned char)(program & 0x7F));
}

void MIDI_ControlChange(int channel, int controller, int value)
{
    MIDI_SendByte((unsigned char)(0xB0 | (channel & 0x0F)));
    MIDI_SendByte((unsigned char)(controller & 0x7F));
    MIDI_SendByte((unsigned char)(value & 0x7F));
}

void MIDI_PitchBend(int channel, int value)
{
    MIDI_SendByte((unsigned char)(0xE0 | (channel & 0x0F)));
    MIDI_SendByte((unsigned char)(value & 0x7F));
    MIDI_SendByte((unsigned char)((value >> 7) & 0x7F));
}

void MIDI_AllNotesOff(void)
{
    int ch;
    if (!midi_available) return;

    /* Drain any pending bytes first. */
    midi_drain_all();

    /* Send silence directly (RGR=5 set by midi_send_direct).
     * Sustain off, All Notes Off, All Sound Off on all channels. */
    for (ch = 0; ch < 16; ch++) {
        midi_send_direct((unsigned char)(0xB0 | ch));
        midi_send_direct(64);
        midi_send_direct(0);
    }
    for (ch = 0; ch < 16; ch++) {
        midi_send_direct((unsigned char)(0xB0 | ch));
        midi_send_direct(123);
        midi_send_direct(0);
    }
    for (ch = 0; ch < 16; ch++) {
        midi_send_direct((unsigned char)(0xB0 | ch));
        midi_send_direct(120);
        midi_send_direct(0);
    }
}

void MIDI_SendGMReset(void)
{
    MIDI_SendByte(0xF0);
    MIDI_SendByte(0x7E);
    MIDI_SendByte(0x7F);
    MIDI_SendByte(0x09);
    MIDI_SendByte(0x01);
    MIDI_SendByte(0xF7);
}

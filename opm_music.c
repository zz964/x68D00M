/* opm_music.c -- YM2151 (OPM) FM synthesis driver for X68000 Doom.
 *
 * Parses the GENMIDI WAD lump (OPL2 FM instrument patches) and converts
 * them to YM2151 register values at startup.  Provides note-on/off and
 * patch-loading functions for the MUS sequencer.
 *
 * OPL2 (YM3812) has 2 operators per voice; YM2151 has 4.  We use only
 * M1 and C1 for the OPL2 conversion and silence M2/C2 (TL=127).
 * Both chips share 0.75 dB/step TL, 4-bit SL, similar rate curves.
 *
 * OPL2 connection 0 (FM) -> YM2151 algo 4: M1->C1 (clean 2-op FM)
 * OPL2 connection 1 (AM) -> YM2151 algo 7: M1+C1 (clean 2-op additive)
 *
 * Also supports native YM2151 patches via embedded FB-01 GM bank. */

#include "opm_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iocslib.h>

/* Direct OPM register write -- bypasses IOCS OPMSET which is not
 * reentrant (uses TRAP #15). The Timer-A ISR also writes OPM registers,
 * so we must use direct hardware access to avoid corrupting IOCS state.
 * Busy-wait polls status bit 7 (like ZMusic3/MXDRV). */
#define OPM_ADDR_REG    ((volatile uint8_t *)0xE90001)
#define OPM_DATA_REG    ((volatile uint8_t *)0xE90003)

static void opm_set(int reg, int val)
{
    OPMSET(reg, val);
}


/* ---- Converted patch bank ---- */
opm_patch_t opm_patches[OPM_NUM_PATCHES];

/* ---- Master volume attenuation (0=loud, added to carrier TL) ---- */
static int opm_master_atten = 0;
static volatile int volume_dirty = 0;  /* set by main thread, cleared by ISR */

/* ---- Per-channel pitch state for pitchbend ---- */
static uint8_t channel_base_kc[OPM_NUM_CHANNELS];  /* KC set at note-on */

/* ---- MIDI note -> YM2151 KC (key code) lookup ---- */
/* YM2151 KC encoding: bits 6-4 = octave, bits 3-0 = note.
 * Note values: C=0,C#=1,D=2,Eb=4,E=5,F=6,F#=8,G=9,Ab=10,A=12,Bb=13,B=14
 * MIDI note 0 = C-1.  YM2151 octave 0 ~ MIDI octave 0 (C0-B0).
 * We offset so MIDI note 24 (C1) = YM2151 octave 1, note C. */
static const uint8_t note_to_kc_semitone[12] = {
    0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14
};

static uint8_t midi_note_to_kc(int note)
{
    int octave, semi;
    if (note < 0) note = 0;
    if (note > 107) note = 107;
    octave = note / 12;
    semi = note % 12;
    if (octave > 7) octave = 7;
    return (uint8_t)((octave << 4) | note_to_kc_semitone[semi]);
}

/* ---- YM2151 operator slot offsets ----
 * For channel N (0-7), operator registers are at base + slot_offset[op] + N.
 * Slot mapping: M1=0, M2=8, C1=16, C2=24. */
static const int slot_offset[4] = { 0, 8, 16, 24 };

/* ---- GENMIDI parsing ---- */

/* GENMIDI voice data layout (16 bytes per voice):
 * Each operator is 6 bytes (Chocolate Doom genmidi_op_t):
 *   [+0] tremolo/vib/sustain/KSR/mult   (OPL reg $20)
 *   [+1] attack_rate/decay_rate          (OPL reg $60)
 *   [+2] sustain_level/release_rate      (OPL reg $80)
 *   [+3] waveform select                 (OPL reg $E0)
 *   [+4] KSL (key scale level only)      (OPL reg $40 bits 7-6)
 *   [+5] TL (total level / output level) (OPL reg $40 bits 5-0)
 *
 * Voice layout:
 * [0-5]   Modulator (6 bytes)
 * [6]     Feedback/connection              (OPL reg $C0)
 * [7-12]  Carrier (6 bytes)
 * [13]    Unused
 * [14-15] Base note offset (int16_t, little-endian) */

/* OPL2 rate to YM2151 rate mapping table.
 * OPL2 rates (4-bit, 0-15) have different timing than YM2151 (5-bit, 0-31).
 * This table is based on matching actual envelope times between the chips.
 * OPL2 rate 0 = no change, rate 15 = instant. */
static const uint8_t opl_to_opm_rate[16] = {
     0,  2,  4,  7,  9, 11, 14, 16, 18, 20, 22, 24, 26, 28, 30, 31
};

/* Convert one OPL2 operator (6 bytes) to one YM2151 operator.
 * opl_data[0]=reg$20, [1]=reg$60, [2]=reg$80, [3]=reg$E0, [4]=KSL, [5]=TL
 * waveform: OPL2 waveform select (0-3), used for MUL adjustment. */
static void convert_operator(const uint8_t *opl_data, opm_op_t *op, int waveform)
{
    int mult = opl_data[0] & 0x0F;
    int ksr = (opl_data[0] >> 4) & 0x01;
    int sustain_on = (opl_data[0] >> 5) & 0x01;
    int ar = (opl_data[1] >> 4) & 0x0F;
    int dr = opl_data[1] & 0x0F;
    int sl = (opl_data[2] >> 4) & 0x0F;
    int rr = opl_data[2] & 0x0F;
    int tl = opl_data[5] & 0x3F;
    int ks = ksr ? 2 : 0;  /* OPL2 KSR on/off -> YM2151 KS 0 or 2 */
    int dr_opm;

    /* Waveform 2 (abs-sine): |sin(x)| has dominant component at 2f.
     * Double MUL to match the perceived frequency.
     * |sin(x)| amplitude at 2f is ~81% of sin(2x), so increase TL
     * by 2 steps (~1.5 dB) to compensate for the lower energy.
     *
     * Waveform 3 (pulse-sine): narrow positive pulses at 2x rate.
     * Similar MUL doubling but even quieter (~6 dB less energy). */
    if (waveform == 2) {
        mult = (mult == 0) ? 1 : mult * 2;
        if (mult > 15) mult = 15;
        tl = (tl + 2 <= 63) ? tl + 2 : 63;
    } else if (waveform == 3) {
        mult = (mult == 0) ? 1 : mult * 2;
        if (mult > 15) mult = 15;
        tl = (tl + 8 <= 63) ? tl + 8 : 63;
    }

    op->dt1_mul = (uint8_t)(mult & 0x0F);
    op->tl = (uint8_t)tl;

    /* AR/DR/RR: use mapping table for more accurate timing.
     * KS (bits 7-6): envelope rate scaling from OPL2 KSR bit. */
    op->ks_ar = (uint8_t)(((ks & 0x03) << 6) | (opl_to_opm_rate[ar] & 0x1F));

    dr_opm = opl_to_opm_rate[dr];
    op->d1r = (uint8_t)(dr_opm & 0x1F);

    /* D2R: OPL2 sustain bit controls whether sound holds at SL.
     * sustain=1: hold at SL after first decay (D2R=0).
     * sustain=0: on real OPL2, the envelope ignores SL entirely and
     *   decays from peak to silence at the DR rate. On YM2151, D1R
     *   decays to D1L (=SL), then D2R continues. Set D2R = D1R so
     *   the decay continues at the same rate through SL to silence. */
    if (sustain_on)
        op->dt2_d2r = 0;
    else
        op->dt2_d2r = (uint8_t)(dr_opm & 0x1F);

    op->d1l_rr = (uint8_t)((sl << 4) | (opl_to_opm_rate[rr] >> 1));
}

/* Make a silent operator (TL=127, minimal envelope) */
static void make_silent_op(opm_op_t *op)
{
    op->dt1_mul = 0x00;
    op->tl = 127;
    op->ks_ar = 0x00;
    op->d1r = 0x00;
    op->dt2_d2r = 0x00;
    op->d1l_rr = 0xFF;
}

static void convert_voice(const uint8_t *voice_data, opm_patch_t *patch)
{
    int fb  = (voice_data[6] >> 1) & 0x07;
    int con = voice_data[6] & 0x01;
    int mod_wf = voice_data[3] & 0x03;  /* modulator waveform */
    int car_wf = voice_data[10] & 0x03; /* carrier waveform */
    int algo;

    /* OPL2 connection 0 (FM: Mod->Car) -> YM2151 algo 4: M1->C1 + M2->C2
     * OPL2 connection 1 (AM: Mod+Car)  -> YM2151 algo 7: all additive
     * M2/C2 silenced for clean 2-op conversion. */
    algo = con ? 7 : 4;

    /* Waveform approximation via feedback adjustment.
     *
     * WF1 (half-sine) as modulator: asymmetric modulation (only positive
     * excursion). The DC offset shifts the carrier pitch up, and even
     * harmonics add extra sidebands. We can't reproduce asymmetry on
     * YM2151, but a +1 feedback step adds some spectral richness.
     * FB steps are exponential (each step = 2x), so +1 is gentle.
     *
     * WF3 (pulse-sine) as modulator: narrow pulses create very rich
     * harmonics. +1 feedback gives a bit more bite without distorting. */
    if (mod_wf == 1 || mod_wf == 3) {
        if (fb < 7) fb += 1;
    }

    /* Convert OPL2 modulator -> M1 (ops[0]) */
    convert_operator(&voice_data[0], &patch->ops[0], mod_wf);

    /* M2 (ops[1]) = silent */
    make_silent_op(&patch->ops[1]);

    /* Convert OPL2 carrier -> C1 (ops[2]) */
    convert_operator(&voice_data[7], &patch->ops[2], car_wf);

    /* C2 (ops[3]) = silent */
    make_silent_op(&patch->ops[3]);

    /* RL/FB/CON: both speakers on, feedback from OPL2, algorithm as above */
    patch->rl_fb_con = (uint8_t)(0xC0 | (fb << 3) | algo);

    /* Base note offset (int16_t, little-endian at bytes 14-15) */
    patch->note_offset = (int16_t)(voice_data[14] | (voice_data[15] << 8));
}

void OPM_ParseGENMIDI(const void *lump_data)
{
    const uint8_t *data = (const uint8_t *)lump_data;
    int i;

    /* Skip 8-byte header "#OPL_II#" */
    data += 8;

    for (i = 0; i < OPM_NUM_PATCHES; i++)
    {
        uint16_t flags = data[0] | (data[1] << 8);
        uint8_t fixed_note = data[3];

        /* GENMIDI flags: bit 0 = fixed pitch, bit 2 = double voice */
        opm_patches[i].fixed_pitch = (flags & 0x0001) ? 1 : 0;
        opm_patches[i].fixed_note = fixed_note;

        /* Convert first voice (bytes 4-19) */
        convert_voice(data + 4, &opm_patches[i]);

        data += 36;
    }
}

/* ---- .opm file loader (VOPM format) ---- */
#ifndef OPM_NO_FILE_LOADER

/* Parse a single operator line: "M1: AR D1R D2R RR D1L TL KS MUL DT1 DT2 AMS-EN" */
static void parse_opm_op(const char *line, opm_op_t *op)
{
    int ar, d1r, d2r, rr, d1l, tl, ks, mul, dt1, dt2, ams;
    if (sscanf(line, "%*[^:]: %d %d %d %d %d %d %d %d %d %d %d",
               &ar, &d1r, &d2r, &rr, &d1l, &tl, &ks, &mul, &dt1, &dt2, &ams) < 11)
        return;

    op->dt1_mul = (uint8_t)(((dt1 & 0x07) << 4) | (mul & 0x0F));
    op->tl      = (uint8_t)(tl & 0x7F);
    op->ks_ar   = (uint8_t)(((ks & 0x03) << 6) | (ar & 0x1F));
    op->d1r     = (uint8_t)(d1r & 0x1F);
    op->dt2_d2r = (uint8_t)(((dt2 & 0x03) << 6) | (d2r & 0x1F));
    op->d1l_rr  = (uint8_t)(((d1l & 0x0F) << 4) | (rr & 0x0F));
}

int OPM_LoadOPMFile(const char *filename)
{
    FILE *fp;
    char line[256];
    int cur_patch = -1;
    int count = 0;
    /* Track which operator lines we've seen for the current patch */
    int got_ch = 0, got_m1 = 0, got_c1 = 0, got_m2 = 0, got_c2 = 0;

    fp = fopen(filename, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        /* Skip comments and blank lines */
        if (line[0] == '/' || line[0] == '\n' || line[0] == '\r')
            continue;

        /* Patch header: "@:N Name" */
        if (line[0] == '@' && line[1] == ':') {
            cur_patch = atoi(&line[2]);
            if (cur_patch < 0 || cur_patch >= OPM_NUM_PATCHES)
                cur_patch = -1;
            got_ch = got_m1 = got_c1 = got_m2 = got_c2 = 0;
            continue;
        }

        if (cur_patch < 0)
            continue;

        /* Channel line: "CH: PAN FL CON AMS PMS SLOT NE" */
        if (line[0] == 'C' && line[1] == 'H' && line[2] == ':') {
            int pan, fl, con;
            if (sscanf(line, "CH: %d %d %d", &pan, &fl, &con) >= 3) {
                int rl;
                /* PAN: 64=both(0xC0), 128=left(0x80), 192=right(0x40) */
                if (pan == 128)      rl = 0x80;
                else if (pan == 192) rl = 0x40;
                else                 rl = 0xC0;
                opm_patches[cur_patch].rl_fb_con =
                    (uint8_t)(rl | ((fl & 0x07) << 3) | (con & 0x07));
                opm_patches[cur_patch].note_offset = 0;
                opm_patches[cur_patch].fixed_pitch = 0;
                opm_patches[cur_patch].fixed_note = 0;
                got_ch = 1;
            }
            continue;
        }

        /* Operator lines */
        if (line[0] == 'M' && line[1] == '1' && line[2] == ':') {
            parse_opm_op(line, &opm_patches[cur_patch].ops[0]);
            got_m1 = 1;
        } else if (line[0] == 'M' && line[1] == '2' && line[2] == ':') {
            parse_opm_op(line, &opm_patches[cur_patch].ops[1]);
            got_m2 = 1;
        } else if (line[0] == 'C' && line[1] == '1' && line[2] == ':') {
            parse_opm_op(line, &opm_patches[cur_patch].ops[2]);
            got_c1 = 1;
        } else if (line[0] == 'C' && line[1] == '2' && line[2] == ':') {
            parse_opm_op(line, &opm_patches[cur_patch].ops[3]);
            got_c2 = 1;
        }

        if (got_ch && got_m1 && got_c1 && got_m2 && got_c2) {
            count++;
            got_ch = got_m1 = got_c1 = got_m2 = got_c2 = 0;
        }
    }

    fclose(fp);
    return count;
}

#else
int OPM_LoadOPMFile(const char *filename) { (void)filename; return 0; }
#endif

/* ---- Embedded GM patch bank (no file I/O, no sscanf) ---- */
#include "gm_opm_patches.h"

int OPM_LoadEmbeddedGM(void)
{
    memcpy(opm_patches, gm_opm_patch_data, 128 * sizeof(opm_patch_t));
    return 128;
}

/* ---- YM2151 register writing ---- */

/* Track per-channel state for velocity scaling and live volume update */
static int channel_patch[OPM_NUM_CHANNELS];
static int channel_velocity[OPM_NUM_CHANNELS];

/* Apply carrier TL with velocity + master volume attenuation.
 * ONLY called from ISR context (NoteOn or volume_dirty check). */
static void apply_carrier_tl(int channel)
{
    int patch_num = channel_patch[channel];
    const opm_patch_t *p;
    int con, atten, tl;

    if (patch_num < 0 || patch_num >= OPM_NUM_PATCHES) return;
    p = &opm_patches[patch_num];
    con = p->rl_fb_con & 0x07;

    /* Master slider attenuation + gentle velocity dynamics. */
    atten = opm_master_atten + (127 - channel_velocity[channel]) / 8;
    if (atten > 127) atten = 127;

    /* X68000 FM output is much quieter than ADPCM SFX in the audio mix.
     * Boost carrier TL by 20 steps (15 dB) to bring FM music to a
     * comparable level. Carrier TL = base - boost + atten, clamped. */
#define FM_TL_BOOST 14

    /* C1 (ops[2]) -- carrier in algo 4-7 */
    if (con >= 4) {
        tl = p->ops[2].tl - FM_TL_BOOST + atten;
        if (tl < 0) tl = 0;
        if (tl > 127) tl = 127;
        opm_set(0x60 + slot_offset[2] + channel, (uint8_t)tl);
    }
    /* C2 (ops[3]) -- always a carrier */
    tl = p->ops[3].tl - FM_TL_BOOST + atten;
    if (tl < 0) tl = 0;
    if (tl > 127) tl = 127;
    opm_set(0x60 + slot_offset[3] + channel, (uint8_t)tl);
    /* M1 (ops[0]) -- carrier in algo 7 */
    if (con == 7) {
        tl = p->ops[0].tl - FM_TL_BOOST + atten;
        if (tl < 0) tl = 0;
        if (tl > 127) tl = 127;
        opm_set(0x60 + slot_offset[0] + channel, (uint8_t)tl);
    }
    /* M2 (ops[1]) -- carrier in algo 7 */
    if (con == 7) {
        tl = p->ops[1].tl - FM_TL_BOOST + atten;
        if (tl < 0) tl = 0;
        if (tl > 127) tl = 127;
        opm_set(0x60 + slot_offset[1] + channel, (uint8_t)tl);
    }
}

void OPM_LoadPatch(int channel, int patch_num)
{
    const opm_patch_t *p;
    int op, reg_base;

    if (patch_num < 0 || patch_num >= OPM_NUM_PATCHES) return;
    if (channel < 0 || channel >= OPM_NUM_CHANNELS) return;

    p = &opm_patches[patch_num];
    channel_patch[channel] = patch_num;

    /* Write RL/FB/CON for this channel */
    opm_set(0x20 + channel, p->rl_fb_con);

    /* Write per-operator registers for all 4 operators */
    for (op = 0; op < 4; op++)
    {
        reg_base = slot_offset[op] + channel;
        opm_set(0x40 + reg_base, p->ops[op].dt1_mul);
        opm_set(0x60 + reg_base, p->ops[op].tl);
        opm_set(0x80 + reg_base, p->ops[op].ks_ar);
        opm_set(0xA0 + reg_base, p->ops[op].d1r);
        opm_set(0xC0 + reg_base, p->ops[op].dt2_d2r);
        opm_set(0xE0 + reg_base, p->ops[op].d1l_rr);
    }
}

void OPM_NoteOn(int channel, int note, int velocity)
{
    uint8_t kc;

    if (channel < 0 || channel >= OPM_NUM_CHANNELS) return;

    kc = midi_note_to_kc(note);
    channel_velocity[channel] = velocity;
    channel_base_kc[channel] = kc;

    opm_set(0x28 + channel, kc);
    opm_set(0x30 + channel, 0);

    apply_carrier_tl(channel);

    /* Key on all 4 operators */
    opm_set(0x08, (uint8_t)(0x78 | channel));
}

void OPM_NoteOff(int channel)
{
    if (channel < 0 || channel >= OPM_NUM_CHANNELS) return;
    opm_set(0x08, (uint8_t)(0x00 | channel));
}

void OPM_PitchBend(int channel, int bend)
{
    /* bend: MUS pitchbend value 0-255, center=128, range +/-2 semitones.
     * YM2151 KC: semitone steps (see note_to_kc_semitone table).
     * YM2151 KF: 6-bit fraction between semitones (64 steps per semitone).
     * +/-2 semitones = +/-128 KF units. bend 0-255 maps directly:
     *   offset_kf = (bend - 128) = -128..+127 KF units. */
    int base_kc, octave, semi, offset_kf, new_semi_64, frac;
    int new_octave, new_semi, new_kc;

    if (channel < 0 || channel >= OPM_NUM_CHANNELS) return;

    base_kc = channel_base_kc[channel];
    octave = (base_kc >> 4) & 7;
    semi = base_kc & 0x0F;

    /* Convert KC semitone encoding back to linear (0-11) */
    /* KC note: 0,1,2,4,5,6,8,9,10,12,13,14 -> linear 0-11 */
    {
        static const int kc_to_linear[15] = {
            0,1,2,2, 3,4,5,5, 6,7,8,8, 9,10,11,11
        };
        if (semi > 14) semi = 14;
        semi = kc_to_linear[semi];
    }

    offset_kf = bend - 128;  /* -128 to +127 */

    /* Absolute position in KF units: base semitone * 64 + bend offset */
    new_semi_64 = semi * 64 + offset_kf;

    /* Extract absolute semitone and fractional KF */
    if (new_semi_64 >= 0) {
        new_semi = new_semi_64 / 64;
        frac = new_semi_64 % 64;
    } else {
        /* Handle negative: floor division */
        new_semi = -1 + (new_semi_64 + 1) / 64;
        frac = new_semi_64 - new_semi * 64;
    }

    new_octave = octave;
    while (new_semi >= 12) { new_semi -= 12; new_octave++; }
    while (new_semi < 0) { new_semi += 12; new_octave--; }
    if (new_octave < 0) { new_octave = 0; new_semi = 0; frac = 0; }
    if (new_octave > 7) { new_octave = 7; new_semi = 11; frac = 63; }

    new_kc = (new_octave << 4) | note_to_kc_semitone[new_semi];

    opm_set(0x28 + channel, (uint8_t)new_kc);
    opm_set(0x30 + channel, (uint8_t)((frac & 0x3F) << 2));
}

void OPM_AllNotesOff(void)
{
    int ch;
    for (ch = 0; ch < OPM_NUM_CHANNELS; ch++)
        opm_set(0x08, (uint8_t)(0x00 | ch));
}

void OPM_SetVolume(int volume)
{
    /* Convert 0-15 slider to TL attenuation.
     * vol 15 = 0 atten, vol 0 = 60 atten (45 dB). */
    if (volume <= 0)
        opm_master_atten = 60;
    else if (volume >= 15)
        opm_master_atten = 0;
    else
        opm_master_atten = (15 - volume) * 4;

    /* Flag for ISR to re-apply TL on all channels.
     * We can't write OPM registers from main thread -- the Timer-A ISR
     * also writes OPM regs, and the address register would get clobbered
     * if the ISR fires mid-write. */
    volume_dirty = 1;
}

/* Called from the Timer-A ISR to safely apply pending volume changes. */
void OPM_CheckVolumeDirty(void)
{
    if (volume_dirty) {
        int ch;
        volume_dirty = 0;
        for (ch = 0; ch < OPM_NUM_CHANNELS; ch++)
            apply_carrier_tl(ch);
    }
}

#ifndef OPM_MUSIC_H
#define OPM_MUSIC_H

#include <stdint.h>

/* OPM (YM2151) music driver for X68000 Doom.
 *
 * Parses the GENMIDI WAD lump containing OPL2 FM instrument definitions,
 * converts them to YM2151 register values, and provides note-level
 * control of the 8 YM2151 channels via IOCS OPMSET(). */

#define OPM_NUM_PATCHES     175  /* 128 GM instruments + 47 percussion */
#define OPM_NUM_CHANNELS    8    /* YM2151 has 8 FM channels */

/* Per-operator register values (pre-converted for YM2151). */
typedef struct {
    uint8_t dt1_mul;     /* DT1/MUL register ($40+) */
    uint8_t tl;          /* Total Level register ($60+) */
    uint8_t ks_ar;       /* Key Scale / Attack Rate register ($80+) */
    uint8_t d1r;         /* First Decay Rate register ($A0+) */
    uint8_t dt2_d2r;     /* DT2 / Second Decay Rate register ($C0+) */
    uint8_t d1l_rr;      /* Decay 1 Level / Release Rate register ($E0+) */
} opm_op_t;

/* Pre-converted instrument patch for YM2151.
 * Packed to ensure consistent layout for embedded patch data. */
typedef struct __attribute__((packed)) {
    opm_op_t ops[4];     /* M1(slot0), M2(slot1), C1(slot2), C2(slot3) */
    uint8_t  rl_fb_con;  /* RL/FB/CON register ($20+): output + feedback + algorithm */
    int16_t  note_offset; /* base note offset from GENMIDI (in semitones) */
    uint8_t  fixed_pitch; /* 1 if instrument plays fixed note (percussion) */
    uint8_t  fixed_note;  /* MIDI note for fixed-pitch instruments */
} opm_patch_t;

/* Converted patch bank (populated by OPM_ParseGENMIDI). */
extern opm_patch_t opm_patches[OPM_NUM_PATCHES];

/* Parse GENMIDI lump and convert all 175 OPL2 instruments to YM2151 patches.
 * lump_data points to the raw GENMIDI lump from the WAD. */
void OPM_ParseGENMIDI(const void *lump_data);

/* Load native YM2151 patches from a .opm file (VOPM format).
 * Returns number of patches loaded (0 if file not found). */
int OPM_LoadOPMFile(const char *filename);

/* Load embedded GM patch bank (FB-01 factory presets, no file I/O). */
int OPM_LoadEmbeddedGM(void);

/* Load a patch into a YM2151 channel (0-7). Writes all operator registers. */
void OPM_LoadPatch(int channel, int patch_num);

/* Play a note on a YM2151 channel. note = MIDI note (0-127), vel = 0-127. */
void OPM_NoteOn(int channel, int note, int velocity);

/* Release a note on a YM2151 channel (key off). */
void OPM_NoteOff(int channel);

/* Apply pitchbend to a YM2151 channel. bend: 0-255 (128=center, ±2 semitones). */
void OPM_PitchBend(int channel, int bend);

/* Silence all 8 channels immediately. */
void OPM_AllNotesOff(void);

/* Set master volume attenuation (0=loudest, 127=silent). */
void OPM_SetVolume(int volume);

/* Check for pending volume change (call from ISR only). */
void OPM_CheckVolumeDirty(void);

#endif /* OPM_MUSIC_H */

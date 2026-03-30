#ifndef MUS_PLAYER_H
#define MUS_PLAYER_H

/* MUS format music player for X68000 Doom.
 * Parses Doom's MUS music lumps and drives the YM2151 via opm_music.c.
 * Uses a timer interrupt at 140 Hz (the MUS tick rate). */

/* Start playing a MUS lump.  data = raw MUS lump from WAD.
 * looping: 1 = restart when reaching end of score. */
void MUS_Start(const void *data, int looping);

/* Stop playback and silence all channels. */
void MUS_Stop(void);

/* Pause playback (silence but remember position). */
void MUS_Pause(void);

/* Resume playback after pause. */
void MUS_Resume(void);

/* Process pending MUS ticks and write OPM registers.
 * Must be called from the main thread (e.g. I_UpdateSound). */
void MUS_Update(void);

/* Returns 1 if music is currently playing. */
int MUS_IsPlaying(void);

/* Force-stop the timer ISR (for final shutdown). */
void MUS_ShutdownTimer(void);

/* Set output mode: opm=1 for YM2151 FM, midi=1 for external MIDI.
 * Both can be 0 (silence) but not both 1 (pick one). */
void MUS_SetOutputMode(int opm, int midi);

#endif /* MUS_PLAYER_H */

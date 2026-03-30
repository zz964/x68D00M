/* midi_out.h -- External MIDI output for X68000 Doom.
 *
 * Detects the CZ-6BM1 MIDI interface board and sends raw MIDI
 * messages for external sound modules (SC-55, MT-32, etc.).
 * Safe to call even without MIDI hardware -- all functions are
 * no-ops if no board is detected. */

#ifndef MIDI_OUT_H
#define MIDI_OUT_H

/* Detect MIDI hardware and initialize if present.
 * Returns 1 if MIDI is available, 0 otherwise. */
int MIDI_Init(void);

/* Shut down MIDI: send All Notes Off, All Sound Off on all channels. */
void MIDI_Shutdown(void);

/* Returns 1 if MIDI hardware was detected. */
int MIDI_IsAvailable(void);

/* Send a single raw MIDI byte. */
void MIDI_SendByte(unsigned char byte);

/* Convenience: send a complete MIDI message (1-3 bytes). */
void MIDI_NoteOn(int channel, int note, int velocity);
void MIDI_NoteOff(int channel, int note);
void MIDI_ProgramChange(int channel, int program);
void MIDI_ControlChange(int channel, int controller, int value);
void MIDI_PitchBend(int channel, int value);
void MIDI_AllNotesOff(void);

/* Send GM System On sysex to reset the module to GM mode. */
void MIDI_SendGMReset(void);

/* Select YM3802 register group 5 for TX. Call once before a
 * batch of MIDI_SendByte calls (e.g., at ISR entry). */
void midi_select_tx(void);

/* Drain queued MIDI bytes to hardware (non-blocking).
 * Call from timer ISR every tick to keep MIDI flowing. */
void MIDI_DrainBuffer(void);

#endif /* MIDI_OUT_H */

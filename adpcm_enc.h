#ifndef ADPCM_ENC_H
#define ADPCM_ENC_H

#include <stdint.h>

/* OKI MSM6258 ADPCM encoder.
 *
 * Converts signed 16-bit PCM samples to 4-bit OKI ADPCM.
 * Two nibbles are packed per byte (first sample in high nibble).
 *
 * Returns the number of ADPCM bytes written to dst.
 * dst must be at least (num_samples + 1) / 2 bytes. */
int adpcm_encode(const int16_t *pcm, uint8_t *dst, int num_samples);

/* Resample 8-bit unsigned PCM (Doom format, 128=silence) from
 * src_rate to dst_rate using linear interpolation, outputting
 * signed 16-bit PCM.  Returns number of output samples.
 * dst must be large enough: at least (num_samples * dst_rate / src_rate + 2). */
int pcm_resample_u8_to_s16(const uint8_t *src, int num_samples, int src_rate,
                            int16_t *dst, int dst_rate);

#endif /* ADPCM_ENC_H */

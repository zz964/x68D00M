/* adpcm_enc.c -- OKI MSM6258 4-bit ADPCM encoder + PCM resampler.
 *
 * The OKI ADPCM algorithm uses a 49-entry step size table and a
 * 16-entry index adjustment table.  Each 4-bit nibble encodes the
 * difference between the current sample and the predicted value.
 *
 * Reference: OKI Semiconductor MSM6258 datasheet. */

#include "adpcm_enc.h"

/* Step size table (49 entries). */
static const int16_t step_table[49] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,
    41,  45,  50,  55,  60,  66,  73,  80,  88,  97,
   107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
   279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
   724, 796, 876, 963,1060,1166,1282,1411,1552
};

/* Index adjustment per nibble value (4-bit code bits 0-2). */
static const int8_t index_adjust[8] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

int adpcm_encode(const int16_t *pcm, uint8_t *dst, int num_samples)
{
    int predicted = 0;
    int index = 0;
    int out_bytes = 0;
    int i;

    for (i = 0; i < num_samples; i++)
    {
        int diff = pcm[i] - predicted;
        int step = step_table[index];
        int nibble = 0;
        int delta = 0;

        if (diff < 0) {
            nibble = 8;  /* sign bit */
            diff = -diff;
        }

        if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
        step >>= 1;
        if (diff >= step) { nibble |= 2; diff -= step; delta += step; }
        step >>= 1;
        if (diff >= step) { nibble |= 1;               delta += step; }
        delta += step_table[index] >> 3;  /* add 1/8 step (rounding) */

        if (nibble & 8)
            predicted -= delta;
        else
            predicted += delta;

        /* Clamp to 12-bit signed range (OKI uses 12-bit internal DAC). */
        if (predicted > 2047) predicted = 2047;
        if (predicted < -2048) predicted = -2048;

        /* Update step index. */
        index += index_adjust[nibble & 7];
        if (index < 0) index = 0;
        if (index > 48) index = 48;

        /* Pack two nibbles per byte: first sample in high nibble. */
        if (i & 1)
        {
            dst[out_bytes] |= (nibble & 0x0F);
            out_bytes++;
        }
        else
        {
            dst[out_bytes] = (nibble & 0x0F) << 4;
        }
    }

    /* If odd number of samples, the last byte has a zero low nibble. */
    if (num_samples & 1)
        out_bytes++;

    return out_bytes;
}

int pcm_resample_u8_to_s16(const uint8_t *src, int num_samples, int src_rate,
                            int16_t *dst, int dst_rate)
{
    /* Linear interpolation resampler.
     * Input: 8-bit unsigned PCM (128 = silence).
     * Output: signed 16-bit PCM. */
    int out_count = 0;
    unsigned int pos_frac = 0;  /* 16.16 fixed-point position */
    unsigned int step = ((unsigned int)src_rate << 16) / (unsigned int)dst_rate;
    int i;

    /* Scale 8-bit unsigned [0..255] to signed 16-bit [-32768..32512]. */
    for (i = 0; ; i++)
    {
        unsigned int pos_int = pos_frac >> 16;
        unsigned int frac = pos_frac & 0xFFFF;
        int s0, s1, val;

        if ((int)pos_int >= num_samples - 1)
            break;

        /* Scale 8-bit unsigned [0..255] to 12-bit signed [-2048..2016]
         * to match the OKI MSM6258's 12-bit internal DAC range.
         * Using <<4 instead of <<8 avoids heavy clipping distortion. */
        s0 = ((int)src[pos_int] - 128) << 4;
        s1 = ((int)src[pos_int + 1] - 128) << 4;
        val = s0 + (int)(((long)(s1 - s0) * frac) >> 16);

        dst[out_count++] = (int16_t)val;
        pos_frac += step;
    }

    return out_count;
}

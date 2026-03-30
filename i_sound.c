/* i_sound.c -- X68000 sound interface.
 *
 * Sound effects: Two paths depending on hardware:
 *   Standard: Doom's 8-bit 11025 Hz PCM is resampled to 15625 Hz and
 *     encoded as 4-bit OKI ADPCM at load time. Playback uses IOCS
 *     ADPCMOUT which PCM8A intercepts for multi-channel mixing.
 *   Mercury Unit V4 + PCM8PP: PCM resampled to configurable rate (16-48 kHz)
 *     8-bit signed. Playback uses PCM8PP TRAP #2 with per-channel hardware
 *     volume and multi-channel mixing. Faster startup (no ADPCM encoding).
 *
 * Requires PCM8A.X (standard) or PCM8PP.X (Mercury Unit). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <iocslib.h>
#include <doslib.h>
#include "midi_out.h"

#include "z_zone.h"
#include "i_system.h"

extern int numlumps;  /* from w_wad.c, used for SFX cache validation */
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "doomdef.h"
#include "doomstat.h"
#include "sounds.h"
#include "adpcm_enc.h"
#include "mus_player.h"

/* ---- Mercury Unit V4 detection and PCM8PP support ---- */

#define MERCURY_STATUS  0xECC090

int use_mercury = 0;           /* 1 = PCM path via PCM8PP detected */
int pcm_rate_index = 0;        /* 0=16kHz,1=22kHz,2=32kHz,3=44kHz,4=48kHz */
static int active_rate_index;  /* rate the PCM data was actually encoded at */

/* PCM8PP sample rate table: Hz, format code, display name.
 * 8-bit signed PCM mono formats from PCM8PP.TEC:
 *   $10=15.625k  $11=16k  $12=22.05k  $13=24k
 *   $14=32k      $15=44.1k  $16=48k */
typedef struct { int hz; int fmt; const char *name; } pcm_rate_entry_t;
static const pcm_rate_entry_t pcm_rate_table[] = {
    { 15625, 0x10, "16 KHZ"  }
};
#define NUM_PCM_RATES 1

/* Check if PCM8PP (not PCM8A) is the resident TRAP #2 handler.
 * Scan memory near the handler entry point for "PCM8++" signature. */
static int is_pcm8pp(uint32_t handler_addr)
{
    const char *p = (const char *)handler_addr;
    int i;
    /* Scan 8KB around the handler for the PCM8++ signature */
    for (i = -2048; i < 6144; i++) {
        if (p[i]=='P' && p[i+1]=='C' && p[i+2]=='M' &&
            p[i+3]=='8' && p[i+4]=='+' && p[i+5]=='+') {
            printf("  PCM driver: PCM8++ found at offset %d from handler\n", i);
            return 1;
        }
    }
    /* Also check for "pcm8pp" lowercase */
    for (i = -2048; i < 6144; i++) {
        if (p[i]=='p' && p[i+1]=='c' && p[i+2]=='m' &&
            p[i+3]=='8' && p[i+4]=='p' && p[i+5]=='p') {
            printf("  PCM driver: pcm8pp found at offset %d from handler\n", i);
            return 1;
        }
    }
    /* Log what we found near the handler for debugging */
    printf("  PCM driver at $%06lX: ", (unsigned long)handler_addr);
    for (i = 0; i < 32; i++)
        printf("%02X", (unsigned char)p[i]);
    printf("\n");
    /* Search for any "PCM8" string */
    for (i = -2048; i < 6144; i++) {
        if (p[i]=='P' && p[i+1]=='C' && p[i+2]=='M' && p[i+3]=='8') {
            printf("  Found PCM8 variant at offset %d: %.16s\n", i, &p[i]);
            break;
        }
    }
    return 0;
}

static int detect_mercury(void)
{
    uint32_t trap2_vec;
    int have_pcm8pp = 0;

    /* Check Mercury Unit hardware at $ECC090. */
    if (BUS_ERR((UBYTE *)MERCURY_STATUS, (UBYTE *)(MERCURY_STATUS + 2), 1) != 0)
        return 0;

    /* Check TRAP #2 vector for a loaded PCM driver. */
    trap2_vec = *(volatile uint32_t *)0x88;
    if (trap2_vec == 0 || trap2_vec >= 0x01000000) {
        printf("  Mercury Unit detected but no PCM driver loaded.\n");
        return 0;
    }

    have_pcm8pp = is_pcm8pp(trap2_vec);

    if (!have_pcm8pp) {
        printf("  Mercury Unit detected with PCM8A (not PCM8PP).\n");
        return 0;
    }

    printf("  Mercury Unit + PCM8PP detected. PCM audio enabled.\n");
    return 1;
}

/* PCM8PP TRAP #2 interface.
 * Volume: 0-15 (8 = 0dB, each step = 2dB, range -16dB to +14dB). */
#define PCM8PP_MAX_CHANNELS  8

static int pcm8pp_channel = 0;


static void pcm8pp_play(const int8_t *data, int length, int volume)
{
    int ch = pcm8pp_channel;
    pcm8pp_channel = (pcm8pp_channel + 1) % PCM8PP_MAX_CHANNELS;

    {
        register long r_d0 __asm__("d0") = (long)ch;
        register long r_d1 __asm__("d1") =
            ((long)volume << 16) | (pcm_rate_table[active_rate_index].fmt << 8) | 3;
        register long r_d2 __asm__("d2") = (long)length;
        register long r_d3 __asm__("d3") = 0;
        register const int8_t *r_a1 __asm__("a1") = data;
        __asm__ volatile (
            "trap #2"
            : "+d"(r_d0)
            : "d"(r_d1), "d"(r_d2), "d"(r_d3), "a"(r_a1)
            : "memory"
        );
    }
}

/* ---- Common constants ---- */

#define SFX_HEADER_SIZE  8

/* ADPCM path */
#define ADPCM_RATE      15625
#define ADPCM_MODE      0x0403
#define SFX_VOL_LEVELS  4
static const int sfx_vol_scale[SFX_VOL_LEVELS] = { 100, 66, 33, 15 };
static const int sfx_vol_thresh[SFX_VOL_LEVELS] = { 8, 4, 2, 1 };

static uint8_t *sfx_adpcm[NUMSFX][SFX_VOL_LEVELS];
static int      sfx_adpcm_len[NUMSFX];

/* Mercury path: rate set by pcm_rate_index (default 15625 Hz) */
#define MERCURY_RATE    (pcm_rate_table[active_rate_index].hz)

static int8_t  *sfx_pcm[NUMSFX];
static int      sfx_pcm_len[NUMSFX];

static int      snd_handle = 0;

/* ---- Mercury Unit SFX conversion (8-bit signed PCM) ---- */

static void convert_sfx_mercury(int sfxid)
{
    int lump, lump_size, pcm_samples, src_rate, out_count, i;
    const uint8_t *raw;
    char name[20];

    if (S_sfx[sfxid].link) {
        int link_id = (S_sfx[sfxid].link - S_sfx);
        sfx_pcm[sfxid] = sfx_pcm[link_id];
        sfx_pcm_len[sfxid] = sfx_pcm_len[link_id];
        S_sfx[sfxid].data = sfx_pcm[sfxid];
        return;
    }

    sprintf(name, "ds%s", S_sfx[sfxid].name);
    if (W_CheckNumForName(name) == -1)
        lump = W_GetNumForName("dspistol");
    else
        lump = W_GetNumForName(name);

    lump_size = W_LumpLength(lump);
    if (lump_size <= SFX_HEADER_SIZE) {
        sfx_pcm[sfxid] = NULL;
        sfx_pcm_len[sfxid] = 0;
        return;
    }

    raw = (const uint8_t *)W_CacheLumpNum(lump, PU_STATIC);
    src_rate = raw[2] | (raw[3] << 8);
    pcm_samples = raw[4] | (raw[5] << 8) | (raw[6] << 16) | (raw[7] << 24);

    if (pcm_samples <= 0 || pcm_samples > lump_size - SFX_HEADER_SIZE)
        pcm_samples = lump_size - SFX_HEADER_SIZE;
    if (src_rate <= 0) src_rate = 11025;

    out_count = (int)((long)pcm_samples * MERCURY_RATE / src_rate) + 2;
    sfx_pcm[sfxid] = (int8_t *)malloc(out_count);
    if (!sfx_pcm[sfxid]) {
        Z_Free((void *)raw);
        sfx_pcm_len[sfxid] = 0;
        return;
    }

    /* Resample with linear interpolation, convert unsigned to signed. */
    {
        const uint8_t *src = raw + SFX_HEADER_SIZE;
        int8_t *dst = sfx_pcm[sfxid];
        long frac = 0;
        long step = ((long)src_rate << 16) / MERCURY_RATE;

        for (i = 0; i < out_count; i++) {
            int pos = (int)(frac >> 16);
            int s0, s1, f, val;
            if (pos >= pcm_samples - 1) {
                out_count = i;
                break;
            }
            s0 = (int)src[pos] - 128;
            s1 = (int)src[pos + 1] - 128;
            f = (int)((frac >> 8) & 0xFF);
            val = s0 + ((s1 - s0) * f >> 8);
            dst[i] = (int8_t)val;
            frac += step;
        }
    }

    Z_Free((void *)raw);
    sfx_pcm_len[sfxid] = out_count;
    S_sfx[sfxid].data = sfx_pcm[sfxid];
}

/* ---- Standard ADPCM SFX conversion ---- */

static void convert_sfx_adpcm(int sfxid)
{
    int lump, lump_size, pcm_samples, src_rate, resampled_count, adpcm_size, lv;
    const uint8_t *raw;
    int16_t *resampled;
    char name[20];

    if (S_sfx[sfxid].link) {
        int link_id = (S_sfx[sfxid].link - S_sfx);
        for (lv = 0; lv < SFX_VOL_LEVELS; lv++)
            sfx_adpcm[sfxid][lv] = sfx_adpcm[link_id][lv];
        sfx_adpcm_len[sfxid] = sfx_adpcm_len[link_id];
        S_sfx[sfxid].data = sfx_adpcm[sfxid][0];
        return;
    }

    sprintf(name, "ds%s", S_sfx[sfxid].name);
    if (W_CheckNumForName(name) == -1)
        lump = W_GetNumForName("dspistol");
    else
        lump = W_GetNumForName(name);

    lump_size = W_LumpLength(lump);
    if (lump_size <= SFX_HEADER_SIZE) {
        for (lv = 0; lv < SFX_VOL_LEVELS; lv++)
            sfx_adpcm[sfxid][lv] = NULL;
        sfx_adpcm_len[sfxid] = 0;
        return;
    }

    raw = (const uint8_t *)W_CacheLumpNum(lump, PU_STATIC);
    src_rate = raw[2] | (raw[3] << 8);
    pcm_samples = raw[4] | (raw[5] << 8) | (raw[6] << 16) | (raw[7] << 24);

    if (pcm_samples <= 0 || pcm_samples > lump_size - SFX_HEADER_SIZE)
        pcm_samples = lump_size - SFX_HEADER_SIZE;
    if (src_rate <= 0) src_rate = 11025;

    resampled_count = (int)((long)pcm_samples * ADPCM_RATE / src_rate) + 2;
    resampled = (int16_t *)malloc(resampled_count * sizeof(int16_t));
    if (!resampled) {
        Z_Free((void *)raw);
        for (lv = 0; lv < SFX_VOL_LEVELS; lv++)
            sfx_adpcm[sfxid][lv] = NULL;
        sfx_adpcm_len[sfxid] = 0;
        return;
    }

    resampled_count = pcm_resample_u8_to_s16(
        raw + SFX_HEADER_SIZE, pcm_samples, src_rate,
        resampled, ADPCM_RATE);

    Z_Free((void *)raw);
    adpcm_size = (resampled_count + 1) / 2;

    for (lv = 0; lv < SFX_VOL_LEVELS; lv++) {
        sfx_adpcm[sfxid][lv] = (uint8_t *)malloc(adpcm_size);
        if (!sfx_adpcm[sfxid][lv]) continue;

        if (sfx_vol_scale[lv] == 100) {
            sfx_adpcm_len[sfxid] = adpcm_encode(resampled,
                sfx_adpcm[sfxid][lv], resampled_count);
        } else {
            int k;
            int scale = sfx_vol_scale[lv];
            int16_t *scaled = (int16_t *)malloc(resampled_count * sizeof(int16_t));
            if (!scaled) { free(sfx_adpcm[sfxid][lv]); sfx_adpcm[sfxid][lv] = NULL; continue; }
            for (k = 0; k < resampled_count; k++)
                scaled[k] = (int16_t)((int)resampled[k] * scale / 100);
            sfx_adpcm_len[sfxid] = adpcm_encode(scaled,
                sfx_adpcm[sfxid][lv], resampled_count);
            free(scaled);
        }
    }

    free(resampled);
    S_sfx[sfxid].data = sfx_adpcm[sfxid][0];
}

/* ---- SFX disk cache ----
 * Saves converted SFX to a cache file so subsequent startups skip encoding.
 * Cache is invalidated when WAD, sample rate, or volume levels change. */

#define SFX_CACHE_MAGIC   0x58364643  /* "X6FC" */
#define SFX_CACHE_VERSION 3

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t numlumps;    /* WAD lump count for extra validation */
    uint32_t is_mercury;  /* 0=ADPCM, 1=Mercury PCM */
    uint32_t rate_index;  /* Mercury rate or 0 for ADPCM */
    uint32_t num_sfx;     /* NUMSFX */
} sfx_cache_header_t;

/* Build cache filename from loaded WAD names.
 * e.g. "doom.wad" -> "doom_pcm.cache" or "doom_adp.cache"
 * With PWADs: hash all names into a suffix to distinguish combos. */
static const char *sfx_cache_name(void)
{
    static char name[64];
    extern char *wadfiles[];
    char *base;
    char *p, *dot;
    int i;

    if (!wadfiles[0])
        return use_mercury ? "doom_pcm.cache" : "doom_adp.cache";

    /* Extract base name without path */
    base = wadfiles[0];
    p = base;
    while (*p) {
        if (*p == '/' || *p == '\\') base = p + 1;
        p++;
    }

    /* Copy up to the dot */
    dot = name;
    for (p = base; *p && *p != '.' && (dot - name) < 40; p++)
        *dot++ = *p;

    /* If PWADs are loaded, append a simple hash to distinguish */
    if (wadfiles[1]) {
        unsigned int h = 0;
        for (i = 1; wadfiles[i]; i++) {
            char *w = wadfiles[i];
            while (*w) h = h * 31 + (unsigned char)*w++;
        }
        sprintf(dot, "_%04x", h & 0xFFFF);
        dot += 5;
    }

    sprintf(dot, "_%s.cache", use_mercury ? "pcm" : "adp");
    return name;
}

static int sfx_cache_load(void)
{
    FILE *fp;
    sfx_cache_header_t hdr;
    int i;
    const char *cachefile = sfx_cache_name();

    /* Check file exists before fopen -- avoids issues with fopen
     * after certain IOCS calls (Mercury detection) on some systems. */
    if (access(cachefile, 0) != 0) return 0;

    printf("  SFX cache: reading %s...\n", cachefile);
    fp = fopen(cachefile, "rb");
    if (!fp) return 0;

    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) { fclose(fp); return 0; }
    if (hdr.magic != SFX_CACHE_MAGIC ||
        hdr.version != SFX_CACHE_VERSION ||
        hdr.numlumps != (uint32_t)numlumps ||
        hdr.is_mercury != (uint32_t)use_mercury ||
        hdr.num_sfx != (uint32_t)NUMSFX) {
        fclose(fp);
        return 0;
    }
    if (use_mercury && hdr.rate_index != (uint32_t)active_rate_index) {
        fclose(fp);
        return 0;
    }

    /* Sanity check: file must be larger than just the header.
     * Also reject impossibly large files (>4MB) to guard against
     * corrupted length fields causing huge mallocs. */
    {
        long pos;
        fseek(fp, 0, SEEK_END);
        pos = ftell(fp);
        fseek(fp, sizeof(hdr), SEEK_SET);
        if (pos <= (long)sizeof(hdr) || pos > 4 * 1024 * 1024) {
            fclose(fp);
            return 0;
        }
    }

    if (use_mercury) {
        /* Read Mercury PCM data */
        for (i = 1; i < NUMSFX; i++) {
            int32_t len;
            if (fread(&len, 4, 1, fp) != 1) goto fail;
            if (len < 0 || len > 256 * 1024) goto fail;  /* sanity: max 256KB per SFX */
            sfx_pcm_len[i] = len;
            if (len > 0) {
                sfx_pcm[i] = (int8_t *)malloc(len);
                if (!sfx_pcm[i]) goto fail;
                if (fread(sfx_pcm[i], 1, len, fp) != (size_t)len) goto fail;
            } else {
                sfx_pcm[i] = NULL;
            }
            S_sfx[i].data = sfx_pcm[i];
        }
    } else {
        /* Read ADPCM data */
        for (i = 1; i < NUMSFX; i++) {
            int32_t len;
            int lv;
            if (fread(&len, 4, 1, fp) != 1) goto fail;
            if (len < 0 || len > 256 * 1024) goto fail;
            sfx_adpcm_len[i] = len;
            for (lv = 0; lv < SFX_VOL_LEVELS; lv++) {
                int32_t lvlen;
                if (fread(&lvlen, 4, 1, fp) != 1) goto fail;
                if (lvlen < 0 || lvlen > 256 * 1024) goto fail;
                if (lvlen > 0) {
                    sfx_adpcm[i][lv] = (uint8_t *)malloc(lvlen);
                    if (!sfx_adpcm[i][lv]) goto fail;
                    if (fread(sfx_adpcm[i][lv], 1, lvlen, fp) != (size_t)lvlen)
                        goto fail;
                } else {
                    sfx_adpcm[i][lv] = NULL;
                }
            }
            S_sfx[i].data = sfx_adpcm[i][0];
        }
    }

    fclose(fp);
    return 1;

fail:
    fclose(fp);
    return 0;
}

static void sfx_cache_save(void)
{
    FILE *fp;
    sfx_cache_header_t hdr;
    int i;
    const char *cachefile = sfx_cache_name();
    int ok = 1;

    fp = fopen(cachefile, "wb");
    if (!fp) return;

    hdr.magic = SFX_CACHE_MAGIC;
    hdr.version = SFX_CACHE_VERSION;
    hdr.numlumps = (uint32_t)numlumps;
    hdr.is_mercury = (uint32_t)use_mercury;
    hdr.rate_index = use_mercury ? (uint32_t)active_rate_index : 0;
    hdr.num_sfx = (uint32_t)NUMSFX;
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) ok = 0;

    if (ok && use_mercury) {
        for (i = 1; i < NUMSFX && ok; i++) {
            int32_t len = sfx_pcm_len[i];
            if (fwrite(&len, 4, 1, fp) != 1) { ok = 0; break; }
            if (len > 0 && sfx_pcm[i])
                if (fwrite(sfx_pcm[i], 1, len, fp) != (size_t)len)
                    { ok = 0; break; }
        }
    } else if (ok) {
        for (i = 1; i < NUMSFX && ok; i++) {
            int32_t len = sfx_adpcm_len[i];
            int lv;
            if (fwrite(&len, 4, 1, fp) != 1) { ok = 0; break; }
            for (lv = 0; lv < SFX_VOL_LEVELS && ok; lv++) {
                int32_t lvlen = (sfx_adpcm[i][lv] && len > 0) ? len : 0;
                if (fwrite(&lvlen, 4, 1, fp) != 1) { ok = 0; break; }
                if (lvlen > 0)
                    if (fwrite(sfx_adpcm[i][lv], 1, lvlen, fp) != (size_t)lvlen)
                        { ok = 0; break; }
            }
        }
    }

    fclose(fp);

    if (!ok) {
        /* Write failed (disk full?) -- delete partial cache */
        remove(cachefile);
        printf("  SFX cache: write failed, removed %s\n", cachefile);
    }
}


/* ---- Init / Play / Stop ---- */

void I_InitSound(void)
{
    int i;

    use_mercury = detect_mercury();

    if (use_mercury) {
        if (pcm_rate_index < 0) pcm_rate_index = 0;
        if (pcm_rate_index >= NUM_PCM_RATES) pcm_rate_index = 0;
        active_rate_index = pcm_rate_index;
        {
            static const char *pcm8pp_sflag[] = {
                "-S3", "-S4", "-S0", "-S1", "-S2"
            };
            printf("I_InitSound: Mercury Unit + PCM8PP detected (%s PCM)\n",
                   pcm_rate_table[active_rate_index].name);
            printf("  Recommended PCM8PP option: pcm8pp %s\n",
                   pcm8pp_sflag[active_rate_index]);
        }
        memset(sfx_pcm, 0, sizeof(sfx_pcm));
        memset(sfx_pcm_len, 0, sizeof(sfx_pcm_len));

        if (sfx_cache_load()) {
            printf("  SFX: loaded from cache (%s)\n", sfx_cache_name());
        } else {
            printf("  SFX: converting from WAD...\n");
            for (i = 1; i < NUMSFX; i++) {
                convert_sfx_mercury(i);
                if ((i % 20) == 0 || i == NUMSFX - 1)
                    printf("  SFX: %d/%d\n", i, NUMSFX - 1);
            }
            printf("  SFX cache: writing %s...\n", sfx_cache_name());
            sfx_cache_save();
            printf("  SFX cache: done\n");
        }
    } else {
        /* Check if a PCM driver (PCM8A or similar) is loaded.
         * Without one, IOCS ADPCMOUT plays only one sound at a time.
         * TRAP #2 vector at $88: 0 or >= $01000000 means no driver. */
        {
            uint32_t trap2_vec = *(volatile uint32_t *)0x88;
            if (trap2_vec == 0 || trap2_vec >= 0x01000000) {
                int has_mercury = (BUS_ERR((UBYTE *)MERCURY_STATUS,
                                  (UBYTE *)(MERCURY_STATUS + 2), 1) == 0);
                printf("I_InitSound: no PCM driver detected.\n");
                printf("  SFX will play one at a time (no mixing).\n");
                if (has_mercury) {
                    printf("  Mercury Unit detected. Load PCM8PP.X -S3\n");
                    printf("  for multi-channel SFX with hardware volume.\n");
                    printf("  Mercury Unit V4: run REQEN.X first.\n");
                } else {
                    printf("  For multi-channel SFX mixing, load PCM8A.X\n");
                    printf("  in CONFIG.SYS (uses some CPU for mixing).\n");
                }
            } else {
                printf("I_InitSound: PCM driver detected (ADPCM mode)\n");
            }
        }
        printf("I_InitSound: converting SFX to ADPCM (%d levels)\n", SFX_VOL_LEVELS);
        memset(sfx_adpcm, 0, sizeof(sfx_adpcm));
        memset(sfx_adpcm_len, 0, sizeof(sfx_adpcm_len));

        if (sfx_cache_load()) {
            printf("  SFX: loaded from cache (%s)\n", sfx_cache_name());
        } else {
            printf("  SFX: converting from WAD...\n");
            for (i = 1; i < NUMSFX; i++) {
                convert_sfx_adpcm(i);
                if ((i % 20) == 0 || i == NUMSFX - 1)
                    printf("  SFX: %d/%d\n", i, NUMSFX - 1);
            }
            printf("  SFX cache: writing %s...\n", sfx_cache_name());
            sfx_cache_save();
            printf("  SFX cache: done\n");
        }
    }

    printf("I_InitSound: sound module ready\n");
}

void I_UpdateSound(void)
{
    MUS_Update();
}

void I_SubmitSound(void)
{
}

void I_ShutdownSound(void)
{
    if (use_mercury) {
        /* Stop all PCM8PP channels */
        int ch;
        for (ch = 0; ch < PCM8PP_MAX_CHANNELS; ch++) {
            register long r_d0 __asm__("d0") = (long)ch;
            register long r_d1 __asm__("d1") = -1;
            register long r_d2 __asm__("d2") = 0;  /* length 0 = stop */
            register long r_d3 __asm__("d3") = 0;
            register const int8_t *r_a1 __asm__("a1") = NULL;
            __asm__ volatile (
                "trap #2"
                : "+d"(r_d0)
                : "d"(r_d1), "d"(r_d2), "d"(r_d3), "a"(r_a1)
                : "memory"
            );
        }
    } else {
        ADPCMMOD(0);
    }
}

void I_SetChannels(void)
{
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    if (id < 1 || id >= NUMSFX)
        return 0;

    if (use_mercury) {
        /* Mercury path: 8-bit PCM with hardware volume.
         * PCM8PP volume: 0-15, where 8=0dB, each step=2dB.
         *   0=-16dB, 8=0dB, 15=+14dB.
         *
         * Input vol is 0 to snd_SfxVolume (0-15), linearly scaled by
         * distance. Use a lookup table for precise perceptual mapping.
         * PCM8PP 8=0dB is the natural sample level; going above 8
         * amplifies and can distort.
         *
         * Distance table maps game vol 0-15 to PCM8PP vol.
         * Squared falloff: close at pcm 13 (+10dB), far at pcm 1 (-14dB).
         *
         * Slider applied as dB offset in PCM8PP domain (subtracted from
         * table output). Each PCM8PP step = 2dB. This avoids the old
         * problem where scaling the input compounded with the curve. */
        static const int vol_to_pcm[16] = {
         /* game:  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15 */
                   0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 11, 12, 13, 13
        };
        /* Slider attenuation: how many PCM8PP steps to subtract.
         * Most of the range stays near full volume; only the bottom
         * few slider positions pull it down significantly.
         * slider 15-10: no change (0), 8: -1, 4: -3, 0: -13 */
        static const int slider_atten[16] = {
         /* slider: 0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15 */
                   13, 10,  7,  5,  3,  3,  2,  2,  1,  1,  0,  0,  0,  0,  0,  0
        };
        int pcm_vol;
        if (sfx_pcm_len[id] <= 0 || !sfx_pcm[id])
            return 0;
        if (vol <= 0) return 0;
        if (vol > 15) vol = 15;
        pcm_vol = vol_to_pcm[vol] - slider_atten[snd_SfxVolume];
        if (pcm_vol < 1) pcm_vol = 1;
        pcm8pp_play(sfx_pcm[id], sfx_pcm_len[id], pcm_vol);
    } else {
        /* ADPCM path: pre-encoded volume levels. */
        int lv;
        uint8_t *data;

        if (sfx_adpcm_len[id] <= 0)
            return 0;

        for (lv = 0; lv < SFX_VOL_LEVELS; lv++) {
            if (vol >= sfx_vol_thresh[lv])
                break;
        }
        if (lv >= SFX_VOL_LEVELS)
            return 0;

        data = sfx_adpcm[id][lv];
        if (!data)
            data = sfx_adpcm[id][0];
        if (!data)
            return 0;

        ADPCMMOD(0);
        ADPCMOUT((unsigned char *)data, ADPCM_MODE, sfx_adpcm_len[id]);
    }

    snd_handle++;
    return snd_handle;
}

void I_StopSound(int handle)
{
    /* PCM8PP: ADPCMMOD(0) stops the last IOCS channel.
     * Raw ADPCM (no driver): ADPCMMOD(0) stops the hardware channel.
     * PCM8A: ADPCMMOD(0) has no effect on PCM8A mixer channels,
     *   but is harmless. Sounds play to completion regardless. */
    ADPCMMOD(0);
}

int I_SoundIsPlaying(int handle)
{
    if (use_mercury)
        return 0;  /* PCM8PP: channels managed internally */
    return ADPCMSNS() != 0;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
}

/* ---- YM2151 FM music via OPM ---- */

#include "opm_music.h"

static void *registered_song_data = NULL;

void I_InitMusic(void)
{
    int lump;
    int opm_count;

    opm_count = OPM_LoadEmbeddedGM();
    if (opm_count > 0) {
        printf("I_InitMusic: loaded %d embedded GM patches\n", opm_count);
    }

    if (opm_count < 128) {
        lump = W_GetNumForName("GENMIDI");
        if (lump >= 0) {
            if (opm_count == 0) {
                void *genmidi = W_CacheLumpNum(lump, PU_STATIC);
                OPM_ParseGENMIDI(genmidi);
                printf("I_InitMusic: GENMIDI conversion (%d patches)\n",
                       OPM_NUM_PATCHES);
            }
        }
    }

    if (MIDI_Init())
        printf("I_InitMusic: external MIDI output detected\n");

    {
        extern int music_mode;
        if (music_mode < 0) {
            music_mode = MIDI_IsAvailable() ? 2 : 0;
        } else if (music_mode == 2 && !MIDI_IsAvailable()) {
            music_mode = 0;
        }
        I_SetMusicMode(music_mode);
    }

    printf("I_InitMusic: YM2151 FM music ready\n");
}

void I_ShutdownMusic(void)
{
    MUS_Stop();
    MUS_ShutdownTimer();
    OPM_AllNotesOff();
    MIDI_Shutdown();
}

void I_SetMusicVolume(int volume)
{
    OPM_SetVolume(volume);
}

/* Music mode: 0=FM OPL2, 1=FM GM, 2=General MIDI.
 * Called from M_CycleMusicMode when user changes mode. */
void I_SetMusicMode(int mode)
{
    extern int music_mode;

    /* Stop current music and silence both outputs. */
    MUS_Stop();
    OPM_AllNotesOff();
    if (MIDI_IsAvailable())
        MIDI_AllNotesOff();

    /* Apply the mode. */
    music_mode = mode;

    switch (mode) {
    case 0:  /* FM OPL2 Emulation - use GENMIDI conversion */
        {
            int lump = W_GetNumForName("GENMIDI");
            if (lump >= 0) {
                void *genmidi = W_CacheLumpNum(lump, PU_STATIC);
                OPM_ParseGENMIDI(genmidi);
            }
        }
        MUS_SetOutputMode(1, 0);  /* OPM on, MIDI off */
        break;

    case 1:  /* FM GM (FB-01 patches) */
        OPM_LoadEmbeddedGM();
        MUS_SetOutputMode(1, 0);  /* OPM on, MIDI off */
        break;

    case 2:  /* General MIDI */
        if (MIDI_IsAvailable()) {
            MIDI_SendGMReset();
            MUS_SetOutputMode(0, 1);  /* OPM off, MIDI on */
        } else {
            /* No MIDI hardware: fall back to FM GM */
            OPM_LoadEmbeddedGM();
            MUS_SetOutputMode(1, 0);
            players[consoleplayer].message = "No MIDI hardware - using FM";
        }
        break;

    }

    /* Restart the current song if one was playing. */
    if (registered_song_data)
        MUS_Start(registered_song_data, 1);
}

void I_PauseSong(int handle)
{
    MUS_Pause();
}

void I_ResumeSong(int handle)
{
    MUS_Resume();
}

int I_RegisterSong(void *data)
{
    registered_song_data = data;
    return 1;
}

void I_PlaySong(int handle, int looping)
{
    if (registered_song_data)
        MUS_Start(registered_song_data, looping);
}

void I_StopSong(int handle)
{
    MUS_Stop();
}

void I_UnRegisterSong(int handle)
{
    MUS_Stop();
    registered_song_data = NULL;
}

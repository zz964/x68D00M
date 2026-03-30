#ifndef DOOM_PERF_H
#define DOOM_PERF_H

#include <stdint.h>

/* Per-second workload counters.
 * Incremented inline in the asm inner loops (r_draw_asm.S) for near-zero
 * overhead (~2 cycles per call).  Reset each second in i_video.c. */
extern uint32_t g_perf_span_calls;   /* # of spanfunc calls */
extern uint32_t g_perf_span_pixels;  /* total pixels drawn by spanfunc */
extern uint32_t g_perf_col_calls;    /* # of colfunc calls */
extern uint32_t g_perf_col_pixels;   /* total pixels drawn by colfunc */

/* Per-frame microsecond sub-phase accumulators (high-res timer).
 * Accumulated across all frames in a 1-second window, then divided by
 * frame count in the log output for per-frame averages.
 * All values in microseconds (I_GetTimeUs() units). */
extern uint32_t g_perf_bsp_us;          /* R_RenderBSPNode total (includes segs) */
extern uint32_t g_perf_segs_setup_us;   /* R_StoreWallRange setup (scale, tex, angle) */
extern uint32_t g_perf_segs_draw_us;    /* R_RenderSegLoop (column clip + colfunc) */
extern uint32_t g_perf_planes_us;       /* R_DrawPlanes total */
extern uint32_t g_perf_masked_us;       /* R_DrawMasked (sprites + weapons) */
extern uint32_t g_perf_render_us;       /* total R_RenderPlayerView */
extern uint32_t g_perf_blit_us;         /* I_BlitBlock / DMA blit */
extern uint32_t g_perf_visplanes;       /* visplane count (per-window accumulator) */

/* Install lightweight profiling wrappers over spanfunc and colfunc.
 * NOTE: with inline asm counters, this is no longer needed for counting.
 * Kept for backwards compatibility but the call in R_ExecuteSetViewSize
 * can be left commented out. */
void Perf_InstallFuncWrappers(void);

#endif /* DOOM_PERF_H */

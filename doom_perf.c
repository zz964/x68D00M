#include "doom_perf.h"
#include "r_main.h"   /* spanfunc, colfunc, basecolfunc */
#include "r_draw.h"   /* ds_x1, ds_x2, dc_yl, dc_yh */

/* Per-second workload counters (incremented inline in r_draw_asm.S) */
uint32_t g_perf_span_calls   = 0;
uint32_t g_perf_span_pixels  = 0;
uint32_t g_perf_col_calls    = 0;
uint32_t g_perf_col_pixels   = 0;

/* Per-frame microsecond sub-phase accumulators */
uint32_t g_perf_bsp_us          = 0;
uint32_t g_perf_segs_setup_us   = 0;
uint32_t g_perf_segs_draw_us    = 0;
uint32_t g_perf_planes_us       = 0;
uint32_t g_perf_masked_us       = 0;
uint32_t g_perf_render_us       = 0;
uint32_t g_perf_blit_us         = 0;
uint32_t g_perf_visplanes       = 0;

/* Saved originals installed by Perf_InstallFuncWrappers */
static void (*real_spanfunc)(void);
static void (*real_colfunc)(void);

static void span_prof(void)
{
    g_perf_span_calls++;
    g_perf_span_pixels += (uint32_t)(ds_x2 - ds_x1 + 1);
    real_spanfunc();
}

static void col_prof(void)
{
    g_perf_col_calls++;
    g_perf_col_pixels += (uint32_t)(dc_yh - dc_yl + 1);
    real_colfunc();
}

void Perf_InstallFuncWrappers(void)
{
    real_spanfunc = spanfunc;
    spanfunc = span_prof;

    real_colfunc = colfunc;
    /* Also replace basecolfunc so r_things.c "colfunc = basecolfunc" restores
     * the profiling wrapper rather than the bare R_DrawColumn. */
    colfunc = basecolfunc = col_prof;
}

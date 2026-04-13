* r_draw_asm.S  --  hand-optimised hot renderers for X68000 (68030/060)
*
* WHY NOT THE SWAP TRICK:
*   After SWAP, frac.integer is in the LOW word and frac.fraction is in the
*   HIGH word.  Standard ADD.L propagates carry from low->high (integer ->
*   fraction), but we need carry from high->low (fraction -> integer).
*   SWAP + ADD.L therefore silently drops fraction-to-integer carries and
*   produces wrong texture row indices (horizontal stripe artefact).
*   The correct fix is SWAP + ADDX.L with a pre-loop priming sequence
*   (used by ADoom), but that requires extra setup complexity.
*
* THIS FILE instead keeps frac in standard 16.16 (non-swapped) form.
*   - ADD.L for frac stepping is always correct.
*   - BFEXTU {#9:#7} extracts (frac>>16)&$7F (column) in one 3-cycle op.
*   - Three independent BFEXTU ops can all be in-flight simultaneously on
*     the 68060 pOEP, each pairing with its own ADD.L on sOEP.
*   - This lets the source fetch for pixel A start at C4 while pixels B
*     and C bfextus complete at C5 and C6 respectively.
*
* R_DrawColumn: 3-pixel unrolled.
*   C1-C3: three bfextu+advance pairs (all in-flight simultaneously)
*   C4-C6: source fetches A/B/C (each ready as the prior bfextu finishes)
*   C6-C8: and.l zero-extends pair with source fetches
*   C7-C9: colormap stores pair with and.l zero-extends
*   C9:    lea dest advance pairs with colormap C store
*   C10:   subq loop counter || bge branch (paired on sOEP+pOEP)
*   => ~10 cycles / 3 pixels = ~3.3 cycles/pixel on 68060 estimated.
*
* R_DrawSpan: 4-pixel unrolled SWAP+ADDX+OR/AND+biased-ptr (ADoom technique).
*             ~7.75 c/pixel.
*
* ABI (xdev68k GCC, -fcall-used-d2 -fcall-used-a2):
*   Caller-saved: d0/d1/d2/a0/a1/a2
*   Callee-saved: d3..d7, a3..a6

RUNS_HUMAN_VERSION      equ     3
        .cpu    68060
        .include doscall.inc
        .include iocscall.inc

* TARGET_68060: set to 0 by makefile sed for 68030 builds.
TARGET_68060            equ     1

        .globl  _R_DrawColumn
        .globl  _R_DrawSpan

        .if     TARGET_68060
        .globl  _FixedMul
        .globl  _FixedDiv
        .globl  _FixedDiv2
        .globl  _I_SetFPCRFloor
        .endif

* Per-second workload counters (defined in doom_perf.c, reset in i_video.c).
* Incremented inline here for near-zero overhead (~2 cycles per call).
        .globl  _g_perf_col_calls
        .globl  _g_perf_col_pixels
        .globl  _g_perf_span_calls
        .globl  _g_perf_span_pixels

* Set to 1 to use hand-asm R_MapPlane / R_DrawPlanes.
* Set to 0 to fall back to C versions (e.g. while optimising table placement).
HAVE_ASM_PLANES         equ     1

* Set to 1 to enable inline profiling counters in R_DrawColumn / R_DrawSpan.
* Set to 0 for release builds (saves ~4 instructions per draw call).
PROF_COUNTERS           equ     0


* ===========================================================================
* R_DrawColumn
*
* 3-pixel unrolled inner loop for the 68060 dual-issue pipeline.
*
* Three BFEXTU ops (A,B,C) are issued at C1,C2,C3 -- each paired with its
* ADD.L frac-advance on sOEP for zero additional cost.  Their results land
* at C4, C5, C6 exactly when the previous source fetch completes, so the
* source fetches chain without stalls.  The zero-extends (and.l #255) pair
* with the next source fetch or colormap store, and the final lea (dest
* advance) pairs with the last colormap store.  The loop branch (bge) pairs
* with the preceding subq.l on sOEP+pOEP.
*
* Callee-saved used: d3/d4/d5/d6/d7
*
* Register map (inner loop):
*   d0  frac_A    (standard 16.16, advances by 3*fracstep each iteration)
*   d1  frac_B    (standard 16.16)
*   d2  frac_C    (standard 16.16)
*   d3  scratch A (bfextu result, then source byte, then colormap index)
*   d4  scratch B
*   d5  scratch C
*   d6  3*fracstep (loop-invariant)
*   d7  remaining count - 3 (decremented by 3; loop runs while >= 0)
*   a0  dest (screens[0] current pixel, advances by 3*SCREENWIDTH=960)
*   a1  source (128-byte texture column)
*   a2  colormap (256-entry table)
* ===========================================================================

        .align  4                       * 16-byte align for I-cache
_R_DrawColumn:
        movem.l d3/d4/d5/d6/d7,-(sp)

        * d3 = dc_yl (temp), d7 = count-1
        move.l  _dc_yl,d3
        move.l  _dc_yh,d7
        sub.l   d3,d7                   * d7 = dc_yh - dc_yl  (= count-1)
        blt     RDC_done                * dc_yh < dc_yl: nothing to draw

        .if     PROF_COUNTERS
        addq.l  #1,_g_perf_col_calls
        move.l  d7,d5
        addq.l  #1,d5                   * d5 = pixel count (count-1+1)
        add.l   d5,_g_perf_col_pixels
        .endif

        * dest = ylookup2[dc_yl] + dc_x*2  (16-bit pixels, 640-byte row stride)
        move.l  _ylookup2(d3.l*4),a0
        move.l  _dc_x,d5
        add.l   d5,d5                   * d5 = dc_x * 2 (word offset)
        add.l   d5,a0

        move.l  _dc_source,a1
        move.l  _dc_colormap,a2

        * d4 = fracstep (temp)
        move.l  _dc_iscale,d4

        * frac_A = dc_texturemid + (dc_yl - centery) * fracstep
        *
        * MULS.W fast path: dc_iscale <= 32767 covers all walls that appear
        * more than ~1 screen-height away.  MULS.W is a native 68060 op (~1c);
        * MULS.L (32x32) is 060SP-emulated (~70c).  dc_yl-centery is always
        * in [-100,+99] so the product fits in 32 bits -- result is identical.
        sub.l   _centery,d3             * d3 = dc_yl - centery
        cmp.l   #32767,d4               * is fracstep small enough for MULS.W?
        bls.b   RDC_mul_fast            * yes (<=32767): skip the slow path
        muls.l  d4,d3                   * slow: dc_iscale>32767; 060SP ~70c
        bra.b   RDC_mul_cont
RDC_mul_fast:
        muls.w  d4,d3                   * fast: native MULS.W ~1c
RDC_mul_cont:
        add.l   _dc_texturemid,d3       * d3 = frac_A
        move.l  d3,d0                   * d0 = frac_A

        * frac_B = frac_A + fracstep;  frac_C = frac_A + 2*fracstep
        move.l  d0,d1
        add.l   d4,d1                   * d1 = frac_B
        move.l  d1,d2
        add.l   d4,d2                   * d2 = frac_C

        * d6 = 3*fracstep
        move.l  d4,d6
        add.l   d6,d6                   * d6 = 2*fracstep
        add.l   d4,d6                   * d6 = 3*fracstep

        * Loop entry: run while (count - 3) >= 0, i.e. count >= 3.
        * d7 = count-1 currently; after: d7 = count-3.
        addq.l  #1,d7                   * d7 = count
        subq.l  #3,d7                   * d7 = count - 3
        blt     RDC_tail_check          * count < 3: skip to tail
        bra     RDC_3loop               * jump over alignment padding

        * ------- 3-pixel loop (16-bit colormap16 output) -------
        * colormap16[idx] via (a2,dN.l*2): word read with *2 scale.
        * Dest write: move.w to screens16 (640-byte row stride).
        * Row offsets: 640, 1280, LEA 1920.
        .align  4                       * 16-byte align inner loop
RDC_3loop:
        * --- Extract texture indices (3 dual-issue pairs) ---
        bfextu  d0{#9:#7},d3            * index_A = (frac_A>>16)&$7F  (pOEP)
        add.l   d6,d0                   * frac_A += 3*step             (sOEP) DUAL
        bfextu  d1{#9:#7},d4            * index_B                      (pOEP)
        add.l   d6,d1                   * frac_B += 3*step             (sOEP) DUAL
        bfextu  d2{#9:#7},d5            * index_C                      (pOEP)
        add.l   d6,d2                   * frac_C += 3*step             (sOEP) DUAL
        * --- Source lookups (interleaved for pipeline) ---
        * bfextu zero-fills d3/d4/d5 to 32 bits (0-127), move.b only
        * overwrites low byte, so upper bytes stay 0.  No AND needed.
        move.b  (a1,d3.w),d3           * source[A]                    (pOEP)
        move.b  (a1,d4.w),d4           * source[B]                    (pOEP)
        move.b  (a1,d5.w),d5           * source[C]                    (pOEP)
        * --- Colormap + write (interleave writes with independent ops) ---
        move.w  (a2,d3.l*2),(a0)       * colormap16[A]->(a0)          (pOEP)
        move.w  (a2,d4.l*2),(640,a0)   * colormap16[B]->(a0+640)      (pOEP)
        move.w  (a2,d5.l*2),(1280,a0)  * colormap16[C]->(a0+1280)     (pOEP)
        lea     (1920,a0),a0            * dest += 3 rows               (pOEP)
        subq.l  #3,d7                   * remaining -= 3               (sOEP) DUAL
        bge     RDC_3loop

        * After loop: d7 = (count mod 3) - 3  (negative)
        * Restore: d7 = count mod 3, the number of tail pixels (0, 1, or 2)
RDC_tail_check:
        addq.l  #3,d7                   * d7 = tail pixel count (0, 1, or 2)

        * Draw 0, 1, or 2 remaining pixels.
        * d0=frac_A, d1=frac_B are correct for the tail pixels.
        * a0 points to the correct destination row.
        tst.l   d7
        beq     RDC_done                * 0 tail pixels
        bfextu  d0{#9:#7},d3
        move.b  (a1,d3.w),d3
        move.w  (a2,d3.l*2),(a0)       * draw tail pixel A (16-bit)
        subq.l  #1,d7
        beq     RDC_done                * only 1 tail pixel
        bfextu  d1{#9:#7},d3           * draw tail pixel B
        move.b  (a1,d3.w),d3
        move.w  (a2,d3.l*2),(640,a0)   * tail pixel B (16-bit, +640)

RDC_done:
        movem.l (sp)+,d3/d4/d5/d6/d7
        rts


* ===========================================================================
* R_DrawSpan  --  4-pixel unrolled span renderer for 68060.
*
* Ported from ADoom's R_DrawSpan_060 by Aki Laukkanen to HAS060/X68000.
*
* WHY NOT BFEXTU FOR SPANS:
*   For columns, three independent BFEXTUs can all be in-flight simultaneously,
*   hiding their 3-cycle latencies completely.  For spans, the two fracs
*   (xfrac, yfrac) must be *combined* into a single index (y*64 + x), which
*   creates a LSL->OR dependency stall at C3 that cannot be hidden.
*   ADoom's approach eliminates the stall entirely using the SWAP form.
*
* ALGORITHM -- INTERLEAVED SWAP FORM:
*   The fraction/integer words of the two fracs are interleaved across d0/d1
*   so that a single ADDX.L pair advances both fracs with the correct carry:
*
*     d0 = { yfrac.fraction : xfrac.integer }
*     d1 = { xfrac.fraction : yfrac.integer<<6 }
*     d2 = { ystep.fraction : xstep.integer }     (same layout as d0)
*     d3 = { xstep.fraction : ystep.integer<<6 }  (same layout as d1)
*
*   ADDX.L d2,d0: advances xfrac.integer (d0.low) and yfrac.fraction (d0.high).
*                 Carry-out = did yfrac.fraction overflow? -> used by d1 ADDX.
*   ADDX.L d3,d1: takes that carry, advances yfrac.integer<<6 (d1.low) and
*                 xfrac.fraction (d1.high).
*                 Carry-out = did xfrac.fraction overflow? -> used by d0 ADDX
*                 in the next iteration.
*
*   The two instructions share a single X-flag carry chain that alternates
*   between xfrac and yfrac overflow, correctly propagating both.
*
* INDEX CONSTRUCTION (no shift in hot path):
*   After setup pre-shifts yfrac.integer by 6 (lsl.w #6,d1):
*     or.w  #$ffc0,d0       -> d0.low = $ffc0 | x_idx       (bits  5:0)
*     or.w  #$f03f,d1       -> d1.low = $f03f | y_idx<<6    (bits 11:6)
*     move.w d0,d6
*     and.w  d1,d6          -> d6.w  = $f000 | y_idx<<6 | x_idx
*   Source pointer is biased: a1 = ds_source - $f000
*   So (a1,d6.l) = ds_source + y*64 + x  (correct flat-texture address)
*
* PIXEL ACCUMULATION (4 bytes -> one MOVE.L):
*   move.w (a2,d5.l),d4  -> d4[15:8]=colormap[p0], d4[7:0]=garbage
*   move.b (a2,d5.l),d4  -> d4[7:0]=colormap[p1]    (overwrites garbage)
*   swap   d4             -> d4[31:16]={p0,p1}, d4[15:0]=garbage
*   move.w (a2,d5.l),d4  -> d4[15:8]=colormap[p2], d4[7:0]=garbage
*   move.b (a2,d5.l),d4  -> d4[7:0]=colormap[p3]    (at top of next iter)
*   move.l d4,(a0)+       -> writes {p0,p1,p2,p3}  (big-endian: p0 at lowest addr)
*
* Claimed: 7.75 cycles/pixel (per ADoom source comment at the DBF instruction).
*
* Register map (main loop):
*   d0  { yfrac.frac : xfrac.int }     d1  { xfrac.frac : yfrac.int<<6 }
*   d2  { ystep.frac : xstep.int }     d3  { xstep.frac : ystep.int<<6 }
*   d4  pixel accumulator              d5  source byte (zero-extended)
*   d6  texture index ($f000|y<<6|x)   d7  DBF counter
*   a0  dest (screens[0] byte array)   a1  ds_source - $f000 (biased)
*   a2  ds_colormap                    a3  tail pixel count (0-3)
*
* Callee-saved used: d3/d4/d5/d6/d7/a3
* ===========================================================================

        .align  4                       * 16-byte align for I-cache
_R_DrawSpan:
        movem.l d3/d4/d5/d6/d7/a3,-(sp)

        * bounds check: skip if ds_x2 < ds_x1
        move.l  _ds_x1,d6
        move.l  _ds_x2,d7
        cmp.l   d6,d7
        blt     RDS4_done

        * count = ds_x2 - ds_x1 + 1
        sub.l   d6,d7
        addq.l  #1,d7                   * d7 = count (1..320)

        .if     PROF_COUNTERS
        addq.l  #1,_g_perf_span_calls
        add.l   d7,_g_perf_span_pixels
        .endif

        * dest = ylookup2[ds_y] + ds_x1*2  (16-bit pixels, 640-byte row stride)
        move.l  _ds_y,d5
        move.l  _ylookup2(d5.l*4),a0
        add.l   d6,d6                   * d6 = ds_x1 * 2 (word offset)
        add.l   d6,a0                   * a0 = dest (16-bit aligned)

        move.l  _ds_source,a1
        move.l  _ds_colormap,a2
        move.l  _ds_xfrac,d0
        move.l  _ds_yfrac,d1
        move.l  _ds_xstep,d2
        move.l  _ds_ystep,d3

        * --- Alignment preamble: 0-1 pixel to reach 4-byte dest ---
        * With 16-bit pixels, dest is always word-aligned.  Check for MOVE.L
        * alignment (4-byte): if bit 1 is set, draw 1 scalar pixel.
        move.l  a0,d4
        btst    #1,d4
        beq.b   RDS4_aligned            * already 4-byte aligned

        * draw 1 scalar pixel to align:
        move.l  d0,d5
        move.l  d1,d6
        swap    d5
        swap    d6
        and.w   #$3f,d5
        and.w   #$3f,d6
        lsl.w   #6,d6
        or.w    d5,d6
        move.b  (a1,d6.w),d5
        and.l   #255,d5
        add.l   d2,d0
        move.w  (a2,d5.l*2),(a0)+     * colormap16[source] -> dest (16-bit)
        add.l   d3,d1
        subq.l  #1,d7

RDS4_aligned:
        * Save tail count and setup main loop counter
        move.l  d7,d6
        and.w   #3,d6
        move.l  d6,a3                   * a3 = tail count (0-3)

        * Transform fracs to interleaved SWAP form.
        eor.w   d0,d1
        eor.w   d1,d0
        eor.w   d0,d1
        eor.w   d2,d3
        eor.w   d3,d2
        eor.w   d2,d3
        swap    d0                      * d0 = { yfrac.frac : xfrac.int }
        swap    d1                      * d1 = { xfrac.frac : yfrac.int }
        swap    d2                      * d2 = { ystep.frac : xstep.int }
        swap    d3                      * d3 = { xstep.frac : ystep.int }
        lsl.w   #6,d1                   * d1.low = yfrac.int<<6
        lsl.w   #6,d3                   * d3.low = ystep.int<<6
        moveq   #0,d5
        moveq   #0,d6
        sub.l   #$f000,a1               * bias: a1 = ds_source - $f000

        * Check for full 4-pixel groups
        lsr.l   #2,d7                   * d7 = count / 4
        beq.w   RDS4_skip_main          * no full groups -> tail only

        subq.l  #1,d7                   * DBF counter = groups - 1

        * Prime X flag
        sub.w   d3,d1
        add.l   d3,d1

        * Pre-compute first texture index
        or.w    #$ffc0,d0
        or.w    #$f03f,d1
        move.w  d0,d6
        and.w   d1,d6
        bra.b   RDS4_start

        * ---- Top of loop: finish previous group ----
        * On entry: d5=src_byte_p3, d4={p2,incomplete}, d6=index(next p0)
        .align  4                       * 16-byte align inner loop
RDS4_loop:
        or.w    #$ffc0,d0               *                         (pOEP)
        or.w    #$f03f,d1               *                         (pOEP)
        move.w  (a2,d5.l*2),d4         * colormap16[p3]->d4.lo   (pOEP)
        move.w  d0,d6                   * start index             (sOEP) DUAL
        move.l  d4,(a0)+                * write pixels 2-3        (pOEP)
        and.w   d1,d6                   * finalize index          (sOEP) DUAL

        * ---- Loop body: 4 pixels, 2 MOVE.L writes ----
        * Reordered for 68060 dual-issue (sOEP pairing).
        * d5 upper bytes always 0 (moveq init; move.b only writes low).
        *
        * Key trick: swap d4 is deferred from pixel N to the start of
        * pixel N+1, so it pairs with addx.l (pOEP) instead of stalling
        * on the colormap load that just wrote d4.
        *
        * pOEP-only: ADDX, BFEXTU, or.w #imm, memory ops, dbf
        * sOEP-eligible: move.w Dn,Dn / and.w Dn,Dn / swap Dn / add.l Dn,Dn
RDS4_start:
        * PIXEL 0:
        addx.l  d2,d0                   * advance xfrac           (pOEP)
        addx.l  d3,d1                   * advance yfrac           (pOEP, X dep)
        move.b  (a1,d6.l),d5           * source[prev_idx]        (pOEP)
        or.w    #$ffc0,d0               * mask xfrac              (pOEP)
        or.w    #$f03f,d1               * mask yfrac              (pOEP)
        move.w  d0,d6                   * start index             (sOEP) DUAL
        move.w  (a2,d5.l*2),d4         * colormap16[p0]          (pOEP)
        and.w   d1,d6                   * finalize index          (sOEP) DUAL
        * swap d4 deferred to pixel 1 for dual-issue

        * PIXEL 1:
        addx.l  d2,d0                   *                         (pOEP)
        swap    d4                       * d4[31:16]=p0            (sOEP) DUAL (d4 ready)
        addx.l  d3,d1                   *                         (pOEP, X dep)
        move.b  (a1,d6.l),d5           * source[idx]             (pOEP)
        or.w    #$ffc0,d0               *                         (pOEP)
        or.w    #$f03f,d1               *                         (pOEP)
        move.w  d0,d6                   *                         (sOEP) DUAL
        move.w  (a2,d5.l*2),d4         * colormap16[p1]->d4.lo   (pOEP)
        and.w   d1,d6                   * finalize index          (sOEP) DUAL
        move.l  d4,(a0)+                * write pixels 0-1        (pOEP)

        * PIXEL 2:
        addx.l  d2,d0                   *                         (pOEP)
        addx.l  d3,d1                   *                         (pOEP, X dep)
        move.b  (a1,d6.l),d5           * source[idx]             (pOEP)
        or.w    #$ffc0,d0               *                         (pOEP)
        or.w    #$f03f,d1               *                         (pOEP)
        move.w  d0,d6                   *                         (sOEP) DUAL
        move.w  (a2,d5.l*2),d4         * colormap16[p2]          (pOEP)
        and.w   d1,d6                   * finalize index          (sOEP) DUAL
        * swap d4 deferred to pixel 3 for dual-issue

        * PIXEL 3 (source preloaded for top-of-loop):
        addx.l  d2,d0                   *                         (pOEP)
        swap    d4                       * d4[31:16]=p2            (sOEP) DUAL (d4 ready)
        addx.l  d3,d1                   *                         (pOEP, X dep)
        move.b  (a1,d6.l),d5           * source[idx]             (pOEP)
        dbf     d7,RDS4_loop

        * Finish last group
        move.w  (a2,d5.l*2),d4        * colormap16[p3]
        move.l  d4,(a0)+               * write pixels 2-3

RDS4_skip_main:
        * Re-prime X flag for tail loop
        sub.w   d3,d1
        add.l   d3,d1
        move.l  a3,d7                   * d7 = tail count (0-3)
        bra.b   RDS4_tail_end

RDS4_tail_loop:
        * One 16-bit pixel: OR/AND index, ADDX advance, fetch, store
        or.w    #$ffc0,d0
        or.w    #$f03f,d1
        move.w  d0,d6
        and.w   d1,d6
        addx.l  d2,d0
        addx.l  d3,d1
        move.b  (a1,d6.l),d5
        move.w  (a2,d5.l*2),(a0)+     * colormap16[source] -> dest (16-bit)

RDS4_tail_end:
        dbf     d7,RDS4_tail_loop

RDS4_done:
        movem.l (sp)+,d3/d4/d5/d6/d7/a3
        rts


        .if     TARGET_68060
* ===========================================================================
* FixedMul(a, b) -- (int32)(((int64)a * (int64)b) >> 16)
*
* FPU implementation: the 68060 FPU is native silicon (~3-6 cycles each op)
* and completely avoids the 060SP software exception handler that fires for
* MULS.L (32×32->64, ~70 cycles).
*
* Algorithm:
*   fmove.l (4,sp),fp0  -- load a as signed integer -> extended float (exact)
*   fmul.l  (8,sp),fp0  -- fp0 = a * b  (integers; 64-bit mantissa is exact)
*   fmul.s  FM_INV65536 -- fp0 *= 2^-16 (exact: 1/65536 is a power of 2)
*   fmove.l fp0,d0      -- d0 = floor(fp0) using FPCR round-toward-minus-inf
*
* Precision: 68060 FPU extended precision has a 64-bit mantissa, which is
* sufficient to hold any 32×32 integer product (64-bit) exactly.
*
* Rounding: FPCR must be set to round-toward-minus-infinity (set by
* I_SetFPCRFloor at startup) to match the arithmetic-right-shift semantics
* of the original MULS.L bit extraction.
*
* Callee-saved: nothing (fp0 is caller-saved under the standard m68k ABI;
* GCC compiled with -msoft-float never touches FPU registers).
* ===========================================================================
        .globl  _FM_INV65536
_FM_INV65536:
FM_INV65536:
        dc.l    $37800000               * 1.0/65536.0 as IEEE single = 2^-16
_FixedMul:
        fmove.l (4,sp),fp0              * fp0 = a (int -> extended float)
        fmul.l  (8,sp),fp0              * fp0 = a * b  (integer source, exact)
        fmul.s  FM_INV65536,fp0         * fp0 = a*b/65536  (scale by 2^-16, exact)
        fmove.l fp0,d0                  * d0 = floor(a*b/65536)  [FPCR floor mode]
        rts


* ===========================================================================
* FixedDiv2(a, b) -- (int32)(((int64)a << 16) / (int32)b)
*
* FPU implementation: avoids 060SP-emulated DIVS.L (64÷32, ~70-100 cycles).
*   fmove.l (4,sp),fp0  -- a as extended float
*   fmul.s  FM_65536    -- fp0 = a * 65536 (= a << 16 in float space)
*   fdiv.l  (8,sp),fp0  -- fp0 = (a*65536) / b  (native FPU divide)
*   fmove.l fp0,d0      -- d0 = floor result  [FPCR floor mode]
*
* Called only after the caller has verified no overflow (result fits 32 bits).
* ===========================================================================
FM_65536:
        dc.l    $47800000               * 65536.0 as IEEE single = 2^16
_FixedDiv2:
        fmove.l (4,sp),fp0              * fp0 = a (int -> extended float)
        fmul.s  FM_65536,fp0            * fp0 = a * 65536
        fdiv.l  (8,sp),fp0              * fp0 = (a*65536) / b  (native FPU)
        fmove.l fp0,d0                  * d0 = floor result
        rts


* ===========================================================================
* FixedDiv(a, b) -- overflow-checked fixed-point division
*
* Overflow check: if (abs(a)>>14) >= abs(b), result would exceed 32 bits.
*   In that case return MAXINT ($7FFFFFFF) or MININT ($80000000) depending
*   on the sign of a^b.
*
* Non-overflow path uses the FPU (same approach as FixedDiv2).
*
* Registers (all caller-saved under the xdev68k ABI):
*   d0  a (input), abs(a)>>14 (during check), return value
*   d1  b (input, preserved for sign test in overflow path)
*   d2  abs(b) (during check); clobber OK (-fcall-used-d2)
*
* Two lsr.l #8 instructions are used instead of lsr.l #14 because HAS060
* restricts immediate shift counts to 1-8.  (lsr #8 + lsr #6 = lsr #14.)
* ===========================================================================
_FixedDiv:
        move.l  (4,sp),d0               * d0 = a
        move.l  (8,sp),d1               * d1 = b (preserved for sign test below)

        * --- abs(a) >> 14 ---
        bpl.b   FDV_a_pos
        neg.l   d0                      * d0 = abs(a)
FDV_a_pos:
        lsr.l   #8,d0
        lsr.l   #6,d0                   * d0 = abs(a) >> 14  (two shifts: HAS060 max = 8)

        * --- abs(b) ---
        move.l  d1,d2                   * d2 = b  (d2 caller-saved)
        bpl.b   FDV_b_pos
        neg.l   d2                      * d2 = abs(b)
FDV_b_pos:
        cmp.l   d0,d2                   * abs(b) vs abs(a)>>14
        bgt.b   FDV_no_overflow         * abs(b) > abs(a)>>14: result fits, divide

        * --- Overflow: saturate ---
        move.l  (4,sp),d0               * reload a
        eor.l   d1,d0                   * d0 = a^b  (d1 = original b, unchanged)
        bmi.b   FDV_minint
        move.l  #$7FFFFFFF,d0           * MAXINT
        rts
FDV_minint:
        move.l  #$80000000,d0           * MININT
        rts

        * --- No overflow: FPU divide ---
FDV_no_overflow:
        fmove.l (4,sp),fp0              * fp0 = a (int -> extended float)
        fmul.s  FM_65536,fp0            * fp0 = a * 65536
        fdiv.l  (8,sp),fp0              * fp0 = (a*65536) / b  (native FPU)
        fmove.l fp0,d0                  * d0 = floor result
        rts


* ===========================================================================
* I_SetFPCRFloor -- set 68060 FPCR rounding mode to "toward minus infinity"
*
* FPCR bits [5:4] control rounding:
*   00 = round to nearest/even  (IEEE default)
*   01 = round toward zero      (truncate)
*   10 = round toward -inf      (floor)  ← we want this
*   11 = round toward +inf      (ceiling)
*
* "Floor" (toward -inf) matches the arithmetic-right-shift semantics of the
* original MULS.L bit-extraction method for FixedMul: both round negative
* fixed-point remainders toward -inf, not toward zero.  Without this, FPU
* FixedMul would use round-to-nearest and could differ by 1 LSB for values
* whose fractional part is exactly 0.5.  This matters for game-logic
* determinism (P_ physics), not for rendering correctness.
*
* Called once at startup from I_Init.  Does not need to save d0 (caller-saved).
* ===========================================================================
_I_SetFPCRFloor:
        fmove.l fpcr,d0
        or.b    #$20,d0                 * set bit 5
        and.b   #$EF,d0                 * clear bit 4  ->  FPCR[5:4] = 10 = toward -inf
        fmove.l d0,fpcr
        rts

        .endif  * TARGET_68060


* ===========================================================================
* void I_BlitBlock(const byte *src, short *dst, int rows, const short *remap)
*
* Blits 'rows' rows of 320 pixels from the packed byte source buffer to the
* GVRAM word buffer, mapping each byte through remap[] (pixel_remap16).
*
* MOVE.L inner loop: two pixels packed into one 32-bit GVRAM write per pair.
* Packing without extra registers:
*   MOVE.W remap[A] -> d2[15:0]          (d2[31:16] = leftover from prev iter, ignored)
*   SWAP d2                               -> d2[31:16] = remap[A], d2[15:0] = leftover
*   MOVE.W remap[B] -> d2[15:0]          -> overwrites leftover; d2 = remap[A]:remap[B]
*   MOVE.L d2,(a1)+                       -> one 32-bit GVRAM write, both pixels
* Big-endian MOVE.L: high word (remap[A]) -> lower address (pixel A). Correct order.
* MOVE.B preserves bits[31:8]; single MOVEQ #0 init per register suffices.
*
* Pipeline per 2-pixel iteration (68060 dual-issue):
*   C1: pOEP(MOVE.B src->d0)              d0 ready C2
*   C2: pOEP(MOVE.B src->d1)              d1 ready C3
*   C3: pOEP(MOVE.W remap[d0]->d2)        d2 ready C4  [d0 arrives C2: zero stall]
*   C4: pOEP(SWAP d2)                      d2 ready C5  [d2 arrives C4: zero stall]
*   C5: pOEP(MOVE.W remap[d1]->d2)        d2 ready C6  [d1 arrives C3: zero stall]
*   C6: pOEP(MOVE.L d2->(a1)+) || sOEP(SUBQ.L #1,d4)  [d2 arrives C6: zero stall]
*   C7: pOEP(BGE loop)
*   => 3.5 cycles/pixel CPU compute, 0.5 GVRAM bus cycles/pixel (vs 1.0 with MOVE.W)
*
* Stack on entry (no regs pushed yet):
*   4(sp)  src   -- const byte*  (320-wide packed byte buffer)
*   8(sp)  dst   -- short*       (GVRAM, 512-word / 1024-byte row stride)
*  12(sp)  rows  -- int          (168 for 3-D view, 200 for full screen)
*  16(sp)  remap -- const short* (pixel_remap16[256] lookup table)
*
* Callee-saved used: d4/d5/a3  (saved/restored via MOVEM; d3 no longer needed)
* SCREENWIDTH (320) is always even; no tail loop needed.
* ===========================================================================
        .globl  _I_BlitBlock
_I_BlitBlock:
        movem.l d4/d5/a3,-(sp)          * save 3 regs = 12 bytes; args shift by 12

        move.l  (16,sp),a0              * src
        move.l  (20,sp),a1              * dst
        move.l  (24,sp),d5              * rows
        move.l  (28,sp),a2              * remap

        moveq   #0,d0                   * d0 zero-extended once; MOVE.B preserves bits[31:8]
        moveq   #0,d1                   * same for d1

IBB_row:
        move.l  a1,a3                   * save row-start for dst row advance
        move.l  #159,d4                 * 160 iterations of 2 pixels = 320 pixels per row

IBB_pixel2:
        move.b  (a0)+,d0                * C1 pOEP: pixel A  (bits[31:8] stay 0)
        move.b  (a0)+,d1                * C2 pOEP: pixel B  (bits[31:8] stay 0)
        move.w  (a2,d0.l*2),d2         * C3 pOEP: remap[A]->d2[15:0]  [d0 ready C2]
        swap    d2                      * C4 pOEP: d2[31:16]=remap[A]  [d2 ready C4]
        move.w  (a2,d1.l*2),d2         * C5 pOEP: remap[B]->d2[15:0]  [d1 ready C3]
        move.l  d2,(a1)+               * C6 pOEP: 2-pixel GVRAM write  [d2 ready C6] } PAIR
        subq.l  #1,d4                  * C6 sOEP: decrement count                     }
        bge.b   IBB_pixel2             * C7 pOEP: loop while count >= 0

        * Advance dst to start of next GVRAM row (512-word / 1024-byte stride).
        * Physical GVRAM row = 512 pixels * 2 bytes = 1024 bytes.
        * 1:1 vertical mapping: each game row occupies one physical GVRAM row.
        * a1 has advanced 320*2=640 bytes in the inner loop; a3 holds the row start.
        lea     (1024,a3),a1
        subq.l  #1,d5
        bgt.b   IBB_row

        movem.l (sp)+,d4/d5/a3
        rts

* ---------------------------------------------------------------------------
* I_BlitCopy16 -- pure MOVE.L copy from screens16 (16-bit) to GVRAM.
*   void I_BlitCopy16(const unsigned short *src, short *dst, int rows)
*
* No per-pixel remap -- screens16 already contains GVRAM-format words.
* 80 MOVE.L per row (320 pixels * 2 bytes / 4 bytes per MOVE.L).
* ---------------------------------------------------------------------------
        .globl  _I_BlitCopy16
_I_BlitCopy16:
        movem.l d4/a3,-(sp)            * save 2 regs = 8 bytes
        move.l  (12,sp),a0             * src (screens16)
        move.l  (16,sp),a1             * dst (GVRAM)
        move.l  (20,sp),d4             * rows
IBC16_row:
        move.l  a1,a3                   * save GVRAM row start
        move.l  #159,d0               * 160 MOVE.L = 320 pixels * 2 bytes / 4
IBC16_px:
        move.l  (a0)+,(a1)+
        dbf     d0,IBC16_px
        lea     (1024,a3),a1           * GVRAM next row (1024-byte stride)
        subq.l  #1,d4
        bgt.b   IBC16_row
        movem.l (sp)+,d4/a3
        rts

* ---------------------------------------------------------------------------
* I_BlitCopy16_LowDetail -- write every other pixel to GVRAM (vertical scanlines).
*   void I_BlitCopy16_LowDetail(const unsigned short *src, short *dst, int rows)
*
* Source (screens16) has 320 pixels/row (doubled by renderer).
* We write only even columns: src[0]->dst[0], src[2]->dst[2], src[4]->dst[4]...
* Odd GVRAM columns stay black (pre-cleared once on mode switch).
* 160 MOVE.W per row (half the bus traffic of the full blit).
* ---------------------------------------------------------------------------
        .globl  _I_BlitCopy16_LowDetail
_I_BlitCopy16_LowDetail:
        movem.l d4/a3,-(sp)
        move.l  (12,sp),a0             * src (screens16)
        move.l  (16,sp),a1             * dst (GVRAM)
        move.l  (20,sp),d4             * rows
IBC16LD_row:
        move.l  a1,a3                   * save GVRAM row start
        move.l  #159,d0               * 160 pixels to write
IBC16LD_px:
        move.w  (a0),(a1)              * write even pixel
        addq.l  #4,a0                  * skip 2 source pixels (4 bytes)
        addq.l  #4,a1                  * skip 2 GVRAM pixels (4 bytes)
        dbf     d0,IBC16LD_px
        lea     (1024,a3),a1           * GVRAM next row
        subq.l  #1,d4
        bgt.b   IBC16LD_row
        movem.l (sp)+,d4/a3
        rts

* ---------------------------------------------------------------------------
* I_BlitCopy16_Delta -- delta blit: only write changed pixels to GVRAM.
*   void I_BlitCopy16_Delta(const unsigned short *src,
*                           unsigned short *shadow,
*                           short *dst, int rows)
*
* Compares 2 pixels (32 bits) at a time from src vs shadow.
* If equal: skip GVRAM write. If different: write to GVRAM + update shadow.
* ---------------------------------------------------------------------------
        .globl  _I_BlitCopy16_Delta
.if PROF_COUNTERS
        .globl  _g_delta_changed
        .comm   _g_delta_changed,4
.endif
_I_BlitCopy16_Delta:
.if PROF_COUNTERS
        movem.l d1/d4/d5/a2/a3,-(sp)  * save 5 regs = 20 bytes
        move.l  (24,sp),a0            * src (screens16)
        move.l  (28,sp),a2            * shadow
        move.l  (32,sp),a1            * dst (GVRAM)
        move.l  (36,sp),d5            * rows
        moveq.l #0,d1                 * changed pixel-pair counter
.else
        movem.l d4/d5/a2/a3,-(sp)     * save 4 regs = 16 bytes
        move.l  (20,sp),a0
        move.l  (24,sp),a2
        move.l  (28,sp),a1
        move.l  (32,sp),d5
.endif
IBD16_row:
        move.l  a1,a3                  * save GVRAM row start
        move.l  #159,d4               * 160 longwords = 320 pixels
IBD16_px:
        move.l  (a0)+,d0              * 2 pixels from src (post-inc)
        cmp.l   (a2)+,d0              * vs shadow (post-inc: free advance)
        beq.b   IBD16_skip
        move.l  d0,(a1)               * write to GVRAM (slow bus)
        move.l  d0,(-4,a2)            * update shadow (writeback to pre-inc addr)
.if PROF_COUNTERS
        addq.l  #1,d1                 * count changed pairs
.endif
IBD16_skip:
        addq.l  #4,a1                 * advance GVRAM
        dbf     d4,IBD16_px
        lea     (1024,a3),a1          * GVRAM next row (1024-byte stride)
        subq.l  #1,d5
        bgt.b   IBD16_row
.if PROF_COUNTERS
        move.l  d1,_g_delta_changed
        movem.l (sp)+,d1/d4/d5/a2/a3
.else
        movem.l (sp)+,d4/d5/a2/a3
.endif
        rts

* ---------------------------------------------------------------------------
* I_BlitCopy16_LowDetail_Delta -- delta blit for low detail scanline mode.
*   void I_BlitCopy16_LowDetail_Delta(const unsigned short *src,
*                                     unsigned short *shadow,
*                                     short *dst, int rows)
*
* Compares every other pixel (even columns only). 160 pixels per row.
* ---------------------------------------------------------------------------
        .globl  _I_BlitCopy16_LowDetail_Delta
_I_BlitCopy16_LowDetail_Delta:
.if PROF_COUNTERS
        movem.l d1/d4/d5/a2/a3,-(sp)
        move.l  (24,sp),a0
        move.l  (28,sp),a2
        move.l  (32,sp),a1
        move.l  (36,sp),d5
        moveq.l #0,d1
.else
        movem.l d4/d5/a2/a3,-(sp)
        move.l  (20,sp),a0
        move.l  (24,sp),a2
        move.l  (28,sp),a1
        move.l  (32,sp),d5
.endif
IBDLD_row:
        move.l  a1,a3
        move.l  #159,d4               * 160 even pixels
IBDLD_px:
        move.w  (a0),d0               * even pixel from src
        cmp.w   (a2),d0               * vs shadow
        beq.b   IBDLD_skip
        move.w  d0,(a1)               * write to GVRAM
        move.w  d0,(a2)               * update shadow
.if PROF_COUNTERS
        addq.l  #1,d1
.endif
IBDLD_skip:
        addq.l  #4,a0
        addq.l  #4,a2
        addq.l  #4,a1
        dbf     d4,IBDLD_px
        lea     (1024,a3),a1
        subq.l  #1,d5
        bgt.b   IBDLD_row
.if PROF_COUNTERS
        move.l  d1,_g_delta_changed
        movem.l (sp)+,d1/d4/d5/a2/a3
.else
        movem.l (sp)+,d4/d5/a2/a3
.endif
        rts


* ===========================================================================
* R_MapPlane / R_DrawPlanes (with R_MakeSpans logic inlined)
*
* R_MakeSpans is inlined directly into R_DrawPlanes, eliminating the ~320
* function calls per plane that a separate-function approach requires.
* R_MapPlane is kept as a called function (invoked once per visible row,
* not once per column, so its call overhead is much smaller).
*
* R_MapPlane uses inline FPU for all FixedMul operations, avoiding jsr
* _FixedMul overhead (~10+ c each) and 060SP soft-float traps.
*
* visplane_t field offsets:
*   +0  height  +4  picnum  +8  lightlevel  +12 minx  +16 maxx
*   +20 pad1(=top[-1])  +21 top[0..319]  +341 pad2
*   +342 pad3(=bottom[-1])  +343 bottom[0..319]  +663 pad4  sizeof=664
* ===========================================================================

        .if     HAVE_ASM_PLANES
        .globl  _R_MapPlane
        .globl  _R_DrawPlanes

RMP_MAXLIGHTZ           equ     128
RMP_LIGHTZSHIFT         equ     20
RMP_LIGHTSEGSHIFT       equ     4
RMP_LIGHTLEVELS         equ     16
RMP_ANGLETOFINESHIFT    equ     19
RMP_PU_STATIC           equ     1
RMP_PU_CACHE            equ     3

vp_height               equ     0
vp_picnum               equ     4
vp_lightlevel           equ     8
vp_minx                 equ     12
vp_maxx                 equ     16
vp_top                  equ     21
vp_bottom               equ     343
vp_SIZEOF               equ     664

* ---------------------------------------------------------------------------
* R_MapPlane(int y, int x1, int x2)
*
* Stack after callee-save push of 2 regs (8 bytes):
*   12(sp)=y  16(sp)=x1  20(sp)=x2
*
* d2 used for distance (caller-saved; no save needed).
* Callee-saved used: d3/d4 only.
* Tail-calls spanfunc on exit: R_DrawPlanes' "lea 12(sp),sp" cleans the args.
* ---------------------------------------------------------------------------
        .align  4                       * 16-byte align for I-cache
_R_MapPlane:
        movem.l d3/d4,-(sp)

        move.l  12(sp),d3               * d3 = y
        move.l  16(sp),d4               * d4 = x1

        * Cache check: planeheight == cachedheight[y] ?
        move.l  _planeheight,d0
        lea     _cachedheight,a0
        cmp.l   (a0,d3.l*4),d0
        beq.w   RMP_cached

        * --- Cache miss: compute and store distance, xstep, ystep ---
        move.l  d0,(a0,d3.l*4)          * cachedheight[y] = planeheight

        * fp0 = FixedMul(planeheight, yslope[y])  overlap angle int-pipe
        movea.l _yslope,a1
        fmove.l d0,fp0
        fmul.l  (a1,d3.l*4),fp0
        fmul.s  FM_INV65536,fp0        * fp0 = distance

        move.l  _viewangle,d1
        movea.l _xtoviewangle,a1
        add.l   (a1,d4.l*4),d1
        swap    d1
        lsr.w   #3,d1
        and.l   #$1FFF,d1              * d1 = fine angle (hides FPU latency)

        fmove.l fp0,d2                  * d2 = distance (integer; caller-saved)

        lea     _cacheddistance,a0
        move.l  d2,(a0,d3.l*4)

        move.l  _basexscale,d0
        fmove.l d2,fp1
        fmul.l  d0,fp1
        fmul.s  FM_INV65536,fp1

        move.l  _baseyscale,d0
        fmove.l d2,fp2
        fmul.l  d0,fp2
        fmul.s  FM_INV65536,fp2

        fmove.l fp1,d0
        move.l  d0,_ds_xstep
        lea     _cachedxstep,a0
        move.l  d0,(a0,d3.l*4)

        fmove.l fp2,d0
        move.l  d0,_ds_ystep
        lea     _cachedystep,a0
        move.l  d0,(a0,d3.l*4)

        bra.w   RMP_cache_done

RMP_cached:
        lea     _cacheddistance,a0
        move.l  (a0,d3.l*4),d2

        lea     _cachedxstep,a0
        move.l  (a0,d3.l*4),d0
        move.l  d0,_ds_xstep

        lea     _cachedystep,a0
        move.l  (a0,d3.l*4),d0
        move.l  d0,_ds_ystep

        move.l  _viewangle,d1
        movea.l _xtoviewangle,a1
        add.l   (a1,d4.l*4),d1
        swap    d1
        lsr.w   #3,d1
        and.l   #$1FFF,d1              * d1 = fine angle

RMP_cache_done:
        * d1=angle  d4=x1  d2=distance
        * fp0 = FixedMul(distance, distscale[x1]) = length
        movea.l _distscale,a0
        fmove.l d2,fp0
        fmul.l  (a0,d4.l*4),fp0
        fmul.s  FM_INV65536,fp0        * fp0 = length

        move.l  d3,_ds_y
        move.l  d4,_ds_x1
        move.l  20(sp),_ds_x2          * 20(sp): 2 saved regs(8) + retaddr(4) + y(4) + x1(4)

        * ds_xfrac = viewx + FixedMul(finecosine[angle], length)
        * IMPORTANT: do NOT use fmove.x fp0,fp1 to save length — on 68060 extended-
        * precision FP reg→reg copies are not native and trap to 060SP (~100 cycles).
        * Instead extract length as a 32-bit integer (fmove.l is native), then
        * reload it into fp1 via fmove.l (integer→FP, also native).
        movea.l _finecosine,a0
        fmove.l fp0,d0                 * d0 = length (integer); fp0 still valid
        fmul.l  (a0,d1.l*4),fp0       * fp0 = finecosine[angle] * length
        fmul.s  FM_INV65536,fp0

        * ds_yfrac = -viewy - FixedMul(finesine[angle], length)
        movea.l _finesine_fast,a0
        fmove.l d0,fp1                 * fp1 = length (integer→FP; native op)
        fmul.l  (a0,d1.l*4),fp1
        fmul.s  FM_INV65536,fp1

        fmove.l fp0,d0
        add.l   _viewx,d0
        move.l  d0,_ds_xfrac

        fmove.l fp1,d0
        move.l  _viewy,d1
        neg.l   d1
        sub.l   d0,d1
        move.l  d1,_ds_yfrac

        * ds_colormap
        move.l  _fixedcolormap,d0
        bne.b   RMP_fixed_cmap

        move.l  d2,d0
        swap    d0
        lsr.w   #4,d0
        and.l   #$FFFF,d0
        cmp.l   #RMP_MAXLIGHTZ,d0
        blo.b   RMP_lightz_ok
        moveq   #RMP_MAXLIGHTZ-1,d0
RMP_lightz_ok:
        movea.l _planezlight,a0
        move.l  (a0,d0.l*4),_ds_colormap
        bra.b   RMP_cmap_done

RMP_fixed_cmap:
        move.l  d0,_ds_colormap

RMP_cmap_done:
        movea.l _spanfunc,a0
        movem.l (sp)+,d3/d4              * restore; a0 (caller-saved) still holds spanfunc
        jmp     (a0)                     * tail call: spanfunc rts → R_DrawPlanes caller

* ---------------------------------------------------------------------------
* R_DrawPlanes -- outer plane loop with R_MakeSpans logic inlined.
*
* Register map (inner column loop):
*   d3  x            d4  stop          d5  t1        d6  b1
*   d7  t2  (callee-saved; survives R_MapPlane/spanfunc calls)
*   a2  pl  (caller-saved; clobbered by W_CacheLumpNum, Z_ChangeTag2)
*   a3  &top[x] advancing    a4  x-1 (precomputed each column; callee-saved)
*   a5  &spanstart[0]  (stable across all calls in flat path)
*   a6  pl backup for W_CacheLumpNum/Z_ChangeTag2  (flat path)
*       OR &spanstart save in sky path
*
* b2 is kept in one stack slot while loops 1+2 execute (where R_MapPlane
* calls can clobber d0-d2), then popped into d2 for loops 3+4 (no calls).
* ---------------------------------------------------------------------------
        .align  4                       * 16-byte align for I-cache
_R_DrawPlanes:
        movem.l d3/d4/d5/d6/d7/a2/a3/a4/a5/a6,-(sp)   * 10 regs = 40 bytes

        lea     _visplanes,a2
        lea     _spanstart,a5           * a5 stable across flat inner loop
        clr.l   _rdp_plane_idx

RDP_plane_loop:
        move.l  _rdp_plane_idx,d0
        lea     _visplane_modified,a0
        tst.b   (a0,d0.l)
        beq.w   RDP_next                * not modified: skip

        move.l  vp_minx(a2),d3
        move.l  vp_maxx(a2),d4
        cmp.l   d3,d4
        blt.w   RDP_next                * minx > maxx: skip

        move.l  vp_picnum(a2),d0
        cmp.l   _skyflatnum,d0
        beq.w   RDP_sky

        * --- Regular flat ---
        movea.l a2,a6                   * a6 = pl backup (C calls clobber a2)

        * ds_source = W_CacheLumpNum(firstflat + flattranslation[picnum], PU_STATIC)
        move.l  _firstflat,d0
        movea.l _flattranslation,a0
        move.l  vp_picnum(a2),d1
        add.l   (a0,d1.l*4),d0
        move.l  #RMP_PU_STATIC,-(sp)
        move.l  d0,-(sp)
        jsr     _W_CacheLumpNum
        addq.l  #8,sp
        movea.l a6,a2
        move.l  d0,_ds_source

        * planeheight = abs(pl->height - viewz)
        move.l  vp_height(a2),d0
        sub.l   _viewz,d0
        bpl.b   RDP_hp_pos
        neg.l   d0
RDP_hp_pos:
        move.l  d0,_planeheight

        * planezlight = zlight[clamp(lightlevel>>4 + extralight, 0, LIGHTLEVELS-1)]
        move.l  vp_lightlevel(a2),d0
        lsr.l   #RMP_LIGHTSEGSHIFT,d0
        add.l   _extralight,d0
        bmi.b   RDP_light_zero
        cmp.l   #RMP_LIGHTLEVELS,d0
        blt.b   RDP_light_ok
        moveq   #RMP_LIGHTLEVELS-1,d0
        bra.b   RDP_light_ok
RDP_light_zero:
        moveq   #0,d0
RDP_light_ok:
        lea     _zlight,a0
        lsl.l   #8,d0                   * *= 256
        add.l   d0,d0                   * *= 512  (MAXLIGHTZ * 4)
        move.l  a0,_planezlight
        add.l   d0,_planezlight

        * Sentinels: top[maxx+1]=0xFF, top[minx-1]=0xFF
        move.b  #$FF,vp_top+1(a2,d4.l)
        move.b  #$FF,vp_top-1(a2,d3.l)

        addq.l  #1,d4                   * d4 = stop = maxx + 1

        * a3 = &top[minx-1]:  top[x] at offset 21+x, so top[minx-1] = 20+minx.
        * bottom[x] = top[x]+322.  After (a3)+ advances past top[x], a3 points
        * to top[x+1], so 321(a3) == bottom[x].  This frees a4 for x-1 below.
        lea     20(a2,d3.l),a3

        * Pre-read t1=top[minx-1]=0xFF, b1=bottom[minx-1]=0
        moveq   #0,d5
        moveq   #0,d6
        move.b  (a3)+,d5                * d5=t1; a3→&top[minx]
        move.b  321(a3),d6              * d6=b1=bottom[minx-1]

        move.l  vp_minx(a2),d3         * d3=x=minx

        * ----------------------------------------------------------
        * Column loop: one iteration per x in [minx, stop]
        * ----------------------------------------------------------
RDP_ms_loop:
        moveq   #0,d7
        move.b  (a3)+,d7                * d7=t2; a3→&top[x+1]
        moveq   #0,d2
        move.b  321(a3),d2              * d2=b2=bottom[x]  (321 = 322-1, see above)
        move.l  d3,a4                   * a4=x-1: callee-saved, survives R_MapPlane
        subq.l  #1,a4
        move.l  d2,-(sp)                * save b2; R_MapPlane calls clobber d2

        * Loop 1: while (t1<t2 && t1<=b1)  R_MapPlane(t1, spanstart[t1], x-1); t1++
RDP_l1:
        cmp.l   d7,d5                   * t1 < t2 ?
        bge.b   RDP_l1_done
        cmp.l   d6,d5                   * t1 <= b1 ?
        bgt.b   RDP_l1_done
        move.l  a4,-(sp)                * x-1 (callee-saved, 1 insn vs 3)
        move.l  (a5,d5.l*4),-(sp)      * spanstart[t1]
        move.l  d5,-(sp)                * y = t1
        jsr     _R_MapPlane
        lea     12(sp),sp               * pop 3 args; b2 back at (sp)
        addq.l  #1,d5                   * t1++
        bra.b   RDP_l1
RDP_l1_done:

        * Loop 2: while (b1>b2 && b1>=t1)  R_MapPlane(b1, spanstart[b1], x-1); b1--
RDP_l2:
        move.l  (sp),d2                 * reload b2 (clobbered by R_MapPlane)
        cmp.l   d2,d6                   * b1 > b2 ?
        ble.b   RDP_l2_done
        cmp.l   d5,d6                   * b1 >= t1 ?
        blt.b   RDP_l2_done
        move.l  a4,-(sp)                * x-1 (callee-saved, 1 insn vs 3)
        move.l  (a5,d6.l*4),-(sp)      * spanstart[b1]
        move.l  d6,-(sp)                * y = b1
        jsr     _R_MapPlane
        lea     12(sp),sp
        subq.l  #1,d6                   * b1--
        bra.b   RDP_l2
RDP_l2_done:

        move.l  (sp)+,d2                * pop b2 for loops 3+4 (no calls; d2 stable)

        * Loop 3: while (t2<t1 && t2<=b2)  spanstart[t2++]=x
        * DBF count = min(t1-t2-1, b2-t2); d7=t2 (advancing)
        move.l  d5,d0                   * d0 = t1
        sub.l   d7,d0                   * d0 = t1-t2
        ble.b   RDP_l3_done             * t1<=t2: no iters
        subq.l  #1,d0                   * DBF adj: t1-t2-1
        move.l  d2,d1                   * d1 = b2-t2
        sub.l   d7,d1
        bmi.b   RDP_l3_done             * b2<t2: no iters
        cmp.l   d1,d0
        ble.b   RDP_l3_go
        move.l  d1,d0                   * use smaller of the two
RDP_l3_go:
RDP_l3_loop:
        move.l  d3,(a5,d7.l*4)
        addq.l  #1,d7
        dbf     d0,RDP_l3_loop
RDP_l3_done:

        * Loop 4: while (b2>b1 && b2>=t2)  spanstart[b2--]=x
        * DBF count = min(b2-b1-1, b2-t2); d2=b2 (descending)
        move.l  d2,d0                   * d0 = b2-b1
        sub.l   d6,d0
        ble.b   RDP_l4_done             * b2<=b1: no iters
        subq.l  #1,d0                   * DBF adj: b2-b1-1
        move.l  d2,d1                   * d1 = b2-t2
        sub.l   d7,d1
        bmi.b   RDP_l4_done             * b2<t2: no iters
        cmp.l   d1,d0
        ble.b   RDP_l4_go
        move.l  d1,d0
RDP_l4_go:
RDP_l4_loop:
        move.l  d3,(a5,d2.l*4)
        subq.l  #1,d2
        dbf     d0,RDP_l4_loop
RDP_l4_done:

        * t1 = original top[x], b1 = original bottom[x].
        * d7/d2 were modified by loops 3/4, so re-read the unmodified bytes.
        * a3 is at &top[x+1]; -1(a3)=top[x], 321(a3)=bottom[x].
        moveq   #0,d5
        moveq   #0,d6
        move.b  -1(a3),d5               * t1 = top[x]
        move.b  321(a3),d6              * b1 = bottom[x]

        addq.l  #1,d3                   * x++
        cmp.l   d4,d3
        ble.w   RDP_ms_loop

        * Z_ChangeTag2(ds_source, PU_CACHE)
        move.l  #RMP_PU_CACHE,-(sp)
        move.l  _ds_source,-(sp)
        jsr     _Z_ChangeTag2
        addq.l  #8,sp
        movea.l a6,a2                   * restore pl

RDP_next:
        lea     vp_SIZEOF(a2),a2        * pl++
        addq.l  #1,_rdp_plane_idx
        cmp.l   _lastvisplane,a2
        blo.w   RDP_plane_loop

RDP_done:
        movem.l (sp)+,d3/d4/d5/d6/d7/a2/a3/a4/a5/a6
        rts

        * ----------------------------------------------------------
        * Sky flat path
        * d3=minx d4=maxx (addq #1 not yet applied) on entry here.
        * a5 temporarily holds colfunc; a6 saves &spanstart.
        * d4 holds pl after move.l a2,d4 (a2 clobbered by R_GetColumn).
        * ----------------------------------------------------------
RDP_sky:
        move.l  _pspriteiscale,d0
        move.l  _detailshift,d1
        lsr.l   d1,d0                   * pspriteiscale >> detailshift
        move.l  d0,_dc_iscale
        move.l  _colormaps,d0
        move.l  d0,_dc_colormap
        move.l  _skytexturemid,d0
        move.l  d0,_dc_texturemid

        lea     21(a2,d3.l),a3          * a3→top[minx]
        lea     343(a2),a4
        adda.l  d3,a4                   * a4→bottom[minx]

        move.l  d4,d7
        sub.l   d3,d7                   * d7=maxx-minx  (DBF count)

        move.l  a2,d4                   * d4=pl backup (a2 clobbered by R_GetColumn)
        movea.l a5,a6                   * a6=save &spanstart
        movea.l _colfunc,a5             * a5=colfunc

RDP_sky_loop:
        moveq   #0,d0
        moveq   #0,d1
        move.b  (a3)+,d0
        move.b  (a4)+,d1
        move.l  d0,_dc_yl
        move.l  d1,_dc_yh
        cmp.l   d1,d0                   * dc_yl <= dc_yh ?
        bgt.b   RDP_sky_next

        move.l  _viewangle,d0
        movea.l _xtoviewangle,a0
        add.l   (a0,d3.l*4),d0
        swap    d0
        lsr.w   #6,d0
        and.l   #$3FF,d0               * 10-bit sky angle

        move.l  d0,-(sp)
        move.l  _skytexture,-(sp)
        jsr     _R_GetColumn
        addq.l  #8,sp
        move.l  d0,_dc_source

        move.l  d3,_dc_x
        jsr     (a5)

RDP_sky_next:
        addq.l  #1,d3
        dbf     d7,RDP_sky_loop

        movea.l d4,a2                   * restore pl
        movea.l a6,a5                   * restore &spanstart
        bra.w   RDP_next

        .bss
        .even
_rdp_plane_idx: .ds.l   1

        .endif  * HAVE_ASM_PLANES

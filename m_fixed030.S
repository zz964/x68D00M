* m_fixed030.S -- 68030-native fixed-point arithmetic
*
* Uses MULS.L Dh:Dl (32x32->64) and DIVSL.L Dr:Dq (64/32->32)
* which are native 68030 instructions (~28c and ~90c respectively).
* Replaces the C versions that use __muldi3/__divdi3 library calls
* (~100-150c each due to function call overhead + software division).
*
* Linked only in the TARGET=030 build. The 68060 build uses FPU
* versions from r_draw_asm.S instead.

RUNS_HUMAN_VERSION      equ     3
        .cpu    68030
        .include doscall.inc
        .include iocscall.inc

        .globl  _FixedMul
        .globl  _FixedDiv
        .globl  _FixedDiv2


* ===========================================================================
* FixedMul(a, b) -> (int32)(((int64)a * (int64)b) >> 16)
*
* MULS.L Dn,Dh:Dl gives 64-bit signed product in Dh:Dl.
* We need bits 47..16 of the 64-bit result (i.e. >> 16).
* That's the low 16 bits of Dh combined with the high 16 bits of Dl.
*
* Stack: (4,sp) = a, (8,sp) = b
* Returns result in d0.
* ===========================================================================
_FixedMul:
        move.l  (4,sp),d0               * d0 = a
        move.l  (8,sp),d1               * d1 = b
        muls.l  d1,d1:d0                * d1:d0 = a * b  (d1=high, d0=low)
        * Need (d1 << 16) | (d0 >> 16):
        *   1. shift d0 right 16 (unsigned, keeps high 16 bits of low word)
        *   2. swap d1 and clear high word (moves low 16 of high to position)
        *   3. OR them together
        clr.w   d0                      * clear low 16 bits of d0
        swap    d0                      * d0 = d0 >> 16 (unsigned)
        swap    d1                      * d1 low word = old d1 low word
        clr.w   d1                      * clear the old high part
        or.l    d1,d0                   * d0 = (d1_lo << 16) | (d0 >> 16)
        rts


* ===========================================================================
* FixedDiv2(a, b) -> (int32)(((int64)a << 16) / (int32)b)
*
* Constructs 64-bit dividend (a << 16) in d0:d1, then uses
* DIVSL.L to divide by b.
*
* (a << 16) as 64-bit:
*   high 32 bits = a >> 16  (arithmetic shift, preserves sign)
*   low  32 bits = a << 16
*
* Stack: (4,sp) = a, (8,sp) = b
* Returns result in d0.
* ===========================================================================
_FixedDiv2:
        move.l  (4,sp),d0               * d0 = a
        move.l  d0,d1                   * d1 = a (copy)
        * Build high 32 bits of (a << 16) in d0:
        swap    d0                      * d0 = (a.lo << 16) | (a.hi >> 16)
        ext.l   d0                      * sign-extend: d0 = a >> 16 (arithmetic)
        * Build low 32 bits of (a << 16) in d1:
        swap    d1                      * move low word to high
        clr.w   d1                      * d1 = a << 16
        * Divide:
        divsl.l (8,sp),d0:d1            * d1 = (d0:d1) / b, d0 = remainder
        move.l  d1,d0                   * result in d0
        rts


* ===========================================================================
* FixedDiv(a, b) -> overflow-checked fixed-point division
*
* If (abs(a) >> 14) >= abs(b), result would overflow 32 bits.
* Return MAXINT or MININT depending on sign of a^b.
* Otherwise call FixedDiv2.
*
* Stack: (4,sp) = a, (8,sp) = b
* Returns result in d0.
* ===========================================================================
_FixedDiv:
        move.l  (4,sp),d0               * d0 = a
        move.l  (8,sp),d1               * d1 = b
        * abs(a) >> 14
        move.l  d0,d2
        bpl.b   FD30_a_pos
        neg.l   d2
FD30_a_pos:
        lsr.l   #8,d2
        lsr.l   #6,d2                   * d2 = abs(a) >> 14
        * abs(b)
        tst.l   d1
        bpl.b   FD30_b_pos
        neg.l   d1
FD30_b_pos:
        cmp.l   d1,d2                   * abs(a)>>14 >= abs(b)?
        blt.b   FD30_no_overflow
        * Overflow: return MAXINT or MININT based on sign of a^b
        move.l  (4,sp),d0
        move.l  (8,sp),d2
        eor.l   d2,d0
        bmi.b   FD30_neg
        move.l  #$7FFFFFFF,d0           * MAXINT
        rts
FD30_neg:
        move.l  #$80000000,d0           * MININT
        rts
FD30_no_overflow:
        bra     _FixedDiv2

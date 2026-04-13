/* Emacs style mode select   -*- C++ -*- */
/*----------------------------------------------------------------------------- */
/* */
/* $Id:$ */
/* */
/* Copyright (C) 1993-1996 by id Software, Inc. */
/* */
/* This program is free software; you can redistribute it and/or */
/* modify it under the terms of the GNU General Public License */
/* as published by the Free Software Foundation; either version 2 */
/* of the License, or (at your option) any later version. */
/* */
/* This program is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the */
/* GNU General Public License for more details. */
/* */
/* DESCRIPTION: */
/*	Fixed point arithemtics, implementation. */
/* */
/*----------------------------------------------------------------------------- */


#ifndef __M_FIXED__
#define __M_FIXED__


#if !defined(TARGET_68030) && !defined(TARGET_68060)
#ifdef __GNUG__
#pragma interface
#endif
#endif


/* */
/* Fixed point, 32bit as 16.16. */
/* */
#define FRACBITS                16
#define FRACUNIT                (1<<FRACBITS)

typedef int fixed_t;

/* ASM symbol name for FM_INV65536 constant.
 * xdev68k (HAS/HLK) uses underscore prefix; elf2x68k (ELF) does not. */
#ifndef ASM_SYM_FM_INV65536
#define ASM_SYM_FM_INV65536 "_FM_INV65536"
#endif

#ifdef TARGET_68060
/* Inline FixedMul for 68060: native FPU, ~6 cycles.
 * Eliminates ~20 cycles of jsr/rts overhead per call. */
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    fixed_t result;
    __asm__ volatile (
        "fmove.l %1,%%fp0\n\t"
        "fmul.l  %2,%%fp0\n\t"
        "fmul.s  " ASM_SYM_FM_INV65536 ",%%fp0\n\t"
        "fmove.l %%fp0,%0"
        : "=d"(result)
        : "d"(a), "d"(b)
    );
    return result;
}
#elif defined(TARGET_68030)
/* Inline FixedMul for 68030: native MULS.L 32x32->64, ~28 cycles. */
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    long hi;
    long lo = a;
    __asm__ volatile (
        "muls.l %2,%1:%0"
        : "+d"(lo), "=&d"(hi)
        : "d"(b)
    );
    return (fixed_t)(((unsigned long)lo >> 16) | ((unsigned long)hi << 16));
}
#else
fixed_t FixedMul        (fixed_t a, fixed_t b);
#endif

#ifdef TARGET_68060
/* Inline FixedDiv for 68060: overflow check + FPU divide.
 * 65536.0 as IEEE-754 single = 0x47800000. */
static inline fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b))
        return (a ^ b) < 0 ? ((fixed_t)0x80000000) : ((fixed_t)0x7FFFFFFF);
    {
        static const long fm_65536 = 0x47800000;
        fixed_t result;
        __asm__ volatile (
            "fmove.l %1,%%fp0\n\t"
            "fmul.s  %3,%%fp0\n\t"
            "fdiv.l  %2,%%fp0\n\t"
            "fmove.l %%fp0,%0"
            : "=d"(result)
            : "d"(a), "d"(b), "m"(fm_65536)
        );
        return result;
    }
}
#elif defined(TARGET_68030)
/* Inline FixedDiv for 68030: native DIVS.L 64/32, ~90 cycles.
 * Avoids the ~300+ cycle __divdi3 libgcc helper for 64-bit division. */
static inline fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b))
        return (a ^ b) < 0 ? ((fixed_t)0x80000000) : ((fixed_t)0x7FFFFFFF);
    {
        long hi = a >> 16;     /* sign-extended high half of (a << 16) */
        long lo = a << 16;     /* low half of (a << 16) */
        __asm__ volatile (
            "divs.l %2,%1:%0"
            : "+d"(lo), "+d"(hi)
            : "d"(b)
        );
        return (fixed_t)lo;
    }
}
#else
fixed_t FixedDiv        (fixed_t a, fixed_t b);
#endif

fixed_t FixedDiv2       (fixed_t a, fixed_t b);



#endif
/*----------------------------------------------------------------------------- */
/* */
/* $Log:$ */
/* */
/*----------------------------------------------------------------------------- */

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
/*	System specific interface stuff. */
/* */
/*----------------------------------------------------------------------------- */


#ifndef __R_MAIN__
#define __R_MAIN__

#include "d_player.h"
#include "r_data.h"


#ifdef __GNUG__
#pragma interface
#endif


/* */
/* POV related. */
/* */
extern fixed_t viewcos;
extern fixed_t viewsin;

extern int viewwidth;
extern int viewheight;
extern int viewwindowx;
extern int viewwindowy;



extern int centerx;
extern int centery;

extern fixed_t centerxfrac;
extern fixed_t centeryfrac;
extern fixed_t projection;

extern int validcount;

extern int linecount;
extern int loopcount;


/* */
/* Lighting LUT. */
/* Used for z-depth cuing per column/row, */
/*  and other lighting effects (sector ambient, flash). */
/* */

/* Lighting constants. */
/* Now why not 32 levels here? */
#define LIGHTLEVELS             16
#define LIGHTSEGSHIFT            4

#define MAXLIGHTSCALE           48
#define LIGHTSCALESHIFT         12
#define MAXLIGHTZ              128
#define LIGHTZSHIFT             20

extern lighttable_t*    scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
extern lighttable_t*    scalelightfixed[MAXLIGHTSCALE];
extern lighttable_t*    zlight[LIGHTLEVELS][MAXLIGHTZ];

extern int extralight;
extern lighttable_t*    fixedcolormap;


/* Number of diminishing brightness levels. */
/* There a 0-31, i.e. 32 LUT in the COLORMAP lump. */
#define NUMCOLORMAPS            32


/* Blocky/low detail mode. */
/*B remove this? */
/*  0 = high, 1 = low */
extern int detailshift;


/* */
/* Function pointers to switch refresh/drawing functions. */
/* Used to select shadow mode etc. */
/* */
extern void (*colfunc) (void);
extern void (*basecolfunc) (void);
extern void (*fuzzcolfunc) (void);
/* No shadow effects on floors. */
extern void (*spanfunc) (void);


/* */
/* Utility functions. */
/* R_PointOnSide -- inlined for the BSP traversal hot path.
 * Called once per BSP node (~200-400 times/frame); inlining eliminates
 * the function call overhead (movem save/restore + stack args).
 * The FixedMul slow path uses swap+muls.w (ADoom technique): the high
 * word of a fixed_t is the integer part, and on big-endian 68k it's
 * also the first word in memory, so muls.w on the struct field reads
 * the >>16 value directly.  Native MULS.W is ~1 cycle on 68060. */
static inline int
R_PointOnSide
        ( fixed_t x,
        fixed_t y,
        node_t*       node )
{
	if (!node->dx)
	{
		if (x <= node->x)
			return node->dy > 0;
		return node->dy < 0;
	}
	if (!node->dy)
	{
		if (y <= node->y)
			return node->dx < 0;
		return node->dx > 0;
	}

	{
		fixed_t dx = (x - node->x);
		fixed_t dy = (y - node->y);

		/* Try to quickly decide by looking at sign bits. */
		if ( (node->dy ^ node->dx ^ dx ^ dy) & 0x80000000 )
		{
			if ( (node->dy ^ dx) & 0x80000000 )
				return 1;	/* left is negative */
			return 0;
		}

		/* Slow path: cross-product comparison.
		 * left  = (node->dy >> 16) * (dx >> 16)
		 * right = (dy >> 16) * (node->dx >> 16)
		 * Use swap to get >>16 into low word, then muls.w for
		 * a native 16x16->32 signed multiply (~1c on 68060). */
		{
			int left, right;
			__asm__ volatile (
				"swap %0\n\t"
				"swap %1\n\t"
				"muls.w %2,%0\n\t"
				"muls.w %3,%1"
				: "=d" (left), "=d" (right)
				: "m" (*(short *)&node->dy),
				  "m" (*(short *)&node->dx),
				  "0" (dx), "1" (dy)
			);
			return right >= left;
		}
	}
}

/* Keep non-inline version for any other callers */
int R_PointOnSide_Func(fixed_t x, fixed_t y, node_t *node);

int
R_PointOnSegSide
        ( fixed_t x,
        fixed_t y,
        seg_t*        line );

angle_t
R_PointToAngle
        ( fixed_t x,
        fixed_t y );

angle_t
R_PointToAngle2
        ( fixed_t x1,
        fixed_t y1,
        fixed_t x2,
        fixed_t y2 );

fixed_t
R_PointToDist
        ( fixed_t x,
        fixed_t y );


fixed_t R_ScaleFromGlobalAngle (angle_t visangle);

subsector_t*
R_PointInSubsector
        ( fixed_t x,
        fixed_t y );

void
R_AddPointToBox
        ( int x,
        int y,
        fixed_t*      box );



/* */
/* REFRESH - the actual rendering functions. */
/* */

/* Called by G_Drawer. */
void R_RenderPlayerView (player_t *player);

/* Called by startup code. */
void R_Init (void);

/* Called by M_Responder. */
void R_SetViewSize (int blocks, int detail);

#endif
/*----------------------------------------------------------------------------- */
/* */
/* $Log:$ */
/* */
/*----------------------------------------------------------------------------- */

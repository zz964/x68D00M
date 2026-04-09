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
/* $Log:$ */
/* */
/* DESCRIPTION: */
/*	Rendering main loop and setup functions, */
/*	 utility functions (BSP, geometry, trigonometry). */
/*	See tables.c, too. */
/* */
/*----------------------------------------------------------------------------- */


static const char rcsid[] = "$Id: r_main.c,v 1.5 1997/02/03 22:45:12 b1 Exp $";



#include <stdlib.h>
#include <string.h>
#include <math.h>


#include "doomdef.h"
#include "d_net.h"

#include "m_bbox.h"

#include "r_local.h"
#include "r_sky.h"
#include "doom_perf.h"
#include "doom_log.h"
#include "i_system.h"





/* Fineangles in the SCREENWIDTH wide window. */
#define FIELDOFVIEW             2048



int viewangleoffset;

/* increment every time a check is made */
int validcount = 1;


lighttable_t*           fixedcolormap;
extern lighttable_t**   walllights;

int centerx;
int centery;

fixed_t centerxfrac;
fixed_t centeryfrac;
fixed_t projection;

/* just for profiling purposes */
int framecount;

int sscount;
int linecount;
int loopcount;

fixed_t viewx;
fixed_t viewy;
fixed_t viewz;

angle_t viewangle;

fixed_t viewcos;
fixed_t viewsin;

player_t*               viewplayer;

/* 0 = high, 1 = low */
#ifndef X68_NO_LOWDETAIL
int detailshift;
#endif

/* */
/* precalculated math tables */
/* */
angle_t clipangle;

/* The viewangletox[viewangle + FINEANGLES/4] lookup */
/* maps the visible view angles to screen X coordinates, */
/* flattening the arc to a flat projection plane. */
/* There will be many angles mapped to the same X. */
/* Allocated in HIMEM by R_SetViewSize on first call. */
int *viewangletox;

/* The xtoviewangleangle[] table maps a screen pixel */
/* to the lowest viewangle that maps back to x ranges */
/* from clipangle to -clipangle. */
angle_t *xtoviewangle;


/* UNUSED. */
/* The finetangentgent[angle+FINEANGLES/4] table */
/* holds the fixed_t tangent values for view angles, */
/* ranging from MININT to 0 to MAXINT. */
/* fixed_t		finetangent[FINEANGLES/2]; */

/* fixed_t		finesine[5*FINEANGLES/4]; */
fixed_t*                finecosine = &finesine[FINEANGLES/4];

/* Local-RAM copy of finesine for hot render paths.  Set by R_InitFastTables().
 * Falls back to finesine (system RAM) if himem alloc fails. */
fixed_t*                finesine_fast;

/* HIMEM copies of finetangent and tantoangle.  Set by R_InitFastTables().
 * Fall back to the static system-RAM originals if alloc fails. */
fixed_t*                finetangent_fast;
angle_t*                tantoangle_fast;


lighttable_t*           scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t*           scalelightfixed[MAXLIGHTSCALE];
lighttable_t*           zlight[LIGHTLEVELS][MAXLIGHTZ];

/* bumped light from gun blasts */
int extralight;



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);



/* */
/* R_AddPointToBox */
/* Expand a given bbox */
/* so that it encloses a given point. */
/* */
void
R_AddPointToBox
        ( int x,
        int y,
        fixed_t*      box )
{
	if (x< box[BOXLEFT])
		box[BOXLEFT] = x;
	if (x> box[BOXRIGHT])
		box[BOXRIGHT] = x;
	if (y< box[BOXBOTTOM])
		box[BOXBOTTOM] = y;
	if (y> box[BOXTOP])
		box[BOXTOP] = y;
}


/* */
/* R_PointOnSide -- inlined version in r_main.h for the hot BSP path.
 * This non-inline version kept for R_PointInSubsector and any other
 * callers that don't include the inline definition. */
int
R_PointOnSide_Func
        ( fixed_t x,
        fixed_t y,
        node_t*       node )
{
	return R_PointOnSide(x, y, node);
}


int
R_PointOnSegSide
        ( fixed_t x,
        fixed_t y,
        seg_t*        line )
{
	fixed_t lx;
	fixed_t ly;
	fixed_t ldx;
	fixed_t ldy;
	fixed_t dx;
	fixed_t dy;
	fixed_t left;
	fixed_t right;

	lx = line->v1->x;
	ly = line->v1->y;

	ldx = line->v2->x - lx;
	ldy = line->v2->y - ly;

	if (!ldx)
	{
		if (x <= lx)
			return ldy > 0;

		return ldy < 0;
	}
	if (!ldy)
	{
		if (y <= ly)
			return ldx < 0;

		return ldx > 0;
	}

	dx = (x - lx);
	dy = (y - ly);

	/* Try to quickly decide by looking at sign bits. */
	if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
	{
		if  ( (ldy ^ dx) & 0x80000000 )
		{
			/* (left is negative) */
			return 1;
		}
		return 0;
	}

	left = FixedMul ( ldy>>FRACBITS, dx );
	right = FixedMul ( dy, ldx>>FRACBITS );

	if (right < left)
	{
		/* front side */
		return 0;
	}
	/* back side */
	return 1;
}


/* */
/* R_PointToAngle */
/* To get a global angle from cartesian coordinates, */
/*  the coordinates are flipped until they are in */
/*  the first octant of the coordinate system, then */
/*  the y (<=x) is scaled and divided by x to get a */
/*  tangent (slope) value which is looked up in the */
/*  tantoangle[] table. */

/* */




angle_t
R_PointToAngle
        ( fixed_t x,
        fixed_t y )
{
	x -= viewx;
	y -= viewy;

	if ( (!x) && (!y) )
		return 0;

	if (x>= 0)
	{
		/* x >=0 */
		if (y>= 0)
		{
			/* y>= 0 */

			if (x>y)
			{
				/* octant 0 */
				return tantoangle_fast[ SlopeDiv(y,x)];
			}
			else
			{
				/* octant 1 */
				return ANG90-1-tantoangle_fast[ SlopeDiv(x,y)];
			}
		}
		else
		{
			/* y<0 */
			y = -y;

			if (x>y)
			{
				/* octant 8 */
				return -tantoangle_fast[SlopeDiv(y,x)];
			}
			else
			{
				/* octant 7 */
				return ANG270+tantoangle_fast[ SlopeDiv(x,y)];
			}
		}
	}
	else
	{
		/* x<0 */
		x = -x;

		if (y>= 0)
		{
			/* y>= 0 */
			if (x>y)
			{
				/* octant 3 */
				return ANG180-1-tantoangle_fast[ SlopeDiv(y,x)];
			}
			else
			{
				/* octant 2 */
				return ANG90+ tantoangle_fast[ SlopeDiv(x,y)];
			}
		}
		else
		{
			/* y<0 */
			y = -y;

			if (x>y)
			{
				/* octant 4 */
				return ANG180+tantoangle_fast[ SlopeDiv(y,x)];
			}
			else
			{
				/* octant 5 */
				return ANG270-1-tantoangle_fast[ SlopeDiv(x,y)];
			}
		}
	}
	return 0;
}


angle_t
R_PointToAngle2
        ( fixed_t x1,
        fixed_t y1,
        fixed_t x2,
        fixed_t y2 )
{
	viewx = x1;
	viewy = y1;

	return R_PointToAngle (x2, y2);
}


fixed_t
R_PointToDist
        ( fixed_t x,
        fixed_t y )
{
	int angle;
	fixed_t dx;
	fixed_t dy;
	fixed_t temp;
	fixed_t dist;

	dx = abs(x - viewx);
	dy = abs(y - viewy);

	if (dy>dx)
	{
		temp = dx;
		dx = dy;
		dy = temp;
	}

	angle = (tantoangle_fast[ FixedDiv(dy,dx)>>DBITS ]+ANG90) >> ANGLETOFINESHIFT;

	/* use as cosine */
	dist = FixedDiv (dx, finesine_fast[angle] );

	return dist;
}




/* */
/* R_InitPointToAngle */
/* */
void R_InitPointToAngle (void)
{
	/* UNUSED - now getting from tables.c */
#if 0
	int i;
	long t;
	float f;
/* */
/* slope (tangent) to angle lookup */
/* */
	for (i=0; i<=SLOPERANGE; i++)
	{
		f = atan( (float)i/SLOPERANGE )/(3.141592657*2);
		t = 0xffffffff*f;
		tantoangle[i] = t;
	}
#endif
}


/* */
/* R_ScaleFromGlobalAngle */
/* Returns the texture mapping scale */
/*  for the current line (horizontal span) */
/*  at the given angle. */
/* rw_distance must be calculated first. */
/* */
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
	fixed_t scale;
	int anglea;
	int angleb;
	int sinea;
	int sineb;
	fixed_t num;
	int den;

	/* UNUSED */
#if 0
	{
		fixed_t dist;
		fixed_t z;
		fixed_t sinv;
		fixed_t cosv;

		sinv = finesine[(visangle-rw_normalangle)>>ANGLETOFINESHIFT];
		dist = FixedDiv (rw_distance, sinv);
		cosv = finecosine[(viewangle-visangle)>>ANGLETOFINESHIFT];
		z = abs(FixedMul (dist, cosv));
		scale = FixedDiv(projection, z);
		return scale;
	}
#endif

	anglea = ANG90 + (visangle-viewangle);
	angleb = ANG90 + (visangle-rw_normalangle);

	/* both sines are allways positive */
	sinea = finesine[anglea>>ANGLETOFINESHIFT];
	sineb = finesine[angleb>>ANGLETOFINESHIFT];
	num = FixedMul(projection,sineb)<<detailshift;
	den = FixedMul(rw_distance,sinea);

	if (den > num>>16)
	{
		scale = FixedDiv (num, den);

		if (scale > 64*FRACUNIT)
			scale = 64*FRACUNIT;
		else if (scale < 256)
			scale = 256;
	}
	else
		scale = 64*FRACUNIT;

	return scale;
}



/* */
/* R_InitTables */
/* */
void R_InitTables (void)
{
	/* UNUSED: now getting from tables.c */
#if 0
	int i;
	float a;
	float fv;
	int t;

	/* viewangle tangent table */
	for (i=0; i<FINEANGLES/2; i++)
	{
		a = (i-FINEANGLES/4+0.5)*PI*2/FINEANGLES;
		fv = FRACUNIT*tan (a);
		t = fv;
		finetangent[i] = t;
	}

	/* finesine table */
	for (i=0; i<5*FINEANGLES/4; i++)
	{
		/* OPTIMIZE: mirror... */
		a = (i+0.5)*PI*2/FINEANGLES;
		t = FRACUNIT*sin (a);
		finesine[i] = t;
	}
#endif

}



/* */
/* R_InitTextureMapping */
/* */
void R_InitTextureMapping (void)
{
	int i;
	int x;
	int t;
	fixed_t focallength;

	/* Use tangent table to generate viewangletox: */
	/*  viewangletox will give the next greatest x */
	/*  after the view angle. */
	/* */
	/* Calc focallength */
	/*  so FIELDOFVIEW angles covers SCREENWIDTH. */
	focallength = FixedDiv (centerxfrac,
	                        finetangent[FINEANGLES/4+FIELDOFVIEW/2] );

	for (i=0; i<FINEANGLES/2; i++)
	{
		if (finetangent[i] > FRACUNIT*2)
			t = -1;
		else if (finetangent[i] < -FRACUNIT*2)
			t = viewwidth+1;
		else
		{
			t = FixedMul (finetangent[i], focallength);
			t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

			if (t < -1)
				t = -1;
			else if (t>viewwidth+1)
				t = viewwidth+1;
		}
		viewangletox[i] = t;
	}

	/* Scan viewangletox[] to generate xtoviewangle[]: */
	/*  xtoviewangle will give the smallest view angle */
	/*  that maps to x. */
	for (x=0; x<=viewwidth; x++)
	{
		i = 0;
		while (viewangletox[i]>x)
			i++;
		xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
	}

	/* Take out the fencepost cases from viewangletox. */
	for (i=0; i<FINEANGLES/2; i++)
	{
		t = FixedMul (finetangent[i], focallength);
		t = centerx - t;

		if (viewangletox[i] == -1)
			viewangletox[i] = 0;
		else if (viewangletox[i] == viewwidth+1)
			viewangletox[i]  = viewwidth;
	}

	clipangle = xtoviewangle[0];
}



/* */
/* R_InitLightTables */
/* Only inits the zlight table, */
/*  because the scalelight table changes with view size. */
/* */
#define DISTMAP         2

void R_InitLightTables (void)
{
	int i;
	int j;
	int level;
	int startmap;
	int scale;

	/* Calculate the light levels to use */
	/*  for each level / distance combination. */
	for (i=0; i< LIGHTLEVELS; i++)
	{
		startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
		for (j=0; j<MAXLIGHTZ; j++)
		{
			scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
			scale >>= LIGHTSCALESHIFT;
			level = startmap - scale/DISTMAP;

			if (level < 0)
				level = 0;

			if (level >= NUMCOLORMAPS)
				level = NUMCOLORMAPS-1;

			zlight[i][j] = colormaps + level*256;
		}
	}
}



/* */
/* R_SetViewSize */
/* Do not really change anything here, */
/*  because it might be in the middle of a refresh. */
/* The change will take effect next refresh. */
/* */
boolean setsizeneeded;
int setblocks;
int setdetail;


void
R_SetViewSize
        ( int blocks,
        int detail )
{
	setsizeneeded = true;
	setblocks = blocks;
	setdetail = detail;
}


/* */
/* R_ExecuteSetViewSize */
/* */
void R_ExecuteSetViewSize (void)
{
	fixed_t cosadj;
	fixed_t dy;
	int i;
	int j;
	int level;
	int startmap;

	setsizeneeded = false;

	/* Allocate runtime lookup tables. Try local RAM first (fast on 060turbo),
	 * fall back to system RAM malloc (required for 68030). */
	if (!xtoviewangle) {
		xtoviewangle = (angle_t *)I_HimemMalloc((SCREENWIDTH+1) * sizeof(angle_t));
		if (!xtoviewangle) {
			xtoviewangle = (angle_t *)malloc((SCREENWIDTH+1) * sizeof(angle_t));
			I_RegisterAlloc(xtoviewangle);
		}
	}
	if (!yslope) {
		yslope = (fixed_t *)I_HimemMalloc(SCREENHEIGHT * sizeof(fixed_t));
		if (!yslope) {
			yslope = (fixed_t *)malloc(SCREENHEIGHT * sizeof(fixed_t));
			I_RegisterAlloc(yslope);
		}
	}
	if (!distscale) {
		distscale = (fixed_t *)I_HimemMalloc(SCREENWIDTH * sizeof(fixed_t));
		if (!distscale) {
			distscale = (fixed_t *)malloc(SCREENWIDTH * sizeof(fixed_t));
			I_RegisterAlloc(distscale);
		}
	}
	if (!viewangletox) {
		viewangletox = (int *)I_HimemMalloc((FINEANGLES/2) * sizeof(int));
		if (!viewangletox) {
			viewangletox = (int *)malloc((FINEANGLES/2) * sizeof(int));
			I_RegisterAlloc(viewangletox);
		}
	}

	if (setblocks == 11)
	{
		scaledviewwidth = SCREENWIDTH;
		viewheight = SCREENHEIGHT;
	}
	else
	{
		scaledviewwidth = setblocks*32;
		viewheight = (setblocks*168/10)&~7;
	}

#ifndef X68_NO_LOWDETAIL
	detailshift = setdetail;
#endif
	viewwidth = scaledviewwidth>>detailshift;

	/* Guard against zero-size viewport (can happen during rapid
	 * screen size changes from the menu). */
	if (viewwidth < 1) viewwidth = 1;
	if (viewheight < 1) viewheight = 1;
	if (scaledviewwidth < 1) scaledviewwidth = 1;

	centery = viewheight/2;
	centerx = viewwidth/2;
	centerxfrac = centerx<<FRACBITS;
	centeryfrac = centery<<FRACBITS;
	projection = centerxfrac;

	if (!detailshift)
	{
		colfunc = basecolfunc = R_DrawColumn;
		fuzzcolfunc = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc = R_DrawSpan;
	}
	else
	{
		colfunc = basecolfunc = R_DrawColumnLow;
		fuzzcolfunc = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc = R_DrawSpanLow;
	}

	/* Profiling wrappers disabled: ~22k calls/sec overhead costs ~2fps.
	 * Re-enable by uncommenting when profiling span/col workload. */
	/* Perf_InstallFuncWrappers(); */

	R_InitBuffer (scaledviewwidth, viewheight);

	R_InitTextureMapping ();

	/* psprite scales */
	pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;
	pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;

	/* thing clipping */
	for (i=0; i<viewwidth; i++)
		screenheightarray[i] = viewheight;

	/* planes */
	for (i=0; i<viewheight; i++)
	{
		dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;
		dy = abs(dy);
		yslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);
	}

	for (i=0; i<viewwidth; i++)
	{
		cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
		distscale[i] = FixedDiv (FRACUNIT,cosadj);
	}

	/* Calculate the light levels to use */
	/*  for each level / scale combination. */
	for (i=0; i< LIGHTLEVELS; i++)
	{
		startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
		for (j=0; j<MAXLIGHTSCALE; j++)
		{
			level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;

			if (level < 0)
				level = 0;

			if (level >= NUMCOLORMAPS)
				level = NUMCOLORMAPS-1;

			scalelight[i][j] = colormaps + level*256;
		}
	}
}



/* */
/* R_InitFastTables */
/* Copy compile-time-initialised trig tables to 060turbo local RAM so that  */
/* hot render paths avoid system-bus latency on every lookup.                */
/* Call once from D_DoomMain after R_Init().                                 */
/* */
void R_InitFastTables (void)
{
	/* finesine / finecosine (10,240 entries = 40 KB) */
	{
		int sz = (5*FINEANGLES/4) * sizeof(fixed_t);
		fixed_t *p = (fixed_t *)I_HimemMalloc(sz);
		if (!p) { p = (fixed_t *)malloc(sz); I_RegisterAlloc(p); }
		if (p)
		{
			memcpy(p, finesine, sz);
			finesine_fast = p;
			finecosine    = p + FINEANGLES/4;
			dlog("R_InitFastTables: finesine   -> 0x%08lx", (unsigned long)p);
		}
		else
		{
			finesine_fast = finesine;
			dlog("R_InitFastTables: finesine alloc failed, stays in system RAM");
		}
	}

	/* finetangent (4,096 entries = 16 KB) */
	{
		int sz = (FINEANGLES/2) * sizeof(fixed_t);
		fixed_t *p = (fixed_t *)I_HimemMalloc(sz);
		if (!p) { p = (fixed_t *)malloc(sz); I_RegisterAlloc(p); }
		if (p)
		{
			memcpy(p, finetangent, sz);
			finetangent_fast = p;
			dlog("R_InitFastTables: finetangent -> 0x%08lx", (unsigned long)p);
		}
		else
		{
			finetangent_fast = finetangent;
			dlog("R_InitFastTables: finetangent alloc failed, stays in system RAM");
		}
	}

	/* tantoangle (2,049 entries = 8 KB) */
	{
		int sz = (SLOPERANGE+1) * sizeof(angle_t);
		angle_t *p = (angle_t *)I_HimemMalloc(sz);
		if (!p) { p = (angle_t *)malloc(sz); I_RegisterAlloc(p); }
		if (p)
		{
			memcpy(p, tantoangle, sz);
			tantoangle_fast = p;
			dlog("R_InitFastTables: tantoangle  -> 0x%08lx", (unsigned long)p);
		}
		else
		{
			tantoangle_fast = tantoangle;
			dlog("R_InitFastTables: tantoangle alloc failed, stays in system RAM");
		}
	}
}


/* */
/* R_Init */
/* */
extern int detailLevel;
extern int screenblocks;



void R_Init (void)
{
	R_InitData ();
	R_InitPointToAngle ();
	printf ("R_InitPointToAngle\n");
	R_InitTables ();
	/* viewwidth / viewheight / detailLevel are set by the defaults */
	printf ("R_InitTables\n");

	R_SetViewSize (screenblocks, detailLevel);
	R_InitPlanes ();
	printf ("R_InitPlanes\n");
	R_InitLightTables ();
	printf ("R_InitLightTables\n");
	R_InitSkyMap ();
	printf ("R_InitSkyMap\n");
	R_InitTranslationTables ();
	printf ("R_InitTranslationsTables\n");

	framecount = 0;
}


/* */
/* R_PointInSubsector */
/* */
subsector_t*
R_PointInSubsector
        ( fixed_t x,
        fixed_t y )
{
	node_t*     node;
	int side;
	int nodenum;

	/* single subsector is a special case */
	if (!numnodes)
		return subsectors;

	nodenum = numnodes-1;

	while (!(nodenum & NF_SUBSECTOR) )
	{
		node = &nodes[nodenum];
		side = R_PointOnSide (x, y, node);
		nodenum = node->children[side];
	}

	return &subsectors[nodenum & ~NF_SUBSECTOR];
}



/* */
/* R_SetupFrame */
/* */
void R_SetupFrame (player_t* player)
{
	int i;

	viewplayer = player;
	viewx = player->mo->x;
	viewy = player->mo->y;
	viewangle = player->mo->angle + viewangleoffset;
	extralight = player->extralight;

	viewz = player->viewz;

	viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
	viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

	sscount = 0;

	if (player->fixedcolormap)
	{
		fixedcolormap =
			colormaps
			+ player->fixedcolormap*256;

		walllights = scalelightfixed;

		for (i=0; i<MAXLIGHTSCALE; i++)
			scalelightfixed[i] = fixedcolormap;
	}
	else
		fixedcolormap = 0;

	framecount++;
	validcount++;
}



/* */
/* R_RenderView */
/* */
/* Sub-phase profiling accumulators -- old centisecond versions kept for
 * the existing log path; new microsecond versions in doom_perf.h give
 * much finer granularity. */
extern int g_perf_bsp_tics;
extern int g_perf_planes_tics;
extern int g_perf_masked_tics;

#include "i_prof_timer.h"
#include "doom_perf.h"

void R_RenderPlayerView (player_t* player)
{
	R_SetupFrame (player);

	/* Clear buffers. */
	R_ClearClipSegs ();
	R_ClearDrawSegs ();
	R_ClearPlanes ();
	R_ClearSprites ();

#ifndef X68_NO_NETGAME
	/* check for new console commands. */
	NetUpdate ();
#endif

	/* The head node is the last node output. */
#ifdef DOOM_LOG
	{
		int _t; uint32_t _us0;
		_t = I_GetTimeCs(); _us0 = I_GetTimeUs();
		R_RenderBSPNode (numnodes-1);
		g_perf_bsp_us += I_GetTimeUs() - _us0;
		g_perf_bsp_tics += I_GetTimeCs() - _t;
	}
#else
	R_RenderBSPNode (numnodes-1);
#endif

#ifndef X68_NO_NETGAME
	/* Check for new console commands. */
	NetUpdate ();
#endif

#ifdef DOOM_LOG
	{
		extern visplane_t *lastvisplane;
		extern visplane_t visplanes[];
		int _t; uint32_t _us0;
		g_perf_visplanes += (uint32_t)(lastvisplane - visplanes);
		_t = I_GetTimeCs(); _us0 = I_GetTimeUs();
		R_DrawPlanes ();
		g_perf_planes_us += I_GetTimeUs() - _us0;
		g_perf_planes_tics += I_GetTimeCs() - _t;
	}
#else
	R_DrawPlanes ();
#endif

#ifndef X68_NO_NETGAME
	/* Check for new console commands. */
	NetUpdate ();
#endif

#ifdef DOOM_LOG
	{
		int _t; uint32_t _us0;
		_t = I_GetTimeCs(); _us0 = I_GetTimeUs();
		R_DrawMasked ();
		g_perf_masked_us += I_GetTimeUs() - _us0;
		g_perf_masked_tics += I_GetTimeCs() - _t;
	}
#else
	R_DrawMasked ();
#endif

#ifndef X68_NO_NETGAME
	/* Check for new console commands. */
	NetUpdate ();
#endif
}

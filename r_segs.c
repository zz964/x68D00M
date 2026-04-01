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
/*	All the clipping: columns, horizontal spans, sky columns. */
/* */
/*----------------------------------------------------------------------------- */


static const char
        rcsid[] = "$Id: r_segs.c,v 1.3 1997/01/29 20:10:19 b1 Exp $";





#include <stdlib.h>

#include "i_system.h"
#include "i_prof_timer.h"
#include "doom_perf.h"

#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"

#include "r_local.h"
#include "r_sky.h"

extern fixed_t *finetangent_fast; /* HIMEM copy; set by R_InitFastTables */

/* Inline fast path for R_GetColumn -- avoids the full W_CacheLumpNum +
 * Z_ChangeTag call chain when the lump is already cached (the common case
 * during rendering).  Falls back to the real R_GetColumn for cache misses
 * and composite textures.
 *
 * Hot path: 5 memory lookups + 1 branch + 1 direct tag write.
 * Original: 5 lookups + function call + range check + Z_ChangeTag macro
 * (ZONEID check) + Z_ChangeTag2 call (another ZONEID check + tag write).
 *
 * DoomAttack uses the same technique in its R_GetColumn2 macro. */
#include "w_wad.h"   /* lumpcache */
#include "z_zone.h"  /* memblock_t, PU_CACHE */

extern int*             texturewidthmask;
extern short**          texturecolumnlump;
extern unsigned short** texturecolumnofs;
extern byte**           texturecomposite;
extern void             R_GenerateComposite(int texnum);

static inline byte* R_GetColumn_Fast(int tex, int col)
{
	int lump;
	int ofs;

	col &= texturewidthmask[tex];
	lump = texturecolumnlump[tex][col];
	ofs = texturecolumnofs[tex][col];

	if (lump > 0)
	{
		void *cached = lumpcache[lump];
		if (cached)
		{
			/* Lump is cached -- just mark it PU_CACHE to prevent
			 * the zone allocator from freeing it.  Write the tag
			 * directly at the known memblock_t offset, skipping
			 * the full Z_ChangeTag/Z_ChangeTag2 call chain. */
			((memblock_t *)((byte *)cached - sizeof(memblock_t)))->tag = PU_CACHE;
			return (byte *)cached + ofs;
		}
		/* Cache miss -- fall through to full function */
		return (byte *)W_CacheLumpNum(lump, PU_CACHE) + ofs;
	}

	if (!texturecomposite[tex])
		R_GenerateComposite(tex);

	return texturecomposite[tex] + ofs;
}


/* OPTIMIZE: closed two sided lines as single sided */

/* True if any of the segs textures might be visible. */
boolean segtextured;

/* False if the back side is the same plane. */
boolean markfloor;
boolean markceiling;

boolean maskedtexture;
int toptexture;
int bottomtexture;
int midtexture;


angle_t rw_normalangle;
/* angle to line origin */
int rw_angle1;

/* */
/* regular wall */
/* */
int rw_x;
int rw_stopx;
angle_t rw_centerangle;
fixed_t rw_offset;
fixed_t rw_distance;
fixed_t rw_scale;
fixed_t rw_scalestep;
fixed_t rw_midtexturemid;
fixed_t rw_toptexturemid;
fixed_t rw_bottomtexturemid;

int worldtop;
int worldbottom;
int worldhigh;
int worldlow;

fixed_t pixhigh;
fixed_t pixlow;
fixed_t pixhighstep;
fixed_t pixlowstep;

fixed_t topfrac;
fixed_t topstep;

fixed_t bottomfrac;
fixed_t bottomstep;


lighttable_t**  walllights;

short*          maskedtexturecol;



/* */
/* R_RenderMaskedSegRange */
/* */
void
R_RenderMaskedSegRange
        ( drawseg_t*    ds,
        int x1,
        int x2 )
{
	unsigned index;
	column_t*   col;
	int lightnum;
	int texnum;

	/* Calculate light table. */
	/* Use different light tables */
	/*   for horizontal / vertical / diagonal. Diagonal? */
	/* OPTIMIZE: get rid of LIGHTSEGSHIFT globally */
	curline = ds->curline;
	frontsector = curline->frontsector;
	backsector = curline->backsector;
	texnum = texturetranslation[curline->sidedef->midtexture];

	lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

	if (curline->v1->y == curline->v2->y)
		lightnum--;
	else if (curline->v1->x == curline->v2->x)
		lightnum++;

	if (lightnum < 0)
		walllights = scalelight[0];
	else if (lightnum >= LIGHTLEVELS)
		walllights = scalelight[LIGHTLEVELS-1];
	else
		walllights = scalelight[lightnum];

	maskedtexturecol = ds->maskedtexturecol;

	rw_scalestep = ds->scalestep;
	spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
	mfloorclip = ds->sprbottomclip;
	mceilingclip = ds->sprtopclip;

	/* find positioning */
	if (curline->linedef->flags & ML_DONTPEGBOTTOM)
	{
		dc_texturemid = frontsector->floorheight > backsector->floorheight
		                ? frontsector->floorheight : backsector->floorheight;
		dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
	}
	else
	{
		dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight
		                ? frontsector->ceilingheight : backsector->ceilingheight;
		dc_texturemid = dc_texturemid - viewz;
	}
	dc_texturemid += curline->sidedef->rowoffset;

	if (fixedcolormap)
		dc_colormap = fixedcolormap;

	/* draw the columns */
	for (dc_x = x1; dc_x <= x2; dc_x++)
	{
		/* calculate lighting */
		if (maskedtexturecol[dc_x] != MAXSHORT)
		{
			if (!fixedcolormap)
			{
				index = spryscale>>LIGHTSCALESHIFT;

				if (index >=  MAXLIGHTSCALE )
					index = MAXLIGHTSCALE-1;

				dc_colormap = walllights[index];
			}

			sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
			dc_iscale = 0xffffffffu / (unsigned)spryscale;

			/* draw the texture */
			col = (column_t *)(
				(byte *)R_GetColumn_Fast(texnum,maskedtexturecol[dc_x]) -3);

			R_DrawMaskedColumn (col);
			maskedtexturecol[dc_x] = MAXSHORT;
		}
		spryscale += rw_scalestep;
	}

}




/* */
/* R_RenderSegLoop */
/* Draws zero, one, or two textures (and possibly a masked */
/*  texture) for walls. */
/* Can draw or mark the starting pixel of floor and ceiling */
/*  textures. */
/* CALLED: CORE LOOPING ROUTINE. */
/* */
#define HEIGHTBITS              12
#define HEIGHTUNIT              (1<<HEIGHTBITS)

void R_RenderSegLoop (void)
{
	angle_t angle;
	unsigned index;
	int yl;
	int yh;
	int mid;
	fixed_t texturecolumn;
	int top;
	int bottom;

	/* Localize hot variables and use advancing pointers instead of
	 * array[lx] indexing. On 68k, *(ptr)++ is cheaper than base[idx]
	 * which requires index scaling on every access. */
	int lx = rw_x;
	fixed_t ltopfrac = topfrac;
	fixed_t lbottomfrac = bottomfrac;
	fixed_t lscale = rw_scale;

	/* Advancing pointers for arrays indexed by lx */
	short *cc = &ceilingclip[lx];   /* ceilingclip */
	short *fc = &floorclip[lx];     /* floorclip */
	byte  *cpt = &ceilingplane->top[lx];
	byte  *cpb = &ceilingplane->bottom[lx];
	byte  *fpt = &floorplane->top[lx];
	byte  *fpb = &floorplane->bottom[lx];
	angle_t *xvp = &xtoviewangle[lx];
	short *mtc = maskedtexture ? &maskedtexturecol[lx] : NULL;

	for (; lx < rw_stopx; lx++, cc++, fc++, cpt++, cpb++, fpt++, fpb++, xvp++)
	{
		/* mark floor / ceiling areas */
		yl = (ltopfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

		/* no space above wall? */
		if (yl < *cc+1)
			yl = *cc+1;

		if (markceiling)
		{
			top = *cc+1;
			bottom = yl-1;

			if (bottom >= *fc)
				bottom = *fc-1;

			if (top <= bottom)
			{
				*cpt = top;
				*cpb = bottom;
			}
		}

		yh = lbottomfrac>>HEIGHTBITS;

		if (yh >= *fc)
			yh = *fc-1;

		if (markfloor)
		{
			top = yh+1;
			bottom = *fc-1;
			if (top <= *cc)
				top = *cc+1;
			if (top <= bottom)
			{
				*fpt = top;
				*fpb = bottom;
			}
		}

		/* texturecolumn and lighting are independent of wall tiers */
		if (segtextured)
		{
			/* calculate texture offset */
			angle = (rw_centerangle + *xvp)>>ANGLETOFINESHIFT;
#ifdef TARGET_68060
			/* Inline FPU FixedMul: 68060 FPU is native silicon, ~3-6c per op. */
			{
				fixed_t _ft = finetangent_fast[angle];
				fixed_t _rd = rw_distance;
				fixed_t _fm;
				__asm__ volatile (
					"fmove.l %1,%%fp0\n\t"
					"fmul.l  %2,%%fp0\n\t"
					"fmul.s  _FM_INV65536,%%fp0\n\t"
					"fmove.l %%fp0,%0"
					: "=d" (_fm)
					: "d" (_ft), "m" (_rd)
				);
				texturecolumn = rw_offset - _fm;
			}
#else
			texturecolumn = rw_offset - FixedMul(finetangent[angle], rw_distance);
#endif
			texturecolumn >>= FRACBITS;
			/* calculate lighting */
			index = lscale>>LIGHTSCALESHIFT;

			if (index >=  MAXLIGHTSCALE )
				index = MAXLIGHTSCALE-1;

			dc_colormap = walllights[index];
			dc_x = lx;
			dc_iscale = 0xffffffffu / (unsigned)lscale;
		}

		/* draw the wall tiers */
		if (midtexture)
		{
			/* single sided line */
			dc_yl = yl;
			dc_yh = yh;
			dc_texturemid = rw_midtexturemid;
			dc_source = R_GetColumn_Fast(midtexture,texturecolumn);
			colfunc ();
			*cc = viewheight;
			*fc = -1;
		}
		else
		{
			/* two sided line */
			if (toptexture)
			{
				/* top wall */
				mid = pixhigh>>HEIGHTBITS;
				pixhigh += pixhighstep;

				if (mid >= *fc)
					mid = *fc-1;

				if (mid >= yl)
				{
					dc_yl = yl;
					dc_yh = mid;
					dc_texturemid = rw_toptexturemid;
					dc_source = R_GetColumn_Fast(toptexture,texturecolumn);
					colfunc ();
					*cc = mid;
				}
				else
					*cc = yl-1;
			}
			else
			{
				/* no top wall */
				if (markceiling)
					*cc = yl-1;
			}

			if (bottomtexture)
			{
				/* bottom wall */
				mid = (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
				pixlow += pixlowstep;

				/* no space above wall? */
				if (mid <= *cc)
					mid = *cc+1;

				if (mid <= yh)
				{
					dc_yl = mid;
					dc_yh = yh;
					dc_texturemid = rw_bottomtexturemid;
					dc_source = R_GetColumn_Fast(bottomtexture,
					                             texturecolumn);
					colfunc ();
					*fc = mid;
				}
				else
					*fc = yh+1;
			}
			else
			{
				/* no bottom wall */
				if (markfloor)
					*fc = yh+1;
			}

			if (mtc)
			{
				/* save texturecol */
				/*  for backdrawing of masked mid texture */
				*mtc++ = texturecolumn;
			}
		}

		lscale += rw_scalestep;
		ltopfrac += topstep;
		lbottomfrac += bottomstep;
	}

	/* Write back */
	rw_x = lx;
	rw_scale = lscale;
	topfrac = ltopfrac;
	bottomfrac = lbottomfrac;
}




/* */
/* R_StoreWallRange */
/* A wall segment will be drawn */
/*  between start and stop pixels (inclusive). */
/* */
void
R_StoreWallRange
        ( int start,
        int stop )
{
	fixed_t hyp;
	fixed_t sineval;
	angle_t distangle, offsetangle;
#ifdef DOOM_LOG
	uint32_t _swr_t0 = I_GetTimeUs();
#endif
	fixed_t vtop;
	int lightnum;

	/* don't overflow and crash */
	if (ds_p == &drawsegs[MAXDRAWSEGS])
		return;

#ifdef RANGECHECK
	if (start >=viewwidth || start > stop)
		I_Error ("Bad R_RenderWallRange: %i to %i", start, stop);
#endif

	sidedef = curline->sidedef;
	linedef = curline->linedef;

	/* mark the segment as visible for auto map */
	linedef->flags |= ML_MAPPED;

	/* calculate rw_distance for scale calculation */
	rw_normalangle = curline->angle + ANG90;
	offsetangle = abs(rw_normalangle-rw_angle1);

	if (offsetangle > ANG90)
		offsetangle = ANG90;

	distangle = ANG90 - offsetangle;
	hyp = R_PointToDist (curline->v1->x, curline->v1->y);
	sineval = finesine[distangle>>ANGLETOFINESHIFT];
	rw_distance = FixedMul (hyp, sineval);


	ds_p->x1 = rw_x = start;
	ds_p->x2 = stop;
	ds_p->curline = curline;
	rw_stopx = stop+1;

	/* calculate scale at both ends and step */
	ds_p->scale1 = rw_scale =
			       R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

	if (stop > start )
	{
		ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
		ds_p->scalestep = rw_scalestep =
					  (ds_p->scale2 - rw_scale) / (stop-start);
	}
	else
	{
		/* UNUSED: try to fix the stretched line bug */
#if 0
		if (rw_distance < FRACUNIT/2)
		{
			fixed_t trx,try;
			fixed_t gxt,gyt;

			trx = curline->v1->x - viewx;
			try = curline->v1->y - viewy;

			gxt = FixedMul(trx,viewcos);
			gyt = -FixedMul(try,viewsin);
			ds_p->scale1 = FixedDiv(projection, gxt-gyt)<<detailshift;
		}
#endif
		ds_p->scale2 = ds_p->scale1;
	}

	/* calculate texture boundaries */
	/*  and decide if floor / ceiling marks are needed */
	worldtop = frontsector->ceilingheight - viewz;
	worldbottom = frontsector->floorheight - viewz;

	midtexture = toptexture = bottomtexture = maskedtexture = 0;
	ds_p->maskedtexturecol = NULL;

	if (!backsector)
	{
		/* single sided line */
		midtexture = texturetranslation[sidedef->midtexture];
		/* a single sided line is terminal, so it must mark ends */
		markfloor = markceiling = true;
		if (linedef->flags & ML_DONTPEGBOTTOM)
		{
			vtop = frontsector->floorheight +
			       textureheight[sidedef->midtexture];
			/* bottom of texture at bottom */
			rw_midtexturemid = vtop - viewz;
		}
		else
		{
			/* top of texture at top */
			rw_midtexturemid = worldtop;
		}
		rw_midtexturemid += sidedef->rowoffset;

		ds_p->silhouette = SIL_BOTH;
		ds_p->sprtopclip = screenheightarray;
		ds_p->sprbottomclip = negonearray;
		ds_p->bsilheight = MAXINT;
		ds_p->tsilheight = MININT;
	}
	else
	{
		/* two sided line */
		ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
		ds_p->silhouette = 0;

		if (frontsector->floorheight > backsector->floorheight)
		{
			ds_p->silhouette = SIL_BOTTOM;
			ds_p->bsilheight = frontsector->floorheight;
		}
		else if (backsector->floorheight > viewz)
		{
			ds_p->silhouette = SIL_BOTTOM;
			ds_p->bsilheight = MAXINT;
			/* ds_p->sprbottomclip = negonearray; */
		}

		if (frontsector->ceilingheight < backsector->ceilingheight)
		{
			ds_p->silhouette |= SIL_TOP;
			ds_p->tsilheight = frontsector->ceilingheight;
		}
		else if (backsector->ceilingheight < viewz)
		{
			ds_p->silhouette |= SIL_TOP;
			ds_p->tsilheight = MININT;
			/* ds_p->sprtopclip = screenheightarray; */
		}

		if (backsector->ceilingheight <= frontsector->floorheight)
		{
			ds_p->sprbottomclip = negonearray;
			ds_p->bsilheight = MAXINT;
			ds_p->silhouette |= SIL_BOTTOM;
		}

		if (backsector->floorheight >= frontsector->ceilingheight)
		{
			ds_p->sprtopclip = screenheightarray;
			ds_p->tsilheight = MININT;
			ds_p->silhouette |= SIL_TOP;
		}

		worldhigh = backsector->ceilingheight - viewz;
		worldlow = backsector->floorheight - viewz;

		/* hack to allow height changes in outdoor areas */
		if (frontsector->ceilingpic == skyflatnum
		    && backsector->ceilingpic == skyflatnum)
		{
			worldtop = worldhigh;
		}


		if (worldlow != worldbottom
		    || backsector->floorpic != frontsector->floorpic
		    || backsector->lightlevel != frontsector->lightlevel)
		{
			markfloor = true;
		}
		else
		{
			/* same plane on both sides */
			markfloor = false;
		}


		if (worldhigh != worldtop
		    || backsector->ceilingpic != frontsector->ceilingpic
		    || backsector->lightlevel != frontsector->lightlevel)
		{
			markceiling = true;
		}
		else
		{
			/* same plane on both sides */
			markceiling = false;
		}

		if (backsector->ceilingheight <= frontsector->floorheight
		    || backsector->floorheight >= frontsector->ceilingheight)
		{
			/* closed door */
			markceiling = markfloor = true;
		}


		if (worldhigh < worldtop)
		{
			/* top texture */
			toptexture = texturetranslation[sidedef->toptexture];
			if (linedef->flags & ML_DONTPEGTOP)
			{
				/* top of texture at top */
				rw_toptexturemid = worldtop;
			}
			else
			{
				vtop =
					backsector->ceilingheight
					+ textureheight[sidedef->toptexture];

				/* bottom of texture */
				rw_toptexturemid = vtop - viewz;
			}
		}
		if (worldlow > worldbottom)
		{
			/* bottom texture */
			bottomtexture = texturetranslation[sidedef->bottomtexture];

			if (linedef->flags & ML_DONTPEGBOTTOM )
			{
				/* bottom of texture at bottom */
				/* top of texture at top */
				rw_bottomtexturemid = worldtop;
			}
			else /* top of texture at top */
				rw_bottomtexturemid = worldlow;
		}
		rw_toptexturemid += sidedef->rowoffset;
		rw_bottomtexturemid += sidedef->rowoffset;

		/* allocate space for masked texture tables */
		if (sidedef->midtexture)
		{
			/* masked midtexture */
			maskedtexture = true;
			ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
			lastopening += rw_stopx - rw_x;
		}
	}

	/* calculate rw_offset (only needed for textured lines) */
	segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

	if (segtextured)
	{
		offsetangle = rw_normalangle-rw_angle1;

		if (offsetangle > ANG180)
			offsetangle = -offsetangle;

		if (offsetangle > ANG90)
			offsetangle = ANG90;

		sineval = finesine[offsetangle >>ANGLETOFINESHIFT];
		rw_offset = FixedMul (hyp, sineval);

		if (rw_normalangle-rw_angle1 < ANG180)
			rw_offset = -rw_offset;

		rw_offset += sidedef->textureoffset + curline->offset;
		rw_centerangle = ANG90 + viewangle - rw_normalangle;

		/* calculate light table */
		/*  use different light tables */
		/*  for horizontal / vertical / diagonal */
		/* OPTIMIZE: get rid of LIGHTSEGSHIFT globally */
		if (!fixedcolormap)
		{
			lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

			if (curline->v1->y == curline->v2->y)
				lightnum--;
			else if (curline->v1->x == curline->v2->x)
				lightnum++;

			if (lightnum < 0)
				walllights = scalelight[0];
			else if (lightnum >= LIGHTLEVELS)
				walllights = scalelight[LIGHTLEVELS-1];
			else
				walllights = scalelight[lightnum];
		}
	}

	/* if a floor / ceiling plane is on the wrong side */
	/*  of the view plane, it is definitely invisible */
	/*  and doesn't need to be marked. */


	if (frontsector->floorheight >= viewz)
	{
		/* above view plane */
		markfloor = false;
	}

	if (frontsector->ceilingheight <= viewz
	    && frontsector->ceilingpic != skyflatnum)
	{
		/* below view plane */
		markceiling = false;
	}


	/* calculate incremental stepping values for texture edges */
	worldtop >>= 4;
	worldbottom >>= 4;

	topstep = -FixedMul (rw_scalestep, worldtop);
	topfrac = (centeryfrac>>4) - FixedMul (worldtop, rw_scale);

	bottomstep = -FixedMul (rw_scalestep,worldbottom);
	bottomfrac = (centeryfrac>>4) - FixedMul (worldbottom, rw_scale);

	if (backsector)
	{
		worldhigh >>= 4;
		worldlow >>= 4;

		if (worldhigh < worldtop)
		{
			pixhigh = (centeryfrac>>4) - FixedMul (worldhigh, rw_scale);
			pixhighstep = -FixedMul (rw_scalestep,worldhigh);
		}

		if (worldlow > worldbottom)
		{
			pixlow = (centeryfrac>>4) - FixedMul (worldlow, rw_scale);
			pixlowstep = -FixedMul (rw_scalestep,worldlow);
		}
	}

	/* render it */
	if (markceiling)
		ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);

	if (markfloor)
		floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

#ifdef DOOM_LOG
	{
		uint32_t _swr_t1 = I_GetTimeUs();
		uint32_t _swr_t2;
		g_perf_segs_setup_us += _swr_t1 - _swr_t0;
		R_RenderSegLoop ();
		_swr_t2 = I_GetTimeUs();
		g_perf_segs_draw_us += _swr_t2 - _swr_t1;
	}
#else
	R_RenderSegLoop ();
#endif

	/* save sprite clipping info */
	if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
	     && !ds_p->sprtopclip)
	{
		memcpy (lastopening, ceilingclip+start, 2*(rw_stopx-start));
		ds_p->sprtopclip = lastopening - start;
		lastopening += rw_stopx - start;
	}

	if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)
	     && !ds_p->sprbottomclip)
	{
		memcpy (lastopening, floorclip+start, 2*(rw_stopx-start));
		ds_p->sprbottomclip = lastopening - start;
		lastopening += rw_stopx - start;
	}

	if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
	{
		ds_p->silhouette |= SIL_TOP;
		ds_p->tsilheight = MININT;
	}
	if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
	{
		ds_p->silhouette |= SIL_BOTTOM;
		ds_p->bsilheight = MAXINT;
	}
	ds_p++;
}


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
/*	DOOM graphics stuff for X11, UNIX. */
/* */
/*----------------------------------------------------------------------------- */

static const char
        rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include <stdlib.h>
/*
#include <unistd.h>
#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
*/
/*
 #include <netinet/in.h>
 #include <signal.h>
 */


#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"


#include <iocslib.h>
#include <doslib.h>
#include "vc.h"
#include "crtc.h"
#include "tvram.h"
#include "mfp.h"

#include "serial.h"
#include "doom_log.h"
#include <stdio.h>
#include "w_wad.h"
#include "z_zone.h"
#include "i_prof_timer.h"
char errmsg[1024];
#include <string.h>

/* create an x68000 color from 3 bytes
// TODO: fix this to use F8 as a mask instead of 1F*/
#define X68_GRB(r, g, b, i) ( ( ((b&0xF8)>>2) | (((g)&0xF8)<<8) | (((r)&0xF8)<<3) ) | i )
/*#define X68_GRB(r, g, b) ( (  ((b&0x1F)<<1) | (((g)&0x1F)<<11) | (((r)&0x1F)<<6) ) )*/

/* the original game code just shifts an 8-bit value 2 bits to the right = 6-bit value */
/*gammatable[usegamma][*palette++]&0x1F<<6 | gammatable[usegamma][*palette++]&0x1F<<11 | \
			gammatable[usegamma][*palette++]&0x1F<<1;*/

#define GVRAM_BASE (0xC00000)
#define GVRAM0 (GVRAM_BASE)
#define GVRAM1 (GVRAM_BASE+0x80000)
#define GVRAM2 (GVRAM_BASE+0x100000)
#define GVRAM3 (GVRAM_BASE+0x180000)

short *gvram0 = (short*)GVRAM0;
byte oldTVPal[32];
byte *screens16 = NULL;  /* 16-bit GVRAM-format framebuffer (128KB, HIMEM) */

/* ---- Video mode table ---- */
int video_mode = 2;  /* 0=15kHz, 1=25kHz, 2=31kHz (default) */
#define NUM_VIDEO_MODES 3

typedef struct {
    short r20, r00, r01, r02, r03, r04, r05, r06, r07, r08;
    const char *name;
} video_mode_t;

static const video_mode_t video_modes[NUM_VIDEO_MODES] = {
    /* 0: 15kHz ~61Hz progressive 512x256.
     * Standard 512x256 15kHz mode from Inside X68000 book, unmodified.
     * R20=$0101: 512-dot, 256-color, HF=0 (15.98kHz), VD=00 progressive.
     * Game's 320x200 centered via GVRAM scroll registers.
     * fH=15.98kHz, fV=61.46Hz. */
    { 0x0101, 0x4B, 0x03, 0x05, 0x45,
      0x103, 0x02, 0x10, 0x100, 0x2C, "15 KHZ 61 HZ" },
    /* 1: 25kHz ~55Hz (line-doubled, original Doom mode). */
    { 0x1110, 0x38, 0x07, 0x0A, 0x32,
      0x1D0, 0x07, 0x20, 0x1C0, 0x1B, "25 KHZ 55 HZ" },
    /* 2: 31kHz ~55Hz (VGA-compatible).
     * Standard 512x512 31kHz mode from the book with VD=00 (256 vert).
     * Interlaced: 256 GVRAM rows displayed as 512 raster lines
     * (line-doubled, like the 25kHz mode).
     * R20=$0111: 512-dot, 256-color, HF=1 (31.5kHz), VD=00.
     * Game's 320x200 centered via GVRAM scroll registers. */
    { 0x0111, 0x5B, 0x09, 0x11, 0x51,
      0x237, 0x05, 0x28, 0x228, 0x1B, "31 KHZ 55 HZ" },
};

/* Lookup table: maps Doom palette index -> GVRAM hardware palette index.
 * Multiples of 16 (transparent in GVRAM) are remapped to N+1.
 * Initialised by I_InitPixelRemap16() before R_Init so that colormap16
 * tables can be built during R_InitColormaps. */
unsigned short pixel_remap16[256];

/* Build pixel_remap16 early (before R_Init) so colormap16 tables can
 * be built during R_InitColormaps.  Called from D_DoomMain. */
void I_InitPixelRemap16(void)
{
	int j;
	/* X68000 GVRAM in 256-color mode: pixels with low nibble = 0 are
	 * transparent (show text layer).  Remap those indices to the nearest
	 * neighbor.  Only 4 of 16 boundaries have large color jumps (160,
	 * 192, 208, 224); the rest shift by < 8 RGB units. */
	/* X68000 GVRAM in 256-color mode: pixels with low nibble = 0 are
	 * transparent.  Remap those to the closest palette entry with
	 * the same color.  Uses PLAYPAL for color distance matching. */
	{
		int lump = W_GetNumForName("PLAYPAL");
		const byte *pal = (const byte *)W_CacheLumpNum(lump, PU_CACHE);

		for (j = 0; j < 256; j++) {
			if (j & 0x0F) {
				pixel_remap16[j] = (unsigned short)j;
			} else {
				const byte *c = &pal[j * 3];
				int best_j = (j < 255) ? j + 1 : j - 1;
				int best_dist = 999999;
				int k;
				for (k = 1; k < 256; k++) {
					int dist;
					const byte *ck;
					if ((k & 0x0F) == 0) continue;
					ck = &pal[k * 3];
					dist = (c[0]-ck[0])*(c[0]-ck[0])
					     + (c[1]-ck[1])*(c[1]-ck[1])
					     + (c[2]-ck[2])*(c[2]-ck[2]);
					if (dist < best_dist) {
						best_dist = dist;
						best_j = k;
					}
				}
				pixel_remap16[j] = (unsigned short)best_j;
			}
		}
	}
}


/*dosstuff -newly added */
/*byte* dascreen;*/

char keydown[128];
volatile char extendedkeydown[128];
volatile char nextkeyextended;
void initkeyhandler();
void killkeyhandler();
void keyhandler();
static char oldkeystate[128];
static char oldextendedkeystate[128];

void initkeyhandler()
{
	int i;
	memset(keydown, 0, 128);
	memset(oldkeystate, 0, 128);
	/*for (i=0; i<128; i++) keydown[i]=0;*/
	for (i=0; i<128; i++) extendedkeydown[i]=0;
	for (i=0; i<128; i++) oldkeystate[i]=0;
	for (i=0; i<128; i++) oldextendedkeystate[i]=0;
	nextkeyextended=0;

}

void killkeyhandler()
{
}

void keyhandler()
{
	unsigned char keyhandlercurrkey;



	if (nextkeyextended)
	{
		if ((keyhandlercurrkey&0x80)==0)
			extendedkeydown[keyhandlercurrkey&0x7f]=1;
		else
			extendedkeydown[keyhandlercurrkey&0x7f]=0;
		nextkeyextended=0;
	}
	else
	{
		if ((keyhandlercurrkey&0x80)==0)
			keydown[keyhandlercurrkey&0x7f]=1;
		else
			keydown[keyhandlercurrkey&0x7f]=0;
	}

	if (keyhandlercurrkey==0xe0)
		nextkeyextended=1;



}

/* create a 2d translation table to avoid redefining this one
   X68XlatTable[0xf][0x8] = ASCII_Scan_Code */
/* keyboard tab = - */
unsigned char X68XlatTable[16][8] = { \
{ 0, 1, 2, 3, 4, 5, 6, 7},\
{ 8, 9,10,11,12, 0, 0,14},\
{15,16,17,18,19,20,21,22},\
{23,24,25, 0,26,28,30,31},\
{32,33,34,35,36,37,38,13},\
{ 43,27,44,45,46,47,48,49},\
{50,51,52,53, 0, 57, 0, 0},\
{ 0, 0, 0,88,89,90,91, 0},\
{ 0, 74, 0,71, 72, 73, 78, 81},\
{ 76, 77, 0, 79, 80, 81, 0, 82},\
{ 0, 0, 0, 0, 0,104, 103, 0},\
{ 0, 0, 0, 0, 0, 0, 0, 0},\
{ 0, 0, 0, 92,93,94,95,96},\
{ 97,98,99,100,101, 0, 0, 0},\
{ 102, 0, 0, 0, 0, 0, 0, 0},\
{ 0, 0, 0, 0, 0, 0, 0, 0}};

unsigned char ASCIINames[] =             /* Unshifted ASCII for scan codes */
{
/*	 0   1   2   3   4   5   6   7 */
	 0, 27, '1','2','3','4','5','6', /* 8  */
	'7','8','9','0','-','=', 8,  9 , /* 16 */
	'q','w','e','r','t','y','u','i', /* 24 */
	'o','p','[',']',13,  0,'a', 's', /* 32 */ 
	'd','f','g','h','j','k','l',';', /* 40 */
	 39,'`', 0, 92, 'z','x','c','v', /* 48 */
	'b','n','m',',','.','/', 0, '*', /* 56 */
	 0, ' ', 0,  0,  0,  0,  0,  0 , /* 64 */
	 0,  0,  0,  0,  0,  0,  0, '7', /* 72 */
	'8','9','-','4','5','6','+','1', /* 80 */
	'2','3','0',127, 0,  0,  0,  0,  /* 88 */
	 172,173,174,175, 187,188,189,190,  /* 96 */
	 191,192,193,194,195,196,182,184,  /* 104*/
	 157,  0,  0,  0,  0,  0,  0,  0,  /* 112*/
	 0,  0,  0,  0,  0,  0,  0,  0,  /* 120*/
	 0,  0,  0,  0,  0,  0,  0,  0   /* 128*/
};

/*end of newly added stuff */


void I_ShutdownGraphics(void)
{
	/* Clear GVRAM so game pixels don't bleed through after mode switch. */
	memset((void *)GVRAM0, 0, 512*1024);

	/* Restore text palette before mode switch. */
	memcpy(tpal, &oldTVPal, 32);

	/* Use IOCS CRTMOD to fully reset to standard 768x512 text mode.
	 * This restores all CRTC registers, scroll, layer config, and
	 * video timing in one call -- much more reliable than manually
	 * setting individual registers. Mode 16 = 768x512 16-colour. */
	CRTMOD(16);

	TGUSEMD(1, 1);
	TGUSEMD(0, 1);
	C_CURON();
}



/* */
/* I_StartFrame */
/* */
void I_StartFrame (void)
{
}


void I_GetEvent()
{
	/* Diagnostic: log raw BITSNS values and events posted.
	 * Log once per ~35 calls (~1 s at TICRATE=35) to avoid flooding. */
	static int kbd_log_counter = 0;
	kbd_log_counter++;
	boolean do_log = (kbd_log_counter % 35 == 1);

	/* X68000 mouse via IOCS MS_GETDT ($74).
	 * Return value: bits 31-24 = dx (signed), bits 23-16 = dy (signed),
	 * bits 15-8 = left button (0x00=off, 0xFF=on),
	 * bits 7-0 = right button (0x00=off, 0xFF=on). */
	{
		static int lastbuttons = 0;
		int msdata = MS_GETDT();
		signed char dx = (signed char)((msdata >> 24) & 0xFF);
		signed char dy = (signed char)((msdata >> 16) & 0xFF);
		int buttons = 0;
		event_t mev;

		if (msdata & 0x0000FF00) buttons |= 1;  /* left button -> fire */
		if (msdata & 0x000000FF) buttons |= 2;  /* right button -> strafe */

		if (dx != 0 || dy != 0 || buttons != lastbuttons)
		{
			extern int novert;  /* m_menu.c: 1 = disable mouse Y axis */
			mev.type = ev_mouse;
			mev.data1 = buttons;
			mev.data2 = dx << 2;    /* scale up for reasonable sensitivity */
			mev.data3 = novert ? 0 : -(dy << 2);
			D_PostEvent(&mev);
		}
		lastbuttons = buttons;
	}

	char key;
	unsigned char tempkey[128];	
	unsigned char bitsnsmask=1;
	unsigned char i;
	unsigned char j;
	event_t event;

	memset(&event, 0, sizeof(event));
	memset(tempkey, 0, 128);

	for(i=0;i<16;i++)
	{
		key = BITSNS(i);
		if (do_log && key != 0)
			dlog("BITSNS(%d)=0x%02x", (int)i, (unsigned char)key);
		for(j=0;j<8;j++)
		{
			if(key&bitsnsmask)
				tempkey[X68XlatTable[i][j]] = 1;
			else
				tempkey[X68XlatTable[i][j]] = 0;
			bitsnsmask<<=1;	
		}
		bitsnsmask = 1;
	}
	
	bitsnsmask = 1;
	
	for (i=0; i<128; i++)
	{
		if (ASCIINames[i] == 0)
			continue;  /* unmapped key ? skip to avoid posting garbage events */

		if ((tempkey[i]==1)&&(oldkeystate[i]==0))
		{
			event.type=ev_keydown;
			event.data1=ASCIINames[i];
			dlog("ev_keydown: scan=%d doom_key=%d", (int)i, (int)ASCIINames[i]);
			D_PostEvent(&event);
		}
		else if ((tempkey[i]==0)&&(oldkeystate[i]==1))
		{
			event.type=ev_keyup;
			event.data1=ASCIINames[i];
			dlog("ev_keyup: scan=%d doom_key=%d", (int)i, (int)ASCIINames[i]);
			D_PostEvent(&event);
		}
		
	}
	/* WASD support: post synthetic arrow key events so W/A/S/D work
	 * as forward/strafe-left/backward/strafe-right alongside the
	 * regular 'w','a','s','d' character events.
	 * Skip when entering save game name -- otherwise A/D also inject
	 * comma/period into the text input.
	 * Must run BEFORE memcpy overwrites oldkeystate. */
	{
		extern int saveStringEnter;
		/* WASD: post synthetic movement events.
		 * A/D use ',' / '.' (key_strafeleft / key_straferight)
		 * which strafe directly without needing a modifier.
		 * ST_Responder filters non-alphanumeric keys for cheats. */
		static const struct { int scan; int doom_key; } wasd[] = {
			{ 17, 0xad },  /* W (scan 17) -> KEY_UPARROW (forward) */
			{ 30, ','   },  /* A (scan 30) -> strafe left */
			{ 31, 0xaf },  /* S (scan 31) -> KEY_DOWNARROW (backward) */
			{ 32, '.'   },  /* D (scan 32) -> strafe right */
		};
		int wi;
		if (!saveStringEnter)
		{
			for (wi = 0; wi < 4; wi++)
			{
				int sc = wasd[wi].scan;
				if (tempkey[sc] && !oldkeystate[sc])
				{
					event.type = ev_keydown;
					event.data1 = wasd[wi].doom_key;
					D_PostEvent(&event);
				}
				else if (!tempkey[sc] && oldkeystate[sc])
				{
					event.type = ev_keyup;
					event.data1 = wasd[wi].doom_key;
					D_PostEvent(&event);
				}
			}
		}
	}

	memcpy(oldkeystate,tempkey,128);
}


/* */
/* I_StartTic */
/* */
void I_StartTic()
{
	I_GetEvent();
	/*i dont think i have to do anything else here */

}



/* */
/* I_UpdateNoBlit */
/* */
void I_UpdateNoBlit (void)
{
	
	/* what is this? */
}

/* Per-phase profiling accumulators - written from d_main.c, r_main.c and i_video.c,
 * read and reset here once per second alongside the FPS counter log.
 * All values are in centiseconds (I_GetTimeCs() units, 10 ms per tick). */
int g_perf_render_tics = 0;
int g_perf_blit_tics   = 0;
int g_perf_tryrun_tics = 0;
int g_perf_bsp_tics    = 0;
int g_perf_planes_tics = 0;
int g_perf_masked_tics = 0;

/* Per-second workload counters from doom_perf.c profiling wrappers. */
#include "doom_perf.h"

/* Set to 1 by st_lib.c / st_stuff.c whenever the status bar area of screens[0]
 * is updated.  I_FinishUpdate blits the full 200-row screen when set, then
 * clears the flag.  On frames where the status bar is unchanged only the top
 * 168 rows (the 3-D view) are blitted, saving ~16% of total blit time. */
int st_needs_blit = 1;


static int lowdetail_dirty = 1;  /* set when low-detail odd columns need re-clearing */
/* Delta blit: compare-before-write to skip unchanged GVRAM pixels.
 * Disabled for native GVRAM (bus ratio too low for net benefit).
 * Enable for TS-6BGA or PhantomX where VRAM:RAM ratio is 1:10+. */
#ifdef DELTA_BLIT
static unsigned short *shadow16 = NULL;
static int shadow_dirty = 1;
#endif

/* Copy border pixels from screens[0] into screens16 (always, for
 * persistence and menu overlay) and optionally GVRAM (only when
 * menu is not active, to avoid flicker). */
void I_BlitBorder(void)
{
    extern int viewwindowx, viewwindowy;
    extern int scaledviewwidth, viewheight;
    extern boolean menuactive;
    extern int use_vsync;
    int view_rows = SCREENHEIGHT - 32;
    unsigned short *s16 = (unsigned short *)screens16;
    byte *s0 = screens[0];
    int x, y, pass;
    unsigned short px;
    int write_gvram = !menuactive;
    /* With vsync, write borders to BOTH GVRAM buffers so neither
     * buffer has stale content after a viewport resize. */
    int num_passes = (write_gvram && use_vsync) ? 2 : 1;

    for (pass = 0; pass < num_passes; pass++)
    {
        volatile unsigned short *gv;
        if (!write_gvram)
            gv = NULL;
        else if (pass == 0)
            gv = (volatile unsigned short *)GVRAM_BASE;
        else
            gv = (volatile unsigned short *)(GVRAM_BASE + 256 * 1024);

    /* Top strip */
    for (y = 0; y < viewwindowy; y++)
        for (x = 0; x < SCREENWIDTH; x++) {
            px = pixel_remap16[s0[y * SCREENWIDTH + x]];
            if (pass == 0) s16[y * SCREENWIDTH + x] = px;
            if (gv) gv[y * 512 + x] = px;
        }

    /* Left and right strips */
    for (y = viewwindowy; y < viewwindowy + viewheight; y++) {
        for (x = 0; x < viewwindowx; x++) {
            px = pixel_remap16[s0[y * SCREENWIDTH + x]];
            if (pass == 0) s16[y * SCREENWIDTH + x] = px;
            if (gv) gv[y * 512 + x] = px;
        }
        for (x = viewwindowx + scaledviewwidth; x < SCREENWIDTH; x++) {
            px = pixel_remap16[s0[y * SCREENWIDTH + x]];
            if (pass == 0) s16[y * SCREENWIDTH + x] = px;
            if (gv) gv[y * 512 + x] = px;
        }
    }

    /* Bottom strip */
    for (y = viewwindowy + viewheight; y < view_rows; y++)
        for (x = 0; x < SCREENWIDTH; x++) {
            px = pixel_remap16[s0[y * SCREENWIDTH + x]];
            if (pass == 0) s16[y * SCREENWIDTH + x] = px;
            if (gv) gv[y * 512 + x] = px;
        }

    } /* end pass loop */

    /* In low detail mode, clear odd columns in the viewport area
     * of screens16. Don't write to GVRAM here -- when the menu is
     * active, I_BlitBlock handles GVRAM (writing all columns).
     * GVRAM odd columns get cleared by lowdetail_dirty on the
     * next gameplay frame. */
    {
        extern int detailLevel;
        if (detailLevel == 1) {
            int vy, vx;
            unsigned short *s16p = (unsigned short *)screens16;
            for (vy = viewwindowy; vy < viewwindowy + viewheight; vy++)
                for (vx = viewwindowx + 1;
                     vx < viewwindowx + scaledviewwidth; vx += 2)
                    s16p[vy * SCREENWIDTH + vx] = 0;
            lowdetail_dirty = 1;
        }
    }
#ifdef DELTA_BLIT
    shadow_dirty = 1;  /* border wrote directly to GVRAM */
#endif
}

void I_FinishUpdate(void)
{
	int blit_rows;
	static int fps_shown = 0;

	/* ---- devparm frame-timing dots ---- */
	if (devparm)
	{
		static int lasttic;
		int i;
		int tics = I_GetTimeCs() - lasttic;
		lasttic = I_GetTimeCs();
		if (tics > 20) tics = 20;
		for (i=0; i<tics*2; i+=2)
			screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
		for (; i<20*2; i+=2)
			screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
	}

	/* ---- FPS counter: draws 3x5 pixel digits in top-right corner ---- */
	{
		extern int show_fps;
		static int fps_frames   = 0;
		static int fps_lasttime = -1;
		/* fps_shown is file-scope (see below FPS draw section) */
		static const byte dig[10][5] = {
			{7,5,5,5,7}, /* 0: ### / #.# / #.# / #.# / ### */
			{6,2,2,2,7}, /* 1: ##. / .#. / .#. / .#. / ### */
			{7,1,7,4,7}, /* 2: ### / ..# / ### / #.. / ### */
			{7,1,7,1,7}, /* 3: ### / ..# / ### / ..# / ### */
			{5,5,7,1,1}, /* 4: #.# / #.# / ### / ..# / ..# */
			{7,4,7,1,7}, /* 5: ### / #.. / ### / ..# / ### */
			{7,4,7,5,7}, /* 6: ### / #.. / ### / #.# / ### */
			{7,1,2,2,2}, /* 7: ### / ..# / .#. / .#. / .#. */
			{7,5,7,5,7}, /* 8: ### / #.# / ### / #.# / ### */
			{7,5,7,1,7}, /* 9: ### / #.# / ### / ..# / ### */
		};
		int now = I_GetTimeCs();
		int di, row, col;

		if (fps_lasttime < 0)
			fps_lasttime = now;
		fps_frames++;
		/* Trigger every 100 centiseconds (= 1 second at 100 Hz). */
		if (now - fps_lasttime >= 100)
		{
			int elapsed  = now - fps_lasttime;   /* cs in this window (~100) */
			int n_frames = fps_frames;           /* frame count before reset */
			fps_shown    = n_frames ? n_frames * 100 / elapsed : 0;
			fps_frames   = 0;
			fps_lasttime = now;

			if (n_frames > 0)
			{
				/* ---- Coarse centisecond averages (legacy) ---- */
				int ft_ms    = elapsed                * 10 / n_frames;
				int rend_ms  = g_perf_render_tics     * 10 / n_frames;
				int blit_ms  = g_perf_blit_tics        * 10 / n_frames;
				int logic_ms = g_perf_tryrun_tics      * 10 / n_frames;
				int unac_ms  = ft_ms - rend_ms - blit_ms - logic_ms;
				if (unac_ms < 0) unac_ms = 0;

				/* ---- High-res microsecond sub-phase averages ---- */
				/* us / n_frames -> per-frame us, then /1000 -> ms,
				 * but keep fractional ms via us/n_frames for the log. */
				uint32_t render_us = g_perf_render_us / n_frames;
				uint32_t bsp_us    = g_perf_bsp_us    / n_frames;
				uint32_t seg_s_us  = g_perf_segs_setup_us / n_frames;
				uint32_t seg_d_us  = g_perf_segs_draw_us  / n_frames;
				uint32_t planes_us = g_perf_planes_us / n_frames;
				uint32_t masked_us = g_perf_masked_us / n_frames;
				uint32_t blit_us_  = g_perf_blit_us   / n_frames;
				/* BSP overhead = BSP total - segs setup - segs draw */
				uint32_t bsp_ov_us = bsp_us > (seg_s_us + seg_d_us)
				                   ? bsp_us - seg_s_us - seg_d_us : 0;
				/* render overhead = render total - BSP - planes - masked */
				uint32_t rend_ov_us = render_us > (bsp_us + planes_us + masked_us)
				                    ? render_us - bsp_us - planes_us - masked_us : 0;

				/* Per-second pixel/call totals */
				uint32_t col_c  = g_perf_col_calls;
				uint32_t col_px = g_perf_col_pixels;
				uint32_t sp_c   = g_perf_span_calls;
				uint32_t sp_px  = g_perf_span_pixels;
				/* Per-frame averages */
				uint32_t col_c_pf  = col_c  / n_frames;
				uint32_t col_px_pf = col_px / n_frames;
				uint32_t sp_c_pf   = sp_c   / n_frames;
				uint32_t sp_px_pf  = sp_px  / n_frames;

				dlog("--- PERF 1s window: %d frames ---", n_frames);
				dlog("FPS:%d  ft:%dms  render:%d.%dms  blit:%d.%dms  logic:%dms  other:%dms",
				     fps_shown, ft_ms,
				     (int)(render_us / 1000), (int)(render_us % 1000 / 100),
				     (int)(blit_us_  / 1000), (int)(blit_us_  % 1000 / 100),
				     logic_ms, unac_ms);
				dlog("  BSP: %d.%dms  [segs-setup:%d.%dms  segs-draw:%d.%dms  traverse:%d.%dms]",
				     (int)(bsp_us   / 1000), (int)(bsp_us   % 1000 / 100),
				     (int)(seg_s_us / 1000), (int)(seg_s_us % 1000 / 100),
				     (int)(seg_d_us / 1000), (int)(seg_d_us % 1000 / 100),
				     (int)(bsp_ov_us/ 1000), (int)(bsp_ov_us% 1000 / 100));
				{
					uint32_t vp_pf = g_perf_visplanes / n_frames;
					/* Estimate span pixel-push time: 7.75 c/px @ 50MHz */
					uint32_t span_push_us = sp_px_pf * 155 / 1000;
					uint32_t planes_ov_us = planes_us > span_push_us
					                      ? planes_us - span_push_us : 0;
					dlog("  planes: %d.%dms  [push~%d.%dms  overhead~%d.%dms]  %lu visplanes/frame",
					     (int)(planes_us  / 1000), (int)(planes_us  % 1000 / 100),
					     (int)(span_push_us/1000), (int)(span_push_us%1000 / 100),
					     (int)(planes_ov_us/1000), (int)(planes_ov_us%1000 / 100),
					     (unsigned long)vp_pf);
				}
				dlog("  masked: %d.%dms  rend-overhead: %d.%dms",
				     (int)(masked_us / 1000), (int)(masked_us % 1000 / 100),
				     (int)(rend_ov_us/ 1000), (int)(rend_ov_us% 1000 / 100));
				dlog("  col: %lu calls %lu px/frame  span: %lu calls %lu px/frame",
				     (unsigned long)col_c_pf, (unsigned long)col_px_pf,
				     (unsigned long)sp_c_pf,  (unsigned long)sp_px_pf);
				dlog("  render%%:%d  blit%%:%d  logic%%:%d  other%%:%d",
				     ft_ms ? rend_ms  * 100 / ft_ms : 0,
				     ft_ms ? blit_ms  * 100 / ft_ms : 0,
				     ft_ms ? logic_ms * 100 / ft_ms : 0,
				     ft_ms ? unac_ms  * 100 / ft_ms : 0);
#if 0 /* enable when PROF_COUNTERS equ 1 in r_draw_asm.S */
				{
					extern unsigned long g_delta_changed;
					int total_pairs = (SCREENWIDTH / 2) * (SCREENHEIGHT - 32);
					int pct = total_pairs ? (int)(g_delta_changed * 100 / (unsigned long)total_pairs) : 0;
					dlog("  delta-changed: %d%%", pct);
					g_delta_changed = 0;
				}
#endif
			}
			else
			{
				dlog("FPS:%d (no frames rendered)", fps_shown);
			}

			/* Reset all accumulators */
			g_perf_render_tics = 0;
			g_perf_blit_tics   = 0;
			g_perf_tryrun_tics = 0;
			g_perf_bsp_tics    = 0;
			g_perf_planes_tics = 0;
			g_perf_masked_tics = 0;
			g_perf_span_calls  = 0;
			g_perf_span_pixels = 0;
			g_perf_col_calls   = 0;
			g_perf_col_pixels  = 0;
			g_perf_render_us   = 0;
			g_perf_bsp_us      = 0;
			g_perf_segs_setup_us = 0;
			g_perf_segs_draw_us  = 0;
			g_perf_planes_us   = 0;
			g_perf_masked_us   = 0;
			g_perf_blit_us     = 0;
			g_perf_visplanes   = 0;
		}

		{
			extern boolean menuactive;
		if (fps_shown > 0 && show_fps && !menuactive)
		{
			extern int detailLevel;
			extern int viewwindowx, scaledviewwidth;
			extern int use_vsync;
			int show_v = use_vsync;  /* "V" suffix when vsync on */
			int scale = (detailLevel == 1) ? 2 : 1;
			int val = fps_shown < 999 ? fps_shown : 999;
			int nd  = (val >= 100) ? 3 : (val >= 10 ? 2 : 1);
			int total_chars = nd + (show_v ? 1 : 0);
			int digits[3];
			int px, py;
			unsigned short *s16 = (unsigned short *)screens16;
			int right_edge = viewwindowx + scaledviewwidth;
			if (right_edge > SCREENWIDTH || right_edge <= 0)
				right_edge = SCREENWIDTH;
			digits[0] = val / 100;
			digits[1] = (val / 10) % 10;
			digits[2] = val % 10;
			{
				extern int viewwindowy;
				px = right_edge - total_chars * 4 * scale;
				py = viewwindowy + 1;
			}

			/* black background strip */
			for (row = 0; row < 5 * scale; row++)
				for (col = 0; col < total_chars * 4 * scale; col++)
					s16[(py + row) * SCREENWIDTH + px + col] = pixel_remap16[0];

			/* white digit pixels */
			for (di = 0; di < nd; di++)
			{
				int d  = digits[3 - nd + di];
				int dx = px + di * 4 * scale;
				for (row = 0; row < 5; row++)
					for (col = 0; col < 3; col++)
						if (dig[d][row] & (4 >> col))
						{
							int sx, sy;
							for (sy = 0; sy < scale; sy++)
								for (sx = 0; sx < scale; sx++)
									s16[(py + row*scale + sy) * SCREENWIDTH
									    + dx + col*scale + sx] = pixel_remap16[255];
						}
			}

			/* "V" indicator when vsync is active */
			if (show_v) {
				/* V glyph: 3x5 bitmap */
				static const byte vglyph[5] = {5,5,5,2,2};
				/* #.# / #.# / #.# / .#. / .#. */
				int vx = px + nd * 4 * scale;
				for (row = 0; row < 5; row++)
					for (col = 0; col < 3; col++)
						if (vglyph[row] & (4 >> col))
						{
							int sx, sy;
							for (sy = 0; sy < scale; sy++)
								for (sx = 0; sx < scale; sx++)
									s16[(py + row*scale + sy) * SCREENWIDTH
									    + vx + col*scale + sx] = pixel_remap16[255];
						}
			}
		}
		} /* menuactive scope */
	}

	/* 'H' key dump: write raw screens16 values + PLAYPAL to framedump.txt.
	 * Scan code 35 = 'H'. BITSNS group 4, bit 3 (0x08).
	 * Disabled by default; change #if 0 to #if 1 to re-enable. */
#if 0
	{
		static int dump_cooldown = 0;
		if (dump_cooldown > 0) dump_cooldown--;
		if ((BITSNS(4) & 0x08) && dump_cooldown == 0) {
			FILE *fp = fopen("framedump.txt", "w");
			if (fp) {
				int dy, dx;
				const unsigned short *s16 = (const unsigned short *)screens16;
				{
				int pi;
				int plump = W_GetNumForName("PLAYPAL");
				const byte *pp = (const byte *)W_CacheLumpNum(plump, PU_CACHE);
				fprintf(fp, "PLAYPAL (index: R G B):\n");
				for (pi = 0; pi < 256; pi++)
					fprintf(fp, "%3d: %3d %3d %3d\n",
					        pi, pp[pi*3], pp[pi*3+1], pp[pi*3+2]);
				fprintf(fp, "\n");
			}
			fprintf(fp, "screens16 dump (320x200, values are palette indices)\n");
				for (dy = 0; dy < SCREENHEIGHT; dy++) {
					for (dx = 0; dx < SCREENWIDTH; dx++)
						fprintf(fp, "%3d ", s16[dy * SCREENWIDTH + dx]);
					fprintf(fp, "\n");
				}
				fclose(fp);
				printf("Frame dumped to framedump.txt\n");
			}
			dump_cooldown = 35;
		}
	}
#endif

	/* Only skip status bar rows during gameplay; fullscreen graphics
	 * (title, intermission, finale, order screen) need all 200 rows. */
	{
		extern int wipe_in_progress;
		blit_rows = (gamestate == GS_LEVEL && !st_needs_blit && !wipe_in_progress)
		            ? (SCREENHEIGHT - 32) : SCREENHEIGHT;
	}

	/* Blit to GVRAM. */
	{
#ifdef DOOM_LOG
		int t_blit = I_GetTimeCs();
		uint32_t _us_blit = I_GetTimeUs();
#endif

		{
			/* Double-buffer vsync: blit to back buffer, then flip
			 * Y scroll at vblank so the monitor always shows a
			 * complete frame. Zero overhead when vsync is off.
			 * Only during gameplay -- menus/overlays blit directly. */
			static int vsync_backbuf = 0;
			int vsync_active = 0;
			{
				extern int use_vsync;
				extern boolean menuactive;
				extern boolean automapactive;
				extern gamestate_t gamestate;
				if (use_vsync && gamestate == GS_LEVEL
				    && !menuactive && !automapactive) {
					gvram0 = (short *)(GVRAM_BASE +
					         (vsync_backbuf ? 256 * 1024 : 0));
					vsync_active = 1;
				} else if (use_vsync) {
					/* Leaving vsync gameplay: always reset scroll
					 * to buffer A so menu/overlay blits are visible. */
					int sy = (video_mode == 0 || video_mode == 2)
					         ? 512 - 20 : 0;
					*crtc_r13 = sy;
					*crtc_r15 = sy;
					vsync_backbuf = 0;
				}
			}

			extern void I_BlitCopy16(const unsigned short *src, short *dst,
			                         int rows);
			extern void I_BlitCopy16_LowDetail(const unsigned short *src,
			                         short *dst, int rows);
#ifdef DELTA_BLIT
			extern void I_BlitCopy16_Delta(const unsigned short *src,
			                         unsigned short *shadow, short *dst, int rows);
			extern void I_BlitCopy16_LowDetail_Delta(const unsigned short *src,
			                         unsigned short *shadow, short *dst, int rows);
#endif
			extern void I_BlitBlock(const byte *src, short *dst,
			                        int rows, const unsigned short *remap);
			extern boolean menuactive;
			extern boolean automapactive;
			extern gamestate_t gamestate;
			extern int detailLevel;
			extern int viewwindowx, viewwindowy;
			extern int scaledviewwidth, viewheight;
			int view_rows = SCREENHEIGHT - 32;  /* 168 rows of 3D view */

			{
			static int was_overlay = 1;
			/* lowdetail_dirty is file-scope, set by I_BlitBorder too */
			static int prev_detail = -1;
			static int prev_viewwidth = -1;
			/* Detect detail level or viewport size change */
			if (detailLevel != prev_detail) {
				if (detailLevel == 1)
					lowdetail_dirty = 1;
				prev_detail = detailLevel;
			}
			if (scaledviewwidth != prev_viewwidth) {
				if (detailLevel == 1)
					lowdetail_dirty = 1;
				/* Force full re-blit of borders + status bar
				 * to both vsync buffers on viewport resize. */
				was_overlay = 1;
				/* When expanding to full width in low detail, clear
				 * old border remnants on odd columns. I_BlitBorder
				 * won't run at full width, and low-detail blit only
				 * writes even columns. Clear screens16 + both GVRAM
				 * buffers so it works from menu and with vsync. */
				if (detailLevel == 1
				    && scaledviewwidth == SCREENWIDTH) {
					int y, x;
					int vr = SCREENHEIGHT - 32;
					unsigned short *s16p = (unsigned short *)screens16;
					volatile unsigned short *gvA =
					    (volatile unsigned short *)GVRAM_BASE;
					volatile unsigned short *gvB =
					    (volatile unsigned short *)(GVRAM_BASE + 256*1024);
					for (y = 0; y < vr; y++) {
						for (x = 1; x < SCREENWIDTH; x += 2) {
							s16p[y * SCREENWIDTH + x] = 0;
							gvA[y * 512 + x] = 0;
							gvB[y * 512 + x] = 0;
						}
					}
				}
				prev_viewwidth = scaledviewwidth;
			}

			{
				extern int wipe_in_progress;
			if (gamestate == GS_LEVEL && !menuactive && !automapactive
			    && !wipe_in_progress)
			{
				/* Clamp viewport bounds to prevent crash during
				 * rapid screen size changes from the options menu. */
				if (viewwindowx < 0) viewwindowx = 0;
				if (viewwindowy < 0) viewwindowy = 0;
				if (scaledviewwidth <= 0 || scaledviewwidth > SCREENWIDTH)
					scaledviewwidth = SCREENWIDTH;
				if (viewheight <= 0 || viewheight > SCREENHEIGHT - 32)
					viewheight = SCREENHEIGHT - 32;
				if (viewwindowx + scaledviewwidth > SCREENWIDTH)
					scaledviewwidth = SCREENWIDTH - viewwindowx;
				if (viewwindowy + viewheight > SCREENHEIGHT - 32)
					viewheight = SCREENHEIGHT - 32 - viewwindowy;

				/* In low detail, re-clear odd columns if something
				 * (overlay blit, level load, mode switch) wrote to them. */
				if (detailLevel == 1 && lowdetail_dirty) {
					int y, x;
					unsigned short *s16 = (unsigned short *)screens16;
					for (y = viewwindowy; y < viewwindowy + viewheight; y++) {
						volatile unsigned short *row =
						    (volatile unsigned short *)gvram0 + y * 512;
						for (x = viewwindowx + 1;
						     x < viewwindowx + scaledviewwidth; x += 2) {
							row[x] = 0;
							s16[y * SCREENWIDTH + x] = 0;
						}
					}
					lowdetail_dirty = 0;
				}

				if (scaledviewwidth == SCREENWIDTH) {
					/* Full width: fast ASM blit */
					if (detailLevel == 1)
						I_BlitCopy16_LowDetail(
						    (const unsigned short *)screens16,
						    gvram0, view_rows);
					else
						I_BlitCopy16(
						    (const unsigned short *)screens16,
						    gvram0, view_rows);
				} else {
					/* Bordered view: only blit the viewport rectangle */
					int y;
					int vw_longs = scaledviewwidth / 2;
					for (y = viewwindowy; y < viewwindowy + viewheight; y++) {
						const unsigned short *src =
						    (const unsigned short *)screens16
						    + y * SCREENWIDTH + viewwindowx;
						volatile unsigned short *dst =
						    (volatile unsigned short *)gvram0
						    + y * 512 + viewwindowx;
						if (detailLevel == 1) {
							int x;
							for (x = 0; x < scaledviewwidth; x += 2)
								dst[x] = src[x];
						} else {
							int x;
							volatile unsigned long *d32 =
							    (volatile unsigned long *)dst;
							const unsigned long *s32 =
							    (const unsigned long *)src;
							for (x = 0; x < vw_longs; x++)
								d32[x] = s32[x];
						}
					}
				}

				/* Status bar: blit when changed, or on first frame
				 * after menu/loading. With vsync double-buffer,
				 * blit to both buffers (2 frames) then stop. */
				{
					static int sbar_pending = 0;
					if (st_needs_blit || was_overlay)
						sbar_pending = vsync_active ? 2 : 1;
					if (sbar_pending > 0) {
						I_BlitBlock(screens[0] + view_rows * SCREENWIDTH,
						            gvram0 + view_rows * (1024/2),
						            SCREENHEIGHT - view_rows,
						            pixel_remap16);
						was_overlay = 0;
						sbar_pending--;
					}
				}
			}
			else
			{
				/* Menu/automap/intermission: D_Display composites
				 * all overlays into screens[0] (8-bit).  Blit
				 * directly with palette remap -- no screens16. */
				I_BlitBlock(screens[0], gvram0, blit_rows, pixel_remap16);
				was_overlay = 1;  /* force status bar re-blit on next gameplay frame */
#ifdef DELTA_BLIT
				shadow_dirty = 1; /* overlay wrote to GVRAM outside screens16 */
#endif
				if (detailLevel == 1)
					lowdetail_dirty = 1;  /* full blit overwrote black stripes */
			}
			}
			}

#ifdef DOOM_LOG
			g_perf_blit_tics += I_GetTimeCs() - t_blit;
			g_perf_blit_us += I_GetTimeUs() - _us_blit;
#endif
			/* Double-buffer vsync: wait for vblank, flip scroll. */
			if (vsync_active) {
				{
					int sy_a, sy_b;
					if (video_mode == 0 || video_mode == 2) {
						sy_a = 512 - 20;
						sy_b = 256 - 20;
					} else {
						sy_a = 0;
						sy_b = 256;
					}
					/* Wait for vblank: MFP GPIP bit 6 is active-low
					 * (0 = vblank, 1 = active display) on X68000.
					 * Wait for active display, then wait for vblank. */
					while (!(*mfp_gpip & 0x40)) ;
					while (*mfp_gpip & 0x40) ;
					/* Flip: show the buffer we just wrote */
					{
						int sy = vsync_backbuf ? sy_b : sy_a;
						*crtc_r13 = sy;
						*crtc_r15 = sy;
					}
					vsync_backbuf ^= 1;
					gvram0 = (short *)GVRAM_BASE;
				}
			}

			/* FPS counter drawn via screens16 (before blit on next frame) */
			st_needs_blit = 0;
			return;
		}

	}
}

/* */
/* I_ReadScreen */
/* */
void I_ReadScreen (byte* scr)
{
	/* Do NOT sync screens16 here. The caller is responsible for
	 * ensuring screens[0] has the correct content before calling.
	 * See D_Display where wipe_StartScreen syncs screens16->screens[0]
	 * before capturing, and wipe_EndScreen skips it because the new
	 * screen was drawn to screens[0] directly. */
	{
		if (0)  /* sync disabled -- done by caller */
		{
			unsigned short *s16 = (unsigned short *)screens16;
			byte *s8 = screens[0];
			int n = SCREENWIDTH * (SCREENHEIGHT - 32);
			int k;
			for (k = 0; k < n; k++)
				s8[k] = (byte)s16[k];
		}
	}
	memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}


void I_SetPalette (byte* palette)
{
	int i;

	for (i = 0; i < 256; i++, palette += 3)
	{
		short color = X68_GRB(gammatable[usegamma][palette[0]],
		                      gammatable[usegamma][palette[1]],
		                      gammatable[usegamma][palette[2]], 0);
		gpal[i] = color;

		/* Every 16th palette entry: also set the text palette.
		 * GVRAM pixels with low nibble = 0 are transparent and
		 * show the text layer through. */
		if ((i & 0x0F) == 0)
			tpal[(i >> 4) & 0x0F] = color;
	}
}


void I_ApplyVideoMode(void)
{
	const video_mode_t *m;
	if (video_mode < 0 || video_mode >= NUM_VIDEO_MODES)
		video_mode = 1;  /* default to 25kHz if invalid */
	m = &video_modes[video_mode];

	{
		*crtc_r20 = m->r20;
		*crtc_r00 = m->r00;
		*crtc_r01 = m->r01;
		*crtc_r02 = m->r02;
		*crtc_r03 = m->r03;
		*crtc_r04 = m->r04;
		*crtc_r05 = m->r05;
		*crtc_r06 = m->r06;
		*crtc_r07 = m->r07;
		*crtc_r08 = m->r08;
		*vidcon_r0 = 0x1;
	}

	*vidcon_r1 = 0x24E4; /* GVRAM layers priority */
	*vidcon_r2 = 1;      /* GVRAM-only display */

	/* Set scrolls. In 15kHz mode (512px active, 320px game),
	 * scroll GVRAM left by 96 pixels to center the game image.
	 * Scroll value wraps within 512px GVRAM, so 512-96 = 416. */
	*crtc_r10 = 0; *crtc_r11 = 0;  /* text X/Y scroll */
	if (video_mode == 0 || video_mode == 2) {
		/* 15kHz/31kHz: center 320x200 in 512-dot active area.
		 * In 256-color mode, both page 0+1 scroll regs must match. */
		int sx = 512 - 96;   /* (512-320)/2 = 96 */
		int sy = 512 - 20;   /* shift up 20px, wraps at 512 */
		*crtc_r12 = sx; *crtc_r13 = sy;
		*crtc_r14 = sx; *crtc_r15 = sy;
	} else {
		/* 25kHz: no scroll needed (320px fills active area) */
		*crtc_r12 = 0; *crtc_r13 = 0;
		*crtc_r14 = 0; *crtc_r15 = 0;
	}
	*crtc_r16 = 0; *crtc_r17 = 0;
	*crtc_r18 = 0; *crtc_r19 = 0;

	/* Clear GVRAM to avoid artifacts */
	memset((void *)GVRAM_BASE, 0x00, 0x100000);

	/* Force status bar and border redraw */
	{
		extern int st_needs_blit;
		st_needs_blit = 1;
	}

	printf("Video mode: %s\n", video_modes[video_mode].name);
}

void I_InitGraphics(void)
{
	static int firsttime=1;


	if(!firsttime)
		return;
	firsttime=0;


	/* Apply CRTC registers for selected video mode (15kHz or 25kHz).
	 * pixel_remap16 already built by I_InitPixelRemap16() called earlier. */
	I_ApplyVideoMode();

	dlog("VID: vidcon_r1 write");
	/* text, graphics, bg/sprite,
	// graphics layers priority, 0, 1, 2, 3*/
	*vidcon_r1 = 0x24E4;
	dlog("VID: vidcon_r2 write");
	/* GVRAM only ? text layer no longer needed now that transparent palette
	 * entries (multiples of 16) are remapped to N+1 in the blit. */
	*vidcon_r2 = 1; /* 0x01: GVRAM layer only, no text overlay */

	/* 8x8 31khz BGn*/
	/**cynthia_res = 0x10;*/

	dlog("VID: TGUSEMD");
	/* disable soft mouse stuff (right click)*/
	TGUSEMD(1, 2);
	TGUSEMD(0, 2);
	dlog("VID: C_CUROFF");
	C_CUROFF();

	dlog("VID: memcpy oldTVPal");
	memcpy(&oldTVPal, tpal, 32);		/* backup old text palette*/
	dlog("VID: memset tvram0");
	memset(tvram0, 0, 512*1024);
	dlog("VID: memset gvram0");
	memset((void *)GVRAM_BASE, 0x00, 0x100000);   /* clear all GVRAM */
	dlog("VID: memset gvram0 done");
	/* gvram0 at GVRAM_BASE: game columns 0-319 align with the H-active area
	 * (R02=10 to R03=50 = 40 H-units).  No horizontal offset needed. */
	gvram0 = (short *)GVRAM_BASE;
	/*memset(tvram0, 0, 512*1024);		/* clear all tvram*/

	/*memset(gpal, 0x0, 256*2);				/* clear all palettes
	memset(gpal, 0xC9, 2);
	memset(gpal+1, 0xF0, 2);
	memset(gpal+2, 0x1, 2);
	memset(tpal, 0, 256*2);*/

	/* 0 is transparent color for tvram. maybe for pcg also.
	// white seems to be transparent color for gvram*/

	/* Allocate screens[0] (8-bit, for status bar / menus / non-3D content)
	 * and screens16 (16-bit GVRAM-format, for 3D renderer output). */
	screens[0] = (char *)I_HimemMalloc(SCREENWIDTH * SCREENHEIGHT);
	if (screens[0])
	{
		memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
		dlog("VID: screens[0]=%p (HIMEM)", (void*)screens[0]);
	}
	else
	{
		screens[0] = (char *)calloc(SCREENWIDTH * SCREENHEIGHT, 1);
		I_RegisterAlloc(screens[0]);
		dlog("VID: screens[0]=%p (system RAM fallback)", (void*)screens[0]);
	}

	/* screens16: 16-bit GVRAM-format framebuffer for the 3D renderer.
	 * 320x200x2 = 128KB.  The renderer writes pre-remapped GVRAM words
	 * directly here; the blit is a pure MOVE.L copy to GVRAM. */
	screens16 = (byte *)I_HimemMalloc(SCREENWIDTH * SCREENHEIGHT * 2);
	if (screens16)
	{
		memset(screens16, 0, SCREENWIDTH * SCREENHEIGHT * 2);
		dlog("VID: screens16=%p (HIMEM, 128KB)", (void*)screens16);
	}
	else
	{
		screens16 = (byte *)calloc(SCREENWIDTH * SCREENHEIGHT * 2, 1);
		I_RegisterAlloc(screens16);
		dlog("VID: screens16=%p (system RAM fallback)", (void*)screens16);
	}

#ifdef DELTA_BLIT
	/* Shadow buffer for delta blit: mirrors GVRAM contents in local RAM.
	 * Init to 0xFF to force full blit on first frame (no pixel == 0xFFFF). */
	shadow16 = (unsigned short *)I_HimemMalloc(SCREENWIDTH * SCREENHEIGHT * 2);
	if (shadow16) {
		memset(shadow16, 0xFF, SCREENWIDTH * SCREENHEIGHT * 2);
		dlog("VID: shadow16=%p (HIMEM, 128KB)", (void*)shadow16);
	} else {
		shadow16 = (unsigned short *)calloc(SCREENWIDTH * SCREENHEIGHT, 2);
		I_RegisterAlloc(shadow16);
		if (shadow16) memset(shadow16, 0xFF, SCREENWIDTH * SCREENHEIGHT * 2);
		dlog("VID: shadow16=%p (system RAM fallback)", (void*)shadow16);
	}
#endif

	/*init keyboard */
	SER_debug("Video inited\n", SER_DEBUG_INFO);
	dlog("VID: initkeyhandler");
	initkeyhandler();
	dlog("VID: done");
	
}



int inited;


void grWaitVSync()
{
#ifdef human68k
	while( !( *mfp_gpip & 0x40 ) );
	while( *mfp_gpip & 0x40 );
	
#endif
}


void grEraseGraphicScreen()
{
#ifdef human68k
	short	iOld21;

	/* ?????l????	*/
	/*iOld21 = CRTC_REG(21);*/

	/* CRTC??@?\???g?p????N???A	*/
	grWaitVSync();
	/*CRTC_REG(21) = 0x0F;
	*CRTC_MODE = 0x02;*/
	grWaitVSync();

	/* ??????????e????	*/
	/*CRTC_REG(21) = iOld21;*/

/*text mode?
*VCR2 |= 0x0020;
*VCR2 &= 0xFFDF;*/
#endif
}
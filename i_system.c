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
/* */
/*----------------------------------------------------------------------------- */

static const char
        rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#ifndef human68k
#include <sys/time.h>
#include <unistd.h>
#endif


#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

#include "mfp.h"
#include "serial.h"
#include "doom_log.h"
#include "i_prof_timer.h"
#include <iocslib.h>
#include <doslib.h>
#include "midi_out.h"
char errmsg[1024];

int mb_used = 6;


/* Track HIMEM allocations for cleanup on exit.
 * Human68k does not reclaim HIMEM on process exit. */
#define MAX_HIMEM_ALLOCS 16
static void *himem_allocs[MAX_HIMEM_ALLOCS];
static int   himem_alloc_count = 0;

/* Track DOS MALLOC (m_MALLOC) allocations. */
static void *zone_dos_ptr = NULL;

/* Track C malloc/calloc allocations for explicit cleanup.
 * We don't know if Human68k reclaims these on exit. */
#define MAX_HEAP_ALLOCS 32
static void *heap_allocs[MAX_HEAP_ALLOCS];
static int   heap_alloc_count = 0;

void I_RegisterAlloc(void *p)
{
	if (p && heap_alloc_count < MAX_HEAP_ALLOCS)
		heap_allocs[heap_alloc_count++] = p;
}

#ifdef human68k

/* IOCS _SYS_STAT ($AC) sub-command $4002:
 * Returns 0 if the logical address is in 060turbo local RAM, -1 otherwise.
 * Also returns -1 when the 060turbo driver is not resident. */
static int sys_stat_is_local(void *addr)
{
	register long   d0 __asm__("d0") = 0x00AC;
	register long   d1 __asm__("d1") = 0x4002;
	register void  *a1 __asm__("a1") = addr;
	__asm__ volatile (
		"trap   #15"
		: "+d"(d0), "+d"(d1), "+a"(a1)
		:
		: "a0", "cc"
	);
	return (int)d0;
}

/* IOCS _HIMEM ($F8) command 1 (HIMEM_MALLOC):
 * Allocate directly from 060turbo local RAM, bypassing Human68k's malloc pool.
 * Works regardless of whether -xm is set in CONFIG.SYS.
 * Returns a pointer on success, NULL on failure or if the driver is absent. */
static void *himem_malloc(long size)
{
	register long   d0 __asm__("d0") = 0x00F8;
	register long   d1 __asm__("d1") = 1;
	register long   d2 __asm__("d2") = size;
	register void  *a1 __asm__("a1");
	__asm__ volatile (
		"trap   #15"
		: "+d"(d0), "+d"(d1), "+d"(d2), "=a"(a1)
		:
		: "a0", "cc"
	);
	return (d0 == 0) ? a1 : NULL;
}

#endif /* human68k */

/* Allocate from 060turbo local RAM on human68k; falls back to malloc elsewhere.
 * Use for hot renderer tables to avoid system-bus latency on every lookup. */
void *I_HimemMalloc(int size)
{
#if defined(human68k) && defined(TARGET_68060)
	void *p = himem_malloc((long)size);
	if (p && himem_alloc_count < MAX_HIMEM_ALLOCS)
		himem_allocs[himem_alloc_count++] = p;
	return p;
#else
	return NULL;  /* fall back to malloc in caller */
#endif
}


void
I_Tactile
        ( int on,
        int off,
        int total )
{
	/* UNUSED. */
	on = off = total = 0;
}

ticcmd_t emptycmd;
ticcmd_t*       I_BaseTiccmd(void)
{
	return &emptycmd;
}


int  I_GetHeapSize (void)
{
	return mb_used*1024*1024;
}

byte* I_ZoneBase (int*  size)
{
	*size = mb_used*1024*1024;
	dlog("I_ZoneBase: requesting %d bytes (%d MB)", *size, mb_used);

#ifdef human68k
	/* Prefer IOCS _HIMEM to guarantee the zone is in 060turbo local RAM.
	 * Goes through I_HimemMalloc so the allocation is tracked for cleanup. */
	byte *mem = (byte *)I_HimemMalloc(*size);
	if (mem != NULL)
	{
		int is_local = sys_stat_is_local(mem);
		dlog("I_ZoneBase: HIMEM_MALLOC -> 0x%08lx (%s)",
		     (unsigned long)mem,
		     is_local == 0 ? "LOCAL RAM ok" : "SYSTEM RAM -- unexpected");
		return mem;
	}

	dlog("I_ZoneBase: HIMEM_MALLOC failed, falling back to malloc");
	mem = (byte *)m_MALLOC(*size);
	if (mem != NULL)
	{
		int is_local = sys_stat_is_local(mem);
		dlog("I_ZoneBase: malloc -> 0x%08lx (%s)",
		     (unsigned long)mem,
		     is_local == 0 ? "LOCAL RAM ok" : "SYSTEM RAM -- heap will be slow");
		zone_dos_ptr = mem;
	}
	else
	{
		dlog("I_ZoneBase: malloc returned NULL");
	}
	return mem;
#else
	byte *mem = (byte *)m_MALLOC(*size);
	dlog("I_ZoneBase: malloc returned %s", mem ? "OK" : "NULL");
	zone_dos_ptr = mem;
	return mem;
#endif
}


/* */
/* I_GetTime */
/* returns time in 1/70th second tics */

/*from toplev.c  / GCC 1.42*/
/*#ifdef __human68k__
  return ONTIME ()* 10000;
#endif
#ifdef USG
  times (&tms);
  return (tms.tms_utime + tms.tms_stime) * (1000000 / HZ);
#endif
*/

/* */
int  I_GetTime (void)
{
	/* ONTIME() returns a counter in centiseconds (100 Hz, 10 ms per tick).
	 * Use -1 as the uninitialised sentinel: using 0 fails when ONTIME()
	 * itself returns 0 at boot, which would reset basetime every frame and
	 * make the game think hundreds of tics need to run each loop iteration. */
	static int basetime = -1;
	int thistime = ONTIME();
	if (basetime < 0)
		basetime = thistime;
	return (thistime - basetime) * TICRATE / 100;
}



/* */
/* I_Init */
int I_GetTimeCs (void)
{
	return ONTIME();
}


/* */
void I_Init (void)
{
#ifdef human68k
#ifdef TARGET_68060
	{
		extern void I_SetFPCRFloor(void);
		I_SetFPCRFloor();
	}
#endif
#endif
	I_InitSound();
	I_InitMusic();
	/*  I_InitGraphics(); */
}

/* Log whether the hot renderer lookup tables landed in 060turbo local RAM
 * or system RAM.  Call after R_Init() + R_InitFastTables() so all tables
 * are allocated and linked. */
void I_LogTableLocations (void)
{
#ifdef human68k
	extern fixed_t   finesine[];
	extern fixed_t  *finecosine;
	extern fixed_t  *finesine_fast;
	extern fixed_t  *finetangent_fast;
	extern angle_t  *tantoangle_fast;
	extern fixed_t  *yslope;
	extern fixed_t  *distscale;
	extern angle_t  *xtoviewangle;
	extern int      *viewangletox;
	extern void     *colormaps; /* lighttable_t*, opaque here */

#define CHKLOC(name, ptr) \
	dlog("%-18s @ 0x%08lx  %s", (name), (unsigned long)(ptr), \
	     sys_stat_is_local((void *)(ptr)) == 0 ? "LOCAL RAM" : "SYSTEM RAM")

	CHKLOC("finesine(orig)",   finesine);
	CHKLOC("finesine_fast",    finesine_fast);
	CHKLOC("finecosine",       finecosine);
	CHKLOC("finetangent_fast", finetangent_fast);
	CHKLOC("tantoangle_fast",  tantoangle_fast);
	CHKLOC("yslope",           yslope);
	CHKLOC("distscale",        distscale);
	CHKLOC("xtoviewangle",     xtoviewangle);
	CHKLOC("viewangletox",     viewangletox);
	CHKLOC("colormaps",        colormaps);

#undef CHKLOC
#endif
}

/* */
/* I_Quit */
/* */
void I_Quit (void)
{
	D_QuitNetGame ();
	I_ShutdownSound();
	I_ShutdownMusic();
	M_SaveDefaults ();
	I_ShutdownGraphics();
	I_ProfTimerShutdown();
	MIDI_AllNotesOff();  /* final safety kill after all shutdown */

	/* Free all HIMEM allocations so Human68k reclaims them.
	 * Without this, repeated runs leak memory (HIMEM is not
	 * reclaimed on process exit). */
#if defined(human68k) && defined(TARGET_68060)
	{
		int hi;
		for (hi = 0; hi < himem_alloc_count; hi++) {
			if (himem_allocs[hi]) {
				register long d0 __asm__("d0") = 0x00F8;
				register long d1 __asm__("d1") = 2;  /* HIMEM free */
				register long d2 __asm__("d2") = (long)himem_allocs[hi];
				__asm__ volatile("trap #15"
					: "+d"(d0), "+d"(d1), "+d"(d2)
					: : "a0", "a1", "cc");
			}
		}
		himem_alloc_count = 0;
	}
#endif

	/* Free DOS-allocated zone memory (m_MALLOC).
	 * Human68k may not reclaim DOS MALLOC blocks on exit(). */
#ifdef human68k
	if (zone_dos_ptr) {
		MFREE(zone_dos_ptr);
		zone_dos_ptr = NULL;
	}
#endif

	/* Free all tracked C heap allocations. */
	{
		int hi;
		for (hi = 0; hi < heap_alloc_count; hi++) {
			if (heap_allocs[hi])
				free(heap_allocs[hi]);
		}
		heap_alloc_count = 0;
	}

	KFLUSHIO(0x06);
	exit(0);
}

void I_WaitVBL(int count)
{
/*
 #ifdef SGI
    sginap(1);
 #else
 #ifdef SUN
    sleep(0);
 #else
    usleep (count * (1000000/70) );
 #endif
 #endif
   while ((inportb(0x3da)&8)!=8);
   while ((inportb(0x3da)&8)==8);
 */
 
	/*while( *mfp_gpip & 0x40 );
	while( !( *mfp_gpip & 0x40 ) );
 */
 }

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*   I_AllocLow(int length)
{
	byte*       mem;

	mem = (byte *)m_MALLOC (length);
	memset (mem,0,length);
	return mem;

}


/* */
/* I_Error */
/* */
extern boolean demorecording;

void I_Error (char *error, ...)
{
	va_list argptr;
	char errbuf[512];

	/* Capture error message first. */
	va_start(argptr, error);
	vsprintf(errbuf, error, argptr);
	va_end(argptr);

	/* Log to debug file before anything else. */
	dlog("I_Error: %s", errbuf);

	/* Write to ERROR.TXT using raw I/O (no stdio, no malloc).
	 * Guarantees disk write even if the heap is corrupted. */
	{
		int fd = open("ERROR.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd >= 0) {
			write(fd, "I_Error: ", 9);
			write(fd, errbuf, strlen(errbuf));
			write(fd, "\n", 1);
			close(fd);
		}
	}

	/* Log the error message to the debug file before anything else. */
	{
	}
	dlog_close();

	/* Message first. */
	va_start (argptr,error);
	fprintf (stderr, "Error: ");
	vfprintf (stderr,error,argptr);
	fprintf (stderr, "\n");
	va_end (argptr);

	fflush( stderr );

	/* Shutdown. Here might be other errors. */
	if (demorecording)
		G_CheckDemoStatus();

	D_QuitNetGame ();
	I_ShutdownSound();
	I_ShutdownMusic();
	I_ShutdownGraphics();
	I_ProfTimerShutdown();
	MIDI_AllNotesOff();
	KFLUSHIO(0x06);

	/* Print error AFTER graphics shutdown so it's visible in text mode. */
	printf("Error: %s\n", errbuf);

	exit(-1);
}

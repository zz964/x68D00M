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
/*	Main program, simply calls D_DoomMain high level loop. */
/* */
/*----------------------------------------------------------------------------- */

static const char
        rcsid[] = "$Id: i_main.c,v 1.4 1997/02/03 22:45:10 b1 Exp $";



#include "doomdef.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <doslib.h>
#include <iocslib.h>
#include "m_argv.h"
#include "d_main.h"
#include "doom_log.h"

#ifdef human68k
/* Check CPU type via the Human68k system byte at $CBC.
 * 060turbo.sys sets this to 6 for 68060.
 * Known values: 0=68000, 3=68030, 6=68060. */
static int check_cpu_type(void)
{
    return *(volatile unsigned char *)0x0CBC;
}
#endif

/* Disable GCC's defer-pop optimisation around B_SUPER calls.
   Modern GCC's stack bulk-correction can corrupt the stack pointer
   when switching supervisor/user mode (see xdev68k example/b_super). */
#pragma GCC push_options
#pragma GCC optimize("-fno-defer-pop")

int main(int argc, char** argv)
{
#ifdef human68k
	intptr_t b_ssp;
#endif

	dlog_open();
	dlog("main() entered, argc=%d", argc);

	myargc = argc;
	myargv = argv;

#ifdef human68k
	dlog("calling allmem()");
	allmem();
	dlog("allmem() done");

	dlog("calling B_SUPER(0)");
	b_ssp = B_SUPER(0);
	dlog("B_SUPER(0) returned b_ssp=%d", (int)b_ssp);

#ifdef TARGET_68060
	/* Reject known-incompatible CPUs (68000-68040).
	 * 68040 FPU is not compatible with 68060 FPU instructions.
	 * Allow 68060+ and any unknown/future CPU types through. */
	if (!M_CheckParm("-force"))
	{
		int mpu = check_cpu_type();
		if (mpu <= 4)
		{
			static const char *names[] = {
				"68000","68010","68020","68030","68040"
			};
			const char *name = (mpu >= 0 && mpu <= 4)
				? names[mpu] : "unknown";
			B_SUPER(b_ssp);
			printf("ERROR: doom060.x requires a 68060 or newer.\n");
			printf("Detected: %s\n", name);
			printf("Please use doom.x (68030 build) instead.\n");
			printf("To override this check: doom060.x -force\n");
			return 1;
		}
	}
#elif defined(TARGET_68030)
	if (!M_CheckParm("-force"))
	{
		int mpu = check_cpu_type();
		/* Reject 68000/010/020 -- too slow, missing 030 instructions */
		if (mpu >= 0 && mpu <= 2)
		{
			static const char *names[] = {"68000","68010","68020"};
			B_SUPER(b_ssp);
			printf("ERROR: Doom requires a 68030 or newer CPU.\n");
			printf("Detected: %s\n", names[mpu]);
			/* Use DOS _EXIT2 directly to avoid C runtime cleanup
			 * which may contain 68030 instructions. */
			EXIT2(1);
		}
		/* Nudge 68060 users toward doom060.x for better performance.
		 * 68040 is fine on the 030 build (can't run 060 build anyway). */
		if (mpu > 4)
		{
			B_SUPER(b_ssp);
			printf("NOTE: You have a 68060 CPU. doom060.x will run faster.\n");
			printf("This is the 68030 build (doom.x) which does not use the FPU.\n");
			printf("To run anyway: doom.x -force\n");
			return 1;
		}
	}
#endif
#endif

	dlog("calling D_DoomMain()");
	D_DoomMain();
	dlog("D_DoomMain() returned (unexpected)");

#ifdef human68k
	dlog("restoring user mode via B_SUPER(%d)", (int)b_ssp);
	B_SUPER(b_ssp);
#endif
	dlog_close();
	return 0;
}

#pragma GCC pop_options

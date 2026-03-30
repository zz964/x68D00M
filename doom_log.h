#ifndef DOOM_LOG_H
#define DOOM_LOG_H

/* Define DOOM_LOG (e.g. -DDOOM_LOG in makefile.x68k) to enable file logging.
 * When not defined all dlog calls compile away to nothing. */
#ifdef DOOM_LOG
#include <stdio.h>
#include <stdarg.h>
void dlog_open(void);
void dlog(const char *fmt, ...);
void dlog_close(void);
#else
#define dlog_open()         ((void)0)
#define dlog(fmt, ...)      ((void)0)
#define dlog_close()        ((void)0)
#endif

#endif

#include "doom_log.h"

#ifdef DOOM_LOG
#include <stdio.h>
#include <stdarg.h>

static FILE *log_fp = NULL;

void dlog_open(void)
{
    log_fp = fopen("doom_log.txt", "w");
    if (log_fp)
    {
        fprintf(log_fp, "=== doom_log opened ===\n");
        fflush(log_fp);
    }
}

void dlog(const char *fmt, ...)
{
    va_list args;
    if (!log_fp)
        return;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fputc('\n', log_fp);
    fflush(log_fp);
}

void dlog_close(void)
{
    if (log_fp)
    {
        fprintf(log_fp, "=== doom_log closed ===\n");
        fclose(log_fp);
        log_fp = NULL;
    }
}

#endif /* DOOM_LOG */

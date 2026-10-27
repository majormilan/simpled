#ifndef UTIL_H
#define UTIL_H

#include <stdarg.h>

void die(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_err(const char *fmt, ...);

#endif

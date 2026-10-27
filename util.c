/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

char *argv0;
char *root = "";

/* A trailing ':' in fmt appends the error of the last call. */
static void
vwarn(const char *fmt, va_list ap)
{
	int e = errno;

	fprintf(stderr, "%s: ", argv0);
	vfprintf(stderr, fmt, ap);
	if (fmt[0] && fmt[strlen(fmt) - 1] == ':')
		fprintf(stderr, " %s", strerror(e));
	fputc('\n', stderr);
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn(fmt, ap);
	va_end(ap);
}

/* PID 1 must not exit: the kernel would panic. It freezes instead. */
void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn(fmt, ap);
	va_end(ap);
	if (getpid() == 1)
		for (;;)
			pause();
	exit(1);
}

void *
ecalloc(size_t n, size_t size)
{
	void *p;

	if (!(p = calloc(n, size)))
		die("calloc:");
	return p;
}

void *
erealloc(void *p, size_t size)
{
	if (!(p = realloc(p, size)))
		die("realloc:");
	return p;
}

char *
estrdup(const char *s)
{
	char *p;

	if (!(p = strdup(s)))
		die("strdup:");
	return p;
}

void
ladd(List *l, const char *s)
{
	l->v = erealloc(l->v, (l->n + 2) * sizeof(*l->v));
	l->v[l->n++] = s ? estrdup(s) : NULL;
	l->v[l->n] = NULL;
}

int
lhas(const List *l, const char *s)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		if (!strcmp(l->v[i], s))
			return 1;
	return 0;
}

void
lfree(List *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = 0;
}

/* Splits s into words as systemd does, without a shell: blanks separate,
 * single and double quotes group, a backslash takes the next character. */
int
splitwords(const char *s, List *l)
{
	Buf w = { 0 };
	char q;

	for (;;) {
		while (isspace((unsigned char)*s))
			s++;
		if (!*s)
			break;
		bufreset(&w);
		bufadd(&w, "", 0);
		for (q = 0; *s && (q || !isspace((unsigned char)*s)); s++) {
			if (*s == '\\' && s[1])
				bufadd(&w, ++s, 1);
			else if (q && *s == q)
				q = 0;
			else if (!q && (*s == '"' || *s == '\''))
				q = *s;
			else
				bufadd(&w, s, 1);
		}
		if (q) {
			free(w.s);
			return -1;
		}
		ladd(l, w.s);
	}
	free(w.s);
	return 0;
}

void
bufadd(Buf *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->s = erealloc(b->s, b->cap);
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = '\0';
}

void
bufprintf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	bufadd(b, "", 0);
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->s = erealloc(b->s, b->cap);
	}
	va_start(ap, fmt);
	vsnprintf(b->s + b->n, n + 1, fmt, ap);
	va_end(ap);
	b->n += n;
}

void
bufreset(Buf *b)
{
	b->n = 0;
	if (b->s)
		b->s[0] = '\0';
}

/* path under the root given with -r ("" normally) */
char *
rpath(char *buf, size_t size, const char *path)
{
	snprintf(buf, size, "%s%s", root, path);
	return buf;
}

/* monotonic milliseconds */
long long
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

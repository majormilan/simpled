/* See LICENSE file for copyright and license details. */
#include <stddef.h>

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* A NULL-terminated vector of strings it owns */
typedef struct {
	char **v;
	size_t n;
} List;

/* A growing string, always NUL-terminated once anything is in it */
typedef struct {
	char *s;
	size_t n, cap;
} Buf;

extern char *argv0;
extern char *root;

void die(const char *, ...);
void warn(const char *, ...);
void *ecalloc(size_t, size_t);
void *erealloc(void *, size_t);
char *estrdup(const char *);

void ladd(List *, const char *);
int lhas(const List *, const char *);
void lfree(List *);
int splitwords(const char *, List *);

void bufadd(Buf *, const char *, size_t);
void bufprintf(Buf *, const char *, ...);
void bufreset(Buf *);

char *rpath(char *, size_t, const char *);
long long now(void);

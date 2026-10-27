#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "util.h"

static char *trim(char *s) {
    char *ptr;
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    ptr = s + strlen(s) - 1;
    while (ptr > s && isspace((unsigned char)*ptr)) ptr--;
    ptr[1] = '\0';
    return s;
}

Service *parse_service_file(const char *path, const char *name) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    Service *s = calloc(1, sizeof(Service));
    if (!s) die("calloc:");
    s->name = strdup(name);
    s->state = STATE_STOPPED;
    s->type = TYPE_SIMPLE;
    s->restart = RESTART_NO;

    char line[1024];
    char *section = NULL;

    while (fgets(line, sizeof(line), fp)) {
        char *l = trim(line);
        if (*l == '\0' || *l == '#' || *l == ';') continue;

        if (*l == '[' && l[strlen(l) - 1] == ']') {
            l[strlen(l) - 1] = '\0';
            section = strdup(l + 1);
            continue;
        }

        char *key = l;
        char *val = strchr(l, '=');
        if (!val) continue;
        *val++ = '\0';
        key = trim(key);
        val = trim(val);

        if (section && strcmp(section, "Unit") == 0) {
            if (strcmp(key, "Description") == 0) s->description = strdup(val);
            else if (strcmp(key, "Requires") == 0) s->requires = strdup(val);
        } else if (section && strcmp(section, "Service") == 0) {
            if (strcmp(key, "ExecStart") == 0) s->exec_start = strdup(val);
            else if (strcmp(key, "User") == 0) s->user = strdup(val);
            else if (strcmp(key, "Group") == 0) s->group = strdup(val);
            else if (strcmp(key, "Restart") == 0) {
                if (strcmp(val, "always") == 0) s->restart = RESTART_ALWAYS;
            } else if (strcmp(key, "Type") == 0) {
                if (strcmp(val, "oneshot") == 0) s->type = TYPE_ONESHOT;
            } else if (strcmp(key, "Environment") == 0) {
                s->env = realloc(s->env, sizeof(char *) * (s->env_count + 1));
                s->env[s->env_count++] = strdup(val);
            }
        }
    }

    if (section) free(section);
    fclose(fp);
    return s;
}

void free_service(Service *s) {
    if (!s) return;
    free(s->name);
    free(s->description);
    free(s->requires);
    free(s->exec_start);
    free(s->user);
    free(s->group);
    for (size_t i = 0; i < s->env_count; i++) free(s->env[i]);
    free(s->env);
    free(s);
}

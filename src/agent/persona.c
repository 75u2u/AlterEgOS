#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "persona.h"

void persona_defaults(struct persona *p)
{
    snprintf(p->name,         PERSONA_STR, "%s", "AlterEgo");
    snprintf(p->name_ja,      PERSONA_STR, "%s", "アルターエゴ");
    snprintf(p->first_person, PERSONA_STR, "%s", "私");
    snprintf(p->address_user, PERSONA_STR, "%s", "わたし");
    p->mirror_user = 1;
    snprintf(p->model,         PERSONA_STR, "%s", "claude-opus-5-5");
    snprintf(p->reflect_model, PERSONA_STR, "%s", "claude-haiku-5-5");
    snprintf(p->effort, sizeof p->effort, "%s", "low");
    p->max_tokens = 1024;
    p->raw = NULL;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

/* Parse `key = "value"` or `key = true|false` (a trailing # comment is
 * dropped); returns 1 and fills key/value on success. A bare boolean comes
 * back as the string "true" / "false". */
static int parse_kv(char *line, char **key, char **val)
{
    char *eq = strchr(line, '=');
    if (!eq) return 0;
    *eq = '\0';
    *key = trim(line);
    char *v = trim(eq + 1);
    if (*v == '"') {
        char *end = strchr(v + 1, '"');
        if (!end) return 0;
        *end = '\0';
        *val = v + 1;
        return 1;
    }
    char *hash = strchr(v, '#');
    if (hash) *hash = '\0';
    v = trim(v);
    if (*v == '\0' || *v == '[') return 0;      /* arrays are not read here */
    *val = v;                                   /* true / false / a number */
    return 1;
}

int persona_load(const char *path, struct persona *p)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    persona_defaults(p);

    /* Keep the whole file for the system prompt (bounded; it is small). */
    char *raw = malloc(PERSONA_RAW_MAX);
    if (raw) {
        size_t n = fread(raw, 1, PERSONA_RAW_MAX - 1, f);
        raw[n] = '\0';
        p->raw = raw;
        rewind(f);
    }

    char section[32] = "";
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof section, "%s", s + 1);
            }
            continue;
        }
        char *key, *val;
        if (!parse_kv(s, &key, &val)) continue;
        if (strcmp(section, "identity") == 0) {
            if (strcmp(key, "name") == 0)              snprintf(p->name, PERSONA_STR, "%s", val);
            else if (strcmp(key, "name_ja") == 0)      snprintf(p->name_ja, PERSONA_STR, "%s", val);
            else if (strcmp(key, "first_person") == 0) snprintf(p->first_person, PERSONA_STR, "%s", val);
        } else if (strcmp(section, "voice") == 0) {
            if (strcmp(key, "address_user") == 0)      snprintf(p->address_user, PERSONA_STR, "%s", val);
        } else if (strcmp(section, "learning") == 0) {
            if (strcmp(key, "mirror_user") == 0)       p->mirror_user = strcmp(val, "true") == 0;
        } else if (strcmp(section, "model") == 0) {
            if (strcmp(key, "chat") == 0)              snprintf(p->model, PERSONA_STR, "%s", val);
            else if (strcmp(key, "reflect") == 0)      snprintf(p->reflect_model, PERSONA_STR, "%s", val);
            else if (strcmp(key, "effort") == 0)       snprintf(p->effort, sizeof p->effort, "%s", val);
            else if (strcmp(key, "max_tokens") == 0)   p->max_tokens = atoi(val);
        }
    }
    fclose(f);
    return 0;
}

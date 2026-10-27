#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "persona.h"

void persona_defaults(struct persona *p)
{
    snprintf(p->name,         PERSONA_STR, "%s", "AlterEgo");
    snprintf(p->name_ja,      PERSONA_STR, "%s", "アルターエゴ");
    snprintf(p->first_person, PERSONA_STR, "%s", "私");
    snprintf(p->address_user, PERSONA_STR, "%s", "わたし");
    p->mirror_user = 1;
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
    if (strcmp(v, "true") != 0 && strcmp(v, "false") != 0) return 0;
    *val = v;
    return 1;
}

int persona_load(const char *path, struct persona *p)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    persona_defaults(p);

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
        }
    }
    fclose(f);
    return 0;
}

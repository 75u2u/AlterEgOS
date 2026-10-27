#ifndef ALTEREGO_PERSONA_H
#define ALTEREGO_PERSONA_H

/* The persona file is the seed, not the personality: the default name
 * AlterEgo answers to until the user names it, the default name for the
 * user ("user") until they introduce themselves, and how many
 * conversations each growth stage takes. The parser is deliberately tiny:
 * `key = "value"` and `key = number` lines inside [identity], [voice] and
 * [growth]. It is not a general TOML parser and does not try to be. */

#define PERSONA_STR 64

struct persona {
    char name[PERSONA_STR];          /* identity.name,         "AlterEgo"     */
    char name_ja[PERSONA_STR];       /* identity.name_ja,      "アルターエゴ"  */
    char first_person[PERSONA_STR];  /* identity.first_person, "私"           */
    char address_user[PERSONA_STR];  /* voice.address_user,    "user"         */
    char address_user_en[PERSONA_STR]; /* voice.address_user_en, "user"       */
    int  stage_turns[3];             /* growth.stage1/2/3: conversations until
                                        babbling, chatty, grown               */
};

void persona_defaults(struct persona *p);
int  persona_load(const char *path, struct persona *p);   /* 0 ok, -1 errno set */

#endif

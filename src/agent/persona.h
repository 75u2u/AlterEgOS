#ifndef ALTEREGO_PERSONA_H
#define ALTEREGO_PERSONA_H

/* The persona is the one part of the agent the user writes and the agent
 * never rewrites. Day 1 reads only what the greeting needs; the full file
 * (voice, values, behavior) is parsed from day 3 when the model is wired in.
 * The parser is deliberately tiny: `key = "value"` and `key = true|false`
 * lines inside [identity], [voice] and [learning]. It is not a general TOML
 * parser and does not try to be. */

#define PERSONA_STR 64

struct persona {
    char name[PERSONA_STR];          /* identity.name,         "AlterEgo"     */
    char name_ja[PERSONA_STR];       /* identity.name_ja,      "アルターエゴ"  */
    char first_person[PERSONA_STR];  /* identity.first_person, "私"           */
    char address_user[PERSONA_STR];  /* voice.address_user,    "わたし"        */
    char address_user_en[PERSONA_STR]; /* voice.address_user_en, "Watashi":
                                        the same name when talking English */
    int  mirror_user;                /* learning.mirror_user: learn the user's
                                        personality and speech habits every
                                        turn and project them onto itself   */
    int  mirror_after;               /* learning.mirror_after: utterances
                                        until AlterEgo talks fully in the
                                        user's voice                        */
};

void persona_defaults(struct persona *p);
int  persona_load(const char *path, struct persona *p);   /* 0 ok, -1 errno set */

#endif

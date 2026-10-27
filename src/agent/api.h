#ifndef ALTEREGO_API_H
#define ALTEREGO_API_H

#include <stddef.h>

/* The one network call the agent makes: POST /v1/messages on the Claude
 * API over HTTPS (libcurl + the system CA bundle). No tools, no streaming
 * yet; a reply is the concatenated text blocks of the response. */

#define API_STR 96

struct api_cfg {
    char key[256];              /* x-api-key                              */
    char url[128];              /* https://api.anthropic.com/v1/messages  */
    char model[API_STR];        /* conversation model                     */
    char reflect_model[API_STR];/* the cheaper model used for reflection  */
    char effort[16];            /* output_config.effort                   */
    int  max_tokens;
};

struct chat_msg {
    const char *role;           /* "user" or "assistant" */
    char *text;
};

void api_cfg_defaults(struct api_cfg *c);

/* Returns 0 and a malloc'd *reply on success; -1 with a message in err. A
 * safety refusal counts as success with reply = NULL. */
int api_chat(const struct api_cfg *c, const char *model, const char *system,
             const struct chat_msg *msgs, int nmsgs, char **reply,
             char *err, size_t errlen);

#endif

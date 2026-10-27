#define _GNU_SOURCE
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "json.h"

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

void api_cfg_defaults(struct api_cfg *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->url, sizeof c->url, "%s", "https://api.anthropic.com/v1/messages");
    snprintf(c->model, sizeof c->model, "%s", "claude-opus-5-5");
    snprintf(c->reflect_model, sizeof c->reflect_model, "%s", "claude-haiku-5-5");
    snprintf(c->effort, sizeof c->effort, "%s", "low");
    c->max_tokens = 1024;
}

static size_t on_body(char *data, size_t size, size_t nmemb, void *user)
{
    struct sbuf *b = user;
    return sbuf_add(b, data, size * nmemb) == 0 ? size * nmemb : 0;
}

/* Server-side refusal fallbacks exist for the Opus/Sonnet/Fable models on
 * the Claude API; Haiku has none and would reject the parameter. */
static int wants_fallbacks(const char *model)
{
    return strncmp(model, "claude-haiku", 12) != 0;
}

static int build_request(struct sbuf *b, const struct api_cfg *c, const char *model,
                         const char *system, const struct chat_msg *msgs, int n)
{
    if (sbuf_adds(b, "{\"model\":") < 0) return -1;
    if (sbuf_add_json_string(b, model) < 0) return -1;
    if (sbuf_addf(b, ",\"max_tokens\":%d,\"output_config\":{\"effort\":", c->max_tokens) < 0) return -1;
    if (sbuf_add_json_string(b, c->effort) < 0) return -1;
    if (sbuf_adds(b, "},\"system\":") < 0) return -1;
    if (sbuf_add_json_string(b, system) < 0) return -1;
    if (wants_fallbacks(model) && sbuf_adds(b, ",\"fallbacks\":\"default\"") < 0) return -1;
    if (sbuf_adds(b, ",\"messages\":[") < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (sbuf_addf(b, "%s{\"role\":\"%s\",\"content\":", i ? "," : "", msgs[i].role) < 0) return -1;
        if (sbuf_add_json_string(b, msgs[i].text) < 0) return -1;
        if (sbuf_adds(b, "}") < 0) return -1;
    }
    return sbuf_adds(b, "]}");
}

/* Concatenate every {"type":"text","text":"..."} block of the response. */
static char *extract_text(const char *body)
{
    struct sbuf out;
    sbuf_init(&out);
    const char *p = body;
    while ((p = strstr(p, "\"type\":\"text\"")) != NULL) {
        p += 13;
        const char *t = strstr(p, "\"text\":\"");
        if (!t) break;
        const char *end;
        char *s = json_unquote(t + 8, &end);
        if (!s) break;
        if (out.len) sbuf_adds(&out, "\n");
        sbuf_adds(&out, s);
        free(s);
        p = end;
    }
    if (!out.p) return strdup("");
    return out.p;
}

static void api_error_message(const char *body, long http, char *err, size_t errlen)
{
    const char *m = json_value(body, "message");
    char *s = (m && *m == '"') ? json_unquote(m + 1, NULL) : NULL;
    snprintf(err, errlen, "HTTP %ld: %s", http, s ? s : "(no message)");
    free(s);
}

int api_chat(const struct api_cfg *c, const char *model, const char *system,
             const struct chat_msg *msgs, int nmsgs, char **reply,
             char *err, size_t errlen)
{
    *reply = NULL;
    struct sbuf req, body;
    sbuf_init(&req);
    sbuf_init(&body);
    if (build_request(&req, c, model, system, msgs, nmsgs) < 0) {
        snprintf(err, errlen, "out of memory building the request");
        sbuf_free(&req);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, errlen, "curl_easy_init failed");
        sbuf_free(&req);
        return -1;
    }
    char keyhdr[300];
    snprintf(keyhdr, sizeof keyhdr, "x-api-key: %s", c->key);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "content-type: application/json");
    hdrs = curl_slist_append(hdrs, "anthropic-version: 2026-10-21");
    if (wants_fallbacks(model))
        hdrs = curl_slist_append(hdrs, "anthropic-beta: server-side-fallback-2026-07-01");
    hdrs = curl_slist_append(hdrs, keyhdr);

    curl_easy_setopt(curl, CURLOPT_URL, c->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.p);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "alteregos/0.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    if (access(CA_BUNDLE, R_OK) == 0) curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE);

    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    sbuf_free(&req);

    int ret = -1;
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    } else if (http != 200) {
        api_error_message(body.p ? body.p : "", http, err, errlen);
    } else {
        const char *stop = json_value(body.p, "stop_reason");
        if (stop && strncmp(stop, "\"refusal\"", 9) == 0) {
            *reply = NULL;              /* declined by the safety classifiers */
        } else {
            *reply = extract_text(body.p);
        }
        ret = 0;
    }
    sbuf_free(&body);
    return ret;
}

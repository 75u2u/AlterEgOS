/* Stand-in for libcurl's header, used ONLY by `make check` (syntax-only), so
 * the sources can be checked with a cross compiler whose sysroot has no
 * libcurl (CI runs `make check CC=aarch64-linux-gnu-gcc`). Real builds get
 * the real header through pkg-config. Lists just what api.c uses; if api.c
 * starts using more of libcurl, add it here. */
#ifndef ALTEREGO_CHECK_CURL_H
#define ALTEREGO_CHECK_CURL_H
#include <stddef.h>

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
#define CURLE_OK 0
enum { CURLOPT_URL, CURLOPT_HTTPHEADER, CURLOPT_POSTFIELDS, CURLOPT_POSTFIELDSIZE,
       CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, CURLOPT_USERAGENT, CURLOPT_TIMEOUT,
       CURLOPT_CONNECTTIMEOUT, CURLOPT_CAINFO };
enum { CURLINFO_RESPONSE_CODE };
struct curl_slist { char *data; struct curl_slist *next; };

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *, CURLoption, ...);
CURLcode curl_easy_perform(CURL *);
CURLcode curl_easy_getinfo(CURL *, CURLINFO, ...);
void curl_easy_cleanup(CURL *);
const char *curl_easy_strerror(CURLcode);
struct curl_slist *curl_slist_append(struct curl_slist *, const char *);
void curl_slist_free_all(struct curl_slist *);
#endif

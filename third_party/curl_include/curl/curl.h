/* HengeVM: the slice of libcurl's easy API the restore tools use, for iOS,
   where host/curl_shim.m implements it on NSURLSession. Option and info
   numbers follow libcurl's so the calls read the same. */
#ifndef HENGE_CURL_H
#define HENGE_CURL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
/* What the real header pulls in, which callers rely on. */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURL;
typedef int64_t curl_off_t;
typedef size_t (*curl_write_callback)(char *ptr, size_t size, size_t nmemb, void *userdata);
typedef int (*curl_xferinfo_callback)(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
typedef int (*curl_progress_callback)(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

typedef enum {
    CURLE_OK = 0,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_WRITE_ERROR = 23,
} CURLcode;

#define CURL_ERROR_SIZE 256
#define CURL_GLOBAL_ALL 3
#define CURL_GLOBAL_DEFAULT CURL_GLOBAL_ALL

typedef enum {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_ERRORBUFFER = 10010,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_HEADERDATA = 10029,
    CURLOPT_XFERINFODATA = 10057,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_PROGRESSFUNCTION = 20056,
    CURLOPT_HEADERFUNCTION = 20079,
    CURLOPT_XFERINFOFUNCTION = 20219,
    CURLOPT_VERBOSE = 41,
    CURLOPT_NOPROGRESS = 43,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_POSTFIELDSIZE = 60,
    CURLOPT_SSL_VERIFYPEER = 64,
    CURLOPT_HTTPGET = 80,
} CURLoption;

typedef enum {
    CURLINFO_RESPONSE_CODE = 0x200002,
} CURLINFO;

CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *c);
CURLcode curl_easy_setopt(CURL *c, CURLoption opt, ...);
CURLcode curl_easy_perform(CURL *c);
CURLcode curl_easy_getinfo(CURL *c, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode c);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s);
void curl_slist_free_all(struct curl_slist *list);

#ifdef __cplusplus
}
#endif
#endif

/* The part of libcurl's easy API the restore tools use (libtatsu's signing
   requests, idevicerestore's downloads and the restore OS's URL requests),
   on NSURLSession. iOS has no libcurl, and this way there is no TLS library
   to build either: the system's networking does HTTPS. Synchronous, like
   curl_easy_perform. Built for iOS only; the Mac links the real libcurl. */

#import <Foundation/Foundation.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "curl/curl.h"

struct curl_slist_shim {
    char *data;
    struct curl_slist_shim *next;
};

typedef struct {
    char *url, *useragent;
    const void *postfields;
    long postsize;
    int post, follow;
    curl_write_callback write_cb, header_cb;
    void *write_data, *header_data;
    curl_xferinfo_callback xferinfo;
    void *xferinfo_data;
    int noprogress;
    struct curl_slist *headers;
    char *errorbuffer;
    long response_code;
} Easy;

CURLcode curl_global_init(long flags) {
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void) {}

CURL *curl_easy_init(void) {
    Easy *e = calloc(1, sizeof *e);
    e->noprogress = 1;
    return (CURL *)e;
}

void curl_easy_cleanup(CURL *c) {
    Easy *e = (Easy *)c;
    if (!e) return;
    free(e->url);
    free(e->useragent);
    free(e);
}

CURLcode curl_easy_setopt(CURL *c, CURLoption opt, ...) {
    Easy *e = (Easy *)c;
    va_list ap;
    va_start(ap, opt);
    switch (opt) {
    case CURLOPT_URL: free(e->url); e->url = strdup(va_arg(ap, const char *)); break;
    case CURLOPT_USERAGENT: free(e->useragent); e->useragent = strdup(va_arg(ap, const char *)); break;
    case CURLOPT_POSTFIELDS: e->postfields = va_arg(ap, const void *); e->post = 1; break;
    case CURLOPT_POSTFIELDSIZE: e->postsize = va_arg(ap, long); break;
    case CURLOPT_HTTPGET: e->post = 0; (void)va_arg(ap, long); break;
    case CURLOPT_FOLLOWLOCATION: e->follow = (int)va_arg(ap, long); break;
    case CURLOPT_WRITEFUNCTION: e->write_cb = va_arg(ap, curl_write_callback); break;
    case CURLOPT_WRITEDATA: e->write_data = va_arg(ap, void *); break;
    case CURLOPT_HEADERFUNCTION: e->header_cb = va_arg(ap, curl_write_callback); break;
    case CURLOPT_HEADERDATA: e->header_data = va_arg(ap, void *); break;
    case CURLOPT_HTTPHEADER: e->headers = va_arg(ap, struct curl_slist *); break;
    case CURLOPT_ERRORBUFFER: e->errorbuffer = va_arg(ap, char *); break;
    case CURLOPT_XFERINFOFUNCTION: e->xferinfo = va_arg(ap, curl_xferinfo_callback); break;
    case CURLOPT_PROGRESSFUNCTION: e->xferinfo = (curl_xferinfo_callback)va_arg(ap, void *); break;
    case CURLOPT_XFERINFODATA: e->xferinfo_data = va_arg(ap, void *); break;
    case CURLOPT_NOPROGRESS: e->noprogress = (int)va_arg(ap, long); break;
    default: (void)va_arg(ap, void *); break; /* VERBOSE, SSL_VERIFYPEER, ...: nothing to do */
    }
    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *c, CURLINFO info, ...) {
    Easy *e = (Easy *)c;
    va_list ap;
    va_start(ap, info);
    if (info == CURLINFO_RESPONSE_CODE) *va_arg(ap, long *) = e->response_code;
    va_end(ap);
    return CURLE_OK;
}

/* Streams the body to the write callback as it arrives (downloads can be
   large), and the headers first. */
@interface HvCurlDelegate : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) Easy *easy;
@property(nonatomic) dispatch_semaphore_t done;
@property(nonatomic) NSError *error;
@property(nonatomic) long long expected, received;
@property(nonatomic) BOOL aborted;
@end

@implementation HvCurlDelegate
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveResponse:(NSURLResponse *)r
    completionHandler:(void (^)(NSURLSessionResponseDisposition))handler {
    NSHTTPURLResponse *h = (NSHTTPURLResponse *)r;
    Easy *e = self.easy;
    e->response_code = h.statusCode;
    self.expected = r.expectedContentLength;
    if (e->header_cb) {
        NSString *status = [NSString stringWithFormat:@"HTTP/1.1 %ld\r\n", (long)h.statusCode];
        e->header_cb((char *)status.UTF8String, 1, strlen(status.UTF8String), e->header_data);
        [h.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL *stop) {
            NSString *line = [NSString stringWithFormat:@"%@: %@\r\n", k, v];
            e->header_cb((char *)line.UTF8String, 1, strlen(line.UTF8String), e->header_data);
        }];
        e->header_cb("\r\n", 1, 2, e->header_data);
    }
    handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)t didReceiveData:(NSData *)data {
    Easy *e = self.easy;
    __block BOOL ok = YES;
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
        size_t n = range.length;
        size_t w = e->write_cb ? e->write_cb((char *)bytes, 1, n, e->write_data) : fwrite(bytes, 1, n, (FILE *)e->write_data);
        if (w != n) {
            ok = NO;
            *stop = YES;
        }
    }];
    self.received += data.length;
    if (!e->noprogress && e->xferinfo &&
        e->xferinfo(e->xferinfo_data, self.expected > 0 ? self.expected : 0, self.received, 0, 0) != 0)
        ok = NO;
    if (!ok) {
        self.aborted = YES;
        [t cancel];
    }
}

- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t
    willPerformHTTPRedirection:(NSHTTPURLResponse *)r newRequest:(NSURLRequest *)req
             completionHandler:(void (^)(NSURLRequest *))handler {
    handler(self.easy->follow ? req : nil);
}

- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)err {
    self.error = err;
    dispatch_semaphore_signal(self.done);
}
@end

CURLcode curl_easy_perform(CURL *c) {
    Easy *e = (Easy *)c;
    @autoreleasepool {
        NSURL *url = [NSURL URLWithString:@(e->url)];
        if (!url) return CURLE_URL_MALFORMAT;
        NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
        req.timeoutInterval = 300;
        if (e->useragent) [req setValue:@(e->useragent) forHTTPHeaderField:@"User-Agent"];
        for (struct curl_slist *h = e->headers; h; h = h->next) {
            NSString *line = @(h->data);
            NSRange colon = [line rangeOfString:@":"];
            if (colon.location == NSNotFound) continue;
            NSString *k = [line substringToIndex:colon.location];
            NSString *v = [[line substringFromIndex:colon.location + 1]
                stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            [req setValue:v forHTTPHeaderField:k];
        }
        if (e->post) {
            req.HTTPMethod = @"POST";
            long n = e->postsize > 0 ? e->postsize : (long)strlen(e->postfields);
            req.HTTPBody = [NSData dataWithBytes:e->postfields length:(NSUInteger)n];
        }
        HvCurlDelegate *d = [HvCurlDelegate new];
        d.easy = e;
        d.done = dispatch_semaphore_create(0);
        NSURLSessionConfiguration *cfg = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        cfg.timeoutIntervalForResource = 24 * 3600;
        NSURLSession *s = [NSURLSession sessionWithConfiguration:cfg delegate:d delegateQueue:nil];
        [[s dataTaskWithRequest:req] resume];
        dispatch_semaphore_wait(d.done, DISPATCH_TIME_FOREVER);
        [s finishTasksAndInvalidate];
        if (d.aborted) return CURLE_WRITE_ERROR;
        if (d.error) {
            if (e->errorbuffer) snprintf(e->errorbuffer, CURL_ERROR_SIZE, "%s", d.error.localizedDescription.UTF8String);
            return CURLE_COULDNT_CONNECT;
        }
    }
    return CURLE_OK;
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s) {
    struct curl_slist *n = calloc(1, sizeof *n);
    n->data = strdup(s);
    if (!list) return n;
    struct curl_slist *p = list;
    while (p->next) p = p->next;
    p->next = n;
    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *n = list->next;
        free(list->data);
        free(list);
        list = n;
    }
}

const char *curl_easy_strerror(CURLcode c) {
    switch (c) {
    case CURLE_OK: return "No error";
    case CURLE_COULDNT_CONNECT: return "Could not connect";
    case CURLE_WRITE_ERROR: return "Write error";
    default: return "curl error";
    }
}

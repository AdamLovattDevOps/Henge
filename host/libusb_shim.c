/* A libusb-1.0 stand-in whose only device is the virtual Mac's USB port.

   The restore tools (libirecovery, usbmuxd, idevicerestore through them) are
   ordinary libusb programs; linked against this file instead of libusb they
   drive the guest exactly as they would drive a Mac on a cable, which is what
   Virtualization.framework does with its virtual USB device. On iOS there is
   no USB host at all, so this is also the only way they can run there.

   One device, present while the guest is on the bus. Each connection (the
   ROM's DFU mode, iBoot's recovery mode, the restore OS) is a new device with
   a new address, as on a real bus. Transfers run on one worker thread per
   endpoint (so each endpoint's data stays in order); completions are handed
   to callbacks from libusb_handle_events*, which a pipe in the poll set
   wakes. Hotplug callbacks report arrival and departure. */

#include <errno.h>
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "usb_host.h"

static HvVirtioUsb *g_usb;
static int g_log;

void hv_libusb_shim_attach(HvVirtioUsb *d) {
    g_usb = d;
    g_log = getenv("HV_LIBUSB_LOG") != NULL;
}

/* ---- the device ---- */

struct libusb_device {
    int refs;
    uint32_t session;           /* the connection it stands for */
    uint8_t address;
    struct libusb_device_descriptor desc;
    uint8_t *configs[8];        /* raw configuration descriptors */
    uint16_t config_len[8];
    int gone;
};

struct libusb_device_handle {
    struct libusb_device *dev;
    int config;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct libusb_device *g_dev; /* the current connection's device, or NULL */
static uint8_t g_next_address = 1;
static uint8_t g_outstanding[32];   /* IN requests the guest still holds, per endpoint index */

static int ep_index(uint8_t ep) { return (ep & 0x0f) | (ep & 0x80 ? 16 : 0); }

static void dev_unref(struct libusb_device *d) {
    if (!d) return;
    pthread_mutex_lock(&g_mu);
    int last = --d->refs == 0;
    pthread_mutex_unlock(&g_mu);
    if (!last) return;
    for (int i = 0; i < 8; i++) free(d->configs[i]);
    free(d);
}

/* Read the descriptors of a fresh connection. */
static struct libusb_device *enumerate(uint32_t session) {
    if (hv_usb_reset(g_usb, 2000) != 0) return NULL;
    struct libusb_device *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->refs = 1;
    d->session = session;
    if (hv_usb_control(g_usb, 0x80, 6, 0x100, 0, &d->desc, 18, 2000) != 18) goto fail;
    for (int i = 0; i < d->desc.bNumConfigurations && i < 8; i++) {
        uint8_t head[9];
        if (hv_usb_control(g_usb, 0x80, 6, (uint16_t)(0x200 | i), 0, head, 9, 2000) < 4) goto fail;
        uint16_t total = (uint16_t)(head[2] | head[3] << 8);
        d->configs[i] = malloc(total);
        if (!d->configs[i] || hv_usb_control(g_usb, 0x80, 6, (uint16_t)(0x200 | i), 0, d->configs[i], total, 2000) != total)
            goto fail;
        d->config_len[i] = total;
    }
    d->address = g_next_address++;
    if (g_next_address > 127) g_next_address = 1;
    if (g_log)
        fprintf(stderr, "libusb shim: device %04x:%04x arrived (address %u, %u configurations)\n", d->desc.idVendor,
                d->desc.idProduct, d->address, d->desc.bNumConfigurations);
    return d;
fail:
    if (g_log) fprintf(stderr, "libusb shim: enumeration failed\n");
    dev_unref(d);
    return NULL;
}

typedef struct Hotplug {
    int handle, events;
    libusb_hotplug_callback_fn cb;
    void *user;
    struct Hotplug *next;
} Hotplug;
static Hotplug *g_hotplug;
static int g_hotplug_next = 1;
static pthread_mutex_t g_cb_mu = PTHREAD_MUTEX_INITIALIZER; /* callbacks run one at a time */

static void fire(struct libusb_device *d, int event) {
    Hotplug *list[16];
    int n = 0;
    pthread_mutex_lock(&g_mu);
    for (Hotplug *h = g_hotplug; h && n < 16; h = h->next)
        if (h->events & event) list[n++] = h;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < n; i++) list[i]->cb(NULL, d, (libusb_hotplug_event)event, list[i]->user);
}

/* Bring g_dev up to date with the guest: called from the device list and the
   event loop. */
static void refresh(void) {
    if (!g_usb) return;
    uint32_t session;
    int on = hv_vusb_online(g_usb, &session);
    pthread_mutex_lock(&g_mu);
    struct libusb_device *old = g_dev;
    int stale = old && (!on || old->session != session);
    if (stale) {
        old->gone = 1;
        g_dev = NULL;
        memset(g_outstanding, 0, sizeof g_outstanding);
    }
    int want = on && !g_dev;
    pthread_mutex_unlock(&g_mu);
    if (stale) {
        if (g_log) fprintf(stderr, "libusb shim: device left\n");
        fire(old, LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);
        dev_unref(old);
    }
    if (!want) return;
    static pthread_mutex_t enum_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&enum_mu);
    struct libusb_device *d = NULL;
    pthread_mutex_lock(&g_mu);
    int still = !g_dev;
    pthread_mutex_unlock(&g_mu);
    if (still) d = enumerate(session);
    if (d) {
        pthread_mutex_lock(&g_mu);
        g_dev = d;
        pthread_mutex_unlock(&g_mu);
        fire(d, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED);
    }
    pthread_mutex_unlock(&enum_mu);
}

/* ---- transfers ---- */

typedef struct Xfer {
    struct libusb_transfer *t;
    int cancel;
    struct Xfer *next;
} Xfer;

typedef struct Worker {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Xfer *head, *tail;
    int ep;
} Worker;
static Worker g_workers[32];
static pthread_once_t g_workers_once = PTHREAD_ONCE_INIT;

static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static Xfer *g_done_head, *g_done_tail;
static int g_pipe[2] = {-1, -1};

static Xfer *g_active[32]; /* the transfer each worker is running, for cancel */

static void sync_cb(struct libusb_transfer *t);

static void complete(Xfer *x, enum libusb_transfer_status st, int actual) {
    x->t->status = st;
    x->t->actual_length = actual;
    /* Synchronous calls may come from inside a callback (usbmuxd sets the
       configuration from one), so they must not wait for the event loop. */
    if (x->t->callback == sync_cb) {
        sync_cb(x->t);
        return;
    }
    pthread_mutex_lock(&g_done_mu);
    x->next = NULL;
    if (g_done_tail) g_done_tail->next = x;
    else g_done_head = x;
    g_done_tail = x;
    pthread_mutex_unlock(&g_done_mu);
    char c = 1;
    (void)!write(g_pipe[1], &c, 1);
}

/* Run one transfer synchronously, in slices so a cancel is noticed. */
static void run(Xfer *x) {
    struct libusb_transfer *t = x->t;
    struct libusb_device *d = t->dev_handle->dev;
    int slice = 200, waited = 0, limit = t->timeout ? (int)t->timeout : 0;
    if (d->gone) {
        complete(x, LIBUSB_TRANSFER_NO_DEVICE, 0);
        return;
    }
    if (t->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        struct libusb_control_setup *s = (struct libusb_control_setup *)t->buffer;
        int r = hv_usb_control(g_usb, s->bmRequestType, s->bRequest, libusb_le16_to_cpu(s->wValue),
                               libusb_le16_to_cpu(s->wIndex), t->buffer + 8, libusb_le16_to_cpu(s->wLength),
                               limit ? limit : 60000);
        if (r >= 0) complete(x, LIBUSB_TRANSFER_COMPLETED, r); /* data bytes, not counting the setup */
        else complete(x, r == HV_USB_ESTALL ? LIBUSB_TRANSFER_STALL : r == HV_USB_ETIMEOUT ? LIBUSB_TRANSFER_TIMED_OUT
                                                                                            : LIBUSB_TRANSFER_NO_DEVICE, 0);
        return;
    }
    uint8_t ep = t->endpoint;
    int in = (ep & 0x80) != 0, ix = ep_index(ep);
    int acks = 1; /* responses that end this transfer */
    if (!in) {
        /* On a real bus the data goes in packets and fills the device's
           posted reads one after another; here each request is one read,
           which must fit the guest's buffer (16 KB, usbmux's receive size; a
           bigger one panics the guest's driver). So send it in pieces. */
        static int chunk;
        if (!chunk) chunk = getenv("HV_USB_OUT_CHUNK") ? atoi(getenv("HV_USB_OUT_CHUNK")) : 16384;
        acks = 0;
        if (t->length == 0) { /* a zero-length packet: this link delimits transfers itself */
            complete(x, LIBUSB_TRANSFER_COMPLETED, 0);
            return;
        }
        for (int off = 0; off < t->length || (off == 0 && t->length == 0); off += chunk) {
            int n = t->length - off < chunk ? t->length - off : chunk;
            if (hv_vusb_send(g_usb, HV_VUSB_REQ_DATA, ep, t->buffer + off, (uint32_t)n) != 0) {
                complete(x, LIBUSB_TRANSFER_NO_DEVICE, 0);
                return;
            }
            acks++;
            if (t->length == 0) break;
        }
    } else {
        pthread_mutex_lock(&g_mu);
        int have = g_outstanding[ix] > 0;
        if (!have) g_outstanding[ix]++;
        pthread_mutex_unlock(&g_mu);
        /* A request a cancelled transfer left with the guest is still good:
           its answer is the next data in the stream, so wait for that. */
        if (!have && hv_vusb_send_in(g_usb, ep, (uint32_t)t->length) != 0) {
            complete(x, LIBUSB_TRANSFER_NO_DEVICE, 0);
            return;
        }
    }
    for (;;) {
        HvVusbMsg m;
        int r = hv_vusb_recv(g_usb, ep, &m, slice);
        if (r < 0) {
            complete(x, LIBUSB_TRANSFER_NO_DEVICE, 0);
            return;
        }
        if (r > 0) {
            if (in) {
                pthread_mutex_lock(&g_mu);
                if (g_outstanding[ix]) g_outstanding[ix]--;
                pthread_mutex_unlock(&g_mu);
            }
            int n = 0;
            enum libusb_transfer_status st = LIBUSB_TRANSFER_COMPLETED;
            if (m.type == HV_VUSB_RSP_DATA) {
                n = in ? ((int)m.len < t->length ? (int)m.len : t->length) : t->length;
                if (in && n) memcpy(t->buffer, m.data, (size_t)n);
                if (in && (int)m.len > t->length && g_log)
                    fprintf(stderr, "libusb shim: ep %#x: %u bytes for a %d-byte buffer\n", ep, m.len, t->length);
            } else if (m.type == HV_VUSB_RSP_STALL) {
                st = LIBUSB_TRANSFER_STALL;
            } else {
                st = LIBUSB_TRANSFER_ERROR;
            }
            free(m.data);
            if (st == LIBUSB_TRANSFER_COMPLETED && --acks > 0) continue; /* more pieces to be taken */
            complete(x, st, n);
            return;
        }
        waited += slice;
        if (__atomic_load_n(&x->cancel, __ATOMIC_ACQUIRE)) {
            complete(x, LIBUSB_TRANSFER_CANCELLED, 0);
            return;
        }
        if (limit && waited >= limit) {
            complete(x, LIBUSB_TRANSFER_TIMED_OUT, 0);
            return;
        }
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->head) pthread_cond_wait(&w->cv, &w->mu);
        Xfer *x = w->head;
        w->head = x->next;
        if (!w->head) w->tail = NULL;
        g_active[w->ep] = x;
        pthread_mutex_unlock(&w->mu);
        if (__atomic_load_n(&x->cancel, __ATOMIC_ACQUIRE)) complete(x, LIBUSB_TRANSFER_CANCELLED, 0);
        else run(x);
        pthread_mutex_lock(&w->mu);
        g_active[w->ep] = NULL;
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

static void workers_init(void) {
    if (pipe(g_pipe) == 0) {
        fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
    }
    for (int i = 0; i < 32; i++) {
        Worker *w = &g_workers[i];
        w->ep = i;
        pthread_mutex_init(&w->mu, NULL);
        pthread_cond_init(&w->cv, NULL);
        pthread_create(&w->th, NULL, worker_main, w);
    }
}

/* The private part of a transfer: libusb_alloc_transfer puts it in front. */
typedef struct {
    Xfer x;
    struct libusb_transfer t;
} XferBox;

struct libusb_transfer *libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    XferBox *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->x.t = &b->t;
    return &b->t;
}

static Xfer *xfer_of(struct libusb_transfer *t) {
    return &((XferBox *)((char *)t - offsetof(XferBox, t)))->x;
}

void libusb_free_transfer(struct libusb_transfer *t) {
    if (!t) return;
    if ((t->flags & LIBUSB_TRANSFER_FREE_BUFFER) && t->buffer) free(t->buffer);
    free((char *)t - offsetof(XferBox, t));
}

int libusb_submit_transfer(struct libusb_transfer *t) {
    pthread_once(&g_workers_once, workers_init);
    if (!t->dev_handle || t->dev_handle->dev->gone) return LIBUSB_ERROR_NO_DEVICE;
    Xfer *x = xfer_of(t);
    x->cancel = 0;
    x->next = NULL;
    int ix = t->type == LIBUSB_TRANSFER_TYPE_CONTROL ? 0 : ep_index(t->endpoint);
    Worker *w = &g_workers[ix];
    pthread_mutex_lock(&w->mu);
    if (w->tail) w->tail->next = x;
    else w->head = x;
    w->tail = x;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}

int libusb_cancel_transfer(struct libusb_transfer *t) {
    __atomic_store_n(&xfer_of(t)->cancel, 1, __ATOMIC_RELEASE);
    return 0;
}

static void drain_completions(void) {
    char buf[64];
    while (read(g_pipe[0], buf, sizeof buf) > 0) {
    }
    for (;;) {
        pthread_mutex_lock(&g_done_mu);
        Xfer *x = g_done_head;
        if (x) {
            g_done_head = x->next;
            if (!g_done_head) g_done_tail = NULL;
        }
        pthread_mutex_unlock(&g_done_mu);
        if (!x) break;
        struct libusb_transfer *t = x->t;
        int free_it = (t->flags & LIBUSB_TRANSFER_FREE_TRANSFER) != 0;
        if (t->callback) t->callback(t);
        if (free_it) libusb_free_transfer(t);
    }
}

int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed) {
    (void)ctx;
    pthread_once(&g_workers_once, workers_init);
    pthread_mutex_lock(&g_cb_mu);
    refresh();
    drain_completions();
    pthread_mutex_unlock(&g_cb_mu);
    if (completed && *completed) return 0;
    long us = tv ? tv->tv_sec * 1000000L + tv->tv_usec : 100000;
    if (us > 0) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_pipe[0], &fds);
        struct timeval w = {us / 1000000, (int)(us % 1000000)};
        select(g_pipe[0] + 1, &fds, NULL, NULL, &w);
        pthread_mutex_lock(&g_cb_mu);
        drain_completions();
        pthread_mutex_unlock(&g_cb_mu);
    }
    return 0;
}

int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv) {
    return libusb_handle_events_timeout_completed(ctx, tv, NULL);
}

int libusb_handle_events_completed(libusb_context *ctx, int *completed) {
    struct timeval tv = {0, 100000};
    return libusb_handle_events_timeout_completed(ctx, &tv, completed);
}

int libusb_handle_events(libusb_context *ctx) { return libusb_handle_events_completed(ctx, NULL); }

const struct libusb_pollfd **libusb_get_pollfds(libusb_context *ctx) {
    (void)ctx;
    pthread_once(&g_workers_once, workers_init);
    struct {
        const struct libusb_pollfd *list[2];
        struct libusb_pollfd fd;
    } *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->fd.fd = g_pipe[0];
    p->fd.events = POLLIN;
    p->list[0] = &p->fd;
    return (const struct libusb_pollfd **)p;
}

void libusb_free_pollfds(const struct libusb_pollfd **fds) { free((void *)fds); }

/* Arrivals are found by polling the guest, so the event loop must come back
   now and then even with nothing in flight. */
int libusb_get_next_timeout(libusb_context *ctx, struct timeval *tv) {
    (void)ctx;
    tv->tv_sec = 0;
    tv->tv_usec = 100000;
    return 1;
}

/* ---- synchronous API ---- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
} SyncWait;

static void sync_cb(struct libusb_transfer *t) {
    SyncWait *s = t->user_data;
    pthread_mutex_lock(&s->mu);
    s->done = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* Callers of the sync API may not run an event loop (or may be inside one):
   the worker completes these directly. */
static int sync_xfer(struct libusb_transfer *t) {
    SyncWait s = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
    t->callback = sync_cb;
    t->user_data = &s;
    int r = libusb_submit_transfer(t);
    if (r) return r;
    pthread_mutex_lock(&s.mu);
    while (!s.done) pthread_cond_wait(&s.cv, &s.mu);
    pthread_mutex_unlock(&s.mu);
    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_TIMED_OUT: return LIBUSB_ERROR_TIMEOUT;
    case LIBUSB_TRANSFER_STALL: return LIBUSB_ERROR_PIPE;
    case LIBUSB_TRANSFER_NO_DEVICE: return LIBUSB_ERROR_NO_DEVICE;
    case LIBUSB_TRANSFER_CANCELLED: return LIBUSB_ERROR_INTERRUPTED;
    default: return LIBUSB_ERROR_IO;
    }
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                            unsigned char *data, uint16_t len, unsigned int timeout) {
    uint8_t *buf = malloc(8u + len);
    if (!buf) return LIBUSB_ERROR_NO_MEM;
    libusb_fill_control_setup(buf, rt, req, val, idx, len);
    if (!(rt & 0x80) && len) memcpy(buf + 8, data, len);
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    libusb_fill_control_transfer(t, h, buf, NULL, NULL, timeout);
    int r = sync_xfer(t);
    int n = t->actual_length;
    if (!r && (rt & 0x80) && n) memcpy(data, buf + 8, (size_t)n);
    libusb_free_transfer(t);
    free(buf);
    return r ? r : n;
}

static int bulkish(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *done,
                   unsigned int timeout, unsigned char type) {
    struct libusb_transfer *t = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(t, h, ep, data, len, NULL, NULL, timeout);
    t->type = type;
    int r = sync_xfer(t);
    if (done) *done = t->actual_length;
    libusb_free_transfer(t);
    return r;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *done,
                         unsigned int timeout) {
    return bulkish(h, ep, data, len, done, timeout, LIBUSB_TRANSFER_TYPE_BULK);
}

int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *done,
                              unsigned int timeout) {
    return bulkish(h, ep, data, len, done, timeout, LIBUSB_TRANSFER_TYPE_INTERRUPT);
}

/* ---- context, devices, descriptors ---- */

int libusb_init(libusb_context **ctx) {
    if (ctx) *ctx = (libusb_context *)&g_usb;
    pthread_once(&g_workers_once, workers_init);
    return 0;
}

int libusb_init_context(libusb_context **ctx, const struct libusb_init_option *opts, int n) {
    (void)opts, (void)n;
    return libusb_init(ctx);
}

void libusb_exit(libusb_context *ctx) { (void)ctx; }
void libusb_set_debug(libusb_context *ctx, int level) { (void)ctx, (void)level; }
int libusb_set_option(libusb_context *ctx, enum libusb_option o, ...) {
    (void)ctx, (void)o;
    return 0;
}

int libusb_has_capability(uint32_t cap) { return cap == LIBUSB_CAP_HAS_CAPABILITY || cap == LIBUSB_CAP_HAS_HOTPLUG; }

const struct libusb_version *libusb_get_version(void) {
    static const struct libusb_version v = {1, 0, 27, 0, "", "https://libusb.info (HengeVM stand-in)"};
    return &v;
}

const char *libusb_error_name(int code) {
    switch (code) {
    case 0: return "LIBUSB_SUCCESS";
    case LIBUSB_ERROR_IO: return "LIBUSB_ERROR_IO";
    case LIBUSB_ERROR_INVALID_PARAM: return "LIBUSB_ERROR_INVALID_PARAM";
    case LIBUSB_ERROR_ACCESS: return "LIBUSB_ERROR_ACCESS";
    case LIBUSB_ERROR_NO_DEVICE: return "LIBUSB_ERROR_NO_DEVICE";
    case LIBUSB_ERROR_NOT_FOUND: return "LIBUSB_ERROR_NOT_FOUND";
    case LIBUSB_ERROR_BUSY: return "LIBUSB_ERROR_BUSY";
    case LIBUSB_ERROR_TIMEOUT: return "LIBUSB_ERROR_TIMEOUT";
    case LIBUSB_ERROR_OVERFLOW: return "LIBUSB_ERROR_OVERFLOW";
    case LIBUSB_ERROR_PIPE: return "LIBUSB_ERROR_PIPE";
    case LIBUSB_ERROR_INTERRUPTED: return "LIBUSB_ERROR_INTERRUPTED";
    case LIBUSB_ERROR_NO_MEM: return "LIBUSB_ERROR_NO_MEM";
    case LIBUSB_ERROR_NOT_SUPPORTED: return "LIBUSB_ERROR_NOT_SUPPORTED";
    default: return "LIBUSB_ERROR_OTHER";
    }
}

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list) {
    (void)ctx;
    refresh();
    libusb_device **l = calloc(2, sizeof *l);
    if (!l) return LIBUSB_ERROR_NO_MEM;
    pthread_mutex_lock(&g_mu);
    ssize_t n = 0;
    if (g_dev) {
        g_dev->refs++;
        l[n++] = g_dev;
    }
    pthread_mutex_unlock(&g_mu);
    *list = l;
    return n;
}

void libusb_free_device_list(libusb_device **list, int unref) {
    if (!list) return;
    if (unref)
        for (libusb_device **p = list; *p; p++) dev_unref(*p);
    free(list);
}

libusb_device *libusb_ref_device(libusb_device *d) {
    pthread_mutex_lock(&g_mu);
    d->refs++;
    pthread_mutex_unlock(&g_mu);
    return d;
}

void libusb_unref_device(libusb_device *d) { dev_unref(d); }

uint8_t libusb_get_bus_number(libusb_device *d) { (void)d; return 1; }
uint8_t libusb_get_device_address(libusb_device *d) { return d->address; }
uint8_t libusb_get_port_number(libusb_device *d) { (void)d; return 1; }
int libusb_get_device_speed(libusb_device *d) { (void)d; return LIBUSB_SPEED_HIGH; }

int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *desc) {
    *desc = d->desc;
    return 0;
}

int libusb_get_max_packet_size(libusb_device *d, unsigned char ep) {
    (void)d;
    return ep & 0x7f ? 512 : 64;
}

/* Parse one raw configuration descriptor into libusb's structures. */
static int parse_config(const uint8_t *raw, int len, struct libusb_config_descriptor **out) {
    if (len < 9) return LIBUSB_ERROR_IO;
    struct libusb_config_descriptor *c = calloc(1, sizeof *c);
    if (!c) return LIBUSB_ERROR_NO_MEM;
    c->bLength = raw[0];
    c->bDescriptorType = raw[1];
    c->wTotalLength = (uint16_t)(raw[2] | raw[3] << 8);
    c->bNumInterfaces = raw[4];
    c->bConfigurationValue = raw[5];
    c->iConfiguration = raw[6];
    c->bmAttributes = raw[7];
    c->MaxPower = raw[8];
    struct libusb_interface *ifs = calloc(c->bNumInterfaces ? c->bNumInterfaces : 1, sizeof *ifs);
    c->interface = ifs;
    struct libusb_interface_descriptor *cur = NULL;
    for (int p = raw[0]; p + 2 <= len && raw[p] >= 2; p += raw[p]) {
        const uint8_t *q = raw + p;
        if (q[1] == LIBUSB_DT_INTERFACE && q[0] >= 9) {
            int n = q[2];
            if (n >= c->bNumInterfaces) continue;
            struct libusb_interface *i = &ifs[n];
            struct libusb_interface_descriptor *a = realloc((void *)i->altsetting, (i->num_altsetting + 1) * sizeof *a);
            if (!a) continue;
            i->altsetting = a;
            cur = &a[i->num_altsetting++];
            memset(cur, 0, sizeof *cur);
            cur->bLength = q[0];
            cur->bDescriptorType = q[1];
            cur->bInterfaceNumber = q[2];
            cur->bAlternateSetting = q[3];
            cur->bNumEndpoints = 0;
            cur->bInterfaceClass = q[5];
            cur->bInterfaceSubClass = q[6];
            cur->bInterfaceProtocol = q[7];
            cur->iInterface = q[8];
        } else if (q[1] == LIBUSB_DT_ENDPOINT && q[0] >= 7 && cur) {
            struct libusb_endpoint_descriptor *e = realloc((void *)cur->endpoint, (cur->bNumEndpoints + 1) * sizeof *e);
            if (!e) continue;
            cur->endpoint = e;
            struct libusb_endpoint_descriptor *x = &e[cur->bNumEndpoints++];
            memset(x, 0, sizeof *x);
            x->bLength = q[0];
            x->bDescriptorType = q[1];
            x->bEndpointAddress = q[2];
            x->bmAttributes = q[3];
            x->wMaxPacketSize = (uint16_t)(q[4] | q[5] << 8);
            x->bInterval = q[6];
        }
    }
    *out = c;
    return 0;
}

int libusb_get_config_descriptor(libusb_device *d, uint8_t index, struct libusb_config_descriptor **c) {
    if (index >= 8 || !d->configs[index]) return LIBUSB_ERROR_NOT_FOUND;
    return parse_config(d->configs[index], d->config_len[index], c);
}

int libusb_get_config_descriptor_by_value(libusb_device *d, uint8_t value, struct libusb_config_descriptor **c) {
    for (int i = 0; i < 8; i++)
        if (d->configs[i] && d->config_len[i] >= 6 && d->configs[i][5] == value)
            return parse_config(d->configs[i], d->config_len[i], c);
    return LIBUSB_ERROR_NOT_FOUND;
}

int libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **c) {
    return libusb_get_config_descriptor(d, 0, c);
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *c) {
    if (!c) return;
    for (int i = 0; i < c->bNumInterfaces; i++) {
        const struct libusb_interface *f = &c->interface[i];
        for (int a = 0; a < f->num_altsetting; a++) free((void *)f->altsetting[a].endpoint);
        free((void *)f->altsetting);
    }
    free((void *)c->interface);
    free(c);
}

int libusb_open(libusb_device *d, libusb_device_handle **h) {
    if (d->gone) return LIBUSB_ERROR_NO_DEVICE;
    libusb_device_handle *x = calloc(1, sizeof *x);
    if (!x) return LIBUSB_ERROR_NO_MEM;
    x->dev = libusb_ref_device(d);
    x->config = d->configs[0] && d->config_len[0] >= 6 ? d->configs[0][5] : 1;
    *h = x;
    return 0;
}

void libusb_close(libusb_device_handle *h) {
    if (!h) return;
    dev_unref(h->dev);
    free(h);
}

libusb_device *libusb_get_device(libusb_device_handle *h) { return h->dev; }

int libusb_get_configuration(libusb_device_handle *h, int *config) {
    uint8_t v = 0;
    int r = libusb_control_transfer(h, 0x80, LIBUSB_REQUEST_GET_CONFIGURATION, 0, 0, &v, 1, 2000);
    if (r == 1) h->config = v;
    *config = h->config;
    return 0;
}

int libusb_set_configuration(libusb_device_handle *h, int config) {
    int r = libusb_control_transfer(h, 0x00, LIBUSB_REQUEST_SET_CONFIGURATION, (uint16_t)config, 0, NULL, 0, 2000);
    if (r < 0) return r;
    h->config = config;
    return 0;
}

int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h, (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h, (void)i; return 0; }
int libusb_kernel_driver_active(libusb_device_handle *h, int i) { (void)h, (void)i; return 0; }
int libusb_detach_kernel_driver(libusb_device_handle *h, int i) { (void)h, (void)i; return 0; }

int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int alt) {
    int r = libusb_control_transfer(h, 0x01, LIBUSB_REQUEST_SET_INTERFACE, (uint16_t)alt, (uint16_t)i, NULL, 0, 2000);
    return r < 0 ? r : 0;
}

int libusb_clear_halt(libusb_device_handle *h, unsigned char ep) {
    int r = libusb_control_transfer(h, 0x02, LIBUSB_REQUEST_CLEAR_FEATURE, 0, ep, NULL, 0, 2000);
    return r < 0 ? r : 0;
}

int libusb_reset_device(libusb_device_handle *h) {
    (void)h;
    return hv_usb_reset(g_usb, 2000) == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *data, int length) {
    uint8_t buf[256];
    if (!idx) return LIBUSB_ERROR_INVALID_PARAM;
    int r = libusb_control_transfer(h, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_STRING << 8 | idx),
                                    0x409, buf, 255, 2000);
    if (r < 2) return r < 0 ? r : LIBUSB_ERROR_IO;
    int k = 0;
    for (int i = 2; i + 1 < r && k < length - 1; i += 2) data[k++] = buf[i + 1] ? '?' : buf[i];
    data[k] = 0;
    return k;
}

int libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags, int vendor, int product,
                                     int dev_class, libusb_hotplug_callback_fn cb, void *user,
                                     libusb_hotplug_callback_handle *handle) {
    (void)ctx, (void)vendor, (void)product, (void)dev_class;
    Hotplug *h = calloc(1, sizeof *h);
    if (!h) return LIBUSB_ERROR_NO_MEM;
    h->events = events;
    h->cb = cb;
    h->user = user;
    pthread_mutex_lock(&g_mu);
    h->handle = g_hotplug_next++;
    h->next = g_hotplug;
    g_hotplug = h;
    struct libusb_device *now = g_dev;
    if (now) now->refs++;
    pthread_mutex_unlock(&g_mu);
    if (handle) *handle = h->handle;
    if ((flags & LIBUSB_HOTPLUG_ENUMERATE) && now && (events & LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED))
        cb(ctx, now, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, user);
    if (now) dev_unref(now);
    return 0;
}

void libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle handle) {
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    for (Hotplug **p = &g_hotplug; *p; p = &(*p)->next)
        if ((*p)->handle == handle) {
            Hotplug *h = *p;
            *p = h->next;
            free(h);
            break;
        }
    pthread_mutex_unlock(&g_mu);
}

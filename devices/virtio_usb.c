#include "virtio_usb.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define REQQ 0
#define RSPQ 1

static void msg_free(HvVusbMsg *m) {
    free(m->data);
    m->data = NULL;
}

static void drop_all_locked(HvVirtioUsb *d) {
    while (d->out_head != d->out_tail) msg_free(&d->out[d->out_head++ % HV_VUSB_RING]);
    while (d->in_head != d->in_tail) msg_free(&d->in[d->in_head++ % HV_VUSB_RING]);
}

static void put_msg(HvVirtioUsb *d, HvVusbMsg *m) {
    if (d->log) fprintf(stderr, "virtio-usb: response type %u ep %#x len %u\n", m->type, m->ep, m->len);
    pthread_mutex_lock(&d->mu);
    if (d->in_tail - d->in_head < HV_VUSB_RING) d->in[d->in_tail++ % HV_VUSB_RING] = *m;
    else msg_free(m);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
}

void hv_vusb_put_response(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len) {
    HvVusbMsg m = {.type = type, .ep = ep, .len = len};
    if (len) {
        m.data = malloc(len);
        if (!m.data) return;
        memcpy(m.data, data, len);
    }
    put_msg(d, &m);
}

int hv_vusb_take_request(HvVirtioUsb *d, HvVusbMsg *m) {
    pthread_mutex_lock(&d->mu);
    int got = d->out_head != d->out_tail;
    if (got) {
        *m = d->out[d->out_head++ % HV_VUSB_RING];
        if (d->log) fprintf(stderr, "virtio-usb: request type %u ep %#x len %u\n", m->type, m->ep, m->len);
    }
    pthread_mutex_unlock(&d->mu);
    return got;
}

void hv_vusb_set_online(HvVirtioUsb *d, int on) {
    pthread_mutex_lock(&d->mu);
    if (on || d->online) d->session++; /* coming on again is a new connection too */
    d->online = on;
    drop_all_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    if (d->log) fprintf(stderr, "virtio-usb: guest %s the bus\n", on ? "went on" : "went off");
}

/* Responses: copy each out of the guest's buffer and give the buffer back. */
static void drain_responses(HvVirtioUsb *d) {
    HvVirtio *v = &d->vio;
    static HvVirtqElem e;
    int any = 0;
    while (hv_virtq_pop(v, RSPQ, &e) > 0) {
        uint32_t total = 0;
        for (int s = 0; s < e.nseg; s++)
            if (!e.seg[s].write) total += e.seg[s].len;
        uint8_t hdr[2] = {0, 0};
        if (total >= 2) hv_virtq_read(v, &e, 0, hdr, 2);
        HvVusbMsg m = {.type = hdr[0], .ep = hdr[1], .len = total >= 2 ? total - 2 : 0};
        if (m.len) {
            m.data = malloc(m.len);
            if (m.data) hv_virtq_read(v, &e, 2, m.data, m.len);
            else m.len = 0;
        }
        put_msg(d, &m);
        hv_virtq_push(v, RSPQ, &e, 0);
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

static void deliver_requests(HvVirtioUsb *d) {
    HvVirtio *v = &d->vio;
    static HvVirtqElem e;
    int any = 0;
    pthread_mutex_lock(&d->mu);
    while (d->out_head != d->out_tail) {
        HvVusbMsg *m = &d->out[d->out_head % HV_VUSB_RING];
        int r = hv_virtq_pop(v, REQQ, &e);
        if (r <= 0) break;
        uint32_t room = hv_virtq_writable(&e), payload = m->data ? m->len : 0;
        if (room < 6 + payload) {
            /* The guest sizes its buffers for the largest transfer it takes;
               a request that does not fit would be cut, so say so loudly. */
            fprintf(stderr, "virtio-usb: request ep %#x len %u does not fit a %u-byte buffer\n", m->ep, m->len, room);
        }
        uint8_t hdr[6];
        memcpy(hdr, &m->len, 4);
        hdr[4] = m->ep;
        hdr[5] = m->type;
        uint32_t n = hv_virtq_write(v, &e, 0, hdr, 6);
        if (payload) n += hv_virtq_write(v, &e, 6, m->data, payload);
        if (d->log) fprintf(stderr, "virtio-usb: request type %u ep %#x len %u\n", m->type, m->ep, m->len);
        hv_virtq_push(v, REQQ, &e, n);
        msg_free(m);
        d->out_head++;
        any = 1;
    }
    pthread_mutex_unlock(&d->mu);
    if (any) hv_virtio_interrupt(v);
}

static void notify(HvVirtio *v, int q) {
    HvVirtioUsb *d = v->dev;
    if (q == RSPQ) drain_responses(d);
    else if (q == REQQ) deliver_requests(d);
}

static void track_status(HvVirtioUsb *d) {
    int on = (d->vio.status & 4) != 0;
    if (on == d->pci_on) return;
    d->pci_on = on;
    hv_vusb_set_online(d, on);
}

void hv_virtio_usb_poll(HvVirtioUsb *d) {
    if (!d) return;
    track_status(d);
    if (!d->pci_on) return;
    if (d->vio.q[RSPQ].enabled) drain_responses(d);
    if (d->vio.q[REQQ].enabled) deliver_requests(d);
}

HvVirtioUsb *hv_virtio_usb_create(HvMachine *m, HvPcie *bus) {
    HvVirtioUsb *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    /* virtio device type 0 is unused by the spec; Apple's transport matches
       on the PCI ID alone. Class: serial bus, USB device (0x0c03fe). */
    hv_virtio_init(&d->vio, m, 0, 0x0c03fe, 2, 0);
    d->vio.pci.config[0] = 0x6b, d->vio.pci.config[1] = 0x10;  /* vendor Apple */
    d->vio.pci.config[2] = 0x01, d->vio.pci.config[3] = 0x1a;  /* device 0x1a01 */
    d->vio.pci.config[0x2c] = 0x6b, d->vio.pci.config[0x2d] = 0x10;
    d->vio.pci.config[0x2e] = 0x01, d->vio.pci.config[0x2f] = 0x1a;
    d->vio.dev = d;
    d->vio.notify = notify;
    d->log = getenv("HV_VUSB_LOG") != NULL;
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    int slot = hv_pcie_attach(bus, &d->vio.pci);
    if (slot < 0) {
        free(d);
        return NULL;
    }
    if (d->log) fprintf(stderr, "virtio-usb: PCI slot %d\n", slot);
    return d;
}

int hv_vusb_online(HvVirtioUsb *d, uint32_t *session) {
    pthread_mutex_lock(&d->mu);
    int on = d->online;
    if (session) *session = d->session;
    pthread_mutex_unlock(&d->mu);
    return on;
}

int hv_vusb_send(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len) {
    HvVusbMsg m = {.type = type, .ep = ep, .len = len};
    if (len) {
        m.data = malloc(len);
        if (!m.data) return -1;
        if (data) memcpy(m.data, data, len);
        else memset(m.data, 0, len);
    }
    pthread_mutex_lock(&d->mu);
    int ok = d->online && d->out_tail - d->out_head < HV_VUSB_RING;
    if (ok) d->out[d->out_tail++ % HV_VUSB_RING] = m;
    pthread_mutex_unlock(&d->mu);
    if (!ok) {
        free(m.data);
        return -1;
    }
    hv_machine_wake(d->vio.m);
    return 0;
}

int hv_vusb_send_in(HvVirtioUsb *d, uint8_t ep, uint32_t len) {
    HvVusbMsg m = {.type = HV_VUSB_REQ_DATA, .ep = ep, .len = len};
    pthread_mutex_lock(&d->mu);
    int ok = d->online && d->out_tail - d->out_head < HV_VUSB_RING;
    if (ok) d->out[d->out_tail++ % HV_VUSB_RING] = m;
    pthread_mutex_unlock(&d->mu);
    if (ok) hv_machine_wake(d->vio.m);
    return ok ? 0 : -1;
}

int hv_vusb_recv(HvVirtioUsb *d, uint8_t ep, HvVusbMsg *out, int timeout_ms) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec until = {now.tv_sec + timeout_ms / 1000, now.tv_usec * 1000 + (long)(timeout_ms % 1000) * 1000000};
    if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
    pthread_mutex_lock(&d->mu);
    uint32_t session = d->session;
    for (;;) {
        if (!d->online || d->session != session) {
            pthread_mutex_unlock(&d->mu);
            return -1;
        }
        /* Take the first matching response, keeping the order of the rest. */
        for (uint32_t i = d->in_head; i != d->in_tail; i++) {
            HvVusbMsg *m = &d->in[i % HV_VUSB_RING];
            if (ep == 0xff || m->ep == ep || m->type == HV_VUSB_RSP_RESET) {
                *out = *m;
                for (uint32_t j = i; j != d->in_head; j--) d->in[j % HV_VUSB_RING] = d->in[(j - 1) % HV_VUSB_RING];
                d->in_head++;
                pthread_mutex_unlock(&d->mu);
                return 1;
            }
        }
        if (pthread_cond_timedwait(&d->cv, &d->mu, &until) == ETIMEDOUT) {
            pthread_mutex_unlock(&d->mu);
            return 0;
        }
    }
}

void hv_vusb_flush(HvVirtioUsb *d) {
    pthread_mutex_lock(&d->mu);
    drop_all_locked(d);
    pthread_mutex_unlock(&d->mu);
}

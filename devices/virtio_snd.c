#include "virtio_snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTLQ 0
#define EVENTQ 1
#define TXQ 2
#define RXQ 3

/* request codes */
#define R_JACK_INFO 1
#define R_JACK_REMAP 2
#define R_PCM_INFO 0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE 0x0102
#define R_PCM_RELEASE 0x0103
#define R_PCM_START 0x0104
#define R_PCM_STOP 0x0105
#define R_CHMAP_INFO 0x0200
/* status codes */
#define S_OK 0x8000
#define S_BAD_MSG 0x8001
#define S_NOT_SUPP 0x8002

#define CHMAP_FL 3
#define CHMAP_FR 4

static int logging(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_SND_LOG") != NULL;
    return on;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned format_bytes(unsigned f) {
    return f == HV_VSND_FMT_S16 ? 2 : 4;
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int i = 0; i < e->nseg; i++)
        if (!e->seg[i].write) n += e->seg[i].len;
    return n;
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ---- control queue ---- */

static uint32_t control(HvVirtioSnd *d, const HvVirtqElem *e) {
    HvVirtio *v = &d->vio;
    uint8_t req[32] = {0}, resp[4 + 32];
    uint32_t got = hv_virtq_read(v, e, 0, req, sizeof req);
    uint32_t code, status = S_OK, rlen = 4;
    memcpy(&code, req, 4);
    memset(resp, 0, sizeof resp);
    uint32_t start, count, size, stream;
    memcpy(&start, req + 4, 4);
    memcpy(&count, req + 8, 4);
    memcpy(&size, req + 12, 4);
    stream = start; /* virtio_snd_pcm_hdr: stream_id at the same place */
    if (got < 4) code = 0;
    switch (code) {
    case R_PCM_INFO:
    case R_CHMAP_INFO:
        if (start != 0 || count != 1 || size == 0 || size > 32) {
            status = S_BAD_MSG;
            break;
        }
        if (code == R_PCM_INFO) {
            uint64_t formats = 1ull << HV_VSND_FMT_S16 | 1ull << HV_VSND_FMT_S32 | 1ull << HV_VSND_FMT_FLOAT;
            uint64_t rates = 1ull << HV_VSND_RATE_44100 | 1ull << HV_VSND_RATE_48000;
            memcpy(resp + 4 + 8, &formats, 8);
            memcpy(resp + 4 + 16, &rates, 8);
            resp[4 + 24] = 0; /* output */
            resp[4 + 25] = 1; /* channels min */
            resp[4 + 26] = 2; /* channels max */
        } else {
            resp[4 + 4] = 0; /* output */
            resp[4 + 5] = 2;
            resp[4 + 6] = CHMAP_FL;
            resp[4 + 7] = CHMAP_FR;
        }
        rlen = 4 + size;
        break;
    case R_JACK_INFO: /* one line-out jack, always connected */
        if (start != 0 || count != 1 || size == 0 || size > 32) {
            status = S_BAD_MSG;
            break;
        }
        put32(resp + 4 + 8, 0x01014010); /* HDA pin default config: line out, rear, green jack */
        put32(resp + 4 + 12, 0x10);      /* pin capabilities: output */
        resp[4 + 16] = 1;                /* connected */
        rlen = 4 + size;
        break;
    case R_JACK_REMAP:
        break;
    case R_PCM_SET_PARAMS: {
        uint32_t buffer_bytes, period_bytes;
        memcpy(&buffer_bytes, req + 8, 4);
        memcpy(&period_bytes, req + 12, 4);
        unsigned ch = req[20], fmt = req[21], rate = req[22];
        if (stream != 0 || ch < 1 || ch > 2 || (fmt != HV_VSND_FMT_S16 && fmt != HV_VSND_FMT_S32 && fmt != HV_VSND_FMT_FLOAT) ||
            (rate != HV_VSND_RATE_44100 && rate != HV_VSND_RATE_48000) || d->running) {
            status = S_NOT_SUPP;
            break;
        }
        d->channels = ch;
        d->format = fmt;
        d->rate = rate;
        d->rate_hz = rate == HV_VSND_RATE_44100 ? 44100 : 48000;
        d->frame_bytes = ch * format_bytes(fmt);
        d->buffer_bytes = buffer_bytes;
        d->period_bytes = period_bytes;
        break;
    }
    case R_PCM_PREPARE:
        if (stream != 0 || !d->frame_bytes) status = S_BAD_MSG;
        else d->prepared = 1;
        break;
    case R_PCM_START:
        if (stream != 0 || !d->prepared) {
            status = S_BAD_MSG;
            break;
        }
        if (!d->running) {
            d->running = 1;
            d->clock_start_ns = now_ns();
            d->clock_start_consumed = atomic_load(&d->consumed);
            if (d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
                d->backend.start = NULL; /* the host cannot play it: drain by the clock instead */
        }
        break;
    case R_PCM_STOP:
        if (stream != 0) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        break;
    case R_PCM_RELEASE:
        if (stream != 0) {
            status = S_BAD_MSG;
            break;
        }
        if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
        d->running = 0;
        d->prepared = 0;
        /* Every buffer still held goes back now (the spec requires it). */
        for (; d->pcopy != d->ptail; d->pcopy++) d->pending_end[d->pcopy % HV_VSND_PENDING] = atomic_load(&d->written);
        atomic_store(&d->consumed, atomic_load(&d->written));
        break;
    default:
        status = S_NOT_SUPP;
        break;
    }
    if (logging())
        fprintf(stderr, "virtio-snd: request %#x [%u %u %u] -> %#x (ch %u fmt %u rate %u buffer %u period %u)\n", code, start, count, size, status,
                d->channels, d->format, d->rate_hz, d->buffer_bytes, d->period_bytes);
    put32(resp, status);
    return hv_virtq_write(v, e, 0, resp, rlen);
}

/* ---- transmit ---- */

/* Copy waiting buffers' audio into the ring while there is room. Their data is
   still in guest memory, so copying can wait. */

static void fill_ring(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    while (d->pcopy != d->ptail) {
        HvVirtqElem *e = &d->pending[d->pcopy % HV_VSND_PENDING];
        uint32_t len = readable(e);
        len = len > 4 ? len - 4 : 0;
        uint64_t w = atomic_load(&d->written);
        if (len > HV_VSND_RING) len = HV_VSND_RING;
        if (w + len - atomic_load(&d->consumed) > HV_VSND_RING) return; /* full: later */
        uint32_t off = 0;
        while (off < len) {
            uint32_t at = (uint32_t)((w + off) % HV_VSND_RING), n = len - off;
            if (n > HV_VSND_RING - at) n = HV_VSND_RING - at;
            hv_virtq_read(v, e, 4 + off, d->ring + at, n);
            off += n;
        }
        atomic_store_explicit(&d->written, w + len, memory_order_release);
        d->pending_end[d->pcopy % HV_VSND_PENDING] = w + len;
        d->pcopy++;
        d->xfers++;
    }
}

static void take_tx(HvVirtioSnd *d) {
    HvVirtio *v = &d->vio;
    if (!(v->status & 4) || !v->q[TXQ].enabled) return;
    while (d->ptail - d->phead < HV_VSND_PENDING) {
        if (hv_virtq_pop(v, TXQ, &d->pending[d->ptail % HV_VSND_PENDING]) <= 0) break;
        d->ptail++;
    }
    fill_ring(d);
}

void hv_virtio_snd_poll(HvVirtioSnd *d) {
    if (!d) return;
    HvVirtio *v = &d->vio;
    /* No backend: the stream plays at its rate by the wall clock. */
    if (d->running && !d->backend.start && d->frame_bytes) {
        uint64_t now = now_ns();
        uint64_t target = d->clock_start_consumed +
                          (now - d->clock_start_ns) * d->rate_hz / 1000000000ull * d->frame_bytes;
        uint64_t w = atomic_load(&d->written);
        if (target > w) { /* ran dry: restart the clock from here */
            target = w;
            d->clock_start_ns = now;
            d->clock_start_consumed = w;
        }
        if (target > atomic_load(&d->consumed)) atomic_store(&d->consumed, target);
    }
    take_tx(d);
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_acquire);
    int any = 0;
    while (d->phead != d->pcopy && d->pending_end[d->phead % HV_VSND_PENDING] <= c) {
        HvVirtqElem *e = &d->pending[d->phead % HV_VSND_PENDING];
        uint8_t st[8] = {0};
        put32(st, S_OK);
        uint64_t queued = atomic_load(&d->written) - c;
        put32(st + 4, (uint32_t)queued); /* latency_bytes */
        hv_virtq_write(v, e, 0, st, 8);
        hv_virtq_push(v, TXQ, e, 8);
        d->phead++;
        any = 1;
    }
    if (any) {
        hv_virtio_interrupt(v);
        take_tx(d); /* room for more */
    }
}

uint32_t hv_virtio_snd_pull(HvVirtioSnd *d, void *buf, uint32_t len) {
    uint64_t c = atomic_load_explicit(&d->consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&d->written, memory_order_acquire);
    uint32_t n = w - c < len ? (uint32_t)(w - c) : len;
    uint32_t off = 0;
    while (off < n) {
        uint32_t at = (uint32_t)((c + off) % HV_VSND_RING), k = n - off;
        if (k > HV_VSND_RING - at) k = HV_VSND_RING - at;
        memcpy((uint8_t *)buf + off, d->ring + at, k);
        off += k;
    }
    if (n < len) {
        memset((uint8_t *)buf + n, 0, len - n);
        if (d->running) d->underruns++;
    }
    /* compare-exchange: a release on the CPU thread may have moved it on */
    atomic_compare_exchange_strong(&d->consumed, &c, c + n);
    if (n) hv_machine_wake(d->vio.m);
    return n;
}

static void notify(HvVirtio *v, int q) {
    HvVirtioSnd *d = v->dev;
    static HvVirtqElem e;
    if (q == CTLQ) {
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            hv_virtq_push(v, q, &e, control(d, &e));
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
        hv_virtio_snd_poll(d); /* a release may have freed buffers */
    } else if (q == TXQ) {
        take_tx(d);
    } else if (q == RXQ) { /* no input stream: hand capture buffers straight back */
        int any = 0;
        while (hv_virtq_pop(v, q, &e) > 0) {
            uint8_t st[8] = {0};
            put32(st, S_BAD_MSG);
            hv_virtq_write(v, &e, 0, st, 8);
            hv_virtq_push(v, q, &e, 8);
            any = 1;
        }
        if (any) hv_virtio_interrupt(v);
    } /* event queue: kept for events we never send */
}

HvVirtioSnd *hv_virtio_snd_create(HvMachine *m, HvPcie *bus) {
    HvVirtioSnd *d = calloc(1, sizeof *d);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 25, 0x040100, 4, 0); /* sound device, multimedia audio */
    v->dev = d;
    v->notify = notify;
    uint32_t jacks = 1, streams = 1, chmaps = 1;
    memcpy(v->config, &jacks, 4);
    memcpy(v->config + 4, &streams, 4);
    memcpy(v->config + 8, &chmaps, 4);
    v->config_len = 12;
    d->ring = calloc(1, HV_VSND_RING);
    hv_pcie_attach(bus, &v->pci);
    return d;
}

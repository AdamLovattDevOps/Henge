#ifndef HENGEVM_VIRTIO_INPUT_H
#define HENGEVM_VIRTIO_INPUT_H

#include <pthread.h>

#include "virtio.h"

/* virtio-input (device type 18), Linux input event codes, as macOS's
   AppleVirtIOInput drivers expect: it calls a device with relative X/Y a
   mouse, one with absolute X/Y a trackpad, and one with keys but no left
   button a keyboard. Events come from the host UI on any thread
   (hv_virtio_input_send) and reach the guest from the CPU thread
   (hv_virtio_input_poll, from the board's tick). */
enum { HV_VINPUT_KEYBOARD, HV_VINPUT_MOUSE };

#define HV_EV_SYN 0
#define HV_EV_KEY 1
#define HV_EV_REL 2
#define HV_REL_X 0
#define HV_REL_Y 1
#define HV_REL_HWHEEL 6
#define HV_REL_WHEEL 8
#define HV_BTN_LEFT 0x110
#define HV_BTN_RIGHT 0x111
#define HV_BTN_MIDDLE 0x112

#define HV_VINPUT_RING 4096

typedef struct HvVirtioInput {
    HvVirtio vio;
    int kind;
    pthread_mutex_t mu;
    struct { uint16_t type, code; uint32_t value; } ring[HV_VINPUT_RING];
    uint32_t head, tail; /* under mu */
    uint64_t delivered, dropped;
} HvVirtioInput;

HvVirtioInput *hv_virtio_input_create(HvMachine *m, HvPcie *bus, int kind);
/* Any thread: queue one event (send HV_EV_SYN, 0, 0 after each group). */
void hv_virtio_input_send(HvVirtioInput *d, uint16_t type, uint16_t code, int32_t value);
/* CPU thread: hand queued events to the guest's buffers. */
void hv_virtio_input_poll(HvVirtioInput *d);

#endif

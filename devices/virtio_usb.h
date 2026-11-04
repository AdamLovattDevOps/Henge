#ifndef HENGEVM_VIRTIO_USB_H
#define HENGEVM_VIRTIO_USB_H

#include <pthread.h>
#include <stdint.h>

#include "virtio.h"

/* Apple's virtio USB device-mode controller (PCI 0x106b:0x1a01), the port a
   virtual Mac uses to show up as a USB device (DFU, recovery, restore) to the
   machine that installs it. The guest is the USB device; the host side here
   plays the USB host.

   Two queues. Queue 0 (requests, host -> guest): the guest posts writable
   buffers and the host fills one per request with a 6-byte header
   {u32 len, u8 endpoint, u8 type} and `len` bytes of payload:
     type 1 on endpoint 0: a setup packet (8 bytes), then the OUT data stage;
     type 1 on another OUT endpoint: the data; on an IN endpoint (bit 7): no
       payload, len is how much the host can take;
     type 2: bus reset; type 3: pause the endpoint.
   Queue 1 (responses, guest -> host): readable buffers with a 2-byte header
   {u8 type, u8 endpoint} and data: type 1 data (IN data, or empty for a
   finished OUT transfer or control status), 2 stall, 3 abort, 4 reset done,
   5 paused. The guest connects (goes on the bus) when it sets DRIVER_OK and
   disconnects by resetting the device. */

#define HV_VUSB_REQ_DATA 1
#define HV_VUSB_REQ_RESET 2
#define HV_VUSB_REQ_PAUSE 3

#define HV_VUSB_RSP_DATA 1
#define HV_VUSB_RSP_STALL 2
#define HV_VUSB_RSP_ABORT 3
#define HV_VUSB_RSP_RESET 4
#define HV_VUSB_RSP_PAUSED 5

#define HV_VUSB_RING 256

typedef struct HvVusbMsg {
    uint8_t type, ep;
    uint32_t len;
    uint8_t *data; /* malloc'd, len bytes; NULL for an IN request (len = how much the host takes) */
} HvVusbMsg;

typedef struct HvVirtioUsb {
    HvVirtio vio;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    HvVusbMsg out[HV_VUSB_RING];   /* requests waiting for a guest buffer */
    uint32_t out_head, out_tail;
    HvVusbMsg in[HV_VUSB_RING];    /* responses waiting for the host */
    uint32_t in_head, in_tail;
    uint32_t session;              /* bumps on every connect and disconnect */
    int online;                    /* a front-end is connected */
    int pci_on;                    /* the PCI front-end saw DRIVER_OK */
    int rom_on;                    /* the ROM's front-end (bdif) owns the port; off once the kernel's driver takes it */
    int log;
} HvVirtioUsb;

HvVirtioUsb *hv_virtio_usb_create(HvMachine *m, HvPcie *bus);
/* CPU thread (board tick): move queued requests into guest buffers. */
void hv_virtio_usb_poll(HvVirtioUsb *d);

/* Host side, any thread. */
int hv_vusb_online(HvVirtioUsb *d, uint32_t *session);
/* Queue a request; data may be NULL. Returns 0, or -1 if the ring is full. */
int hv_vusb_send(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len);
/* An IN transfer request: header only, len = the most the host takes. */
int hv_vusb_send_in(HvVirtioUsb *d, uint8_t ep, uint32_t len);
/* Wait up to timeout_ms for the next response on `ep` (or any endpoint if
   ep == 0xff; reset responses always match). 1: got one (free m->data),
   0: timed out, -1: the guest disconnected. */
int hv_vusb_recv(HvVirtioUsb *d, uint8_t ep, HvVusbMsg *m, int timeout_ms);
/* Drop queued requests and responses (after a reset or reconnect). */
void hv_vusb_flush(HvVirtioUsb *d);

/* For the boot ROM's front-end (bdif channel 0x100000, the same messages
   over Apple's MMIO transport): connect or disconnect, take the next request
   (1 if there was one; free m->data), hand over a response. */
void hv_vusb_set_online(HvVirtioUsb *d, int on);
int hv_vusb_take_request(HvVirtioUsb *d, HvVusbMsg *m);
void hv_vusb_put_response(HvVirtioUsb *d, uint8_t type, uint8_t ep, const void *data, uint32_t len);

#endif

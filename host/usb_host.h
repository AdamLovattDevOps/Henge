#ifndef HENGEVM_USB_HOST_H
#define HENGEVM_USB_HOST_H

#include <stdint.h>

#include "virtio_usb.h"

/* The USB host side of the virtual Mac's device port: synchronous transfers
   over the virtio-usb messages. Returns bytes transferred, or a negative
   HV_USB_E* code. */
#define HV_USB_ETIMEOUT -1
#define HV_USB_ESTALL -2
#define HV_USB_EGONE -3   /* the guest left the bus (rebooting into the next stage) */
#define HV_USB_EIO -4

int hv_usb_reset(HvVirtioUsb *d, int timeout_ms);
int hv_usb_control(HvVirtioUsb *d, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                   void *data, uint16_t wLength, int timeout_ms);
int hv_usb_bulk(HvVirtioUsb *d, uint8_t ep, void *data, uint32_t len, int timeout_ms);
/* Wait until the guest is on the bus; returns its session number, or -1. */
int64_t hv_usb_wait_online(HvVirtioUsb *d, int timeout_ms);

/* host/libusb_shim.c: the device its libusb API drives. */
void hv_libusb_shim_attach(HvVirtioUsb *d);

/* Debugging (HV_VUSB_PROBE=1): a thread that reads the device's descriptors. */
void hv_usb_probe_start(HvVirtioUsb *d);

#endif

/* Minimal USB full-speed MIDI device.
 * The sequencer sends on virtual cable 1 and the host sends to the device on
 * cable 2 (board contract; the directions are not symmetric). Enumeration is
 * polled: the host model issues SET_ADDRESS, GET_DESCRIPTOR(configuration)
 * and SET_CONFIGURATION after reset recovery. */
#include "usb.h"
#include "board.h"

void midi_rx_usb(uint8_t byte); /* provided by midi.c */

#define EP_MIDI 1

static uint8_t setup[8];
static bool configured;

static const uint8_t config_descriptor[] = {
    /* configuration */
    9, 2, 53, 0, 1, 1, 0, 0x80, 50,
    /* interface: audio class, MIDI streaming subclass, two endpoints */
    9, 4, 0, 0, 2, 1, 3, 0, 0,
    /* class-specific MIDI streaming header */
    7, 0x24, 1, 0, 1, 25, 0,
    /* bulk OUT endpoint 0x01 */
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
    /* bulk IN endpoint 0x81 */
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
};

static const uint8_t device_descriptor[18] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1,
};

bool usb_configured(void) { return configured; }

static void ep_send(int ep, const uint8_t *data, int len)
{
    USB_DIEPTSIZ(ep) = (uint32_t)len;
    if (!len) {
        USB_DIEPCTL(ep) = 0x80000000u;
        return;
    }
    for (int i = 0; i < len; i += 4) {
        uint32_t word = 0;
        for (int b = 0; b < 4 && i + b < len; b++)
            word |= (uint32_t)data[i + b] << (8 * b);
        USB_FIFO(ep) = word;
    }
}

static void handle_setup(void)
{
    uint8_t request = setup[1];
    if ((setup[0] & 0x80) && request == 6) { /* GET_DESCRIPTOR */
        uint16_t wanted = (uint16_t)(setup[6] | (setup[7] << 8));
        const uint8_t *data = 0;
        int len = 0;
        if (setup[3] == 2) { data = config_descriptor; len = sizeof(config_descriptor); }
        else if (setup[3] == 1) { data = device_descriptor; len = sizeof(device_descriptor); }
        if (len > wanted) len = wanted;
        ep_send(0, data, len);
        return;
    }
    if (request == 9) { /* SET_CONFIGURATION */
        USB_DOEPCTL(EP_MIDI) = 0x8000;
        USB_DIEPCTL(EP_MIDI) = 0x8000;
        configured = true;
    }
    ep_send(0, 0, 0); /* status stage for OUT-direction requests */
}

static void handle_out_packets(const uint8_t *data, int len)
{
    static const uint8_t CIN_LEN[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    for (int i = 0; i + 4 <= len; i += 4) {
        int cable = data[i] >> 4, n = CIN_LEN[data[i] & 15];
        if (cable != 2) continue; /* host-to-device cable per contract */
        for (int b = 0; b < n; b++)
            midi_rx_usb(data[i + 1 + b]);
    }
}

void usb_poll(void)
{
    uint32_t status = USB_GINTSTS;
    if (status & USB_INT_USBRST) { USB_GINTSTS = USB_INT_USBRST; configured = false; }
    if (status & USB_INT_ENUMDNE) USB_GINTSTS = USB_INT_ENUMDNE;
    if (status & USB_INT_SOF) USB_GINTSTS = USB_INT_SOF;
    while (USB_GINTSTS & USB_INT_RXFLVL) {
        uint32_t entry = USB_GRXSTSP;
        int ep = (int)(entry & 15);
        int len = (int)((entry >> 4) & 0x7ff);
        int kind = (int)((entry >> 17) & 15);
        uint8_t data[64];
        if (len > (int)sizeof(data)) len = sizeof(data); /* 64-byte endpoint */
        for (int i = 0; i < len; i += 4) {
            uint32_t word = USB_FIFO(0);
            for (int b = 0; b < 4 && i + b < len; b++)
                data[i + b] = (uint8_t)(word >> (8 * b));
        }
        if (kind == 6 && len == 8) {
            for (int i = 0; i < 8; i++) setup[i] = data[i];
        } else if (kind == 4) {
            handle_setup();
        } else if (kind == 2 && ep == EP_MIDI) {
            handle_out_packets(data, len);
        }
    }
}

void usb_midi_send(const uint8_t *msg, int len)
{
    if (!configured || len <= 0) return;
    uint8_t packets[256];
    int at = 0;
    if (msg[0] == 0xf0) {
        for (int i = 0; i < len && at + 4 <= (int)sizeof(packets); i += 3) {
            int chunk = len - i < 3 ? len - i : 3;
            packets[at++] = (uint8_t)(0x10 | (i + 3 < len ? 4 : 4 + chunk));
            for (int b = 0; b < 3; b++)
                packets[at++] = b < chunk ? msg[i + b] : 0;
        }
    } else if (msg[0] >= 0xf8) {
        packets[at++] = 0x1f; packets[at++] = msg[0]; packets[at++] = 0; packets[at++] = 0;
    } else {
        packets[at++] = (uint8_t)(0x10 | (msg[0] >> 4));
        for (int b = 0; b < 3; b++)
            packets[at++] = b < len ? msg[b] : 0;
    }
    USB_DIEPTSIZ(EP_MIDI) = (uint32_t)at;
    for (int i = 0; i < at; i += 4)
        USB_FIFO(EP_MIDI) = (uint32_t)packets[i] | ((uint32_t)packets[i + 1] << 8)
                          | ((uint32_t)packets[i + 2] << 16) | ((uint32_t)packets[i + 3] << 24);
}

void usb_init(void)
{
    configured = false;
    USB_GINTMSK = USB_INT_USBRST | USB_INT_ENUMDNE | USB_INT_RXFLVL | USB_INT_SOF;
    USB_GAHBCFG = 0; /* polled operation */
    USB_DCTL = 0;    /* clear soft disconnect: present the device */
}

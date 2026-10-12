#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stdint.h>

void usb_init(void);
void usb_poll(void);  /* enumeration and received MIDI (feeds midi module) */
void usb_midi_send(const uint8_t *msg, int len);  /* one complete message */
bool usb_configured(void);

#endif

/* MIDI transport glue: DIN UART and USB, with a bounded SysEx framer per
 * source. Realtime bytes are dispatched immediately and never disturb an
 * in-progress SysEx frame. */
#include <stdbool.h>

#include "midi.h"
#include "board.h"
#include "usb.h"

#define SYSEX_MAX 1400

typedef struct {
    uint8_t buffer[SYSEX_MAX];
    int length;
    bool active;
} framer_t;

static framer_t framers[2];
static midi_sysex_fn sysex_handler;
static midi_realtime_fn realtime_handler;

static void feed(int source, uint8_t byte)
{
    framer_t *fr = &framers[source];
    if (byte >= 0xf8) {
        if (realtime_handler) realtime_handler(byte);
        return;
    }
    if (byte == 0xf0) {
        fr->length = 0;
        fr->buffer[fr->length++] = byte;
        fr->active = true;
    } else if (byte == 0xf7) {
        if (fr->active && fr->length < SYSEX_MAX) {
            fr->buffer[fr->length++] = byte;
            if (sysex_handler) sysex_handler(fr->buffer, fr->length, source);
        }
        fr->active = false;
    } else if (byte & 0x80) {
        fr->active = false; /* another status byte aborts the frame */
    } else if (fr->active) {
        if (fr->length < SYSEX_MAX - 1) fr->buffer[fr->length++] = byte;
        else fr->active = false; /* oversized input: drop the frame */
    }
}

void midi_rx_usb(uint8_t byte) { feed(MIDI_SRC_USB, byte); }

void midi_init(midi_sysex_fn on_sysex, midi_realtime_fn on_realtime)
{
    sysex_handler = on_sysex;
    realtime_handler = on_realtime;
    framers[0].active = framers[1].active = false;
    usb_init();
}

void midi_poll(void)
{
    usb_poll();
    while (REG32(UART_DIN) & UART_SR_RXNE)
        feed(MIDI_SRC_DIN, (uint8_t)REG32(UART_DIN + 4));
}

void midi_send_to(int source, const uint8_t *msg, int len)
{
    if (source == MIDI_SRC_USB) {
        usb_midi_send(msg, len);
    } else {
        for (int i = 0; i < len; i++)
            REG32(UART_DIN + 4) = msg[i];
    }
}

void midi_send(const uint8_t *msg, int len)
{
    midi_send_to(MIDI_SRC_USB, msg, len);
    midi_send_to(MIDI_SRC_DIN, msg, len);
}

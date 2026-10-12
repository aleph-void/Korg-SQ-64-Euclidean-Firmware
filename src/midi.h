#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>

#define MIDI_SRC_USB 0
#define MIDI_SRC_DIN 1

/* Complete SysEx frame (F0..F7) and single realtime byte handlers. */
typedef void (*midi_sysex_fn)(const uint8_t *frame, int len, int source);
typedef void (*midi_realtime_fn)(uint8_t byte);

void midi_init(midi_sysex_fn on_sysex, midi_realtime_fn on_realtime);
void midi_poll(void);                                 /* drain DIN and USB */
void midi_send(const uint8_t *msg, int len);          /* both transports   */
void midi_send_to(int source, const uint8_t *msg, int len);

#endif

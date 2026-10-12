#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <stdbool.h>
#include <stdint.h>

void outputs_init(void);
void gate_set(int track /*0..2*/, bool active);
void cv_pitch(int track /*0..2*/, uint8_t note);  /* MIDI note 0..127 */
void cv_mod(int track /*0..2*/, uint8_t level);   /* 0..255 */
void outputs_all_off(void);

#endif

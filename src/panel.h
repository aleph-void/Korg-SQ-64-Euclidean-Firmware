#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t sw;   /* (row << 5) | bit */
    bool down;
} panel_event_t;

/* Polled startup scan (before DMA starts): returns the eight decoded
 * 32-bit row values, or all-zero rows when the matrix reads invalid. */
void panel_startup_scan(uint32_t rows[8]);

void panel_init(void);          /* start the circular DMA scan            */
void panel_poll(void);          /* decode any completed scan half-buffers */
bool panel_event_next(panel_event_t *out);
bool panel_down(uint8_t sw);
int panel_encoder_take(int encoder);  /* detents since the last call      */

/* Outputs multiplexed into the scan TX frames. */
void panel_led_columns(const uint32_t rows[8]); /* 21 column bits per row  */
void panel_triggers(uint8_t mask);              /* D1..D8, bit set = fire */
void panel_transport_leds(bool tap, bool stop, bool record, bool play);

#endif

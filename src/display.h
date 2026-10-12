#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

void display_init(void);
void fb_clear(void);
void fb_pixel(int x, int y, uint8_t level);         /* level 0..15 */
void fb_fill(int x, int y, int w, int h, uint8_t level);
void fb_text(int x, int y, const char *s, uint8_t level);
void fb_number(int x, int y, int value, uint8_t level);
void display_flush(void);  /* queue the framebuffer for DMA transfer */
void display_pump(void);   /* start a queued transfer when DMA is idle */
uint8_t *display_framebuffer(void);

#endif

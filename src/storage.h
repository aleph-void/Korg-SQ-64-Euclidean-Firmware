#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* External SPI NOR primitives. Addresses below 0x30000 hold the installed
 * firmware image and are written only by the updater during an installation;
 * all application data lives at or above SETTINGS_ADDR. */
void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length);
void spi_flash_erase_4k(uint32_t address);
void spi_flash_erase_64k(uint32_t address);
void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length);

#define SETTINGS_ADDR 0x40000u

typedef struct {
    uint8_t length;   /* 1..16 steps */
    uint8_t pulses;   /* 0..length  */
    uint8_t rotate;   /* 0..length-1 */
    uint8_t note;     /* MIDI note  */
    uint8_t gate_pct; /* 5..95 % of a step */
} track_cfg_t;

typedef struct {
    uint32_t magic;
    uint16_t bpm;      /* 20..300 */
    uint8_t swing;     /* 50..75 % */
    uint8_t selected;  /* edited track 0..3 */
    track_cfg_t track[4];
    uint32_t hash;
} settings_t;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);

#endif

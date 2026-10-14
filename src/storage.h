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
    uint8_t length;   /* 1..64 steps (four bars of 16) */
    uint8_t pulses;   /* 0..length  */
    uint8_t rotate;   /* 0..length-1 */
    uint8_t note;     /* MIDI note  */
    uint8_t gate_pct; /* 5..95 % of a step */
    uint8_t velocity; /* 1..127 note-on velocity */
    uint8_t mod;      /* 0..15 mod CV level while the gate is open */
} track_cfg_t;

/* Rhythm algorithm per track. EUCLID generates E(pulses, length) rotated by
 * rotate. SHIFT runs a shift register seeded from the unrotated Euclidean
 * pattern: it rotates one step every cycle, and each cycle flips one random
 * bit with probability rotate/length — so rotate is the flip chance in SHIFT
 * mode (0 = pure rotation). The register itself is RAM state, reseeded from
 * the track's Euclidean parameters on load. */
enum { ALGO_EUCLID, ALGO_SHIFT };

typedef struct {
    uint32_t magic;
    uint16_t bpm;      /* 20..300 */
    uint8_t swing;     /* 50..75 % */
    uint8_t selected;  /* edited track 0..3 */
    track_cfg_t track[4];
    uint8_t algo[4];   /* ALGO_* per track */
    uint32_t hash;
} settings_t;

void settings_defaults(settings_t *s);
bool settings_load(settings_t *s);
void settings_save(const settings_t *s);

/* Saved project slots (board contract): slot n is the 0x3d000-byte region at
 * 0xc0000 + n*0x3d000, the same catalog stock firmware uses for its saved
 * projects. This firmware stores one project record (the workspace: patterns,
 * tempo, swing) in the first 4 KiB sector of a slot and never touches the
 * rest of the region. Slots past the end of the fitted SPI part (JEDEC
 * capacity) are unavailable rather than silently aliased. */
#define PROJECT_SLOTS 64
#define PROJECT_BASE 0xc0000u
#define PROJECT_SLOT_BYTES 0x3d000u
#define PROJECT_ADDR(slot) (PROJECT_BASE + (uint32_t)(slot) * PROJECT_SLOT_BYTES)

typedef enum {
    SLOT_EMPTY,       /* erased: no record            */
    SLOT_VALID,       /* committed project record     */
    SLOT_INVALID,     /* unreadable: never loaded     */
    SLOT_UNAVAILABLE, /* beyond the fitted SPI part   */
} slot_state_t;

int project_slots_available(void);
slot_state_t project_state(int slot);
/* Copies the record into *out only when the slot is SLOT_VALID. */
slot_state_t project_load(int slot, settings_t *out);
/* Both report success only after the stored bytes read back verified. */
bool project_save(int slot, const settings_t *s);
bool project_delete(int slot);

#endif

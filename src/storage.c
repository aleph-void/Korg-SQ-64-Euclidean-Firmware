/* Polled SPI NOR driver (chip select on PB9) and settings persistence.
 * Commands used: 0x06 WREN, 0x03 read, 0x02 page program, 0x20 4K erase,
 * 0xd8 64K erase. Settings occupy one 4K sector well above the installed
 * firmware region, inside this firmware's declared user-data range. */
#include <stddef.h>

#include "storage.h"
#include "board.h"

int memcmp(const void *a, const void *b, size_t n); /* startup.c */

/* "EUC3"/"EUP3": version 3 of the record format (v2 lacked the per-track
 * algorithm byte; its records are rejected as invalid rather than loaded). */
#define SETTINGS_MAGIC 0x33435545u
#define PROJECT_MAGIC  0x33505545u

static void cs(bool selected)
{
    GPIO_BSRR(GPIOB) = selected ? 1u << (16 + PIN_FLASH_CS) : 1u << PIN_FLASH_CS;
}

static uint8_t xfer(uint8_t value)
{
    REG8(SPI_STORE_DR) = value;
    while (!(REG32(SPI_STORE_SR) & SPI_SR_RXNE)) { }
    return REG8(SPI_STORE_DR);
}

static void command_address(uint8_t command, uint32_t address)
{
    xfer(command);
    xfer((uint8_t)(address >> 16));
    xfer((uint8_t)(address >> 8));
    xfer((uint8_t)address);
}

static void write_enable(void)
{
    cs(true);
    xfer(0x06);
    cs(false);
}

void spi_flash_read(uint32_t address, uint8_t *data, uint32_t length)
{
    cs(true);
    command_address(0x03, address);
    while (length--) *data++ = xfer(0);
    cs(false);
}

void spi_flash_erase_4k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0x20, address);
    cs(false);
}

void spi_flash_erase_64k(uint32_t address)
{
    write_enable();
    cs(true);
    command_address(0xd8, address);
    cs(false);
}

void spi_flash_program(uint32_t address, const uint8_t *data, uint32_t length)
{
    while (length) {
        uint32_t page_room = 256 - (address & 255);
        uint32_t chunk = length < page_room ? length : page_room;
        write_enable();
        cs(true);
        command_address(0x02, address);
        for (uint32_t i = 0; i < chunk; i++) xfer(data[i]);
        cs(false);
        address += chunk;
        data += chunk;
        length -= chunk;
    }
}

static uint32_t settings_hash(const settings_t *s)
{
    const uint8_t *bytes = (const uint8_t *)s;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < sizeof(*s) - sizeof(uint32_t); i++)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

void settings_defaults(settings_t *s)
{
    static const track_cfg_t defaults[4] = {
        {16, 4, 0, 60, 50, 100, 13}, /* A: four on the floor, C4  */
        {16, 3, 2, 48, 50, 100, 13}, /* B */
        {12, 5, 0, 36, 50, 100, 13}, /* C */
        {16, 7, 0, 38, 50, 100, 13}, /* D: triggers               */
    };
    s->magic = SETTINGS_MAGIC;
    s->bpm = 120;
    s->swing = 50;
    s->selected = 0;
    for (int t = 0; t < 4; t++) {
        s->track[t] = defaults[t];
        s->algo[t] = ALGO_EUCLID;
    }
    s->hash = settings_hash(s);
}

static bool record_valid(const settings_t *s, uint32_t magic)
{
    if (s->magic != magic || s->hash != settings_hash(s)) return false;
    if (s->bpm < 20 || s->bpm > 300 || s->swing < 50 || s->swing > 75 || s->selected > 3)
        return false;
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *c = &s->track[t];
        if (c->length < 1 || c->length > 64 || c->pulses > c->length
            || c->rotate >= c->length || c->note > 127
            || c->gate_pct < 5 || c->gate_pct > 95
            || c->velocity < 1 || c->velocity > 127 || c->mod > 15)
            return false;
        if (s->algo[t] > ALGO_SHIFT) return false;
    }
    return true;
}

bool settings_load(settings_t *s)
{
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)s, sizeof(*s));
    if (record_valid(s, SETTINGS_MAGIC)) return true;
    settings_defaults(s);
    return false;
}

void settings_save(const settings_t *s)
{
    settings_t copy = *s;
    copy.magic = SETTINGS_MAGIC;
    copy.hash = settings_hash(&copy);
    /* Skip the erase/program cycle when the stored bytes already match. */
    settings_t stored;
    spi_flash_read(SETTINGS_ADDR, (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return;
    spi_flash_erase_4k(SETTINGS_ADDR);
    spi_flash_program(SETTINGS_ADDR, (const uint8_t *)&copy, sizeof(copy));
}

/* The board contract fixes the 64-slot catalog geometry but not the fitted
 * SPI part; the JEDEC capacity byte (2^n bytes) bounds which slots exist on
 * this device. The native emulator model fits a 1 MiB part (one whole slot),
 * the browser model 16 MiB (all 64). Unknown capacities fall back to 1 MiB
 * rather than risking address wraparound into other data. */
int project_slots_available(void)
{
    static int available = -1;
    if (available >= 0) return available;
    cs(true);
    xfer(0x9f);
    xfer(0); /* manufacturer */
    xfer(0); /* type */
    uint8_t capacity = xfer(0);
    cs(false);
    uint32_t bytes = (capacity >= 20 && capacity <= 24) ? 1u << capacity : 1u << 20;
    available = bytes <= PROJECT_BASE ? 0 : (int)((bytes - PROJECT_BASE) / PROJECT_SLOT_BYTES);
    if (available > PROJECT_SLOTS) available = PROJECT_SLOTS;
    return available;
}

static slot_state_t slot_read(int slot, settings_t *record)
{
    if (slot < 0 || slot >= project_slots_available()) return SLOT_UNAVAILABLE;
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)record, sizeof(*record));
    if (record_valid(record, PROJECT_MAGIC)) return SLOT_VALID;
    const uint8_t *bytes = (const uint8_t *)record;
    for (uint32_t i = 0; i < sizeof(*record); i++)
        if (bytes[i] != 0xff) return SLOT_INVALID;
    return SLOT_EMPTY;
}

slot_state_t project_state(int slot)
{
    settings_t record;
    return slot_read(slot, &record);
}

slot_state_t project_load(int slot, settings_t *out)
{
    settings_t record;
    slot_state_t state = slot_read(slot, &record);
    if (state == SLOT_VALID) *out = record;
    return state;
}

bool project_save(int slot, const settings_t *s)
{
    if (slot < 0 || slot >= project_slots_available()) return false;
    settings_t copy = *s;
    copy.magic = PROJECT_MAGIC;
    copy.hash = settings_hash(&copy);
    settings_t stored;
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)&stored, sizeof(stored));
    if (memcmp(&stored, &copy, sizeof(copy)) == 0) return true;
    spi_flash_erase_4k(PROJECT_ADDR(slot));
    spi_flash_program(PROJECT_ADDR(slot), (const uint8_t *)&copy, sizeof(copy));
    spi_flash_read(PROJECT_ADDR(slot), (uint8_t *)&stored, sizeof(stored));
    return memcmp(&stored, &copy, sizeof(copy)) == 0;
}

bool project_delete(int slot)
{
    settings_t record;
    slot_state_t state = slot_read(slot, &record);
    if (state == SLOT_UNAVAILABLE) return false;
    if (state == SLOT_EMPTY) return true; /* repeated delete: already empty */
    spi_flash_erase_4k(PROJECT_ADDR(slot));
    state = slot_read(slot, &record);
    return state == SLOT_EMPTY;
}

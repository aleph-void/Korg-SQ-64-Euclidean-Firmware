/* System update receiver (VUP SysEx protocol 1) and version display.
 *
 * The full 0x38000-byte system payload is collected into RAM, validated, and
 * only then programmed: internal flash sectors 0..3 followed by the three
 * installed SPI regions. An error or interruption before the final packet
 * leaves every storage byte untouched. Power loss while the internal flash
 * is actually being programmed is NOT guaranteed to be recoverable (VUP-008);
 * keep the device powered until completion is displayed.
 */
#include <stdbool.h>

#include "updater.h"
#include "board.h"
#include "display.h"
#include "midi.h"
#include "panel.h"
#include "storage.h"

#define PAYLOAD_SIZE 0x38000u
#define PACKET_SIZE 1024u
#define PACKET_COUNT (PAYLOAD_SIZE / PACKET_SIZE)
#define PACKED_SIZE (PACKET_SIZE + (PACKET_SIZE + 6) / 7) /* 1171 */

/* NAK codes from the protocol's acknowledgement error table. */
#define ERR_FORMAT 3
#define ERR_BLOCK 4
#define ERR_PACKET 5
#define ERR_CHECKSUM 6
#define ERR_ABORTED 10
#define ERR_VERSION 11

extern uint8_t _update_buffer[]; /* 0x38000 bytes reserved by the linker */

static const uint8_t PREFIX[6] = {0xf0, 0x42, 0x23, 0x00, 0x01, 0x60};

static enum { IDLE, RECEIVING, COMPLETE } state;
static uint32_t next_packet;
static bool install_allowed; /* true only when booted into update mode */

static void status_line(const char *line1, const char *line2)
{
    fb_clear();
    fb_text(4, 4, "EUCLID SQ-64", 15);
    fb_text(4, 20, line1, 12);
    if (line2) fb_text(4, 32, line2, 9);
    display_flush();
}

static void reply(int source, uint8_t command, const uint8_t *body, int body_len)
{
    uint8_t frame[20];
    for (int i = 0; i < 6; i++) frame[i] = PREFIX[i];
    frame[6] = command;
    for (int i = 0; i < body_len; i++) frame[7 + i] = body[i];
    frame[7 + body_len] = 0xf7;
    midi_send_to(source, frame, body_len + 8);
}

static void ack(int source) { reply(source, 5, (const uint8_t[]){0}, 1); }

static void nak(int source, uint8_t code) { reply(source, 5, &code, 1); }

static void progress(int source, uint32_t done)
{
    uint8_t body[4] = {
        (uint8_t)((done >> 7) & 127), (uint8_t)(done & 127),
        (uint8_t)((PACKET_COUNT >> 7) & 127), (uint8_t)(PACKET_COUNT & 127),
    };
    reply(source, 0x13, body, 4);
}

static uint32_t read_u32(const uint8_t *p) /* big-endian base-128 */
{
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) | ((uint32_t)p[2] << 7) | p[3];
}

static bool payload_valid(const uint8_t *payload)
{
    static const uint32_t regions[4][2] = {
        {0x00000, 0x10000}, {0x10000, 0x4000}, {0x14000, 0xbdd8}, {0x20000, 0x14000},
    };
    uint32_t sp = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
                | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    uint32_t reset = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8)
                   | ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);
    if (sp <= SRAM_BASE || sp > SRAM_BASE + SRAM_SIZE || sp % 8) return false;
    if (!(reset & 1) || (reset & ~1u) < INTERNAL_FLASH_BASE + 8
        || (reset & ~1u) >= INTERNAL_FLASH_BASE + INTERNAL_FLASH_SIZE) return false;
    for (int r = 0; r < 4; r++) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < regions[r][1]; i += 2)
            sum += (uint32_t)payload[regions[r][0] + i]
                 | ((uint32_t)payload[regions[r][0] + i + 1] << 8);
        if (sum & 0xffff) return false;
    }
    return true;
}

/* The installer below executes from RAM: erasing internal flash sectors 0..3
 * destroys the code that received the update, so nothing may run from or
 * return to flash once programming starts. It touches only registers, its
 * stack and RAM buffers, acknowledges the final packet, triggers the
 * pre-rendered completion screen and then idles forever.
 */
#define RAM_XFER(value, out) \
    do { \
        REG8(SPI_STORE_DR) = (uint8_t)(value); \
        while (!(REG32(SPI_STORE_SR) & SPI_SR_RXNE)) { } \
        (out) = REG8(SPI_STORE_DR); \
    } while (0)

__attribute__((noinline, section(".data.ramfunc"), optimize("no-tree-loop-distribute-patterns")))
static void ram_install(const uint8_t *payload, int source, uint8_t *frame_buffer)
{
    uint8_t scratch;
    (void)scratch; /* every SPI transmit must pop its received byte */
    /* Internal flash: erase application sectors 0..3, program the first
     * 64 KiB of the payload word by word through the programming alias. */
    REG32(FLASH_KEYR) = FLASH_KEY1;
    REG32(FLASH_KEYR) = FLASH_KEY2;
    for (uint32_t sector = 0; sector < 4; sector++)
        REG32(FLASH_CR) = FLASH_CR_SER | (sector << 3) | FLASH_CR_STRT;
    REG32(FLASH_CR) = FLASH_CR_PG;
    const uint32_t *words = (const uint32_t *)payload; /* buffer is aligned */
    for (uint32_t i = 0; i < INTERNAL_FLASH_SIZE / 4; i++)
        REG32(INTERNAL_FLASH_ALIAS + i * 4) = words[i];
    REG32(FLASH_CR) = FLASH_CR_LOCK;

    /* External SPI: erase the three installed 64 KiB sectors, then program
     * every non-blank 256-byte page of the three payload regions. */
    const uint32_t regions[3][3] = {
        {0x10000, 0x00000, 0x4000},
        {0x34000, 0x04000, 0x4000},
        {0x14000, 0x10000, 0x20000},
    };
    for (uint32_t sector = 0; sector < 0x30000; sector += 0x10000) {
        GPIO_BSRR(GPIOB) = 1u << (16 + PIN_FLASH_CS);
        RAM_XFER(0x06, scratch);
        GPIO_BSRR(GPIOB) = 1u << PIN_FLASH_CS;
        GPIO_BSRR(GPIOB) = 1u << (16 + PIN_FLASH_CS);
        RAM_XFER(0xd8, scratch);
        RAM_XFER(sector >> 16, scratch);
        RAM_XFER(sector >> 8, scratch);
        RAM_XFER(sector, scratch);
        GPIO_BSRR(GPIOB) = 1u << PIN_FLASH_CS;
    }
    for (int r = 0; r < 3; r++)
        for (uint32_t page = 0; page < regions[r][2]; page += 256) {
            const uint8_t *data = payload + regions[r][0] + page;
            bool blank = true;
            for (int i = 0; i < 256; i++)
                if (data[i] != 0xff) { blank = false; break; }
            if (blank) continue;
            uint32_t address = regions[r][1] + page;
            GPIO_BSRR(GPIOB) = 1u << (16 + PIN_FLASH_CS);
            RAM_XFER(0x06, scratch);
            GPIO_BSRR(GPIOB) = 1u << PIN_FLASH_CS;
            GPIO_BSRR(GPIOB) = 1u << (16 + PIN_FLASH_CS);
            RAM_XFER(0x02, scratch);
            RAM_XFER(address >> 16, scratch);
            RAM_XFER(address >> 8, scratch);
            RAM_XFER(address, scratch);
            for (int i = 0; i < 256; i++)
                RAM_XFER(data[i], scratch);
            GPIO_BSRR(GPIOB) = 1u << PIN_FLASH_CS;
        }

    /* Acknowledge the final data packet on the requesting transport. */
    uint8_t message[9] = {0xf0, 0x42, 0x23, 0x00, 0x01, 0x60, 0x05, 0x00, 0xf7};
    if (source == MIDI_SRC_DIN) {
        for (int i = 0; i < 9; i++)
            REG32(UART_DIN + 4) = message[i];
    } else {
        uint8_t packets[12] = {
            0x14, message[0], message[1], message[2],
            0x14, message[3], message[4], message[5],
            0x17, message[6], message[7], message[8],
        };
        USB_DIEPTSIZ(1) = 12;
        for (int i = 0; i < 12; i += 4)
            USB_FIFO(1) = (uint32_t)packets[i] | ((uint32_t)packets[i + 1] << 8)
                        | ((uint32_t)packets[i + 2] << 16) | ((uint32_t)packets[i + 3] << 24);
    }

    /* Show the pre-rendered completion screen, then hold here forever. */
    while (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN) { }
    REG32(DISPLAY_DMA_STREAM + 8) = SPI_DISP_DR;
    REG32(DISPLAY_DMA_STREAM + 12) = (uint32_t)(uintptr_t)frame_buffer;
    REG32(DISPLAY_DMA_STREAM + 4) = DISPLAY_BYTES / 2;
    REG32(DISPLAY_DMA_STREAM) = DMA_CR_EN | DMA_CR_DIR_M2P | DMA_CR_MINC | DMA_CR_MSIZE16;
    while (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN) { }
    for (;;) { }
}

static void install(int source) __attribute__((noreturn));
static void install(int source)
{
    status_line("PROGRAMMING", "DO NOT REMOVE POWER");
    display_pump();
    while (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN) { }
    progress(source, 0);
    state = COMPLETE;
    /* Pre-render the completion screen; the RAM installer only streams it. */
    status_line("UPDATE COMPLETE", "RESTART THE DEVICE");
    /* Jump through a register so the call reaches the RAM copy. */
    void (*volatile installer)(const uint8_t *, int, uint8_t *) = ram_install;
    installer(_update_buffer, source, display_framebuffer());
    for (;;) { } /* unreachable */
}

static bool unpack_packet(const uint8_t *packed, uint8_t *raw)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < PACKED_SIZE; i += 8) {
        uint32_t group = PACKED_SIZE - i - 1 < 7 ? PACKED_SIZE - i - 1 : 7;
        uint8_t mask = packed[i];
        if (mask >> group) return false; /* noncanonical unused bits */
        for (uint32_t b = 0; b < group; b++)
            raw[out++] = (uint8_t)(packed[i + 1 + b] | (((mask >> b) & 1) << 7));
    }
    return out == PACKET_SIZE;
}

static void handle_begin(int source, const uint8_t *body, int len)
{
    state = IDLE;
    if (len != 16) { nak(source, ERR_FORMAT); return; }
    uint32_t base = read_u32(body + 2), length = read_u32(body + 6);
    uint32_t packet_size = read_u32(body + 12);
    if (body[0] != 0 || body[1] != 0) { nak(source, ERR_BLOCK); return; }
    if (!(body[10] & 0x40) || (body[10] & 0x3f) >= 64 || body[11] >= 128) { nak(source, ERR_VERSION); return; }
    if (base != 0 || length != PAYLOAD_SIZE || packet_size != PACKET_SIZE) { nak(source, ERR_FORMAT); return; }
    state = RECEIVING;
    next_packet = 0;
    status_line("RECEIVING UPDATE", 0);
    ack(source);
}

static void handle_data(int source, const uint8_t *body, int len)
{
    if (state != RECEIVING) { nak(source, ERR_ABORTED); return; }
    if (len != 4 + PACKED_SIZE + 1) { state = IDLE; nak(source, ERR_FORMAT); return; }
    uint32_t index = read_u32(body);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < PACKED_SIZE; i++) sum += body[4 + i];
    if ((sum & 127) != body[4 + PACKED_SIZE]) { state = IDLE; nak(source, ERR_CHECKSUM); return; }
    if (index != next_packet) { state = IDLE; nak(source, ERR_PACKET); return; }
    if (!unpack_packet(body + 4, _update_buffer + index * PACKET_SIZE)) {
        state = IDLE;
        nak(source, ERR_FORMAT);
        return;
    }
    if (++next_packet < PACKET_COUNT) {
        ack(source);
        return;
    }
    if (!payload_valid(_update_buffer)) {
        state = IDLE;
        nak(source, ERR_FORMAT);
        status_line("IMAGE REJECTED", "NOTHING WAS WRITTEN");
        return;
    }
    install(source); /* programs from RAM, acknowledges, never returns */
}

static void updater_sysex(const uint8_t *frame, int len, int source)
{
    if (len < 8) return;
    for (int i = 0; i < 6; i++)
        if (frame[i] != PREFIX[i]) return; /* wrong routing: not ours */
    const uint8_t *body = frame + 7;
    int body_len = len - 8;
    switch (frame[6]) {
    case 0x00: { /* search: protocol 1, packet size 1024, no fill packets */
        uint8_t info[6] = {1, 0, 0, (PACKET_SIZE >> 7) & 127, PACKET_SIZE & 127, 0};
        reply(source, 1, info, 6);
        break;
    }
    case 0x02:
        if (body_len == 1 && body[0] == 0)
            reply(source, 3, (const uint8_t[]){FW_VERSION_MAJOR, FW_VERSION_MINOR}, 2);
        break;
    case 0x04:
        if (install_allowed) handle_begin(source, body, body_len);
        else nak(source, ERR_ABORTED); /* version mode never programs */
        break;
    case 0x06:
        if (install_allowed) handle_data(source, body, body_len);
        else nak(source, ERR_ABORTED);
        break;
    case 0x08: /* fill packets were not advertised */
        nak(source, ERR_FORMAT);
        break;
    default:
        break;
    }
}

void updater_main(void)
{
    state = IDLE;
    install_allowed = true;
    display_init();
    status_line("UPDATE MODE", "WAITING FOR HOST");
    midi_init(updater_sysex, 0);
    bool power_released = false;
    for (;;) {
        midi_poll();
        display_pump();
        /* Power button: shut down only while no transfer is in progress. */
        bool pressed = !(GPIO_IDR(GPIOC) & (1u << PIN_POWER_BTN));
        if (power_released && pressed && state != RECEIVING) {
            status_line("POWERED OFF", 0);
            display_pump();
            GPIO_BSRR(GPIOB) = 1u << (16 + PIN_POWER_HOLD);
        }
        power_released = power_released || !pressed;
    }
}

void version_main(void)
{
    state = IDLE;
    install_allowed = false;
    display_init();
    fb_clear();
    fb_text(4, 4, "EUCLID SQ-64", 15);
    fb_text(4, 20, "VERSION", 12);
    fb_number(52, 20, FW_VERSION_MAJOR, 12);
    fb_text(60, 20, ".", 12);
    fb_number(66, 20, FW_VERSION_MINOR, 12);
    fb_text(4, 40, "ORIGINAL EUCLIDEAN FIRMWARE", 8);
    fb_text(4, 52, "PRESS PLAY TO CONTINUE", 8);
    display_flush();
    midi_init(updater_sysex, 0); /* version requests are answered here too */
    /* BOOT-004: the PLAY indicator is lit while version mode is active, and
     * pressing PLAY continues the normal boot sequence (we return and
     * main_entry falls through to normal_main). */
    panel_init();
    panel_transport_leds(false, false, false, true);
    bool power_released = false;
    for (;;) {
        midi_poll();
        panel_poll();
        panel_event_t ev;
        while (panel_event_next(&ev)) {
            if (ev.sw == SW_PLAY && ev.down) {
                /* Stop the circular scan so normal_main's panel_init
                 * reconfigures idle streams, then clear its event flags. */
                REG32(PANEL_TX_STREAM) = 0;
                REG32(PANEL_RX_STREAM) = 0;
                REG32(DMA2 + 8) = PANEL_HT_FLAG | PANEL_TC_FLAG;
                return;
            }
        }
        display_pump();
        bool pressed = !(GPIO_IDR(GPIOC) & (1u << PIN_POWER_BTN));
        if (power_released && pressed)
            GPIO_BSRR(GPIOB) = 1u << (16 + PIN_POWER_HOLD);
        power_released = power_released || !pressed;
    }
}

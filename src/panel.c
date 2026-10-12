/* Panel matrix scanning, encoder decoding and LED/trigger multiplexing. */
#include "panel.h"
#include "board.h"

/* 16 eight-byte frames per circular DMA buffer; the RX half-buffer finished
 * flags live in DMA2 LISR bits 26/27 (scan TX stream 3). A received frame
 * holds the row selected by the *previous* frame's TX byte 2. */
static uint8_t scan_tx[PANEL_FRAMES * 8] __attribute__((aligned(4)));
static uint8_t scan_rx[PANEL_FRAMES * 8] __attribute__((aligned(4)));

static uint32_t row_state[8];
static uint32_t led_rows[8];
static uint8_t trigger_mask;      /* bit set = trigger output active */
static uint8_t aux_row0 = 0xff, aux_row1 = 0xff;

static panel_event_t events[32];
static uint8_t ev_head, ev_tail;

static int8_t enc_phase[ENC_COUNT];
static int16_t enc_edges[ENC_COUNT];

/* Button bits per row; everything else in a row value is an encoder line,
 * unused, or noise. */
static const uint32_t BUTTON_MASK[8] = {
    0x07ff0003u, 0x07fe0003u, 0x07fe0000u, 0x07fe0000u,
    0x07fe0000u, 0x07fe0000u, 0x07fe0000u, 0x07fe0000u,
};

static uint32_t decode_bits(const uint8_t *r)
{
    /* Eight-byte frame: 15 lead clocks, then the 32 input bits serialized
     * low byte first, MSB first within each byte. */
    uint32_t b0 = ((r[1] & 1u) << 7) | (r[2] >> 1);
    uint32_t b1 = ((r[2] & 1u) << 7) | (r[3] >> 1);
    uint32_t b2 = ((r[3] & 1u) << 7) | (r[4] >> 1);
    uint32_t b3 = ((r[4] & 1u) << 7) | (r[5] >> 1);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void push_event(uint8_t sw, bool down)
{
    uint8_t next = (uint8_t)((ev_head + 1) & 31);
    if (next == ev_tail) return; /* overflow: drop oldest-pending semantics */
    events[ev_head] = (panel_event_t){sw, down};
    ev_head = next;
}

bool panel_event_next(panel_event_t *out)
{
    if (ev_tail == ev_head) return false;
    *out = events[ev_tail];
    ev_tail = (uint8_t)((ev_tail + 1) & 31);
    return true;
}

bool panel_down(uint8_t sw)
{
    return (row_state[SW_ROW(sw)] >> SW_BIT(sw)) & 1u;
}

int panel_encoder_take(int encoder)
{
    int detents = enc_edges[encoder] / 2;
    enc_edges[encoder] -= (int16_t)(detents * 2);
    return detents;
}

static void apply_row(int row, uint32_t value)
{
    uint32_t changed = (row_state[row] ^ value) & BUTTON_MASK[row];
    for (int bit = 0; changed; bit++, changed >>= 1)
        if (changed & 1u)
            push_event((uint8_t)SW(row, bit), (value >> bit) & 1u);
    row_state[row] = (row_state[row] & ~BUTTON_MASK[row]) | (value & BUTTON_MASK[row]);
}

static void apply_encoders(uint32_t value)
{
    static const int8_t GRAY_INDEX[4] = {0, 1, 3, 2}; /* gray -> step index */
    for (int e = 0; e < ENC_COUNT; e++) {
        int idx = GRAY_INDEX[(value >> ENC_FIRST_BIT[e]) & 3u];
        if (enc_phase[e] < 0) { enc_phase[e] = (int8_t)idx; continue; }
        int delta = (idx - enc_phase[e]) & 3;
        enc_phase[e] = (int8_t)idx;
        if (delta == 1 && enc_edges[e] < 4096) enc_edges[e]++;
        else if (delta == 3 && enc_edges[e] > -4096) enc_edges[e]--;
    }
}

static void process_frame(int frame)
{
    uint32_t value = decode_bits(&scan_rx[frame * 8]);
    if (value & PANEL_INVALID_BITS) return;     /* unwired or garbage frame */
    apply_encoders(value);
    apply_row((frame + 7) & 7, value);
}

static void compose_tx(void)
{
    for (int f = 0; f < PANEL_FRAMES; f++) {
        int row = f & 7;
        uint8_t *t = &scan_tx[f * 8];
        uint32_t cols = led_rows[row];
        t[0] = 0; t[1] = 0;
        t[2] = (uint8_t)(1u << row);
        t[3] = (uint8_t)((cols >> 16) & 0x1f);
        t[4] = (uint8_t)(cols >> 8);
        t[5] = (uint8_t)cols;
        t[6] = (uint8_t)~trigger_mask;
        t[7] = row == 0 ? aux_row0 : row == 1 ? aux_row1 : 0xff;
    }
}

void panel_led_columns(const uint32_t rows[8])
{
    for (int r = 0; r < 8; r++) led_rows[r] = rows[r];
    compose_tx();
}

void panel_triggers(uint8_t mask)
{
    trigger_mask = mask;
    compose_tx();
}

void panel_transport_leds(bool tap, bool stop, bool record, bool play)
{
    aux_row0 = (uint8_t)~((tap ? 0x0c : 0) | (stop ? 0x10 : 0));
    aux_row1 = (uint8_t)~((record ? 0x08 : 0) | (play ? 0x10 : 0));
    compose_tx();
}

void panel_startup_scan(uint32_t rows[8])
{
    uint8_t rx[6];
    for (int r = 0; r < 8; r++) rows[r] = 0;
    /* Two-frame pipeline: the sample taken while shifting frame n holds the
     * row latched by frame n-1. Prime with row 0, then read rows 0..7. */
    for (int i = -1; i < 8; i++) {
        GPIO_BSRR(GPIOB) = 1u << (16 + PIN_PANEL_LATCH); /* sample previous */
        uint8_t next_row = (uint8_t)((i + 1) & 7);
        for (int b = 0; b < 6; b++) {
            REG8(SPI_PANEL_DR) = b == 0 ? (uint8_t)(1u << next_row)
                               : b == 4 ? (uint8_t)0xff : (uint8_t)0;
            while (!(REG32(SPI_PANEL_SR) & SPI_SR_RXNE)) { }
            rx[b] = REG8(SPI_PANEL_DR);
        }
        GPIO_BSRR(GPIOB) = 1u << PIN_PANEL_LATCH;        /* latch next row  */
        if (i < 0) continue;
        /* Six polled bytes omit the frame's two leading bytes; bit 7 of the
         * low input byte is not recoverable and reads as zero. */
        uint32_t b0 = rx[0] >> 1;
        uint32_t b1 = ((rx[0] & 1u) << 7) | (rx[1] >> 1);
        uint32_t b2 = ((rx[1] & 1u) << 7) | (rx[2] >> 1);
        uint32_t b3 = ((rx[2] & 1u) << 7) | (rx[3] >> 1);
        uint32_t value = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        rows[i] = (value & PANEL_INVALID_BITS) ? 0 : value;
    }
}

void panel_init(void)
{
    for (int e = 0; e < ENC_COUNT; e++) enc_phase[e] = -1;
    compose_tx();
    /* Moderate serial clock: one full row sweep per 64-byte half-buffer. */
    REG32(SPI_PANEL_CR1) = 6u << 3;
    REG32(PANEL_RX_STREAM + 8) = SPI_PANEL_DR;
    REG32(PANEL_RX_STREAM + 12) = (uint32_t)(uintptr_t)scan_rx;
    REG32(PANEL_RX_STREAM + 4) = PANEL_FRAMES * 8;
    REG32(PANEL_RX_STREAM) = DMA_CR_EN | DMA_CR_CIRC | DMA_CR_MINC;
    REG32(PANEL_TX_STREAM + 8) = SPI_PANEL_DR;
    REG32(PANEL_TX_STREAM + 12) = (uint32_t)(uintptr_t)scan_tx;
    REG32(PANEL_TX_STREAM + 4) = PANEL_FRAMES * 8;
    REG32(PANEL_TX_STREAM) = DMA_CR_EN | DMA_CR_DIR_M2P | DMA_CR_CIRC | DMA_CR_MINC;
}

static void process_half(int first)
{
    for (int f = first; f < first + 8; f++) process_frame(f);
}

void panel_poll(void)
{
    uint32_t flags = REG32(DMA2); /* LISR; reading also advances transfers */
    if (!(flags & (PANEL_HT_FLAG | PANEL_TC_FLAG))) return;
    REG32(DMA2 + 8) = flags & (PANEL_HT_FLAG | PANEL_TC_FLAG);
    if ((flags & (PANEL_HT_FLAG | PANEL_TC_FLAG)) == (PANEL_HT_FLAG | PANEL_TC_FLAG)) {
        /* Fell a whole half-buffer (or more) behind: both halves are pending
         * and the scan may refill either one at any point during this poll,
         * so the "older" half can hold a snapshot from before an edge the
         * fresh half already carries — replaying it resurrects the previous
         * state and turns one press into several. Every row appears in each
         * half, so decode only the half the DMA finished last (per the
         * remaining transfer count) and drop the stale one; a state change
         * that persists is in the fresh half, and one that reversed within
         * the lost half-sweep (~0.3 ms) is below any real button edge. */
        bool first_half_fresh = REG32(PANEL_RX_STREAM + 4) <= PANEL_FRAMES * 4;
        process_half(first_half_fresh ? 0 : 8);
    } else {
        process_half(flags & PANEL_HT_FLAG ? 0 : 8);
    }
}

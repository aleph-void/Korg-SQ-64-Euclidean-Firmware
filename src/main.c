/* Euclidean sequencer for the SQ-64: boot mode selection and normal mode.
 *
 * Four tracks each play a Euclidean rhythm E(pulses, length) with rotation.
 * Tracks A/B/C drive their gate jack, pitch CV and MIDI channels 1..3;
 * track D drives trigger jack 1 and MIDI channel 10. Pads set pulses
 * (SHIFT+pad sets length), encoders edit tempo/swing/pulses/length/rotation/
 * note, GLOBAL commits settings to SPI flash, and the power button saves and
 * shuts down cleanly.
 */
#include <stdbool.h>

#include "board.h"
#include "display.h"
#include "midi.h"
#include "outputs.h"
#include "panel.h"
#include "storage.h"
#include "updater.h"

volatile uint32_t g_ms;

void tick_irq_handler(void)
{
    REG32(TICK_TIM + 0x10) = 0; /* clear update flag */
    g_ms++;
}

static settings_t cfg;
static bool playing;
static uint32_t global_step;
static uint32_t next_step256;   /* ms << 8 timestamps, wrap-safe compares */
static uint32_t next_clock256;
static uint8_t active_note[4] = {0xff, 0xff, 0xff, 0xff};
static uint32_t gate_off_ms[4];
static uint32_t trigger_off_ms;
static uint32_t beat_flash_ms;
static uint32_t tap_last_ms;
static bool ui_dirty = true;
static volatile bool rt_start, rt_stop;

static uint32_t now256(void) { return g_ms << 8; }

static uint32_t step_period256(void) { return (15000u << 8) / cfg.bpm; }

static uint16_t euclid_pattern(const track_cfg_t *t)
{
    uint16_t bits = 0;
    for (int i = 0; i < t->length; i++) {
        int j = (i + t->length - t->rotate) % t->length;
        if ((j * t->pulses) % t->length < t->pulses)
            bits |= (uint16_t)(1u << i);
    }
    return bits;
}

static void send_note(int track, uint8_t status, uint8_t note)
{
    uint8_t channel = track < 3 ? (uint8_t)track : 9;
    uint8_t msg[3] = {(uint8_t)(status | channel), note, status == 0x90 ? 100 : 0};
    midi_send(msg, 3);
}

static void note_off(int track)
{
    if (active_note[track] == 0xff) return;
    send_note(track, 0x80, active_note[track]);
    active_note[track] = 0xff;
    if (track < 3) {
        gate_set(track, false);
        cv_mod(track, 0);
    }
}

static void fire_step(uint32_t now)
{
    uint32_t period = step_period256() >> 8;
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *c = &cfg.track[t];
        int pos = (int)(global_step % c->length);
        if (!((euclid_pattern(c) >> pos) & 1)) continue;
        note_off(t);
        send_note(t, 0x90, c->note);
        active_note[t] = c->note;
        uint32_t gate = period * c->gate_pct / 100;
        gate_off_ms[t] = now + (gate < 5 ? 5 : gate);
        if (t < 3) {
            gate_set(t, true);
            cv_pitch(t, c->note);
            cv_mod(t, 220);
        } else {
            panel_triggers(1);
            trigger_off_ms = now + 20;
        }
    }
    if (global_step % 4 == 0) beat_flash_ms = now + 60;
    global_step++;
    ui_dirty = true;
}

static void transport_start(void)
{
    playing = true;
    global_step = 0;
    next_step256 = now256();
    next_clock256 = now256();
    midi_send((const uint8_t[]){0xfa}, 1);
    ui_dirty = true;
}

static void transport_stop(void)
{
    playing = false;
    for (int t = 0; t < 4; t++) note_off(t);
    panel_triggers(0);
    midi_send((const uint8_t[]){0xfc}, 1);
    ui_dirty = true;
}

static void sequencer_tick(void)
{
    uint32_t now = g_ms;
    for (int t = 0; t < 4; t++)
        if (active_note[t] != 0xff && (int32_t)(now - gate_off_ms[t]) >= 0)
            note_off(t);
    if (trigger_off_ms && (int32_t)(now - trigger_off_ms) >= 0) {
        panel_triggers(0);
        trigger_off_ms = 0;
    }
    if (!playing) return;
    uint32_t t256 = now256();
    if ((int32_t)(t256 - next_clock256) >= 0) {
        midi_send((const uint8_t[]){0xf8}, 1);
        next_clock256 += step_period256() / 6;
    }
    if ((int32_t)(t256 - next_step256) >= 0) {
        fire_step(now);
        /* Swing shifts every second sixteenth later within its pair. */
        uint32_t period = step_period256();
        uint32_t offset = period * (uint32_t)(cfg.swing - 50) / 50;
        next_step256 += (global_step & 1) ? period + offset : period - offset;
    }
}

static void clamp_track(track_cfg_t *c)
{
    if (c->pulses > c->length) c->pulses = c->length;
    if (c->rotate >= c->length) c->rotate = (uint8_t)(c->length - 1);
}

static void handle_pad(int track, int pad0, bool shift)
{
    track_cfg_t *c = &cfg.track[track];
    if (shift) {
        c->length = (uint8_t)(pad0 + 1);
        clamp_track(c);
    } else if (pad0 + 1 <= c->length) {
        c->pulses = (uint8_t)(pad0 + 1);
    }
    cfg.selected = (uint8_t)track;
    ui_dirty = true;
}

static void handle_events(void)
{
    panel_event_t ev;
    while (panel_event_next(&ev)) {
        if (!ev.down) continue;
        switch (ev.sw) {
        case SW_PLAY: transport_start(); continue;
        case SW_STOP: transport_stop(); continue;
        case SW_TAP: {
            uint32_t now = g_ms;
            uint32_t gap = now - tap_last_ms;
            if (gap >= 100 && gap <= 2000) {
                uint32_t bpm = 60000 / gap;
                cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
                ui_dirty = true;
            }
            tap_last_ms = now;
            continue;
        }
        case SW_TRACK_A: cfg.selected = 0; ui_dirty = true; continue;
        case SW_TRACK_B: cfg.selected = 1; ui_dirty = true; continue;
        case SW_TRACK_C: cfg.selected = 2; ui_dirty = true; continue;
        case SW_TRACK_D: cfg.selected = 3; ui_dirty = true; continue;
        case SW_GLOBAL: settings_save(&cfg); ui_dirty = true; continue;
        default: break;
        }
        for (int t = 0; t < 4; t++)
            for (int p = 0; p < 16; p++)
                if (pad_switch(t, p) == ev.sw)
                    handle_pad(t, p, panel_down(SW_SHIFT));
    }
    track_cfg_t *c = &cfg.track[cfg.selected];
    int d;
    if ((d = panel_encoder_take(ENC_TEMPO))) {
        int bpm = cfg.bpm + d;
        cfg.bpm = (uint16_t)(bpm < 20 ? 20 : bpm > 300 ? 300 : bpm);
        ui_dirty = true;
    }
    if ((d = panel_encoder_take(ENC_SWING))) {
        int swing = cfg.swing + d;
        cfg.swing = (uint8_t)(swing < 50 ? 50 : swing > 75 ? 75 : swing);
        ui_dirty = true;
    }
    if ((d = panel_encoder_take(ENC_PARAM1))) {
        int pulses = c->pulses + d;
        c->pulses = (uint8_t)(pulses < 0 ? 0 : pulses > c->length ? c->length : pulses);
        ui_dirty = true;
    }
    if ((d = panel_encoder_take(ENC_PARAM2))) {
        int length = c->length + d;
        c->length = (uint8_t)(length < 1 ? 1 : length > 16 ? 16 : length);
        clamp_track(c);
        ui_dirty = true;
    }
    if ((d = panel_encoder_take(ENC_PARAM3))) {
        int rotate = c->rotate + d;
        while (rotate < 0) rotate += c->length;
        c->rotate = (uint8_t)(rotate % c->length);
        ui_dirty = true;
    }
    if ((d = panel_encoder_take(ENC_PARAM4))) {
        int note = c->note + d;
        c->note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
        ui_dirty = true;
    }
}

static void update_leds(void)
{
    uint32_t rows[8];
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *c = &cfg.track[t];
        uint32_t cols = euclid_pattern(c) << 1; /* pads on columns 1..16 */
        if (playing) {
            uint32_t pos = (global_step ? global_step - 1 : 0) % c->length;
            cols ^= 2u << pos; /* playhead inverts its column */
        }
        if (t == cfg.selected) cols |= 1; /* track select LED, column 0 */
        rows[t * 2] = cols;
        rows[t * 2 + 1] = 0;
    }
    panel_led_columns(rows);
    panel_transport_leds((int32_t)(g_ms - beat_flash_ms) < 0, !playing, false, playing);
}

static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

static void draw_ui(void)
{
    const track_cfg_t *c = &cfg.track[cfg.selected];
    fb_clear();
    fb_text(2, 1, "EUCLID", 15);
    fb_number(60, 1, cfg.bpm, 12);
    fb_text(84, 1, "BPM", 8);
    fb_text(118, 1, "SWING", 8);
    fb_number(154, 1, cfg.swing, 12);
    fb_text(176, 1, "%", 8);
    fb_text(216, 1, playing ? "PLAY" : "STOP", playing ? 15 : 6);

    fb_text(2, 12, "TRACK", 8);
    fb_text(40, 12, (const char[]){(char)('A' + cfg.selected), 0}, 15);
    fb_text(60, 12, "PULSE", 8);
    fb_number(96, 12, c->pulses, 15);
    fb_text(118, 12, "LEN", 8);
    fb_number(142, 12, c->length, 15);
    fb_text(162, 12, "ROT", 8);
    fb_number(186, 12, c->rotate, 15);
    fb_text(206, 12, "NOTE", 8);
    fb_text(236, 12, NOTE_NAMES[c->note % 12], 15);
    fb_number(248, 12, c->note / 12 - 1, 15);

    /* Pattern lane for every track. */
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *tc = &cfg.track[t];
        uint16_t bits = euclid_pattern(tc);
        int y = 24 + t * 8;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, t == cfg.selected ? 15 : 6);
        for (int k = 0; k < 16; k++) {
            int x = 14 + k * 9;
            if (k >= tc->length) continue;
            if ((bits >> k) & 1) fb_fill(x, y, 7, 6, t == cfg.selected ? 13 : 7);
            else fb_fill(x, y + 2, 7, 2, 3);
            if (playing && (int)((global_step ? global_step - 1 : 0) % tc->length) == k)
                fb_fill(x + 2, y + 6, 3, 1, 15);
        }
    }
    fb_text(168, 57, "GLOBAL SAVES", 5);
    display_flush();
}

static void power_off(void)
{
    transport_stop();
    outputs_all_off();
    settings_save(&cfg);
    fb_clear();
    fb_text(2, 28, "POWERED OFF", 12);
    display_flush();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    display_pump();
    for (int i = 0; i < 10000 && (REG32(DISPLAY_DMA_STREAM) & DMA_CR_EN); i++) { }
    GPIO_BSRR(GPIOB) = 1u << (16 + PIN_POWER_HOLD); /* release the hold */
    for (;;) { }
}

static void realtime_in(uint8_t byte)
{
    if (byte == 0xfa) rt_start = true;
    else if (byte == 0xfc) rt_stop = true;
}

static void normal_main(void)
{
    settings_load(&cfg);
    display_init();
    outputs_init();
    panel_init();
    midi_init(0, realtime_in);

    /* 1 ms tick: 216 MHz / (215+1) / (999+1). */
    REG32(TICK_TIM + 0x28) = 215;
    REG32(TICK_TIM + 0x2c) = 999;
    REG32(TICK_TIM + 0x0c) = 1;
    NVIC_ISER(TICK_IRQ / 32) = 1u << (TICK_IRQ % 32);
    REG32(TICK_TIM) = 1;

    bool power_seen_released = false;
    uint32_t last_draw_ms = 0;
    for (;;) {
        panel_poll();
        handle_events();
        midi_poll();
        if (rt_start) { rt_start = false; transport_start(); }
        if (rt_stop) { rt_stop = false; transport_stop(); }
        sequencer_tick();
        update_leds();
        if (ui_dirty && (int32_t)(g_ms - last_draw_ms) >= 40) {
            ui_dirty = false;
            last_draw_ms = g_ms;
            draw_ui();
        }
        display_pump();
        bool pressed = !(GPIO_IDR(GPIOC) & (1u << PIN_POWER_BTN));
        if (power_seen_released && pressed)
            power_off();
        power_seen_released = power_seen_released || !pressed;
    }
}

void main_entry(void)
{
    /* Output pins: PB0 (panel latch), PB7 (power hold), PB9 (flash CS),
     * PB12/PB15/PC9 (gates). Everything idles high; gates are inverted by
     * their drivers, so high means inactive. */
    GPIO_BSRR(GPIOB) = (1u << PIN_PANEL_LATCH) | (1u << PIN_POWER_HOLD)
                     | (1u << PIN_FLASH_CS) | (1u << PIN_GATE_A) | (1u << PIN_GATE_B);
    GPIO_BSRR(GPIOC) = 1u << PIN_GATE_C;
    GPIO_MODER(GPIOB) |= (1u << (2 * PIN_PANEL_LATCH)) | (1u << (2 * PIN_POWER_HOLD))
                       | (1u << (2 * PIN_FLASH_CS)) | (1u << (2 * PIN_GATE_A))
                       | (1u << (2 * PIN_GATE_B));
    GPIO_MODER(GPIOC) |= 1u << (2 * PIN_GATE_C);

    /* Startup chords (BOOT-004): GLOBAL alone selects version mode,
     * GLOBAL+PATTERN selects update mode. Anything else boots normally. */
    uint32_t rows[8];
    panel_startup_scan(rows);
    bool global_held = (rows[SW_ROW(SW_GLOBAL)] >> SW_BIT(SW_GLOBAL)) & 1u;
    bool pattern_held = (rows[SW_ROW(SW_PATTERN)] >> SW_BIT(SW_PATTERN)) & 1u;
    uint32_t others = 0;
    for (int r = 0; r < 8; r++) {
        uint32_t buttons = rows[r] & (r == 0 ? 0x07ff0003u : r == 1 ? 0x07fe0003u : 0x07fe0000u);
        if (r == SW_ROW(SW_GLOBAL)) buttons &= ~(1u << SW_BIT(SW_GLOBAL));
        if (r == SW_ROW(SW_PATTERN)) buttons &= ~(1u << SW_BIT(SW_PATTERN));
        others |= buttons;
    }

    if (global_held && pattern_held && !others)
        updater_main();
    if (global_held && !pattern_held && !others)
        version_main();
    normal_main();
}

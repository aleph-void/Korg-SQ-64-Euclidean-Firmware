/* Euclidean sequencer for the SQ-64: boot mode selection and normal mode.
 *
 * Four tracks each play a Euclidean rhythm E(pulses, length) with rotation,
 * up to 64 steps (four bars of 16). Tracks A/B/C drive their gate jack,
 * pitch CV, mod CV and MIDI channels 1..3; track D drives trigger jack 1 and
 * MIDI channel 10. Encoders edit tempo/swing/pulses/length/rotation/note,
 * GLOBAL commits settings to SPI flash, and the power button saves and shuts
 * down cleanly.
 *
 * The pads edit the selected track through the active edit mode. In the
 * default EUCLID mode pad n addresses step (bar-1)*16+n of the selected bar
 * (keys 1..4): pad = pulses, SHIFT+pad = length. The PITCH, GATE, MOD and
 * CONTROL keys each toggle an edit mode scoping the pads to that one
 * parameter of the selected track: note, gate length, mod CV level and
 * velocity. LOOP toggles a loop over the selected bar of the selected
 * track's pattern; clearing it resumes the free-running step counter.
 *
 * PROJECT toggles project mode, where the 64 pads manage the 64 saved
 * project slots of the board contract's catalog: pad = load, SHIFT+pad =
 * save, RECORD+pad = delete. SHIFT+PROJECT starts a new project (workspace
 * back to the declared defaults) without touching any saved slot.
 */
#include <stdbool.h>

#include "board.h"
#include "display.h"
#include "midi.h"
#include "outputs.h"
#include "panel.h"
#include "storage.h"
#include "updater.h"

/* The browser engine's patched TCI interpreter has a fixed translation
 * budget per basic block (about 120 simple instructions, fewer for
 * load/store-heavy code); a longer straight-line block overflows it and
 * traps the WASM host. Keeping the main-loop phases out of line stops the
 * optimizer from fusing them into one giant block; `make` verifies the
 * final image against the budget (tools/check_blocks.py). */
#define NOINLINE __attribute__((noinline))

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

/* Edit modes (MODE-001): the PITCH/GATE/MOD/CONTROL keys scope what the
 * pads edit on the selected track; pressing the active mode's key again
 * returns to the default EUCLID mode. Entry and exit change no data. */
enum { MODE_EUCLID, MODE_PITCH, MODE_GATE, MODE_MOD, MODE_CONTROL };
static uint8_t edit_mode;

/* Bar selection (BAR-001): keys 1..4 pick which 16-step bar of the 64-step
 * pattern space the pads, pad LEDs and display lanes address. */
static uint8_t edit_bar; /* 0..3 */

/* Loop (LOOP-001): a loop captures the selected bar of the selected track's
 * pattern when switched on and wraps the step counter inside it. It is
 * refused when that bar lies beyond the pattern, clipped to the pattern end,
 * and cleared if a later edit moves the pattern end before the region. */
static bool loop_on;
static uint8_t loop_track, loop_start, loop_len;

/* Project mode (PROJECT button): the 64 pads select the 64 saved project
 * slots, bank A pad 1 = slot 0 .. bank D pad 16 = slot 63. */
static bool project_mode;
static uint8_t slot_states[PROJECT_SLOTS];
static const char *proj_msg = "";
static int proj_msg_slot = -1; /* -1: no slot in the message */

static uint32_t now256(void) { return g_ms << 8; }

static uint32_t step_period256(void) { return (15000u << 8) / cfg.bpm; }

static uint64_t euclid_pattern(const track_cfg_t *t)
{
    uint64_t bits = 0;
    for (int i = 0; i < t->length; i++) {
        int j = (i + t->length - t->rotate) % t->length;
        if ((j * t->pulses) % t->length < t->pulses)
            bits |= 1ull << i;
    }
    return bits;
}

static void send_note(int track, uint8_t status, uint8_t note)
{
    uint8_t channel = track < 3 ? (uint8_t)track : 9;
    uint8_t velocity = status == 0x90 ? cfg.track[track].velocity : 0;
    uint8_t msg[3] = {(uint8_t)(status | channel), note, velocity};
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
            cv_mod(t, (uint8_t)(c->mod * 17));
        } else {
            panel_triggers(1);
            trigger_off_ms = now + 20;
        }
    }
    if (global_step % 4 == 0) beat_flash_ms = now + 60;
    global_step++;
    if (loop_on && (global_step < loop_start
                    || global_step >= (uint32_t)loop_start + loop_len))
        global_step = loop_start;
    ui_dirty = true;
}

static void transport_start(void)
{
    playing = true;
    global_step = loop_on ? loop_start : 0;
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

static NOINLINE void sequencer_tick(void)
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

static void loop_toggle(void)
{
    ui_dirty = true;
    if (loop_on) {
        loop_on = false; /* clear: the step counter runs free again */
        return;
    }
    const track_cfg_t *c = &cfg.track[cfg.selected];
    uint8_t start = (uint8_t)(edit_bar * 16);
    if (start >= c->length) return; /* bar beyond the pattern: refused */
    loop_track = cfg.selected;
    loop_start = start;
    loop_len = (uint8_t)(c->length - start > 16 ? 16 : c->length - start);
    loop_on = true;
}

/* An edit (pad, encoder, project load, new project) may move the looped
 * pattern's end: clip the loop to it, or clear a loop left stranded. */
static void loop_revalidate(void)
{
    if (!loop_on) return;
    const track_cfg_t *c = &cfg.track[loop_track];
    if (loop_start >= c->length) {
        loop_on = false;
        ui_dirty = true;
        return;
    }
    uint8_t span = (uint8_t)(c->length - loop_start);
    if (span > 16) span = 16;
    if (span != loop_len) {
        loop_len = span;
        ui_dirty = true;
    }
}

static void handle_pad(int track, int pad0, bool shift)
{
    track_cfg_t *c = &cfg.track[track];
    int step = edit_bar * 16 + pad0 + 1; /* step index in the selected bar */
    switch (edit_mode) {
    case MODE_EUCLID:
        if (shift) {
            c->length = (uint8_t)step;
            clamp_track(c);
        } else if (step <= c->length) {
            c->pulses = (uint8_t)step;
        }
        break;
    case MODE_PITCH:
        if (shift) return; /* undeclared SHIFT combination: no effect */
        c->note = (uint8_t)(36 + pad0);
        break;
    case MODE_GATE:
        if (shift) return;
        c->gate_pct = (uint8_t)(5 + pad0 * 6);
        break;
    case MODE_MOD:
        if (shift) return;
        c->mod = (uint8_t)pad0;
        break;
    case MODE_CONTROL:
        if (shift) return;
        c->velocity = (uint8_t)(pad0 * 8 + 7);
        break;
    }
    cfg.selected = (uint8_t)track;
    ui_dirty = true;
}

static void project_rescan(void)
{
    for (int n = 0; n < PROJECT_SLOTS; n++)
        slot_states[n] = (uint8_t)project_state(n);
}

static void project_report(int slot, const char *message)
{
    proj_msg = message;
    proj_msg_slot = slot;
    ui_dirty = true;
}

static void project_pad(int slot)
{
    if (panel_down(SW_SHIFT)) {
        if (project_save(slot, &cfg)) {
            slot_states[slot] = SLOT_VALID;
            project_report(slot, "SAVED");
        } else {
            slot_states[slot] = (uint8_t)project_state(slot);
            project_report(slot, slot_states[slot] == SLOT_UNAVAILABLE
                           ? "NO SLOT" : "SAVE FAILED");
        }
        return;
    }
    if (panel_down(SW_RECORD)) {
        if (project_delete(slot)) {
            slot_states[slot] = SLOT_EMPTY;
            project_report(slot, "DELETED");
        } else {
            slot_states[slot] = (uint8_t)project_state(slot);
            project_report(slot, slot_states[slot] == SLOT_UNAVAILABLE
                           ? "NO SLOT" : "DELETE FAILED");
        }
        return;
    }
    settings_t loaded;
    switch (project_load(slot, &loaded)) {
    case SLOT_VALID:
        cfg = loaded;
        project_report(slot, "LOADED");
        break;
    case SLOT_EMPTY: project_report(slot, "EMPTY"); break;
    case SLOT_INVALID: project_report(slot, "INVALID"); break;
    default: project_report(slot, "NO SLOT"); break;
    }
}

static NOINLINE void handle_events(void)
{
    panel_event_t ev;
    while (panel_event_next(&ev)) {
        if (!ev.down) continue;
        switch (ev.sw) {
        case SW_PROJECT:
            if (panel_down(SW_SHIFT)) {
                /* New project: the workspace returns to the declared
                 * defaults; no saved slot or committed settings change. */
                settings_defaults(&cfg);
                project_report(-1, "NEW PROJECT");
            } else {
                project_mode = !project_mode;
                if (project_mode) project_rescan();
                project_report(-1, "");
            }
            continue;
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
        case SW_TRACK_A: if (!project_mode) { cfg.selected = 0; ui_dirty = true; } continue;
        case SW_TRACK_B: if (!project_mode) { cfg.selected = 1; ui_dirty = true; } continue;
        case SW_TRACK_C: if (!project_mode) { cfg.selected = 2; ui_dirty = true; } continue;
        case SW_TRACK_D: if (!project_mode) { cfg.selected = 3; ui_dirty = true; } continue;
        case SW_PITCH:   if (!project_mode) { edit_mode = edit_mode == MODE_PITCH   ? MODE_EUCLID : MODE_PITCH;   ui_dirty = true; } continue;
        case SW_GATE:    if (!project_mode) { edit_mode = edit_mode == MODE_GATE    ? MODE_EUCLID : MODE_GATE;    ui_dirty = true; } continue;
        case SW_MOD:     if (!project_mode) { edit_mode = edit_mode == MODE_MOD     ? MODE_EUCLID : MODE_MOD;     ui_dirty = true; } continue;
        case SW_CONTROL: if (!project_mode) { edit_mode = edit_mode == MODE_CONTROL ? MODE_EUCLID : MODE_CONTROL; ui_dirty = true; } continue;
        case SW_BAR1: if (!project_mode) { edit_bar = 0; ui_dirty = true; } continue;
        case SW_BAR2: if (!project_mode) { edit_bar = 1; ui_dirty = true; } continue;
        case SW_BAR3: if (!project_mode) { edit_bar = 2; ui_dirty = true; } continue;
        case SW_BAR4: if (!project_mode) { edit_bar = 3; ui_dirty = true; } continue;
        case SW_LOOP: if (!project_mode) loop_toggle(); continue;
        case SW_GLOBAL: if (!project_mode) { settings_save(&cfg); ui_dirty = true; } continue;
        default: break;
        }
        for (int t = 0; t < 4; t++)
            for (int p = 0; p < 16; p++)
                if (pad_switch(t, p) == ev.sw) {
                    if (project_mode) project_pad(t * 16 + p);
                    else handle_pad(t, p, panel_down(SW_SHIFT));
                }
    }
    if (project_mode) {
        /* Pads are slot selectors here; drop encoder motion so it does not
         * accumulate and edit the workspace invisibly. */
        for (int e = 0; e < ENC_COUNT; e++) panel_encoder_take(e);
        loop_revalidate(); /* a loaded or new project may move pattern ends */
        return;
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
        c->length = (uint8_t)(length < 1 ? 1 : length > 64 ? 64 : length);
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
    loop_revalidate();
}

static NOINLINE void update_leds(void)
{
    uint32_t rows[8];
    if (project_mode) {
        /* Pad LED n of bank t lights when slot t*16+n holds a project. */
        for (int t = 0; t < 4; t++) {
            uint32_t cols = 0;
            for (int p = 0; p < 16; p++)
                if (slot_states[t * 16 + p] == SLOT_VALID)
                    cols |= 2u << p;
            rows[t * 2] = cols;
            rows[t * 2 + 1] = 0;
        }
        rows[SW_ROW(LED_PROJECT)] |= 1u << SW_BIT(LED_PROJECT);
        panel_led_columns(rows);
        panel_transport_leds(false, !playing, panel_down(SW_RECORD), playing);
        return;
    }
    uint32_t base = (uint32_t)edit_bar * 16; /* pads window the selected bar */
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *c = &cfg.track[t];
        uint32_t cols = (uint32_t)((euclid_pattern(c) >> base) & 0xffffu) << 1;
        if (playing) {
            uint32_t pos = (global_step ? global_step - 1 : 0) % c->length;
            if (pos >= base && pos < base + 16)
                cols ^= 2u << (pos - base); /* playhead inverts its column */
        }
        if (t == cfg.selected) cols |= 1; /* track select LED, column 0 */
        rows[t * 2] = cols;
        rows[t * 2 + 1] = 0;
    }
    static const uint8_t MODE_LEDS[5] = {0xff, LED_PITCH, LED_GATE, LED_MOD, LED_CONTROL};
    static const uint8_t BAR_LEDS[4] = {LED_BAR1, LED_BAR2, LED_BAR3, LED_BAR4};
    if (MODE_LEDS[edit_mode] != 0xff)
        rows[SW_ROW(MODE_LEDS[edit_mode])] |= 1u << SW_BIT(MODE_LEDS[edit_mode]);
    rows[SW_ROW(BAR_LEDS[edit_bar])] |= 1u << SW_BIT(BAR_LEDS[edit_bar]);
    if (loop_on) rows[SW_ROW(LED_LOOP)] |= 1u << SW_BIT(LED_LOOP);
    panel_led_columns(rows);
    panel_transport_leds((int32_t)(g_ms - beat_flash_ms) < 0, !playing, false, playing);
}

static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

static NOINLINE void draw_project_ui(void)
{
    fb_clear();
    fb_text(2, 1, "PROJECT", 15);
    if (proj_msg_slot >= 0) {
        fb_text(80, 1, (const char[]){(char)('A' + proj_msg_slot / 16), 0}, 15);
        fb_number(86, 1, proj_msg_slot % 16 + 1, 15);
        fb_text(104, 1, proj_msg, 12);
    } else {
        fb_text(80, 1, proj_msg, 12);
    }
    fb_text(2, 12, "PAD LOAD", 8);
    fb_text(68, 12, "SHIFT+PAD SAVE", 8);
    fb_text(160, 12, "REC+PAD DELETE", 8);

    /* Slot catalog: one lane per bank, filled = saved project, baseline =
     * empty, half fill = invalid, nothing = beyond the fitted SPI part. */
    for (int t = 0; t < 4; t++) {
        int y = 24 + t * 8;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, 6);
        for (int p = 0; p < 16; p++) {
            int x = 14 + p * 9;
            switch (slot_states[t * 16 + p]) {
            case SLOT_VALID: fb_fill(x, y, 7, 6, 13); break;
            case SLOT_EMPTY: fb_fill(x, y + 2, 7, 2, 3); break;
            case SLOT_INVALID: fb_fill(x, y + 1, 7, 4, 7); break;
            default: break;
            }
        }
    }
    fb_text(168, 57, "SHIFT+PROJ NEW", 5);
    display_flush();
}

static NOINLINE void draw_ui(void)
{
    if (project_mode) {
        draw_project_ui();
        return;
    }
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
    switch (edit_mode) {
    case MODE_PITCH:
        fb_text(60, 12, "PITCH", 8);
        fb_text(98, 12, NOTE_NAMES[c->note % 12], 15);
        fb_number(112, 12, c->note / 12 - 1, 15);
        fb_number(132, 12, c->note, 12);
        break;
    case MODE_GATE:
        fb_text(60, 12, "GATE", 8);
        fb_number(92, 12, c->gate_pct, 15);
        fb_text(110, 12, "%", 8);
        break;
    case MODE_MOD:
        fb_text(60, 12, "MOD", 8);
        fb_number(86, 12, c->mod, 15);
        break;
    case MODE_CONTROL:
        fb_text(60, 12, "VEL", 8);
        fb_number(86, 12, c->velocity, 15);
        break;
    default:
        fb_text(60, 12, "PULSE", 8);
        fb_number(96, 12, c->pulses, 15);
        fb_text(118, 12, "LEN", 8);
        fb_number(142, 12, c->length, 15);
        fb_text(162, 12, "ROT", 8);
        fb_number(186, 12, c->rotate, 15);
        fb_text(206, 12, "NOTE", 8);
        fb_text(236, 12, NOTE_NAMES[c->note % 12], 15);
        fb_number(248, 12, c->note / 12 - 1, 15);
        break;
    }

    /* Pattern lane for every track, windowed on the selected bar. */
    int base = edit_bar * 16;
    for (int t = 0; t < 4; t++) {
        const track_cfg_t *tc = &cfg.track[t];
        uint64_t bits = euclid_pattern(tc);
        int y = 24 + t * 8;
        fb_text(2, y, (const char[]){(char)('A' + t), 0}, t == cfg.selected ? 15 : 6);
        for (int k = 0; k < 16; k++) {
            int step = base + k;
            int x = 14 + k * 9;
            if (step >= tc->length) continue;
            if ((bits >> step) & 1) fb_fill(x, y, 7, 6, t == cfg.selected ? 13 : 7);
            else fb_fill(x, y + 2, 7, 2, 3);
            if (playing && (int)((global_step ? global_step - 1 : 0) % tc->length) == step)
                fb_fill(x + 2, y + 6, 3, 1, 15);
        }
    }
    static const char *const MODE_NAMES[5] = {"EUCLID", "PITCH", "GATE", "MOD", "CTRL"};
    fb_text(2, 57, MODE_NAMES[edit_mode], 5);
    fb_text(46, 57, "BAR", 5);
    fb_number(70, 57, edit_bar + 1, 5);
    if (loop_on) fb_text(84, 57, "LOOP", 12);
    fb_text(168, 57, "GLOBAL SAVES", 5);
    display_flush();
}

static NOINLINE void power_off(void)
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

static NOINLINE void normal_main(void)
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
     * GLOBAL+PATTERN selects update mode. Anything else boots normally.
     * Version mode returns when PLAY is pressed, continuing the normal
     * boot sequence below. */
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

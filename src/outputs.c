/* Gate pins and the SAI/DMA CV slot buffer. Gate drivers invert the MCU
 * level, so driving a pin low turns the jack's gate on. CV samples are
 * signed 24-bit values descending from 0x7fffff as the level rises. */
#include "outputs.h"
#include "board.h"

static uint32_t cv_slots[CV_SLOTS] __attribute__((aligned(4)));

static const struct { uint32_t port; uint8_t pin; } GATES[3] = {
    {GPIOB, PIN_GATE_A}, {GPIOB, PIN_GATE_B}, {GPIOC, PIN_GATE_C},
};

void gate_set(int track, bool active)
{
    if ((unsigned)track >= 3) return;
    uint32_t bit = 1u << GATES[track].pin;
    GPIO_BSRR(GATES[track].port) = active ? bit << 16 : bit;
}

static void cv_write(int slot, uint32_t level_0_to_fffffe)
{
    cv_slots[slot] = (CV_LEVEL_ZERO - level_0_to_fffffe) & 0xffffffu;
}

void cv_pitch(int track, uint8_t note)
{
    if ((unsigned)track >= 3 || note > 127) return;
    cv_write(track * 2 + 1, (uint32_t)note * 0xfffffeu / 127u);
}

void cv_mod(int track, uint8_t level)
{
    if ((unsigned)track >= 3) return;
    cv_write(track * 2, (uint32_t)level * 0xfffffeu / 255u);
}

void outputs_all_off(void)
{
    for (int t = 0; t < 3; t++) {
        gate_set(t, false);
        cv_write(t * 2, 0);
    }
}

void outputs_init(void)
{
    for (int s = 0; s < CV_SLOTS; s++) cv_slots[s] = CV_LEVEL_ZERO;
    /* Eight-slot circular word transfer from SRAM to SAI1 block B. */
    REG32(CV_DMA_STREAM + 8) = SAI1B_DR;
    REG32(CV_DMA_STREAM + 12) = (uint32_t)(uintptr_t)cv_slots;
    REG32(CV_DMA_STREAM + 4) = CV_SLOTS;
    REG32(SAI1B_CR) = SAI1B_CR_RUN;
    REG32(CV_DMA_STREAM) = CV_DMA_CR;
    outputs_all_off();
}

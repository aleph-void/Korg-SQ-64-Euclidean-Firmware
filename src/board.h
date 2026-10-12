/* SQ-64 board interface constants.
 *
 * Every value here is an interoperability fact taken from the independent
 * SQ-64 specification (spec/sq64_spec/board.json, spec/docs/contracts.md) and
 * the independently authored emulator board model. No Korg source code,
 * decompiled functions or vendor headers were used. Physical-board
 * conformance of these facts is still an open item in the spec; see the
 * repository README.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REG16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))

/* Core clock. The emulator's virtual clock runs at 216 MHz (Cortex-M7). */
#define HCLK_HZ 216000000u

/* Memory map */
#define INTERNAL_FLASH_BASE  0x00200000u
#define INTERNAL_FLASH_ALIAS 0x08000000u
#define INTERNAL_FLASH_SIZE  0x00010000u
#define SRAM_BASE            0x20000000u
#define SRAM_SIZE            0x00040000u

/* GPIO (STM32-style ports, 0x400 apart) */
#define GPIOA 0x40020000u
#define GPIOB 0x40020400u
#define GPIOC 0x40020800u
#define GPIO_MODER(p) REG32((p) + 0x00)
#define GPIO_IDR(p)   REG32((p) + 0x10)
#define GPIO_ODR(p)   REG32((p) + 0x14)
#define GPIO_BSRR(p)  REG32((p) + 0x18)

/* Pin assignments (board contract) */
#define PIN_PANEL_LATCH 0  /* PB0: polled-scan sample (low) / row latch (high) */
#define PIN_POWER_HOLD  7  /* PB7: firmware power hold, release to shut down  */
#define PIN_FLASH_CS    9  /* PB9: SPI NOR chip select, active low            */
#define PIN_GATE_A      12 /* PB12: track A gate, driver inverts (low=active) */
#define PIN_GATE_B      15 /* PB15 */
#define PIN_GATE_C      9  /* PC9 */
#define PIN_SYNC_DETECT 4  /* PB4: high = sync jack unplugged                 */
#define PIN_POWER_BTN   13 /* PC13: power button, active low                  */

/* SPI blocks (data/status registers used by the board model) */
#define SPI_STORE_SR 0x40003808u /* external NOR flash */
#define SPI_STORE_DR 0x4000380cu
#define SPI_DISP_SR  0x40003c08u /* display controller */
#define SPI_DISP_DR  0x40003c0cu
#define SPI_PANEL_CR1 0x40013000u /* panel scan */
#define SPI_PANEL_SR  0x40013008u
#define SPI_PANEL_DR  0x4001300cu
#define SPI_SR_RXNE 0x200u

/* MIDI DIN UART (status +0, data +4, control +0xc bit5 = RX interrupt) */
#define UART_DIN  0x40004800u
#define UART_AUX  0x40004400u
#define UART_SR_RXNE 0x20u

/* General purpose timer used for the 1 ms system tick (IRQ 55).
 * Register offsets: +0 CR1, +0xc DIER, +0x10 SR, +0x24 CNT, +0x28 PSC,
 * +0x2c ARR. With reset RCC settings the timer counts HCLK / (PSC + 1). */
#define TICK_TIM   0x40001400u
#define TICK_IRQ   55

/* DMA controllers; stream n registers at base + 0x10 + 0x18*n:
 * +0 CR, +4 NDTR, +8 PAR, +0xc M0AR. LISR/HISR at +0/+4, LIFCR/HIFCR +8/+0xc. */
#define DMA1 0x40026000u
#define DMA2 0x40026400u
#define DMA_STREAM(c, n) ((c) + 0x10u + 0x18u * (n))
#define DMA_CR_EN   0x1u
#define DMA_CR_DIR_M2P 0x40u
#define DMA_CR_CIRC 0x100u
#define DMA_CR_MINC 0x400u
#define DMA_CR_MSIZE16 0x2000u

/* Display: 8192 bytes, 256x64 four-bit pixels, sent as 4096 halfwords by
 * DMA1 stream 5 to the display SPI data register. Low nibble = even x,
 * rows stored bottom-first. */
#define DISPLAY_DMA_STREAM DMA_STREAM(DMA1, 5)
#define DISP_WIDTH 256
#define DISP_HEIGHT 64
#define DISPLAY_BYTES 8192

/* Panel scan: eight-byte full-duplex frames on the panel SPI, TX DMA2
 * stream 3, RX DMA2 stream 2, both circular over a 128-byte (16-frame)
 * buffer. TX byte 2 = one-hot row select, byte 6 = ~trigger outputs,
 * bytes 5/4/3(low 5 bits) = 21 LED columns, byte 7 bits 2..5 = active-low
 * transport LEDs on rows 0/1. RX frame carries 15 lead clocks then the 32
 * active-high input bits, low byte first, MSB first within each byte. */
#define PANEL_TX_STREAM DMA_STREAM(DMA2, 3)
#define PANEL_RX_STREAM DMA_STREAM(DMA2, 2)
#define PANEL_FRAMES 16
#define PANEL_HT_FLAG (1u << 26) /* DMA2 LISR stream-3 half-transfer  */
#define PANEL_TC_FLAG (1u << 27) /* DMA2 LISR stream-3 full-transfer  */

/* CV observer: SAI1 block B, eight 32-bit slots streamed by DMA2 stream 6.
 * Slots 0..5 = A MOD, A PITCH, B MOD, B PITCH, C MOD, C PITCH. Samples are
 * signed 24-bit, descending from 0x7fffff as the output level rises. */
#define SAI1B_CR 0x40015c24u
#define SAI1B_DR 0x40015c40u
#define SAI1B_CR_RUN 0x300c0u
#define CV_DMA_STREAM DMA_STREAM(DMA2, 6)
#define CV_DMA_CR 0x06005541u
#define CV_SLOTS 8
#define CV_LEVEL_ZERO 0x7fffffu

/* Internal flash programming interface (update mode only!) */
#define FLASH_KEYR 0x40023c04u
#define FLASH_CR   0x40023c10u
#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xcdef89abu
#define FLASH_CR_PG   0x1u
#define FLASH_CR_SER  0x2u
#define FLASH_CR_STRT 0x10000u
#define FLASH_CR_LOCK 0x80000000u

/* USB OTG FS device controller */
#define USB_BASE     0x50000000u
#define USB_GAHBCFG  REG32(USB_BASE + 0x008)
#define USB_GINTSTS  REG32(USB_BASE + 0x014)
#define USB_GINTMSK  REG32(USB_BASE + 0x018)
#define USB_GRXSTSP  REG32(USB_BASE + 0x020)
#define USB_DCTL     REG32(USB_BASE + 0x804)
#define USB_DIEPCTL(ep)  REG32(USB_BASE + 0x900 + 0x20u * (ep))
#define USB_DIEPTSIZ(ep) REG32(USB_BASE + 0x910 + 0x20u * (ep))
#define USB_DOEPCTL(ep)  REG32(USB_BASE + 0xb00 + 0x20u * (ep))
#define USB_FIFO(ep)     REG32(USB_BASE + 0x1000 + 0x1000u * (ep))
#define USB_INT_RXFLVL (1u << 4)
#define USB_INT_SOF    (1u << 3)
#define USB_INT_USBRST (1u << 12)
#define USB_INT_ENUMDNE (1u << 13)

/* NVIC */
#define NVIC_ISER(n) REG32(0xe000e100u + 4u * (n))

/* Panel input map: each named switch is (row, bit) in the 32-bit active-high
 * row value. Encoded here as ids (row << 5) | bit. */
#define SW(row, bit) (((row) << 5) | (bit))
#define SW_ROW(id) ((id) >> 5)
#define SW_BIT(id) ((id) & 31)

#define SW_TAP     SW(0, 0)
#define SW_STOP    SW(0, 1)
#define SW_RECORD  SW(1, 0)
#define SW_PLAY    SW(1, 1)
#define SW_CONTROL SW(0, 18)
#define SW_TRACK_A SW(0, 17)
#define SW_TRACK_B SW(0, 16)
#define SW_TRACK_C SW(1, 18)
#define SW_TRACK_D SW(1, 17)
#define SW_GATE    SW(2, 18)
#define SW_PITCH   SW(2, 17)
#define SW_MOD     SW(3, 18)
#define SW_LOOP    SW(3, 17)
#define SW_BAR1    SW(4, 18)
#define SW_BAR2    SW(4, 17)
#define SW_BAR3    SW(5, 18)
#define SW_BAR4    SW(5, 17)
#define SW_GLOBAL  SW(6, 18)
#define SW_PROJECT SW(6, 17)
#define SW_PATTERN SW(7, 18)
#define SW_SHIFT   SW(7, 17)

/* Pad n (1..16) of bank t (0..3 = A..D): row (n-1)&7, bit from the bank
 * column pair; first eight pads use the first column, pads 9..16 the second. */
static inline uint8_t pad_switch(int track, int pad0 /*0..15*/)
{
    static const uint8_t columns[4][2] = {{26, 24}, {25, 23}, {22, 20}, {21, 19}};
    return (uint8_t)SW(pad0 & 7, columns[track][pad0 >> 3]);
}

/* Encoder quadrature line pairs (first bit of each pair in every row value).
 * Positive rotation follows Gray phases 00 -> 01 -> 11 -> 10. */
#define ENC_TEMPO  0
#define ENC_SWING  1
#define ENC_PARAM1 2
#define ENC_PARAM2 3
#define ENC_PARAM3 4
#define ENC_PARAM4 5
#define ENC_COUNT  6
static const uint8_t ENC_FIRST_BIT[ENC_COUNT] = {4, 6, 12, 14, 8, 10};

/* Bits never driven by any input source; a frame with any of them set is
 * treated as invalid (unwired or test-harness default data). */
#define PANEL_INVALID_BITS 0xf800000cu

#endif

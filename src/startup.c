/* Reset vectors, C runtime initialization and fault containment. */
#include <stddef.h>
#include "board.h"

extern uint32_t _data_load, _data_start, _data_end, _bss_start, _bss_end;
extern uint32_t _stack_top;
void main_entry(void);
void tick_irq_handler(void);

/* Freestanding build: the compiler may still emit calls to these. */
void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    for (; n--; pa++, pb++)
        if (*pa != *pb) return *pa < *pb ? -1 : 1;
    return 0;
}

/* An unrecoverable fault: force every top-edge output inactive (the gate
 * pins are the only outputs reachable without DMA) and stop. Persistent
 * storage is never touched from here. */
static void fault_handler(void)
{
    GPIO_BSRR(GPIOB) = (1u << PIN_GATE_A) | (1u << PIN_GATE_B);
    GPIO_BSRR(GPIOC) = 1u << PIN_GATE_C;
    for (;;) { }
}

void reset_handler(void)
{
    const uint32_t *src = &_data_load;
    uint32_t *dst;
    for (dst = &_data_start; dst < &_data_end; )
        *dst++ = *src++;
    for (dst = &_bss_start; dst < &_bss_end; )
        *dst++ = 0;
    main_entry();
    fault_handler();
}

__attribute__((section(".vectors"), used))
const uintptr_t vectors[96] = {
    [0] = (uintptr_t)&_stack_top,
    [1] = (uintptr_t)reset_handler,
    [2] = (uintptr_t)fault_handler,  /* NMI        */
    [3] = (uintptr_t)fault_handler,  /* HardFault  */
    [4] = (uintptr_t)fault_handler,  /* MemManage  */
    [5] = (uintptr_t)fault_handler,  /* BusFault   */
    [6] = (uintptr_t)fault_handler,  /* UsageFault */
    [16 + TICK_IRQ] = (uintptr_t)tick_irq_handler,
};

/*
 * console.c - interrupt-driven UART driver (USART2, PA2 = TX, PA3 = RX).
 *
 * Writing a byte to a UART takes ~87 us at 115200 baud. Instead of making
 * tasks wait for each byte, k_console_put() drops bytes into a ring buffer
 * and returns. The UART raises an interrupt each time it can take another
 * byte (TXE), and the interrupt handler feeds it from the ring. If the ring
 * is full, the writing task sleeps until the interrupt has made room.
 *
 * Received bytes go the other way: RX interrupt -> ring -> waiting reader.
 * Each ring has exactly one producer and one consumer, and all accesses
 * happen with interrupts off, so no further locking is needed.
 */
#include <stdarg.h>

#include "arch.h"
#include "board.h"
#include "kernel.h"

#define RCC_AHB1ENR REG32(0x40023830u)
#define RCC_APB1ENR REG32(0x40023840u)
#define GPIOA_MODER REG32(0x40020000u)
#define GPIOA_AFRL  REG32(0x40020020u)
#define USART2_SR   REG32(0x40004400u)
#define USART2_DR   REG32(0x40004404u)
#define USART2_BRR  REG32(0x40004408u)
#define USART2_CR1  REG32(0x4000440Cu)
#define USART2_IRQ  38u

#define SR_RXNE   (1u << 5)
#define SR_TXE    (1u << 7)
#define CR1_RE    (1u << 2)
#define CR1_TE    (1u << 3)
#define CR1_RXNEIE (1u << 5)
#define CR1_TXEIE (1u << 7)
#define CR1_UE    (1u << 13)

#define TX_SIZE 1024u /* powers of two, so `& (SIZE-1)` wraps the index */
#define RX_SIZE 128u

static uint8_t tx_buf[TX_SIZE];
static uint8_t rx_buf[RX_SIZE];
static uint32_t tx_head, tx_tail; /* head = next write, tail = next read */
static uint32_t rx_head, rx_tail; /* free-running; used = head - tail    */

k_list_t k_console_tx_waiters, k_console_rx_waiters;

void k_console_init(void)
{
    list_init(&k_console_tx_waiters);
    list_init(&k_console_rx_waiters);

    RCC_AHB1ENR |= 1u << 0;  /* GPIOA clock  */
    RCC_APB1ENR |= 1u << 17; /* USART2 clock */
    (void)RCC_APB1ENR;       /* read back: give the clock a cycle to start */

    /* PA2, PA3 -> alternate function 7 (USART2) */
    GPIOA_MODER = (GPIOA_MODER & ~(0xFu << 4)) | (0xAu << 4);
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFFu << 8)) | (0x77u << 8);

    USART2_BRR = (CPU_HZ + CONSOLE_BAUD / 2u) / CONSOLE_BAUD;
    USART2_CR1 = CR1_UE | CR1_TE | CR1_RE | CR1_RXNEIE;

    NVIC_IPR(USART2_IRQ) = PRIO_UART;
    NVIC_ISER(USART2_IRQ / 32u) = 1u << (USART2_IRQ % 32u);
}

/* Queue up to len bytes for transmission; returns how many fit.
 * "\n" is sent as "\r\n" so serial terminals start a fresh line. */
uint32_t k_console_put(const uint8_t *p, uint32_t len)
{
    uint32_t s = irq_save();
    uint32_t n = 0;
    while (n < len) {
        uint32_t need = p[n] == '\n' ? 2u : 1u;
        if (TX_SIZE - (tx_head - tx_tail) < need)
            break;
        if (p[n] == '\n')
            tx_buf[tx_head++ & (TX_SIZE - 1u)] = '\r';
        tx_buf[tx_head++ & (TX_SIZE - 1u)] = p[n++];
    }
    if (n)
        USART2_CR1 |= CR1_TXEIE; /* the ISR will start draining */
    irq_restore(s);
    return n;
}

uint32_t k_console_get(uint8_t *p, uint32_t len)
{
    uint32_t s = irq_save();
    uint32_t n = 0;
    while (n < len && rx_head != rx_tail)
        p[n++] = rx_buf[rx_tail++ & (RX_SIZE - 1u)];
    irq_restore(s);
    return n;
}

void USART2_IRQHandler(void)
{
    uint32_t s = irq_save();
    uint32_t sr = USART2_SR;

    if (sr & SR_RXNE) {
        uint8_t c = (uint8_t)USART2_DR; /* reading DR clears RXNE */
        if (rx_head - rx_tail < RX_SIZE)
            rx_buf[rx_head++ & (RX_SIZE - 1u)] = c;
        k_wake_all(&k_console_rx_waiters, K_OK);
    }

    if ((sr & SR_TXE) && (USART2_CR1 & CR1_TXEIE)) {
        if (tx_head != tx_tail)
            USART2_DR = tx_buf[tx_tail++ & (TX_SIZE - 1u)];
        else
            USART2_CR1 &= ~CR1_TXEIE; /* nothing left: stop interrupting */
        if (TX_SIZE - (tx_head - tx_tail) >= TX_SIZE / 2u)
            k_wake_all(&k_console_tx_waiters, 0);
    }

    k_reschedule();
    irq_restore(s);
}

/* Polled output for boot messages and panics: first flush whatever the
 * ring still holds (to keep output in order), then send s directly. */
static void putc_sync(char c)
{
    while (!(USART2_SR & SR_TXE)) {
    }
    USART2_DR = (uint8_t)c;
}

void k_console_puts_sync(const char *str)
{
    uint32_t s = irq_save();
    while (tx_head != tx_tail)
        putc_sync((char)tx_buf[tx_tail++ & (TX_SIZE - 1u)]);
    for (; *str; str++) {
        if (*str == '\n')
            putc_sync('\r');
        putc_sync(*str);
    }
    irq_restore(s);
}

void kprintf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    k_console_puts_sync(line);
}

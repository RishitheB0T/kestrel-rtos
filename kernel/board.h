/*
 * board.h - the few things that differ between STM32F4 boards.
 *
 * Select a board with `make BOARD=...`. All boards run from the 16 MHz
 * internal HSI oscillator (the reset default), so no clock-tree setup is
 * needed and the same binary logic works on every STM32F4.
 *
 * The console is USART2 on PA2 (TX) / PA3 (RX) at 115200 8N1 everywhere.
 * On Nucleo boards this is wired to the ST-LINK virtual COM port, so you
 * just open the USB serial port. On other boards connect a 3.3 V USB-UART
 * adapter to PA2/PA3/GND.
 */
#ifndef KESTREL_BOARD_H
#define KESTREL_BOARD_H

#if defined(BOARD_QEMU)
/* QEMU "netduinoplus2" machine: an emulated STM32F405. QEMU clocks the
 * core (and therefore SysTick) at 168 MHz regardless of RCC settings. */
#define BOARD_NAME     "qemu-netduinoplus2 (STM32F405)"
#define CPU_HZ         168000000u
#define LED_PORT       0          /* GPIOA; QEMU ignores GPIO writes       */
#define LED_PIN        5
#define LED_ACTIVE_LOW 0
#define HAS_SEMIHOSTING 1

#elif defined(BOARD_NUCLEO_F4)
/* NUCLEO-F401RE / F411RE / F446RE: user LED LD2 on PA5. */
#define BOARD_NAME     "nucleo-f4"
#define CPU_HZ         16000000u
#define LED_PORT       0          /* GPIOA */
#define LED_PIN        5
#define LED_ACTIVE_LOW 0

#elif defined(BOARD_DISCO_F407)
/* STM32F4DISCOVERY (STM32F407VG): green LED LD4 on PD12. */
#define BOARD_NAME     "stm32f4-discovery"
#define CPU_HZ         16000000u
#define LED_PORT       3          /* GPIOD */
#define LED_PIN        12
#define LED_ACTIVE_LOW 0

#elif defined(BOARD_BLACKPILL)
/* WeAct "Black Pill" STM32F401/F411: blue LED on PC13, lit when low. */
#define BOARD_NAME     "blackpill-f4"
#define CPU_HZ         16000000u
#define LED_PORT       2          /* GPIOC */
#define LED_PIN        13
#define LED_ACTIVE_LOW 1

#else
#error "Unknown board: build with make BOARD=qemu|nucleo_f4|disco_f407|blackpill"
#endif

#ifndef HAS_SEMIHOSTING
#define HAS_SEMIHOSTING 0
#endif

#define CONSOLE_BAUD   115200u
#define FLASH_REGION_SIZE (256u * 1024u)  /* must match linker/stm32f4.ld */

/* GPIO ports are 1 KiB apart, which is also a legal MPU region size, so a
 * task can be granted exactly one port. */
#define GPIO_BASE(port) (0x40020000u + 0x400u * (port))
#define LED_GPIO_BASE   GPIO_BASE(LED_PORT)

#endif

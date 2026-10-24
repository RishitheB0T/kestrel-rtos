# Kestrel RTOS build
#
#   make                      build for QEMU (default)
#   make run                  boot it in QEMU with an interactive shell
#   make test                 run the on-target test suite in QEMU
#   make host-test            run the NN kernel tests natively on your PC
#   make BOARD=nucleo_f4      build for a real board (see kernel/board.h)
#   make BOARD=nucleo_f4 flash
#   make model                retrain + requantize the CNN (needs Python deps)

BOARD    ?= qemu
AUTOTEST ?= 0
CROSS    ?= arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size
QEMU     ?= qemu-system-arm
PYTHON   ?= python3

BOARD_DEF_qemu       := BOARD_QEMU
BOARD_DEF_nucleo_f4  := BOARD_NUCLEO_F4
BOARD_DEF_disco_f407 := BOARD_DISCO_F407
BOARD_DEF_blackpill  := BOARD_BLACKPILL
BOARD_DEF := $(BOARD_DEF_$(BOARD))
ifeq ($(BOARD_DEF),)
$(error Unknown BOARD '$(BOARD)'. Use qemu, nucleo_f4, disco_f407 or blackpill)
endif

BUILD := build/$(BOARD)
ifeq ($(AUTOTEST),1)
BUILD := build/$(BOARD)-autotest
endif
ELF := $(BUILD)/kestrel.elf

ARCH := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS := $(ARCH) -std=gnu11 -O2 -g3 -Wall -Wextra -Wshadow -Wno-unused-parameter \
          -ffreestanding -fno-common -fno-strict-aliasing -ffunction-sections \
          -fdata-sections -fno-tree-loop-distribute-patterns \
          -Iinclude -Ikernel -Inn -Iapp -D$(BOARD_DEF) -MMD -MP
ifeq ($(AUTOTEST),1)
CFLAGS += -DKESTREL_AUTOTEST
endif
LDFLAGS := $(ARCH) -nostdlib -nostartfiles -T linker/stm32f4.ld -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/kestrel.map -Wl,--print-memory-usage
LDLIBS := -lgcc

SRCS := $(wildcard kernel/*.c kernel/*.S lib/*.c nn/*.c app/*.c app/*.S)
OBJS := $(patsubst %,$(BUILD)/%.o,$(SRCS))

# -icount shift=0: one emulated instruction = 1 ns of virtual time, which
# makes timing repeatable and lets the firmware report instruction counts.
QEMU_FLAGS := -M netduinoplus2 -display none -serial null -icount shift=0 \
              -semihosting-config enable=on,target=native

.PHONY: all run test host-test model flash size clean

all: $(ELF) $(BUILD)/kestrel.bin
	@$(SIZE) $(ELF)

$(ELF): $(OBJS) linker/stm32f4.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BUILD)/kestrel.bin: $(ELF)
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Interactive shell in QEMU. Quit with Ctrl-A then X.
run: all
	$(QEMU) $(QEMU_FLAGS) -serial mon:stdio -kernel $(ELF)

# On-target test suite in QEMU; exits non-zero if any test fails.
test:
	$(MAKE) BOARD=qemu AUTOTEST=1 all
	$(PYTHON) tools/qemu_run.py build/qemu-autotest/kestrel.elf --log build/test.log \
	    --trace-json build/trace.json

host-test:
	$(MAKE) -C tests/host run

model:
	$(PYTHON) tools/nn/compile_model.py

flash: all
	openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
	    -c "program $(ELF) verify reset exit"

clean:
	rm -rf build
	$(MAKE) -C tests/host clean

-include $(OBJS:.o=.d)

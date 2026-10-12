CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
PYTHON  ?= python3
SPEC_DIR ?= ../../spec
EMULATOR_DIR ?= ../../emulator

VERSION_MAJOR = 1
VERSION_MINOR = 0

# Built as the ARMv6-M (Thumb-1) subset: it executes unchanged on the
# Cortex-M7, and producing no IT blocks keeps interrupt delivery working on
# the native emulator's stock Unicorn build (see the emulator's engine notes
# about the 2.1.3 IT-state patch). Use ARCH=-mcpu=cortex-m7 for Thumb-2.
ARCH ?= -march=armv6-m
CFLAGS = $(ARCH) -mthumb -mfloat-abi=soft -O2 -g \
         -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
         -Wl,--build-id=none

SRCS = src/startup.c src/main.c src/panel.c src/display.c src/outputs.c \
       src/midi.c src/usb.c src/storage.c src/updater.c
HDRS = $(wildcard src/*.h)

all: build/euclidean.vsb

build/euclidean.elf: $(SRCS) $(HDRS) linker.ld tools/check_blocks.py
	@mkdir -p build
	$(CC) $(CFLAGS) -T linker.ld $(SRCS) -lgcc -o $@
	$(PYTHON) tools/check_blocks.py $@ --objdump $(CROSS)objdump

build/euclidean.bin: build/euclidean.elf
	$(OBJCOPY) -O binary $< $@

build/euclidean.vsb: build/euclidean.bin tools/make_vsb.py
	$(PYTHON) tools/make_vsb.py $< $@ --major $(VERSION_MAJOR) --minor $(VERSION_MINOR)

# Run the pinned conformance spec's seven image-profile checks.
check: build/euclidean.vsb
	cd $(SPEC_DIR) && $(PYTHON) -m sq64_spec $(abspath $<) --profile image

# Headless smoke run in the native emulator (requires `pip install -e` there).
smoke: build/euclidean.vsb
	cd $(EMULATOR_DIR) && $(PYTHON) -m sq64_emulator $(abspath $<) \
	  --headless --instructions 2000000 --report $(abspath build/smoke.json)

# Behavioral tests. The Python harness needs the emulator's dependencies
# (unicorn 2.1.3, capstone, pyelftools); the Node tests use the emulator's
# vendored WASM engine and need no installation.
test: build/euclidean.vsb
	$(PYTHON) tools/headless_test.py --emulator $(EMULATOR_DIR)
	node tools/browser_update_test.mjs $(EMULATOR_DIR)/web
	node tools/browser_panel_test.mjs $(EMULATOR_DIR)/web

clean:
	rm -rf build

.PHONY: all check smoke test clean

HOST := $(abspath .)
REPO := $(abspath ../..)
.DEFAULT_GOAL := all
BUILD ?= $(HOST)/build
CLANG ?= clang
LLD ?= ld.lld
LLVM_OBJCOPY ?= llvm-objcopy
LLVM_OBJDUMP ?= llvm-objdump
PYTHON ?= python3
CC ?= cc
CARGO ?= cargo
PATCH ?= patch

TARGET := arm-none-eabi
CPUFLAGS := --target=$(TARGET) -mcpu=arm7tdmi -marm -mfloat-abi=soft
COMMON := $(CPUFLAGS) -ffreestanding -fno-builtin -fno-stack-protector \
  -fno-unwind-tables -fno-asynchronous-unwind-tables -fdata-sections \
  -ffunction-sections -Os -Wall -Wextra -Werror -I$(HOST)/include
# Production runtime; experimental and stub profiles are not shipped.
ifeq ($(strip $(LAUNCHER_POCKET_INPUT)),)
$(error Set LAUNCHER_POCKET_INPUT to the compiled host launcher .pocket package)
endif
LCD_TYPE ?= 0
ifeq ($(strip $(EMBEDDED_POCKET_INPUT)),)
$(error Set EMBEDDED_POCKET_INPUT to an ordinary ipod-photo recovery .pocket package)
endif
DEFINES := -DPJS_A1099_LCD_TYPE=$(LCD_TYPE) -DPJS_BOOTLOADER_HANDOFF=1 -DPJS_PRODUCTION=1
CFLAGS := $(COMMON) -MMD -MP -std=c11 $(DEFINES)
ASFLAGS := $(COMMON) -MMD -MP $(DEFINES)
QUICKJS_SRC ?=
QUICKJS_VERSION ?= 2026-06-04
QUICKJS_PREPARED := $(BUILD)/third-party/quickjs
QUICKJS_STAMP := $(QUICKJS_PREPARED)/.prepared
QUICKJS_NAMES := quickjs dtoa libregexp libunicode cutils
QUICKJS_OBJECTS := $(addprefix $(BUILD)/quickjs-obj/,$(addsuffix .o,$(QUICKJS_NAMES)))

NEWLIB_SYSROOT ?=
NEWLIB_INCLUDE ?= $(NEWLIB_SYSROOT)/include
NEWLIB_LIBC ?= $(NEWLIB_SYSROOT)/newlib/libc_nano.a
NEWLIB_LIBM ?= $(NEWLIB_SYSROOT)/newlib/libm.a
NEWLIB_LIBGCC ?= $(NEWLIB_SYSROOT)/libgcc/libgcc.a
NEWLIB_LIBS := $(NEWLIB_LIBC) $(NEWLIB_LIBM) $(NEWLIB_LIBGCC)

RUST_MANIFEST := $(HOST)/rust-core/Cargo.toml
RUST_TARGET_DIR := $(BUILD)/rust-target
RUST_FEATURE_ARGS := --features "production portable-audio portable-fs"
RUST_LIB := $(RUST_TARGET_DIR)/armv4t-none-eabi/release/libpocketjs_ipod_photo_core.a
CORE_DEP := $(RUST_LIB)
CORE_LINK := $(RUST_LIB)
RUNTIME_DEP := $(QUICKJS_STAMP) $(QUICKJS_OBJECTS) $(NEWLIB_LIBS)
RUNTIME_LINK := $(QUICKJS_OBJECTS) $(NEWLIB_LIBS)
RUNTIME_INCLUDES := -isystem $(NEWLIB_INCLUDE) -isystem $(QUICKJS_PREPARED)
RUNTIME_DEFINES := -D_GNU_SOURCE -DPJS_QUICKJS_BAREMETAL -DCONFIG_VERSION=\"$(QUICKJS_VERSION)\"
RECOVERY_POCKET := $(BUILD)/generated/recovery.pocket
EMBEDDED_PACKAGE_C := $(BUILD)/generated/embedded_package.c
EMBEDDED_PACKAGE_O := $(BUILD)/generated/embedded_package.o
EMBEDDED_PACKAGE_STAMP := $(BUILD)/generated/.package-ready
EMBEDDED_PACKAGE_DEP := $(EMBEDDED_POCKET_INPUT) $(LAUNCHER_POCKET_INPUT)
EMBEDDED_PACKAGE_ARGS := --input-package $(EMBEDDED_POCKET_INPUT) --launcher-package $(LAUNCHER_POCKET_INPUT)
SOURCES := src/audio.c src/audio_clock.c src/audio_dma.c src/audio_pcm.c src/audio_pcm_mixer.c src/backlight.c src/cache.c src/data_fs.c src/fat32.c src/fs_store.c src/heap.c src/input.c src/irq.c src/lcd.c src/libc_shim.c src/lineage_record.c src/main.c src/mem.c src/native_loader.c src/package_store.c src/panic.c src/power.c src/qjs_runtime.c src/scheduler.c src/state_record.c src/storage.c src/timer.c src/timer_irq.c src/usb_device.c src/usb_protocol.c
OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES)) $(EMBEDDED_PACKAGE_O) \
           $(BUILD)/boot/crt0.o $(BUILD)/boot/audio-clock.o $(BUILD)/boot/usb-chainload.o
DEPFILES := $(OBJECTS:.o=.d)
-include $(DEPFILES)

IMAGE_NAME ?= pocketjs-ipod-photo
ELF := $(BUILD)/$(IMAGE_NAME).elf
BIN := $(BUILD)/$(IMAGE_NAME).bin
IPOD := $(BUILD)/$(IMAGE_NAME).ipod
MAP := $(BUILD)/$(IMAGE_NAME).map
DIS := $(BUILD)/$(IMAGE_NAME).dis
SHA256SUMS := $(BUILD)/SHA256SUMS.txt

.PHONY: all inspect manifest rust-core package quickjs-source
all: $(IPOD) $(DIS) inspect manifest

# The main-thread PCM producer must meet a 1024-frame/23.22 ms deadline.
# Optimize these two bounded hot paths for throughput, not minimum code size.
$(BUILD)/src/audio_pcm_mixer.o $(BUILD)/src/audio_pcm.o: CFLAGS += -O2

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) $(RUNTIME_INCLUDES) $(RUNTIME_DEFINES) -c $< -o $@

$(BUILD)/src/qjs_runtime.o: src/qjs_runtime.c $(QUICKJS_STAMP)
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) $(RUNTIME_INCLUDES) $(RUNTIME_DEFINES) -c $< -o $@

$(BUILD)/boot/crt0.o: boot/crt0.S
	@mkdir -p $(dir $@)
	$(CLANG) $(ASFLAGS) -c $< -o $@

$(BUILD)/boot/audio-clock.o: boot/audio-clock.S
	@mkdir -p $(dir $@)
	$(CLANG) $(ASFLAGS) -c $< -o $@

$(BUILD)/boot/usb-chainload.o: boot/usb-chainload.S
	@mkdir -p $(dir $@)
	$(CLANG) $(ASFLAGS) -c $< -o $@

$(EMBEDDED_PACKAGE_STAMP): $(EMBEDDED_PACKAGE_DEP) tools/build_embedded_pocket.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/build_embedded_pocket.py \
	  $(EMBEDDED_PACKAGE_ARGS) \
	  --package $(RECOVERY_POCKET) \
	  --c-output $(EMBEDDED_PACKAGE_C)
	@touch $@

$(RECOVERY_POCKET) $(EMBEDDED_PACKAGE_C): $(EMBEDDED_PACKAGE_STAMP)
	@test -f $@

$(EMBEDDED_PACKAGE_O): $(EMBEDDED_PACKAGE_C)
	@mkdir -p $(dir $@)
	$(CLANG) $(CFLAGS) -c $< -o $@

package: $(RECOVERY_POCKET) $(EMBEDDED_PACKAGE_C)

$(QUICKJS_STAMP): third_party/quickjs.lock third_party/quickjs-baremetal.patch
	@test -n "$(QUICKJS_SRC)" || { echo 'set QUICKJS_SRC=/path/to/pinned QuickJS source' >&2; exit 2; }
	@test -f "$(QUICKJS_SRC)/quickjs.c" || { echo 'QUICKJS_SRC has no quickjs.c' >&2; exit 2; }
	@test -f "$(QUICKJS_SRC)/VERSION" || { echo 'QUICKJS_SRC has no VERSION' >&2; exit 2; }
	@test "$$(cat "$(QUICKJS_SRC)/VERSION")" = "$(QUICKJS_VERSION)" || { \
	  echo 'QuickJS version does not match third_party/quickjs.lock' >&2; exit 2; }
	@test -d "$(NEWLIB_INCLUDE)" || { echo 'set NEWLIB_SYSROOT to the ARMv4T newlib export' >&2; exit 2; }
	@for library in $(NEWLIB_LIBS); do test -f "$$library" || { echo "missing $$library" >&2; exit 2; }; done
	rm -rf $(QUICKJS_PREPARED)
	mkdir -p $(QUICKJS_PREPARED)
	cp -a $(QUICKJS_SRC)/. $(QUICKJS_PREPARED)/
	cd $(QUICKJS_PREPARED) && $(PATCH) -p1 < $(HOST)/third_party/quickjs-baremetal.patch
	@touch $@

quickjs-source: $(QUICKJS_STAMP)

$(BUILD)/quickjs-obj/%.o: $(QUICKJS_STAMP)
	@mkdir -p $(dir $@)
	$(CLANG) $(CPUFLAGS) -ffreestanding -fno-builtin -fno-stack-protector \
	  -fno-unwind-tables -fno-asynchronous-unwind-tables -fdata-sections \
	  -ffunction-sections -fno-strict-aliasing -fwrapv -Os -std=gnu11 -w \
	  -isystem $(NEWLIB_INCLUDE) -I$(QUICKJS_PREPARED) \
	  -D_GNU_SOURCE -DPJS_QUICKJS_BAREMETAL \
	  -DCONFIG_VERSION=\"$(QUICKJS_VERSION)\" \
	  -c $(QUICKJS_PREPARED)/$*.c -o $@

$(RUST_LIB): $(shell find $(HOST)/rust-core $(REPO)/engine/core -type f -not -path '*/target/*')
	@mkdir -p $(RUST_TARGET_DIR)
	cd $(HOST)/rust-core && CARGO_TARGET_DIR=$(RUST_TARGET_DIR) $(CARGO) build --release --locked $(RUST_FEATURE_ARGS)

rust-core: $(RUST_LIB)

$(ELF): $(OBJECTS) $(CORE_DEP) $(RUNTIME_DEP) boot/linker.ld
	$(LLD) -m armelf -T boot/linker.ld -Map=$(MAP) --gc-sections \
	  $(OBJECTS) --start-group $(CORE_LINK) $(RUNTIME_LINK) --end-group -o $@

$(MAP): $(ELF)
	@test -f $@

$(BIN): $(ELF)
	$(LLVM_OBJCOPY) -O binary $< $@

$(IPOD): $(BIN) tools/pack_ipod.py
	$(PYTHON) tools/pack_ipod.py $< $@

$(DIS): $(ELF)
	$(LLVM_OBJDUMP) -d --no-show-raw-insn $< > $@

inspect: $(ELF) $(BIN) $(IPOD) $(MAP)
	$(PYTHON) tools/inspect_image.py $(BIN)
	$(PYTHON) tools/inspect_image.py $(IPOD)
	$(PYTHON) tools/verify_build.py --elf $(ELF) --bin $(BIN) --ipod $(IPOD) \
	  --map $(MAP) --require-handoff-signature --kernel-symbol kernel_main \
	  --max-image-bytes 2097152
	$(PYTHON) tools/verify_runtime.py --elf $(ELF) --bin $(BIN) --ipod $(IPOD) \
	  --backend rust --require-power-telemetry --require-quickjs

manifest: $(ELF) $(BIN) $(IPOD) $(MAP) $(DIS) $(RECOVERY_POCKET)
	@cd $(BUILD) && sha256sum \
	  $$(basename $(ELF)) $$(basename $(BIN)) $$(basename $(IPOD)) \
	  $$(basename $(MAP)) $$(basename $(DIS)) \
	  generated/$$(basename $(RECOVERY_POCKET)) > $$(basename $(SHA256SUMS))

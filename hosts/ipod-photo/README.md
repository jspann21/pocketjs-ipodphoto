# Apple iPod Photo (A1099)

**The `ipod-photo` target runs the PocketJS Rust UI core and QuickJS on the
PP5020, with a native 220 × 176 RGB565 display.** This host owns startup,
interrupts, LCD, wheel/buttons, ATA, audio, USB CDC, and power management.
Rockbox is a hardware reference and an optional bootloader; its runtime is
not linked into PocketJS.

The port follows [repository placement](../../docs/STRUCTURE.md), the
[runtime boundaries](../../docs/RUNTIMES.md), and the shared
[native contract](../../site/content/docs/native-contract.md). App source uses
`@pocketjs/framework/*` and imports Solid primitives from `solid-js`.

## Supported contract

| Property | Value |
| --- | --- |
| Target / ABI | `ipod-photo` / `1` |
| Viewport | 220 × 176, `native`, raster density 1 |
| Capabilities | `input.buttons`, `text.glyphs.baked`, `audio.pcm`, `data.fs` |
| Rendering | Retained UI, layout, baked text/assets, styles and animation |
| Audio | Portable PCM streams mixed into the hardware output |
| Storage | App-id namespaces in two preallocated filesystem banks |
| App packages | Standard `.pocket` container, checked target/ABI/plan/hash |

Touch, analog input, networking, video, and runtime glyph rasterization are
not advertised. The wheel maps to discrete navigation buttons. The host's
launcher lives in `launcher/`; its private catalog bridge is host UI plumbing,
not an API for ordinary apps. Its exact package hash is generated at build time.

## Build

Use Bun 1.4.0 and Linux or WSL2 with Clang/LLD, LLVM binutils, GNU Make,
Python 3, patch, and the ARM bare-metal GCC/newlib packages. On Ubuntu 24.04:

```sh
sudo apt-get install clang lld llvm make python3 patch git \
  gcc-arm-none-eabi libnewlib-arm-none-eabi libnewlib-dev
rustup toolchain install nightly-2026-07-02 --profile minimal --component rust-src
```

From the repository root:

```sh
bun install --frozen-lockfile
bun tools/pocket.ts build --project-root . \
  --manifest apps/ipod-photo/pocket.json --target ipod-photo
bun tools/pocket.ts build --project-root . \
  --manifest hosts/ipod-photo/launcher/launcher.pocket.json --target ipod-photo
bun tools/pocket-pack.ts verify dist/packages/pocketjs-ipod-photo.pocket
bun tools/pocket-pack.ts verify dist/packages/pocketjs-ipod-photo-launcher.pocket
```

Fetch the source pinned in `third_party/quickjs.lock` and export the ARMv4T
libraries. The commands below keep dependencies and outputs under the ignored
host build directory:

```sh
REPO="$PWD"
HOST="$REPO/hosts/ipod-photo"
BUILD="$HOST/build"
mkdir -p "$BUILD/deps"
git clone https://github.com/pocket-stack/quickjs-rs.git "$BUILD/deps/quickjs-rs"
git -C "$BUILD/deps/quickjs-rs" checkout ba5bdd0dc013518768e76cd9e05cd30ed53dd35b
SYSROOT="$BUILD/deps/armv4t-sysroot"
mkdir -p "$SYSROOT/include" "$SYSROOT/newlib" "$SYSROOT/libgcc"
cp -a /usr/include/newlib/. "$SYSROOT/include/"
cp "$(arm-none-eabi-gcc -mcpu=arm7tdmi -marm -mfloat-abi=soft -print-libgcc-file-name)" "$SYSROOT/libgcc/libgcc.a"
cp "$(arm-none-eabi-gcc -mcpu=arm7tdmi -marm -mfloat-abi=soft --specs=nano.specs -print-file-name=libc_nano.a)" "$SYSROOT/newlib/libc_nano.a"
cp "$(arm-none-eabi-gcc -mcpu=arm7tdmi -marm -mfloat-abi=soft -print-file-name=libm.a)" "$SYSROOT/newlib/libm.a"
make -C "$HOST" -j4 \
  QUICKJS_SRC="$BUILD/deps/quickjs-rs/libquickjs-sys/embed/quickjs" \
  NEWLIB_SYSROOT="$SYSROOT" \
  EMBEDDED_POCKET_INPUT="$REPO/dist/packages/pocketjs-ipod-photo.pocket" \
  LAUNCHER_POCKET_INPUT="$REPO/dist/packages/pocketjs-ipod-photo-launcher.pocket"
```

**The default Makefile builds the production runtime.** It checks the ARM
vectors, memory layout, required runtime symbols, image size and iPod wrapper
checksum, then writes `build/pocketjs-ipod-photo.ipod` and `SHA256SUMS.txt`.
Use a fresh `BUILD` directory when changing toolchains or compiler flags.

## Data directory and boot

Prepare initial files in a new directory on the computer:

```sh
python3 hosts/ipod-photo/tools/prepare_data.py \
  hosts/ipod-photo/build/data \
  --recovery dist/packages/pocketjs-ipod-photo.pocket \
  --launcher dist/packages/pocketjs-ipod-photo-launcher.pocket
```

Copy the generated `POCKETJS` directory to the iPod's FAT32 data volume for
a **first installation**. For an existing installation, preserve `STATE0.BIN`,
`STATE1.BIN`, `FSBANK0.BIN` and `FSBANK1.BIN`; do not replace saved banks with
new seeds. Copy ordinary packages to `POCKETJS/APPS/NAME.PKT`, using uppercase
8.3 filenames. `LAUNCHER.PKT` must match the launcher used to build firmware.

A compatible iPod Photo Rockbox bootloader can load the image as
`.rockbox/rockbox.ipod`. The included file installer verifies the image and
backs up the original file before replacing it:

```sh
python3 hosts/ipod-photo/tools/handoff.py install \
  --mount /path/to/ipod --probe hosts/ipod-photo/build/pocketjs-ipod-photo.ipod
python3 hosts/ipod-photo/tools/handoff.py status --mount /path/to/ipod
# To restore the backed-up bootloader file:
python3 hosts/ipod-photo/tools/handoff.py restore --mount /path/to/ipod
```

This is a file-level installation. It does not install a bootloader or modify
the native OSOS firmware partition. Devices already using native OSOS entry
need their own verified firmware backup and install/restore tooling; replacing
`rockbox.ipod` does not change that entry. Device-specific raw-disk scripts and
firmware dumps are not part of this port.

## Controls and development

Wheel moves selection; Center selects; Menu returns within an app. Hold
Menu + Left for two seconds and release to return to Apps. The same chord in
Apps restarts the device. Hold blocks controls. Inactivity on battery enters
the sleep lifecycle; buttons wake it. A hard reset or guest fault can invoke
the last-known-good or embedded recovery package.

The production image exposes diagnostics and RAM package cycles through USB
CDC. On Windows, install `pyserial` and run the runner with Windows Python:

```powershell
py -3 -m pip install pyserial
py -3 hosts/ipod-photo/tools/ipod_usb_runner.py discover
py -3 hosts/ipod-photo/tools/ipod_usb_runner.py info --port COM7 --json
py -3 hosts/ipod-photo/tools/ipod_usb_runner.py maintenance --port COM7
py -3 hosts/ipod-photo/tools/ipod_usb_runner.py cycle --port COM7 --package dist/packages/pocketjs-ipod-photo.pocket --json
```

Replace COM7 with the discovered port. Firmware uploads and persistent USB
package updates are disabled in this profile. USB transfers can require a
handle reopen; inspect the device after a lost mutation reply before retrying.

## Validation scope

The source derives from an A1099 implementation exercised on hardware. The
clean extraction is build- and software-validated; its newly linked image has
not been installed or qualified on hardware. Battery-current measurement,
destructive power-cut testing, and other iPod models are outside the claimed
validation. Historical campaigns and device-specific receipts remain outside
this repository.

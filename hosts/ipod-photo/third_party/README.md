# Third-party runtime inputs

The standalone A1099 host does not vendor generated QuickJS binaries. The build
requires the exact source tree pinned by `quickjs.lock`, copies that tree into
the build directory, applies `quickjs-baremetal.patch`, and compiles it for
ARMv4T with the same soft-float ABI as the host.

The patch is intentionally narrow: it disables host atomics, uses the
freestanding malloc header path, and fixes the unavailable local-time offset to
UTC. Filesystem, process, dynamic-library, and terminal helpers from
`quickjs-libc.c` are not linked into the firmware.

# Runtime

Pingouin's QEMU, built from source: `./build.sh` makes `.build/qemu-gpu-runtime/`
(`bin/qemu-system-aarch64`, its libraries in `lib/` with the Vulkan loader and MoltenVK,
MoltenVK's driver file in `share/vulkan/icd.d/`, QEMU's UEFI firmware in `share/qemu/`). It needs Apple Silicon, macOS 15 or newer, the Command Line Tools,
python3 3.11+ (not Apple's) and pkg-config. A clean build takes about 20 minutes.

- `patches.list`: the patches, in order, for libslirp, QEMU, virglrenderer and SPIRV-Tools, and why
  each is there. The files are in `patches/`. `TAP:` entries come from
  startergo/homebrew-virglrenderer and are downloaded at build time.
- `build.sh`: downloads the pinned sources and bottles (checked by sha256, cached in
  `.build/cache`), applies the patches, runs each patch's tests from `Tests/`, builds
  libslirp, SPIRV-Tools, virglrenderer (OpenGL, and Vulkan through Venus on MoltenVK; no
  video, no KosmicKrisp, which needs macOS 26) and QEMU, then copies, relocates, signs
  (`qemu-hvf.entitlements`) and checks the result, and runs the Venus tests on the staged
  MoltenVK.
- `pinned-runtime-bottles.sh`, `build-python.sh`, `bundle-macho-dependencies.sh`,
  `verify-macos-compatibility.sh`: helpers that `build.sh` uses.

Environment variables the runtime reads all start with `PINGOUIN_`. The ones the app
is most likely to set:
`PINGOUIN_PRODUCT_NAME`, `PINGOUIN_ICON` (the Dock name and icon),
`PINGOUIN_FULLPANEL=1` (full screen including the notch),
`PINGOUIN_BOOT_SPLASH=0` (turns the boot splash off), and
`PINGOUIN_CLEAN_SIZE=1` (inherited size snapping, off by default).

Vulkan needs the device flags `blob=true,venus=true,hostmem=<size>` on `virtio-gpu-gl-pci`;
on M1/M2 Macs (36-bit VM address space) also `highmem-mmio-size=<size>` on the machine so
the host memory BAR fits (`qemu-virt-small-high-window.patch`). `PINGOUIN_VULKAN_DRIVER`
picks the driver (MoltenVK is the only one in this runtime).

Where this came from: `UPSTREAM`. Licences: `../THIRD_PARTY_NOTICES.md` and
`LICENSE.vulkan.txt`.

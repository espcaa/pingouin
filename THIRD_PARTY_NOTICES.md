# Third-party notices

Pingouin's virtual machine runtime (`runtime/`) is built from third-party code.

## omacvm (MIT)

`runtime/` (build script helpers, most of the patches in `runtime/patches/` and the
tests in `runtime/Tests/`) is derived from omacvm's `app/runtime`,
https://github.com/gillesgoetsch/omacvm, commit 7f0ac829.

```
MIT License

Copyright (c) 2026 Gilles Goetsch

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Try Omarchy (MIT)

omacvm builds on Try Omarchy (https://github.com/omacom/try-omarchy). Its QEMU patches
(the ones marked "Try Omarchy contributors" in `runtime/patches/`) and parts of the build
scripts come from there.

```
MIT License

Copyright (c) 2026 Try Omarchy contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## QEMU (GPL-2.0)

The bundled `qemu-system-aarch64` is QEMU 11.1.1 (https://www.qemu.org, commit
c3d48b7d1e89604920e5b81b91140c2ad39a1943). QEMU is licensed under the GNU General Public
License, version 2; some of its files are under other GPL-2.0-compatible licences (see
`COPYING` and `LICENSE` in QEMU's source). We build our binary from that exact source with
the patches in `runtime/patches/`, applied in the order of `runtime/patches.list` by
`runtime/build.sh`. The script records the source's download URL and sha256. Those
patches are distributed under GPL-2.0, like QEMU itself. Two of them,
`qemu-texture-borrowing-11.1.patch` and `qemu-gpu-spike-resolution-fix.patch`, come from
startergo's homebrew-qemu-virgl-kosmickrisp.

## virglrenderer (MIT)

`lib/libvirglrenderer.1.dylib` is virglrenderer 1.3.0
(https://gitlab.freedesktop.org/virgl/virglrenderer). It is MIT-licensed and derived in
many parts from Mesa (MIT); see `COPYING` in its source. It is built with the macOS patches
from startergo/homebrew-virglrenderer (commit de31b78a, downloaded at build time) and
the `virgl-*` patches in `runtime/patches/` (OpenGL, and Vulkan through Venus).

## Vulkan: MoltenVK, the Vulkan loader, SPIRV-Tools (Apache-2.0)

Venus (Vulkan in the guest) uses three Khronos projects. All are Apache-2.0; none has a
NOTICE file. The full licence texts (Apache-2.0, plus MIT for cJSON and BSD-3-Clause for
cereal) are in `runtime/LICENSE.vulkan.txt`, shipped in the runtime as
`share/licenses/vulkan.txt`.

- `lib/libMoltenVK.dylib`: MoltenVK 1.4.2 (https://github.com/KhronosGroup/MoltenVK),
  Homebrew's unmodified arm64_sequoia bottle. Copyright (c) 2015-2026 The Brenwill
  Workshop Ltd. It has SPIRV-Cross, SPIRV-Tools, SPIRV-Headers and Vulkan-Headers
  (Apache-2.0) and cereal (BSD-3-Clause) built in.
- `lib/libvulkan.1.dylib`: the Khronos Vulkan loader 1.4.357
  (https://github.com/KhronosGroup/Vulkan-Loader), Homebrew's unmodified bottle.
  Copyright (c) 2014-2023 The Khronos Group Inc., Valve Corporation, LunarG, Inc.,
  Google Inc., and others. It has cJSON (MIT) built in.
- SPIRV-Tools 2026.3 (https://github.com/KhronosGroup/SPIRV-Tools), with the
  `spirv-tools-*` patches in `runtime/patches/`, built statically into
  `lib/libvirglrenderer.1.dylib` with hidden symbols. Copyright (c) 2015-2026 The Khronos
  Group Inc., Google Inc., and others.
- SPIRV-Headers (commit 29981f65, https://github.com/KhronosGroup/SPIRV-Headers),
  Apache-2.0 OR MIT, used under Apache-2.0. Copyright 2015-2024 The Khronos Group Inc.

## libslirp (BSD-3-Clause)

`lib/libslirp.0.dylib` is libslirp 4.9.4 (https://gitlab.freedesktop.org/slirp/libslirp)
with the `libslirp-*` patches in `runtime/patches/`. BSD-3-Clause; see `LICENSE` and
`COPYRIGHT` in its source.

## UEFI firmware (edk2)

`share/qemu/edk2-aarch64-code.fd` is the edk2 (TianoCore) firmware that QEMU 11.1.1
ships prebuilt in `pc-bios/`. It is unmodified. Its licences (BSD-2-Clause-Patent and
others) are listed in `share/qemu/edk2-licenses.txt`, which is shipped next to it.

## Other bundled libraries

The runtime's `lib/` also contains unmodified libraries from Homebrew's arm64_sequoia
bottles and from startergo's taps. Each keeps its own licence:

| library                                        | licence           |
| ---------------------------------------------- | ----------------- |
| GLib (libglib-2.0)                             | LGPL-2.1-or-later |
| gettext runtime (libintl)                      | LGPL-2.1-or-later |
| libusb                                         | LGPL-2.1-or-later |
| pixman                                         | MIT               |
| SDL 2 compatibility layer (sdl2-compat), SDL 3 | Zlib              |
| PCRE2                                          | BSD-3-Clause      |
| zstd                                           | BSD-3-Clause      |
| lz4 (liblz4)                                   | BSD-2-Clause      |
| xz (liblzma)                                   | 0BSD              |
| libepoxy                                       | MIT               |
| ANGLE (libEGL, libGLESv2)                      | BSD-3-Clause      |

The LGPL libraries are shipped as separate dynamic libraries. You can replace them with
your own builds of the same versions.

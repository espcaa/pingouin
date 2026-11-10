#!/bin/bash
# Build Pingouin's QEMU runtime: QEMU 11.1.1 (HVF, Cocoa, OpenGL and Vulkan (Venus) through
# virglrenderer), with the patches in patches.list, into .build/qemu-gpu-runtime:
#   bin/qemu-system-aarch64           signed with qemu-hvf.entitlements
#   lib/*.dylib                       every library it needs, nothing from Homebrew; for Venus
#                                     the Vulkan loader (libvulkan.1) and MoltenVK
#   share/vulkan/icd.d/               the loader's driver file for MoltenVK
#   share/licenses/vulkan.txt         MoltenVK's, the loader's and SPIRV-Tools' licences
#   share/qemu/edk2-aarch64-code.fd   QEMU's stock UEFI firmware
#
# Apple Silicon, macOS 15 or newer, Command Line Tools. Needs python3 3.11+ (not Apple's)
# and pkg-config on PATH. Every download is pinned by sha256 and kept in .build/cache.
# Sources and build trees go to .build/work; PINGOUIN_KEEP_WORK=1 keeps them afterwards.
# PINGOUIN_JOBS=N limits parallel compile jobs.
#
# Derived from omacvm's build-qemu-gpu-runtime.sh (gillesgoetsch/omacvm @ 7f0ac829), without
# video decode/encode, KosmicKrisp (Mesa's Vulkan on Metal 4, macOS 26 only: Venus runs on
# MoltenVK here) and the edk2 build.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd -P)
out=$here/.build/qemu-gpu-runtime
cache=$here/.build/cache
work=$here/.build/work
target=15.0                     # oldest macOS the runtime runs on
jobs=(${PINGOUIN_JOBS:+-j "$PINGOUIN_JOBS"})

log() { echo "[pingouin-build] $*"; }
die() { echo "pingouin-build: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Pinned sources
# ---------------------------------------------------------------------------
qemu_commit=c3d48b7d1e89604920e5b81b91140c2ad39a1943   # 11.1.1
virgl_tap_commit=de31b78a88b2f9eea04dff32438e6283b400edf7
keycodemap_commit=f5772a62ec52591ff6870b7e8ef32482371f22c6
dtc_commit=b6910bec11614980a21e46fbccc35934b671bd81
# SPIRV-Tools' validator, built into libvirglrenderer for Venus (virgl-venus-spirv-validate.patch):
# the release, and the SPIRV-Headers commit its DEPS names (MoltenVK 1.4.2 uses the same headers).
spirv_tools_version=2026.3
spirv_headers_commit=29981f65241605e08b0ede4cfeb999fe3b723c6a
cmake_version=3.31.6
pypi=https://files.pythonhosted.org/packages

# file name | url | sha256
sources=(
  "qemu-$qemu_commit.tar.gz|https://gitlab.com/qemu-project/qemu/-/archive/$qemu_commit/qemu-$qemu_commit.tar.gz|7563781d7dec46f11509801e027f852597235d29ca7afa44a07ed9d8b108b8cd"
  "libslirp-v4.9.4.tar.gz|https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.4/libslirp-v4.9.4.tar.gz|3998863b020aeda34bddc567097c6efba55a78cdf6eeee6bcd42c11ef23967da"
  "virglrenderer-1.3.0.tar.gz|https://gitlab.freedesktop.org/virgl/virglrenderer/-/archive/1.3.0/virglrenderer-1.3.0.tar.gz|065bc56e89e6f631f96101cd62eba0748e48eb888b434edc86e89d05395e76f3"
  "homebrew-virglrenderer-$virgl_tap_commit.tar.gz|https://codeload.github.com/startergo/homebrew-virglrenderer/tar.gz/$virgl_tap_commit|373db13456610847b97a2d442f1b7cd2ffa0c9a60c6850dc2185d40b09dfa5e4"
  "keycodemapdb-$keycodemap_commit.tar.gz|https://gitlab.com/qemu-project/keycodemapdb/-/archive/$keycodemap_commit/keycodemapdb-$keycodemap_commit.tar.gz|d014b53382dbb17b8196ad12f50de7f20d0ef1b9f7d54b0be51a6cbb14209195"
  "dtc-$dtc_commit.tar.gz|https://git.kernel.org/pub/scm/utils/dtc/dtc.git/snapshot/dtc-$dtc_commit.tar.gz|e115f987eec23a1ba25150a46ced1675de3716072d3b4905afb3a9cda0f007c7"
  "SPIRV-Tools-$spirv_tools_version.tar.gz|https://codeload.github.com/KhronosGroup/SPIRV-Tools/tar.gz/refs/tags/v$spirv_tools_version|fff0667b25dfb98177da3cc4cc61ac277d076786df42baaf8d9635a25bdf9226"
  "SPIRV-Headers-$spirv_headers_commit.tar.gz|https://codeload.github.com/KhronosGroup/SPIRV-Headers/tar.gz/$spirv_headers_commit|232899f1ad4104fb5bc377b94596c7621575eee62ad9a9e8f929b63a7dd8a7ad"
  # CMake builds SPIRV-Tools (only its command and modules are unpacked).
  "cmake-$cmake_version-macos-universal.tar.gz|https://github.com/Kitware/CMake/releases/download/v$cmake_version/cmake-$cmake_version-macos-universal.tar.gz|330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d"
  "meson-1.9.0.tar.gz|https://github.com/mesonbuild/meson/releases/download/1.9.0/meson-1.9.0.tar.gz|cd27277649b5ed50d19875031de516e270b22e890d9db65ed9af57d18ebc498d"
  "ninja-1.13.0-py3-none-macosx_10_9_universal2.whl|$pypi/3c/74/d02409ed2aa865e051b7edda22ad416a39d81a84980f544f8de717cab133/ninja-1.13.0-py3-none-macosx_10_9_universal2.whl|fa2a8bfc62e31b08f83127d1613d10821775a0eb334197154c4d6067b7068ff1"
  "pyyaml-6.0.3.tar.gz|$pypi/05/8e/961c0007c59b8dd7729d542c61a4d537767a59645b82a0b521206e1e25c2/pyyaml-6.0.3.tar.gz|d76623373421df22fb4cf8817020cbb7ef15c725b9d5e45f17e189bfc384190f"
  # QEMU's configure builds its Python venv offline from these.
  "setuptools-84.0.0-py3-none-any.whl|$pypi/95/9c/c510029fc6ef33a6275cd2c5d3cecd6613dfd6aa401d57c54f1c18852ccf/setuptools-84.0.0-py3-none-any.whl|51a52592b3b99e102b609654876bd65f19f999935166d1352678931132b0c670"
  "packaging-26.3-py3-none-any.whl|$pypi/63/34/ba1c580383c9eada3711951fef0795c80b829a078d72188184bcab9dd527/packaging-26.3-py3-none-any.whl|d7193f7c8e4e93f444fde0262bf90af30e16fa0ad0ad44cb553c87339b23cd1c"
  "wheel-0.48.0-py3-none-any.whl|$pypi/2e/29/69cfbb602cd91690c55d38ba9fe53e6a7e76a6fa647bf38f19c138d25449/wheel-0.48.0-py3-none-any.whl|3217dcc807155e45db462d7ef2431f5ddda0d7273b700d05a67b271ceb1287ab"
  "pip-26.2.1-py3-none-any.whl|$pypi/f3/6e/1736e5b4ae2b778ef2f81c47d797de9f891d4d8acb047a24ca37a60294dd/pip-26.2.1-py3-none-any.whl|71138adf1f4ca900cdb7d289c21b7494329f2332b6d85f0e1c42108c0384ed3e"
  # ANGLE (EGL/GLES) and libepoxy built for macOS 15 by startergo's taps.
  "angle-1.0.16.arm64_sequoia.bottle.tar.gz|https://github.com/startergo/homebrew-angle/releases/download/v1.0.16/angle-1.0.16.arm64_sequoia.bottle.tar.gz|29fe2175b157a65f12879f9a12b5c8f94d0a76fafdf41ff009a2fdb4e9df525c"
  "libepoxy-1.0.5.arm64_sequoia.bottle.tar.gz|https://github.com/startergo/homebrew-libepoxy/releases/download/v1.0.5/libepoxy-1.0.5.arm64_sequoia.bottle.tar.gz|109384a1d37edf207a9b9f3d8950710c00767635b3c7ff295e3af83611876ef2"
)
angle_version=1.0.16
epoxy_version=1.0.5
virgl_prefix_version=1.0.42      # the folder name the tap's bottle uses; kept for its .pc files

# glib, pixman, libslirp, libusb, SDL, gettext, pcre2, zstd, lz4, xz, MoltenVK and the Vulkan
# loader: Homebrew's arm64_sequoia bottles, pinned in pinned-runtime-bottles.sh.
# shellcheck source=pinned-runtime-bottles.sh
source "$here/pinned-runtime-bottles.sh"
# shellcheck source=build-python.sh
source "$here/build-python.sh"
bottles() { pinned_core_bottle_manifest; }

sha_of() { shasum -a 256 "$1" | awk '{ print $1 }'; }
fetch() { # NAME URL SHA256: into the cache, once
  local file=$cache/$1
  [[ -f $file && $(sha_of "$file") == "$3" ]] && return
  log "Downloading $1"
  curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
    --retry 3 --connect-timeout 20 --output "$file.part" "$2"
  [[ $(sha_of "$file.part") == "$3" ]] || die "$1: checksum mismatch"
  mv "$file.part" "$file"
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
[[ $(uname -m) == arm64 ]] || die "Apple Silicon only"
(( $(sw_vers -productVersion | cut -d. -f1) >= 15 )) || die "macOS 15 or newer is needed"
for tool in curl patch pkg-config shasum tar codesign install_name_tool otool bunzip2; do
  command -v "$tool" >/dev/null || die "missing tool: $tool"
done
[[ $here != *[[:space:]]* ]] || die "QEMU's configure cannot build in a path with spaces: $here"

mkdir -p "$cache"
rm -rf "$work"
mkdir -p "$work"/{src,deps,tools,tests,python-bin}
src=$work/src deps=$work/deps tools=$work/tools tests=$work/tests
[[ ${PINGOUIN_KEEP_WORK:-} == 1 ]] || trap 'rm -rf "$work"' EXIT

# The build's python3 goes first in PATH, alone in its folder (cc stays Apple's).
python=$(pingouin_build_python "$work/python-bin") || exit 1
PATH="$work/python-bin:$PATH"
log "python3: $python ($(python3 --version))"

# ---------------------------------------------------------------------------
# Download and unpack
# ---------------------------------------------------------------------------
for entry in "${sources[@]}"; do
  IFS='|' read -r name url sha <<<"$entry"
  fetch "$name" "$url" "$sha"
done
while IFS=$'\t' read -r formula version archive _root sha; do
  if ! [[ -f $cache/$archive && $(sha_of "$cache/$archive") == "$sha" ]]; then
    pinned_bottle_download "$formula" "$formula $version" "$sha" "$cache/$archive"
  fi
done < <(bottles)

for name in qemu-$qemu_commit libslirp-v4.9.4 virglrenderer-1.3.0 homebrew-virglrenderer-$virgl_tap_commit \
            SPIRV-Tools-$spirv_tools_version SPIRV-Headers-$spirv_headers_commit; do
  tar -xzf "$cache/$name.tar.gz" -C "$src"
done
cmake_root=cmake-$cmake_version-macos-universal
tar -xzf "$cache/$cmake_root.tar.gz" -C "$tools" "$cmake_root/CMake.app/Contents/bin/cmake" \
  "$cmake_root/CMake.app/Contents/share"
tar -xzf "$cache/meson-1.9.0.tar.gz" -C "$tools"
tar -xzf "$cache/pyyaml-6.0.3.tar.gz" -C "$tools"
ditto -x -k "$cache/ninja-1.13.0-py3-none-macosx_10_9_universal2.whl" "$tools"
tar -xzf "$cache/angle-1.0.16.arm64_sequoia.bottle.tar.gz" -C "$deps"
tar -xzf "$cache/libepoxy-1.0.5.arm64_sequoia.bottle.tar.gz" -C "$deps"
while IFS=$'\t' read -r _f _v archive _r _s; do tar -xzf "$cache/$archive" -C "$deps"; done < <(bottles)

qemu=$src/qemu-$qemu_commit
slirp_src=$src/libslirp-v4.9.4
virgl_src=$src/virglrenderer-1.3.0
tap=$src/homebrew-virglrenderer-$virgl_tap_commit
spirv_tools_src=$src/SPIRV-Tools-$spirv_tools_version
spirv_headers_src=$src/SPIRV-Headers-$spirv_headers_commit
cmake=$tools/$cmake_root/CMake.app/Contents/bin/cmake
meson=$tools/meson-1.9.0/meson.py
ninja=$tools/ninja-1.13.0.data/scripts/ninja
chmod 0755 "$ninja"
export PYTHONPATH=$tools/pyyaml-6.0.3/lib   # virglrenderer generates tables with PyYAML
export PYTHONNOUSERSITE=1

install -m 0644 "$cache"/{setuptools,packaging,wheel,pip}-*.whl "$qemu/python/wheels/"
mkdir -p "$qemu/subprojects/keycodemapdb" "$qemu/subprojects/dtc"
tar -xzf "$cache/keycodemapdb-$keycodemap_commit.tar.gz" -C "$qemu/subprojects/keycodemapdb" --strip-components=1
tar -xzf "$cache/dtc-$dtc_commit.tar.gz" -C "$qemu/subprojects/dtc" --strip-components=1

# ---------------------------------------------------------------------------
# Patches
# ---------------------------------------------------------------------------
# Each patch's own tests (runtime/Tests, from upstream) run right after it, as upstream
# does: most test a patch's logic header, or check how ui/cocoa.m is wired at that point.
# Not run: test-fullscreen-space.sh and test-ime.sh (they look for code from patches we
# drop), and the boot splash's tests (they check Omarchy's logo and the animation,
# which pingouin-splash-penguin.patch replaces).
T=$here/Tests
after_patch() {
  local cocoa=$qemu/ui/cocoa.m
  case $1 in
    qemu-hvf-free-page-reclaim-reusable.patch) "$T/memory/test-reclaim.sh" "$qemu" "$tests/memory" ;;
    qemu-hvf-own-view-sweep.patch)        "$T/memory/test-dcount.sh" "$qemu" "$tests/dcount" ;;
    qemu-sdl-audio-out-size.patch)        "$T/audio/test-out-size.sh" "$qemu/audio/sdlaudio.c" ;;
    qemu-darwin-disk-flush-barrier.patch) "$T/disk/test-flush.sh" "$qemu" "$tests/disk" ;;
    qemu-darwin-surface-memory.patch)     "$T/display/test-surface-memory.sh" "$qemu" "$tests/surface" ;;
    pingouin-cocoa-pointer-guard.patch)     "$T/display/test-pointer-guard.sh" ;;
    qemu-cocoa-gl-view-flush.patch)       test_gl_view_flush ;;
    pingouin-cocoa-system-shortcuts.patch)  "$T/keys/test-shortcuts.sh" ;;
    pingouin-cocoa-pointer-start.patch)     "$T/display/test-pointer-start.sh" ;;
    qemu-cocoa-idle-refresh.patch)        test_idle_refresh ;;
    pingouin-cocoa-graphics-memory.patch)   "$T/display/test-gpu-memory-menu.sh" ;;
    pingouin-cocoa-restart-desktop.patch)   "$T/display/test-restart-desktop.sh" ;;
    pingouin-cocoa-hw-cursor.patch)         "$T/display/test-hw-cursor.sh" ;;
    pingouin-cocoa-clean-size.patch)        "$T/display/test-clean-size.sh" ;;
    pingouin-cocoa-notch-park.patch)        "$T/display/test-notch-park.sh" ;;
    pingouin-cocoa-shutdown-events.patch)   "$T/display/test-shutdown-events.sh" "$cocoa" ;;
    pingouin-cocoa-fullscreen-start.patch)  "$T/display/test-fullscreen-start.sh" "$cocoa" ;;
    pingouin-cocoa-quit-clean.patch)        "$T/display/test-quit-clean.sh" "$cocoa" ;;
    pingouin-cocoa-tap-permission.patch)    "$T/keys/test-tap-permission.sh" "$cocoa" ;;
    pingouin-cocoa-fullpanel.patch)         "$T/display/test-fullpanel.sh" "$cocoa" ;;
    pingouin-cocoa-fullpanel-scale.patch)   "$T/display/test-fullpanel-scale.sh" "$cocoa" ;;
    pingouin-cocoa-fullpanel-window-gone.patch) "$T/display/test-fullpanel-window-gone.sh" "$cocoa" ;;
    pingouin-cocoa-keys-last.patch)         "$T/keys/test-keys-last.sh" "$cocoa" ;;
    pingouin-cocoa-tap-no-ask.patch)        "$T/keys/test-tap-no-ask.sh" "$cocoa" ;;
    pingouin-cocoa-close.patch)             "$T/display/test-close.sh" "$cocoa" ;;
    pingouin-cocoa-present-unseen.patch)    "$T/display/test-present-unseen.sh" "$cocoa" ;;
    pingouin-cocoa-idle-refresh-unseen.patch) "$T/display/test-idle-refresh-unseen.sh" "$cocoa" ;;
    pingouin-cocoa-modifier-state.patch)    "$T/keys/test-modifier-state.sh" "$cocoa" ;;
    pingouin-cocoa-display-settle.patch)    "$T/display/test-display-settle.sh" "$cocoa" ;;
    pingouin-cocoa-fullpanel-retake.patch)  "$T/display/test-fullpanel-retake.sh" "$cocoa" ;;
    pingouin-cocoa-fullpanel-late-arm.patch) "$T/display/test-fullpanel-late-arm.sh" "$cocoa" ;;
    # virglrenderer: the GPU memory guard's rules, and Venus' SPIR-V scan fuzzed on the CPU
    # under the sanitizers, each on its own, from the patch files.
    virgl-gpu-guard-critical-grace.patch)   "$T/virgl/test-gpu-guard-policy.sh" ;;
    virgl-venus-spirv-scan.patch)           "$T/virgl/fuzz-venus-spirv.sh" ;;
  esac >"$tests/last.log" 2>&1 || { cat "$tests/last.log" >&2; die "the test after $1 failed"; }
}

# with_gl_view_ctx() must flush; its copy from ui/cocoa.m runs on Apple's software GL.
test_gl_view_flush() {
  awk '/^static void with_gl_view_ctx\(CodeBlock block\)$/,/^}$/' "$qemu/ui/cocoa.m" > "$tests/with-gl-view-ctx.inc"
  grep -q 'glFlush();' "$tests/with-gl-view-ctx.inc"
  cc -fblocks -Wall -Werror -Wno-deprecated-declarations -I"$tests" \
    "$T/display/test-gl-view-flush.c" -framework OpenGL -o "$tests/test-gl-view-flush"
  "$tests/test-gl-view-flush"
}

# The idle refresh tick's rate logic, cut out of ui/cocoa.m and tested on its own.
test_idle_refresh() {
  awk '/^#define COCOA_REFRESH_SLOW_MS/{f=1} f{print}
       f&&/^static void cocoa_refresh_tick\(bool pending\)$/{t=1} t&&/^}$/{exit}' \
    "$qemu/ui/cocoa.m" > "$tests/idle-refresh.inc"
  cc -Wall -Werror -I"$tests" "$T/display/test-idle-refresh.c" -o "$tests/test-idle-refresh"
  "$tests/test-idle-refresh"
  PINGOUIN_IDLE_REFRESH=0 "$tests/test-idle-refresh" off
}

apply_patches() { # SECTION SOURCE_DIR
  local name file count=0
  while read -r name; do
    case $name in
      TAP:*) file=$tap/patches/${name#TAP:} ;;
      *) file=$here/patches/$name ;;
    esac
    patch -d "$2" -p1 -f -s -i "$file" || die "$name does not apply"
    after_patch "$name"
    count=$((count + 1))
  done < <(awk -v s="[$1]" '/^\[/ {on = ($1 == s); next} on && !/^[[:space:]]*(#|$)/ {print $1}' "$here/patches.list")
  log "Applied $count $1 patches"
}
apply_patches libslirp "$slirp_src"
apply_patches qemu "$qemu"
apply_patches virglrenderer "$virgl_src"
apply_patches spirv-tools "$spirv_tools_src"

# Venus: a guest's Vulkan calls reach the Mac's driver through vkr's proc tables, whose entries
# are never NULL (VKR_MISSING tells a stub, virgl-venus-missing-entries.patch); checked on the
# fully patched tree (vkr_allocator.c keeps tables of its own, venus-protocol's loader fallbacks
# run before the stubs).
if grep -rnE --include='*.[ch]' --exclude=vkr_allocator.c --exclude-dir=venus-protocol \
     '!\(?[[:space:]]*(vk|[A-Za-z_>.-]*proc_table)[[:space:]]*(->|\.)[[:space:]]*[A-Z]|(vk|proc_table)(->|\.)[A-Z][A-Za-z0-9]*[[:space:]]*[!=]=[[:space:]]*NULL' \
     "$virgl_src/src/venus"; then
  die "src/venus: a proc table entry tested against NULL; use VKR_MISSING (missing-entries patch)"
fi

# ---------------------------------------------------------------------------
# Private dependency tree: pkg-config sees only the pinned bottles, never Homebrew
# ---------------------------------------------------------------------------
glib=$deps/$PINNED_GLIB_ROOT pixman=$deps/$PINNED_PIXMAN_ROOT slirp=$deps/$PINNED_LIBSLIRP_ROOT
libusb=$deps/$PINNED_LIBUSB_ROOT sdl2=$deps/$PINNED_SDL2_ROOT sdl3=$deps/$PINNED_SDL3_ROOT
gettext=$deps/$PINNED_GETTEXT_ROOT pcre2=$deps/$PINNED_PCRE2_ROOT
angle=$deps/angle/$angle_version epoxy=$deps/libepoxy/$epoxy_version
virgl=$deps/virglrenderer/$virgl_prefix_version   # our build installs here

# The bottles' .pc files carry Homebrew placeholders: point them at this tree.
sed -i '' "s|@@HOMEBREW_CELLAR@@/libepoxy/$epoxy_version|$epoxy|g" "$epoxy/lib/pkgconfig/epoxy.pc"
sed -i '' "s|^prefix=/opt/homebrew$|prefix=$angle|" "$angle"/lib/pkgconfig/*.pc
sed -i '' -e "s|@@HOMEBREW_CELLAR@@/$PINNED_GLIB_ROOT|$glib|g" \
          -e "s|@@HOMEBREW_PREFIX@@/opt/gettext|$gettext|g" "$glib"/lib/pkgconfig/*.pc
for pair in "$pixman:$PINNED_PIXMAN_ROOT" "$slirp:$PINNED_LIBSLIRP_ROOT" \
            "$pcre2:$PINNED_PCRE2_ROOT" "$libusb:$PINNED_LIBUSB_ROOT"; do
  sed -i '' "s|@@HOMEBREW_CELLAR@@/${pair#*:}|${pair%%:*}|g" "${pair%%:*}"/lib/pkgconfig/*.pc
done
sed -i '' -e "s|^prefix=@@HOMEBREW_PREFIX@@$|prefix=$sdl2|" \
          -e "s|^libdir=@@HOMEBREW_PREFIX@@/lib$|libdir=$sdl2/lib|" \
          -e "s|^includedir=@@HOMEBREW_PREFIX@@/include$|includedir=$sdl2/include|" \
          "$sdl2/lib/pkgconfig/sdl2-compat.pc"
# sdl2-compat loads SDL3 by this name from beside itself.
install -m 0755 "$sdl3/lib/libSDL3.0.dylib" "$sdl2/lib/libSDL3.dylib"

export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR=$virgl/lib/pkgconfig:$epoxy/lib/pkgconfig:$angle/lib/pkgconfig:$glib/lib/pkgconfig:$pixman/lib/pkgconfig:$slirp/lib/pkgconfig:$sdl2/lib/pkgconfig:$pcre2/lib/pkgconfig:$libusb/lib/pkgconfig
libs=$virgl/lib:$epoxy/lib:$angle/lib:$glib/lib:$pixman/lib:$slirp/lib:$sdl2/lib:$gettext/lib:$pcre2/lib:$libusb/lib
export MACOSX_DEPLOYMENT_TARGET=$target
min="-mmacosx-version-min=$target -Werror=unguarded-availability-new"
PATH="$(dirname "$ninja"):$PATH"

# ---------------------------------------------------------------------------
# libslirp (over the bottle's copy, so QEMU links ours)
# ---------------------------------------------------------------------------
log "Building libslirp"
CFLAGS="$min" LDFLAGS="-mmacosx-version-min=$target -Wl,-headerpad_max_install_names" \
  python3 "$meson" setup "$slirp_src/build" "$slirp_src" \
    --prefix="$slirp" --libdir=lib --buildtype=release --wrap-mode=nodownload >"$work/slirp-setup.log"
"$ninja" ${jobs[@]+"${jobs[@]}"} -C "$slirp_src/build"
DYLD_LIBRARY_PATH="$slirp_src/build:$libs" python3 "$meson" test -C "$slirp_src/build" --no-rebuild --print-errorlogs
python3 "$meson" install -C "$slirp_src/build" --no-rebuild >/dev/null

# ---------------------------------------------------------------------------
# SPIRV-Tools: Venus validates the guest's SPIR-V with it before MoltenVK compiles it.
# A static library with hidden symbols, so libvirglrenderer exports none of them (MoltenVK
# carries a private copy of its own); once more with AddressSanitizer for the CPU test.
# ---------------------------------------------------------------------------
build_spirv_tools() { # PREFIX CFLAGS
  local build=$src/build-${1##*/}
  "$cmake" -S "$spirv_tools_src" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" \
    -DCMAKE_C_FLAGS="$2" -DCMAKE_CXX_FLAGS="$2" \
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
    -DSPIRV-Headers_SOURCE_DIR="$spirv_headers_src" \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF \
    >"$build.cmake.log" 2>&1 || { tail -20 "$build.cmake.log" >&2; die "SPIRV-Tools: cmake failed"; }
  "$ninja" ${jobs[@]+"${jobs[@]}"} -C "$build" SPIRV-Tools-static >"$build.log" ||
    { tail -20 "$build.log" >&2; die "SPIRV-Tools: build failed"; }
  mkdir -p "$1/lib/pkgconfig" "$1/include"
  install -m 0644 "$build/source/libSPIRV-Tools.a" "$1/lib/"
  cp -R "$spirv_tools_src/include/spirv-tools" "$1/include/"
}
log "Building SPIRV-Tools $spirv_tools_version (static, hidden symbols)"
spirv=$deps/spirv-tools spirv_asan=$deps/spirv-tools-asan
build_spirv_tools "$spirv" ""
build_spirv_tools "$spirv_asan" "-fsanitize=address -fno-omit-frame-pointer -g"
cat > "$spirv/lib/pkgconfig/pingouin-spirv-tools.pc" <<EOF
Name: pingouin-spirv-tools
Description: SPIRV-Tools $spirv_tools_version, static with hidden symbols (Pingouin's runtime build)
Version: $spirv_tools_version
Libs: $spirv/lib/libSPIRV-Tools.a -lc++
Cflags: -I$spirv/include
EOF
export PKG_CONFIG_LIBDIR=$PKG_CONFIG_LIBDIR:$spirv/lib/pkgconfig
# The validator and vkr's checks on the CPU alone: every real module passes, every module that
# ended a Mac driver's process is refused.
"$T/virgl/test-venus-spirv-validate.sh" "$spirv" "$spirv_asan" "$virgl_src" >"$tests/spirv-validate.log" 2>&1 ||
  { tail -30 "$tests/spirv-validate.log" >&2; die "the Venus SPIR-V validation test failed"; }

# ---------------------------------------------------------------------------
# virglrenderer: OpenGL, and Vulkan through Venus (no video). The render server runs in
# QEMU's process (a thread per context); the Vulkan loader and MoltenVK are loaded at run
# time from the runtime's lib/ (virgl-darwin-vulkan-beside.patch).
# ---------------------------------------------------------------------------
log "Building virglrenderer"
CFLAGS="-I$angle/include $min" OBJCFLAGS="$min" \
LDFLAGS="-mmacosx-version-min=$target -Wl,-headerpad_max_install_names" \
DYLD_LIBRARY_PATH="$libs" \
  python3 "$meson" setup "$virgl_src/build" "$virgl_src" \
    --prefix="$virgl" --libdir=lib --buildtype=debugoptimized -Db_ndebug=false \
    --wrap-mode=nodownload -Ddrm-renderers=[] -Dvenus=true -Drender-server-worker=thread \
    -Dvideo=false -Dtests=false -Dtracing=none >"$work/virgl-setup.log" ||
  { tail -30 "$work/virgl-setup.log" >&2; die "virglrenderer: meson setup failed"; }
"$ninja" ${jobs[@]+"${jobs[@]}"} -C "$virgl_src/build"
# The renderer's own regression tests, without a VM: the tap's shader tests, and ours
# (Tests/virgl/run-regressions.py: upstream's list minus video).
link=("-L$epoxy/lib" -lepoxy -framework Metal -framework CoreFoundation -lobjc
      -framework CoreVideo -framework OpenGL -framework IOSurface
      "$spirv/lib/libSPIRV-Tools.a" -lc++
      "-Wl,-rpath,$epoxy/lib" "-Wl,-rpath,$angle/lib")
DYLD_LIBRARY_PATH="$libs" python3 "$tap/tests/run-driver-regressions.py" \
  "$virgl_src/build" "$tests/virgl" -- "${link[@]}"
DYLD_LIBRARY_PATH="$libs" python3 "$T/virgl/run-regressions.py" \
  "$virgl_src/build" "$tests/virgl" -- "${link[@]}"
python3 "$meson" install -C "$virgl_src/build" --no-rebuild >/dev/null
# SPIRV-Tools stays private to the renderer: no spv symbol exported, so neither its copy nor
# MoltenVK's can bind to the other's.
! nm -gU "$virgl/lib/libvirglrenderer.1.dylib" | grep -E ' _(spv|_ZN8spvtools)' ||
  die "libvirglrenderer exports SPIRV-Tools symbols (build it with hidden visibility)"

# ---------------------------------------------------------------------------
# QEMU
# ---------------------------------------------------------------------------
log "Configuring QEMU"
mkdir "$qemu/build"
(cd "$qemu/build" && DYLD_LIBRARY_PATH="$libs" DYLD_FALLBACK_LIBRARY_PATH="$libs" ../configure \
  --prefix="$work/install" --target-list=aarch64-softmmu --without-default-features \
  --enable-system --enable-hvf --disable-tcg --enable-cocoa --enable-opengl \
  --enable-virglrenderer --enable-pixman --enable-slirp --enable-fdt=internal \
  --enable-sdl --audio-drv-list=sdl --enable-virtfs --enable-libusb \
  --disable-debug-info --disable-werror --disable-download --disable-containers \
  --container-command=false --extra-cflags="$min" \
  --extra-ldflags="-mmacosx-version-min=$target" --ninja="$ninja") >"$work/qemu-configure.log" ||
  { tail -40 "$work/qemu-configure.log" >&2; die "QEMU configure failed"; }

# Checks that belong to kept patches: no 15.4-only strchrnul (strchrnul-compat), and
# display surfaces in plain memory need a build without the D-Bus display (surface-memory).
config=$qemu/build/config-host.h
! grep -Eq '^[[:space:]]*#define[[:space:]]+HAVE_STRCHRNUL([[:space:]]+1)?[[:space:]]*$' "$config" || die "configure enabled the macOS 15.4-only strchrnul"
grep -Eq '^#undef[[:space:]]+CONFIG_DBUS_DISPLAY' "$config" || die "the D-Bus display must be off (qemu-darwin-surface-memory.patch)"

log "Building qemu-system-aarch64"
DYLD_LIBRARY_PATH="$libs" DYLD_FALLBACK_LIBRARY_PATH="$libs" \
  "$ninja" ${jobs[@]+"${jobs[@]}"} -C "$qemu/build" qemu-system-aarch64

# ---------------------------------------------------------------------------
# Stage the runtime: copy, point every library at lib/, sign, verify
# ---------------------------------------------------------------------------
log "Staging the runtime"
stage=$work/stage
mkdir -p "$stage/bin" "$stage/lib" "$stage/share/qemu"
install -m 0755 "$qemu/build/qemu-system-aarch64" "$stage/bin/"
install -m 0755 "$virgl/lib/libvirglrenderer.1.dylib" "$epoxy/lib/libepoxy.0.dylib" \
  "$angle/lib/libEGL.dylib" "$angle/lib/libGLESv2.dylib" "$stage/lib/"
# The bottles' libraries, under the names the binaries ask for (minus the zstd command);
# libslirp is our patched build.
while IFS=$'\t' read -r _archive member dest; do
  case $dest in bin/*) continue ;; esac
  install -m 0755 "$deps/$member" "$stage/$dest"
done < <(pinned_runtime_member_manifest)
# Venus: the Vulkan loader finds MoltenVK through this file (beside lib/, where
# libvirglrenderer points the loader).
mkdir -p "$stage/share/vulkan/icd.d" "$stage/share/licenses"
cat > "$stage/share/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
JSON
chmod 0644 "$stage/share/vulkan/icd.d/MoltenVK_icd.json"
install -m 0644 "$here/LICENSE.vulkan.txt" "$stage/share/licenses/vulkan.txt"
bunzip2 -c "$qemu/pc-bios/edk2-aarch64-code.fd.bz2" > "$stage/share/qemu/edk2-aarch64-code.fd"
install -m 0644 "$qemu/pc-bios/edk2-licenses.txt" "$stage/share/qemu/"
xattr -cr "$stage"

"$here/bundle-macho-dependencies.sh" "$stage"
for lib in "$stage"/lib/*.dylib; do codesign --force --sign - "$lib"; done
codesign --force --sign - --entitlements "$here/qemu-hvf.entitlements" "$stage/bin/qemu-system-aarch64"
"$here/bundle-macho-dependencies.sh" --verify-only "$stage"
"$here/verify-macos-compatibility.sh" "$stage"

q=$stage/bin/qemu-system-aarch64
codesign -d --entitlements - "$q" 2>&1 | grep -q com.apple.security.hypervisor || die "no hypervisor entitlement"
! otool -L "$q" "$stage"/lib/*.dylib | grep -q /opt/homebrew || die "a Homebrew path is left"
env -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$q" --version | grep -q '^QEMU emulator version 11.1.1' ||
  die "the staged QEMU does not start"
"$q" -accel help | grep -q '^hvf' || die "no HVF accelerator"
"$q" -display help | grep -q '^cocoa' || die "no Cocoa display"
devices=$("$q" -device help)
for device in virtio-gpu-gl-pci virtio-pinch-pci virtio-9p-pci virtio-net-pci intel-hda hda-micro virtserialport; do
  grep -q "name \"$device\"" <<<"$devices" || die "missing device $device"
done
"$q" -device virtio-gpu-gl-pci,help | grep -q 'venus=' || die "virtio-gpu-gl-pci has no venus property"
# M1/M2 (36-bit VM address space): Venus' host memory BAR needs a small high PCIe window
# (qemu-virt-small-high-window.patch).
grep -qaF 'highmem-mmio-size cannot be smaller than 1 GiB' "$q" ||
  die "the small high PCIe window patch is missing"
"$q" -machine none -accel qtest -netdev help | grep -q '^user' || die "no user (SLIRP) networking"
"$q" -machine none -accel qtest -audiodev help | grep -q '^sdl' || die "no SDL audio"

# A guest's Vulkan calls never reach MoltenVK with what Metal ends QEMU on, and what ended
# QEMU's process in vkr now ends only the app's context: each case through vkr's own dispatch
# on the staged runtime's MoltenVK, one process per case; objects and binds only.
log "Venus tests on the staged runtime's MoltenVK"
python3 "$T/virgl/run-venus-limits.py" \
  "$virgl_src/build" "$tests/venus-limits" "$stage" -- "${link[@]}" >"$tests/venus-limits.log" 2>&1 ||
  { tail -40 "$tests/venus-limits.log" >&2; die "the Venus limits tests failed"; }
grep '^venus limits' "$tests/venus-limits.log" | tail -5

rm -rf "$out.old"
[[ ! -e $out ]] || mv "$out" "$out.old"
mv "$stage" "$out"
rm -rf "$out.old"
log "Ready: $out"

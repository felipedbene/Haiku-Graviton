#!/bin/sh
# DeBeOS Ladybird Wave-2: build Skia (CPU raster only) for arm64-Haiku via GN.
#
# Produces libskia.so + skcms + the skia modules and an installed skia.pc
# (Version 148) under $PREFIX, satisfying Ladybird's
#   pkg_check_modules(skia skia=148 REQUIRED IMPORTED_TARGET skia)
#
# Milestone: chrome/m148, commit e7c90ecca9444fe09598f1630ab7cee2c0ee027a
#   (read from Ladybird's Meta/CMake/flatpak/org.ladybird.Ladybird.json).
#
# Prereqs (pkgman): llvm21_clang llvm21_lld cmake ninja gn plus the devel libs
#   freetype_devel harfbuzz_devel libpng16_devel libjpeg_turbo_devel
#   libwebp_devel zlib_devel icu74_devel fontconfig_devel expat_devel
# A python3 on PATH is required by Skia's GN (symlink python3.10 -> python3).
#
# Apply skia-haiku-arm64.patch to the Skia checkout first (3 platform fixes:
#   - third_party/{freetype2,harfbuzz}/BUILD.gn: system() hardcodes
#     /usr/include/<lib>; rewrite to Haiku's /boot/system/develop/headers/<lib>.
#   - BUILD.gn: drop `libs += ["dl"]` (Haiku provides dlopen in libroot).
#   - src/ports/SkMemory_malloc.cpp: Haiku libroot has no malloc_usable_size.)
set -e
export PATH=/boot/home/bin:/boot/system/bin:$PATH
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig
export CC=clang CXX=clang++
PREFIX=/boot/home/lbdeps
SKIA=/boot/home/skia

cd "$SKIA"
# NOTE: target_os="linux" — Haiku has no Skia port; Haiku is classified
# SK_BUILD_FOR_UNIX via __unix__, which is the correct minimal surface.
gn gen out/cpu --args='
  is_official_build=true
  is_component_build=true
  is_debug=false
  target_cpu="arm64"
  target_os="linux"
  skia_enable_ganesh=false
  skia_enable_graphite=false
  skia_use_gl=false
  skia_use_vulkan=false
  skia_use_dng_sdk=false
  skia_use_wuffs=false
  skia_use_zlib=true
  skia_use_system_zlib=true
  skia_use_harfbuzz=true
  skia_use_fontconfig=true
  skia_use_icu=true
  skia_use_system_icu=true
  cc="clang"
  cxx="clang++"
  extra_cflags=["-Wno-psabi"]
  extra_cflags_cc=["-DSKCMS_DLL","-USK_HIDE_PATH_EDIT_METHODS"]
'
ninja -j"$(nproc)" -C out/cpu :skia :modules

# Install libs + headers + skia.pc (adapted from Ladybird's flatpak skia-install.sh)
mkdir -p "$PREFIX/lib" "$PREFIX/include/skia/modules" "$PREFIX/lib/pkgconfig"
for p in out/cpu/*.a out/cpu/*.so; do cp "$p" "$PREFIX/lib/$(basename "$p")"; done
( cd include  && find . -name '*.h' | while read h; do mkdir -p "$PREFIX/include/skia/$(dirname "$h")";          cp "$h" "$PREFIX/include/skia/$h"; done )
( cd modules  && find . -name '*.h' | while read h; do mkdir -p "$PREFIX/include/skia/modules/$(dirname "$h")";  cp "$h" "$PREFIX/include/skia/modules/$h"; done )
cat > "$PREFIX/lib/pkgconfig/skia.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include/skia
Name: skia
Description: 2D graphic library for drawing text, geometries and images.
URL: https://skia.org/
Version: 148
Libs: -L\${libdir} -lskia -lskcms
Cflags: -I\${includedir}
EOF
# Some skia headers #include "include/..."; strip the prefix after flattening.
for f in $(grep -rl '#include "include/' "$PREFIX/include/skia" 2>/dev/null); do
  sed -i -e 's|#include "include/|#include "|g' "$f"
done
pkg-config --exists "skia = 148" && echo "skia=148 OK"

#!/bin/sh
# Derive the ladybird package's `requires` mechanically from what its ELF files
# actually load, instead of typing the list by hand.
#
#   derive-requires.sh <pkgroot> <outdir>
#
# Writes, in <outdir>:
#   elves.txt        every ELF file in the package
#   needed.txt       union of their DT_NEEDED entries
#   external.txt     NEEDED entries the package does not ship itself
#   resolution.txt   "<soname> -> <providing package>" for each external entry
#   requires.txt     the requires{} body, one expression per line
#
# Resolution: each external SONAME is located the way runtime_loader would find
# it (system lib dirs) and attributed to the package packagefs says it came
# from, via the automatic SYS:PACKAGE attribute every packagefs node carries
# (src/add-ons/kernel/file_systems/packagefs/util/StringConstantsPrivate.h).
# Haiku package names cannot contain '-', so the name is everything before the
# first '-' of that versioned name. This must therefore run on a box where the
# dependency packages are INSTALLED as packages (the clean-box install, or a
# builder with the pool packages activated) -- a library found outside packagefs
# (a build tree, /boot/home/lbdeps) has no SYS:PACKAGE and is reported
# UNRESOLVED, which fails the script rather than being silently dropped.
#
# A few dependencies are deliberately required through a capability with a
# version floor rather than a package name (see package/README.md: icu78 next to
# icu74, libpng16's libpng16_apng, the in-place bumps). Those are the
# SONAME-keyed policy lines below; everything else maps to the providing
# package's name.
#
# Libraries opened with dlopen() are invisible to DT_NEEDED. RUNTIME_EXTRA names
# them explicitly, each with its reason.
set -eu

R=$1
OUT=$2
mkdir -p "$OUT"

LIBDIRS="/boot/system/lib /boot/system/develop/lib"

# soname-prefix|requires expression
POLICY='libicuuc.so|lib:libicuuc >= 78
libicui18n.so|lib:libicui18n >= 78
libicudata.so|lib:libicudata >= 78
libtommath.so|lib:libtommath >= 1.3
libsimdjson.so|lib:libsimdjson >= 34
libavcodec.so|lib:libavcodec >= 61
libavformat.so|lib:libavformat >= 61
libavutil.so|lib:libavutil >= 59
libswresample.so|lib:libswresample >= 5
libpng16.so|libpng16_apng >= 1.6.53'

# package|reason -- dependencies no DT_NEEDED entry reveals.
RUNTIME_EXTRA='fontconfig|WebContent runs with force_fontconfig=Yes and reads its configuration files at run time
dejavu|font fallback for Arabic, Hebrew, Armenian, Georgian and more: the base image ships only Latin/Greek/Cyrillic Noto faces, so those scripts rendered as tofu
noto_sans_cjk_sc|font fallback for Chinese, Japanese and Korean (pan-CJK Noto Sans CJK); without it CJK text rendered as tofu'

policy_for() {
    echo "$POLICY" | while IFS='|' read -r prefix expression; do
        case "$1" in "$prefix"*) echo "$expression" ;; esac
    done
}

: > "$OUT/elves.txt"
# `if`, not `readelf && echo`: under set -e the loop's status is its last
# command's, so a non-ELF file found last (a licence text) would abort the script.
find "$R" -type f | while read -r f; do
    if readelf -h "$f" >/dev/null 2>&1; then
        echo "$f" >> "$OUT/elves.txt"
    fi
done
[ -s "$OUT/elves.txt" ] || { echo "no ELF files under $R" >&2; exit 1; }

while read -r f; do
    readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
done < "$OUT/elves.txt" | sort -u > "$OUT/needed.txt"

find "$R" \( -type f -o -type l \) -name '*.so*' -exec basename {} \; | sort -u > "$OUT/shipped.txt"
comm -23 "$OUT/needed.txt" "$OUT/shipped.txt" > "$OUT/external.txt"

: > "$OUT/resolution.txt"
: > "$OUT/requires.unsorted"
status=0
while read -r so; do
    path=""
    for d in $LIBDIRS; do
        if [ -e "$d/$so" ]; then path="$d/$so"; break; fi
    done
    provider=""
    if [ -n "$path" ]; then
        provider=$(catattr -d SYS:PACKAGE "$path" 2>/dev/null | sed 's/-.*//') || provider=""
    fi
    if [ -z "$provider" ]; then
        echo "$so -> UNRESOLVED (${path:-not found in $LIBDIRS})" >> "$OUT/resolution.txt"
        status=1
        continue
    fi
    echo "$so -> $provider" >> "$OUT/resolution.txt"
    expression=$(policy_for "$so")
    echo "${expression:-$provider}" >> "$OUT/requires.unsorted"
done < "$OUT/external.txt"

echo "$RUNTIME_EXTRA" | while IFS='|' read -r pkg reason; do
    if [ -n "$pkg" ]; then
        echo "$pkg" >> "$OUT/requires.unsorted"
    fi
done

sort -u "$OUT/requires.unsorted" > "$OUT/requires.txt"
rm -f "$OUT/requires.unsorted"

cat "$OUT/resolution.txt"
if [ "$status" -ne 0 ]; then
    echo "derive-requires: unresolved dependencies, see $OUT/resolution.txt" >&2
    exit 1
fi
echo "derive-requires: $(wc -l < "$OUT/elves.txt") ELF files, $(wc -l < "$OUT/needed.txt") NEEDED," \
     "$(wc -l < "$OUT/external.txt") external, $(wc -l < "$OUT/requires.txt") requires"

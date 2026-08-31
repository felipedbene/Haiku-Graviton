# Creating native packages on DeBeOS arm64 (worked example: WebPositive)

DeBeOS is self-hosting: a lean modular AMI can fetch source, compile it natively,
and produce an installable `.hpkg` — with **no cross-toolchain, no metal, no
workstation help**, using only what `pkgman` installs from the DeBeOS repo. This
document is the proven workflow, using **WebPositive** (the HaikuWebKit browser,
which the arm64 image does not bake) as the worked example. Everything below was
run on a stock instance of the canonical lean AMI (Graviton `c7g.large`,
2 vCPU / 4 GiB).

> Why this matters (roadmap). It closes the loop opened by native package
> *management*: once a box can `pkgman install`, the next self-sufficiency step is
> to *create* packages natively. WebPositive is the trial-by-fire — it links
> HaikuWebKit and pulls in the Tracker/Interface private kits.

## 0. Prerequisites — install the toolchain from the DeBeOS repo

```sh
pkgman refresh
pkgman install -y gcc binutils haiku_devel haikuwebkit haikuwebkit_devel git
```

What each provides for a build like this:

| Package | Provides |
|---|---|
| `gcc` | `cc`/`c++`/`gcc`/`g++` at `/boot/system/bin`, plus `rc`/`xres` (resource tools) in the base |
| `binutils` | `ld` (the linker) |
| `haiku_devel` | Haiku **private** headers under `/boot/system/develop/headers/private/**` and the in-tree **static libs** (`libnetservices.a`, `libcolumnlistview.a`, `libshared.a`, `liblocalestub.a`) + the C-runtime glue (`crti.o` …) |
| `haikuwebkit` | `libWebKitLegacy.so.1` (runtime) |
| `haikuwebkit_devel` | WebKit API headers (`WebView.h`, `WebPage.h`, …) directly in `/boot/system/develop/headers`, and the `libWebKitLegacy.so` link symlink |

Notes:
- There is **no `make`** in the repo yet — drive the build with a shell loop (below)
  or install a build system (`cmake`/`ninja` are in the repo).
- `haiku_devel` is **version-locked** to the running base (`requires haiku == <hrev>`);
  the DeBeOS repo always serves the copy matching the canonical AMI, so it just installs.

## 1. Fetch the source (self-hosted git clone)

```sh
git clone --depth 1 -b graviton https://github.com/felipedbene/Haiku-Graviton.git ~/haiku-src
```

A full shallow clone (~48 s, 26k files) gives the WebPositive sources **and** the
complete `headers/` tree — which matters because some private headers the app
transitively includes are not in the `haiku_devel` subset (see §2).

### git/curl/cargo CA note (fixed in curl 8.21.0-3+)

Earlier `curl` packages compiled libcurl's CA-bundle path as a **package-version-specific**
path (`/packages/curl-<ver>/ca_root_certificates/…`); bumping the `curl` revision dangled
it, breaking HTTPS for curl, git *and* cargo (`[77] SSL CA cert bad` /
`error adding trust anchors`). This is fixed in `curl-8.21.0-3` and later, which point
libcurl at the stable, version-independent `/boot/system/data/ssl/CARootCertificates.pem`
(always present from the base `ca_root_certificates`). Ensure you're current:
`pkgman update curl`. On an older `curl` only, the interim workaround is
`git config --global http.sslCAInfo /boot/system/data/ssl/CARootCertificates.pem`
(note: `CURL_CA_BUNDLE` does *not* help cargo — cargo sets the CA path from libcurl's
compiled default, which is exactly what the `-3` package fixes).

## 2. Compile — translate the Jamfile to a direct `g++` build

In the tree WebPositive is a jam `Application` target
(`src/apps/webpositive/Jamfile`). To build it standalone, mirror what the Jamfile
declares. Three non-obvious things fall out of that:

1. **App-local headers use quoted includes** (`#include "TabManager.h"`), while
   `#include <TabView.h>` means the **OS** `BTabView`. So put the app's own source
   dirs on `-iquote` (searched only for `"…"`), **not** `-I` — otherwise the app's
   `tabview/TabView.h` shadows the system header and you get
   `'BTabView' was not declared`.
2. **Private includes need the private *root*** on the path: e.g.
   `shared/HashSet.h` does `#include <util/OpenHashTable.h>`, and that resolves as
   `headers/private/util/OpenHashTable.h` — so add `-I<clone>/headers/private`
   (plus the named kit subdirs and `private/system`, per `UsePrivateSystemHeaders`).
3. The Jamfile's source *grouping* is cosmetic (jam's `SEARCH_SOURCE` finds files
   anywhere) — the real files may sit in a different subdir than the group implies
   (e.g. `BookmarkBar.cpp` is in the root, not `support/`). List the **actual**
   paths (`find src/apps/webpositive -name '*.cpp'`).

Build script (compile every source, then link):

```sh
WP=~/haiku-src/src/apps/webpositive
SRC=~/haiku-src
PH=$SRC/headers/private
L=/boot/system/develop/lib

# App-local dirs: -iquote (quoted includes only, so <TabView.h> stays the OS one)
LOCALS="-iquote$WP -iquote$WP/autocompletion -iquote$WP/support -iquote$WP/tabview"
# Private Haiku headers: the private ROOT (for util/, shared/, …) + named kits
PRIV="-I$PH -I$PH/system -I$PH/interface -I$PH/netservices -I$PH/shared \
-I$PH/tracker -I$PH/net -I$PH/kernel -I$PH/app -I$PH/support -I$PH/storage -I$PH/locale"
INCS="$LOCALS $PRIV -I$SRC/headers/libs/icon -I$SRC/src/kits/tracker"
CXXFLAGS="-std=c++17 -O1 -Wno-error=sequence-point -Wno-error=format-truncation"

mkdir -p ~/wpbuild && cd ~/wpbuild
for s in $(cd "$WP" && find . -name '*.cpp'); do
    g++ -c $CXXFLAGS $INCS "$WP/$s" -o "$(basename "$s" .cpp).o"
done

# Link. Static libs in a --start-group (they cross-reference); WebKit + system
# shared libs after. --no-gc-sections works around a GNU ld 2.41 aarch64 stub bug.
g++ -o WebPositive *.o \
  -Wl,--start-group \
    "$L/libnetservices.a" "$L/libcolumnlistview.a" "$L/libshared.a" "$L/liblocalestub.a" \
  -Wl,--end-group \
  -lbnetapi -lWebKitLegacy -lbe -lnetwork -ltracker -ltranslation \
  -Wl,--no-gc-sections
```

Result: `WebPositive`, an `ELF 64-bit … ARM aarch64 … dynamically linked`
executable (~1.0 MB). Confirm its runtime deps with `readelf -d WebPositive | grep
NEEDED` — you'll use that list to write the package's `requires`.

## 3. Attach resources (icon, signature, version)

```sh
rc  -o WebPositive.rsrc "$WP/WebPositive.rdef"     # compile the .rdef
xres -o WebPositive WebPositive.rsrc                # attach into the binary
mimeset -f WebPositive                              # (headless: warns "no app_server", harmless)
```

## 4. Package it (`package create`)

A `.hpkg` is the package root (files laid out under `apps/`, `lib/`, `data/`, …)
plus a `.PackageInfo`. **`licenses{}` and `copyrights{}` are mandatory**, and every
license named must be bundled at `data/licenses/<name>`.

```sh
cd ~/wpbuild
rm -rf pkgroot && mkdir -p pkgroot/apps pkgroot/data/licenses
cp WebPositive                       pkgroot/apps/WebPositive
cp /boot/system/data/licenses/MIT    pkgroot/data/licenses/MIT   # referenced below

cat > pkgroot/.PackageInfo <<'EOF'
name        webpositive
version     1.10.0-1
architecture arm64
summary     "WebPositive, the native Haiku web browser (DeBeOS arm64 build)"
description "WebPositive / HaikuWebKit web browser, natively compiled on DeBeOS arm64."
packager    "DeBeOS Haiku-Graviton"
vendor      "DeBeOS"
copyrights  { "2007-2024 Haiku, Inc." }
licenses    { "MIT" }
provides {
	webpositive = 1.10.0
	app:WebPositive = 1.10.0
}
requires {
	haiku
	haikuwebkit
}
EOF

package create -C pkgroot webpositive-1.10.0-1-arm64.hpkg
package list -i webpositive-1.10.0-1-arm64.hpkg     # verify metadata
```

- The canonical filename is `<name>-<version>-<architecture>.hpkg`.
- `requires` follows from §2's `readelf` output: `libWebKitLegacy` → `haikuwebkit`;
  `libbe`/`libnetwork`/`libtracker`/`libtranslation`/`libroot`/`libbnetapi` → `haiku`.
  (`libstdc++`/`libgcc_s` come from the base's `gcc_syslibs`, always present.)
- **Single-vendor repos:** to add this to the DeBeOS repo it must be vendor
  `DeBeOS` (it is, above); publishing tooling re-stamps otherwise. See
  `graviton/scripts/haiku-repo-publish-ephemeral` (the no-metal publisher that
  runs the incremental `haiku-repo-add`).

## 5. Install and verify

```sh
pkgman install -y ~/wpbuild/webpositive-1.10.0-1-arm64.hpkg
ls -l /boot/system/apps/WebPositive        # r-xr-xr-x -> activated via packagefs
```

`pkgman install <file>` resolves `requires` against the installed system (here
`haiku` + `haikuwebkit` are already present), commits the transaction, and
`package_daemon` activates it — the binary appears read-only under
`/boot/system/apps`, served from packagefs. That is the whole loop:
**source → native compile → hpkg → installed package**, done entirely on the box.

## Verified

All of the above was executed on the canonical lean AMI (`ami-0b8a587b3aaf243c7`,
`hrev59996`, Graviton `c7g.large`): 21 sources compiled, linked to a 1.0 MB
aarch64 binary, packaged to `webpositive-1.10.0-1-arm64.hpkg`, and installed +
activated at `/boot/system/apps/WebPositive`. Rendering a page is a separate step
(needs a display; headless Graviton has no framebuffer — see the remote-display
notes), but the build/package/install chain is proven.

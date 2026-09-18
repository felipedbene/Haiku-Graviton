# Vendored: Arm Optimized Routines (aarch64 string)

These `.S` files and `asmdefs.h` are copied **verbatim** from the Arm
Optimized Routines project:

- Upstream: <https://github.com/ARM-software/optimized-routines>
- Path:     `string/aarch64/`
- Commit:   `5288c42dd2bb61ffcadd6945a1a4655f993889b2`
- License:  MIT (see `LICENSE`; upstream is dual MIT OR Apache-2.0 WITH
            LLVM-exception — DeBeOS consumes them under the MIT terms, matching
            the rest of the libroot string code)

Do not edit these files. They define the internal `__<name>_aarch64` symbols
and are kept byte-for-byte identical to upstream so a re-vendor is a clean
copy. DeBeOS glue lives one directory up: `../arm_<name>.S` each `#include`s the
matching file here and exposes the public C name (`memcmp`, `strlen`, ...) as an
alias to the `__<name>_aarch64` entry point. See `../Jamfile` for how the glue
units are wired into libroot and the kernel, ahead of the generic musl sources.

Only a subset of the upstream directory is vendored — the scalar/AdvSIMD
implementations DeBeOS actually builds. The MTE/SVE/MOPS variants, the
dispatch tables and the test/benchmark harness are intentionally omitted.

`memcpy.S` is vendored **only for its `memmove`** (`__memmove_aarch64`, which
performs the overlap-aware backward copy). DeBeOS keeps its own `../memcpy.c`
as the public `memcpy`; the `__memcpy_aarch64` symbol this file also defines is
left unaliased and unused.

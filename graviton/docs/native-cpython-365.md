# Native CPython 3 on DeBeOS arm64 — verified (#365)

**Result: DeBeOS ships a fully working native CPython 3.14 on Graviton (arm64).**
All nine target C-extension modules import, `pip` is available out of the box,
`pip install` works for both pure-Python and compiled-from-sdist C-extension
packages, and a real `boto3` client makes signed calls against live AWS. This is
the substrate #120 (native aws-cli / boto3 / meson) builds on.

This was a **census-and-verify** job, not a from-scratch port: `python3.14` and
its entire C-extension dependency closure were already built and published to the
DeBeOS arm64 pool. The historical concern in `package-chain-status.md` — that the
Python under `haikuporter` was missing `zlib`/`_bz2`/`_lzma` — was a
**bootstrap-guest image artifact**. On a native `pkgman install` those modules are
present and import cleanly. Measured, don't assume.

## What exists in the pool (dependency closure — already published)

`pkgman install python3.14` pulls the whole closure from the DeBeOS repo. The
C-extension dependencies are all published:

| Module needs | Package (published) |
|---|---|
| `_ssl`, `_hashlib` | `openssl3-3.5.7` (also `openssl-1.1.1w`) |
| `_ctypes` | `libffi-3.4.6` |
| `_sqlite3` | `sqlite-3.50.4.0` |
| `readline`, `_curses` | `readline-8.3.003`, `ncurses6`, `libedit` |
| `zlib` | `zlib-1.3.2` |
| `_bz2` | `bzip2-1.0.8` |
| `_lzma` | `xz_utils-5.8.3` |
| `_decimal` | `mpdecimal-4.0.0` |
| `pyexpat` | `expat-2.8.2` |
| `_gdbm` | `gdbm-1.26` |

Interpreter packages published: `python3.14-3.14.7`, plus `python3.10`,
`python3.11`, `python3.12`. **Nothing needed to be built for #365.**

## Import census — python3.14-3.14.7, native install (hrev59996, c7g)

`python3.14 --version` → `Python 3.14.7`. Per-module `import`:

| Module | Result |
|---|---|
| `ssl` | OK |
| `ctypes` | OK |
| `hashlib` | OK |
| `sqlite3` | OK |
| `zlib` | OK (`lib-dynload/zlib.cpython-314.so`) |
| `bz2` | OK |
| `lzma` | OK |
| `readline` | OK (`lib-dynload/readline.cpython-314.so`) |
| `decimal` | OK |

Combined one-shot `import ssl, ctypes, hashlib, sqlite3, zlib, bz2, lzma, readline,
decimal` → `ALL-IMPORTS-OK`. `lib-dynload/` carries the full set (`_ssl`,
`_ctypes`, `_hashlib`, `_sqlite3`, `_lzma`, `_bz2`, `_decimal`, `_curses`,
`readline`, `_socket`, `_zstd`, …).

> `readline` prints `Cannot read termcap database; using dumb terminal settings.`
> under a non-TTY SSM shell — cosmetic (no `TERM`/termcap), the import still
> succeeds. Set `TERM=dumb` to silence it.

## pip is available out of the box

The `python3.14` package does **not** put `pip` on the base interpreter's path
(`python3.14 -m pip` → *No module named pip*), but the **bundled `ensurepip`
(26.2.1) is present and functional**, so pip is one standard step away with no repo
package and no network:

- `python3.14 -m venv <dir>` seeds a working `pip 26.2.1` into the venv **offline**
  (venv → ensurepip). `<venv>/bin/pip --version` → `pip 26.2.1`.
- `python3.14 -m ensurepip` / `get-pip.py` also work.

There is no standalone `pip` hpkg in the pool; it is not needed because ensurepip
is bundled. A `pip_python3.14` package would only be polish (OOTB `pkgman install
pip`).

## Proof — pip install + a real package against live AWS

On the Graviton instance (instance role only, no static creds):

- **Pure-Python install:** `pip install boto3` →
  `boto3-1.43.98 botocore-1.43.98 jmespath-1.1.0 python-dateutil-2.9.0.post0
  s3transfer-0.19.2 six-1.17.0 urllib3-2.8.0`, all from PyPI over NAT egress.
- **Real AWS call 1 (STS):** `boto3.client('sts').get_caller_identity()['Arn']`
  returned the instance-role assumed-role ARN — exercises the full
  `ssl`/`urllib3`/`botocore` SigV4 + TLS path end to end.
- **Real AWS call 2 (S3):** `boto3.client('s3').list_buckets()` returned the
  account's buckets (the `s3 ls` equivalent through the instance role).
- **C-extension install (compiled from sdist):** `pip install MarkupSafe` built
  `markupsafe-3.0.3-cp314-cp314-haiku_...arm64.whl` from source and loaded its
  compiled `markupsafe/_speedups.cpython-314.so`; `markupsafe.escape('<b>&hi')`
  → `&lt;b&gt;&amp;hi`. PyPI ships no Haiku wheels, so this is a genuine local
  compile, not a wheel download.

## Operational notes (for the next agent)

1. **Compiling C-extensions from sdist needs `gcc` AND `haiku_devel`.** `gcc` alone
   fails at `fatal error: assert.h: No such file or directory` — the system C
   headers live in `haiku_devel`. Install both (`pkgman install gcc binutils make
   haiku_devel`). The canonical AMI is runtime-only, so a box that only *runs*
   Python (pure-Python wheels, e.g. boto3) needs no compiler; one that *builds*
   C-extensions does.
2. **Transient PyPI gzip-decode flake.** `pip install` occasionally aborts with
   `Received response with content-encoding: gzip, but failed to decode it (Error
   -3 while decompressing data: incorrect header check)` on a Fastly-edge
   response. It is intermittent (boto3 downloaded clean; setuptools failed twice
   then succeeded on retry) — retry cures it; not a zlib defect in DeBeOS.
3. Use `python3.14 -m venv` for real work: it gives an isolated `pip` without the
   root-user warning and without touching the packaged tree.

# Builder-host orchestration scripts

> **DEPRECATED / historical.** These scripts drove the metal + QEMU-guest build
> fleet, which has been retired along with the shared metal host. Native package
> builds now run directly on Graviton Haiku EC2 instances over SSM — see
> `graviton/scripts/haiku-nativebuild` and `graviton/docs/native-ec2-builds.md`.
> This directory is kept for reference (notably the clock-step post-mortem below).

They drove the **native package build** on the retired fleet: they created QEMU
Haiku guests on the `c7g.metal` builder, fanned ports out across them, ran
`haikuporter` inside each guest, and harvested the resulting hpkgs to
`/opt/haiku/hpkg-out/arm64/` and S3.

They lived in `/opt/haiku/` on the builder and were invoked from there. This
directory is the version-controlled copy — **edit here, then copy out**, not the
other way round.

## Why they are in the repo now

They were not, and it cost days.

`cwork.sh` and `gworker.sh` harvested finished packages with a blanket
`scp 'guest:/boot/home/haikuports/packages/*.hpkg' /opt/haiku/hpkg-out/arm64/`. But
`haiku*.hpkg` in that directory are the chroot's build **inputs**, not its outputs:
`haikuporter` activates its own packagefs from
`/boot/home/haikuports/packages/haiku.hpkg`. So the wildcard copied a build input into
the directory that `mkguest.sh` uses to seed new guests, and every new guest
reinstalled it.

One of those copies, dated 20 Aug, still contained the **pre-fix `system_time()`** —
reading `CNTPCT_EL0` (the *metal's* physical counter, which KVM does not rebase) with
an overflowing `ticks * 1000000`. Every chroot therefore ran on a clock folded into a
4 h 53 m sawtooth built from another machine's counter, roughly 18 h off the truth,
while the kernel stamped mtimes correctly. Because `haikuporter` re-extracts a recipe
whenever `mtime(recipe) <= mtime(sourcePackage)`, **that skew silently reverted recipe
edits** — three consecutive fixes to one recipe vanished with no error anywhere.

Rebuilding the image never fixed it, because the harvest re-imported the stale
artifact afterwards. The bug was in an unversioned shell script, invisible to
`git log`, code review, and every image rebuild. Hence this directory.

Full analysis: `graviton/docs/arm64-clock-step.md`.

## The fix, and the invariant to preserve

Both harvest scripts now stage into a temporary directory and delete
`haiku.hpkg`, `haiku_*.hpkg` and `haiku-*.hpkg` before copying anything into
`hpkg-out/arm64/`.

**Invariant: never let a build input into the guest seed directory.** If you add
another harvest path, exclude `haiku*.hpkg` there too.

Two related gotchas:

- `BuildPlatform.py` takes the **first `os.listdir` match** of `haiku.hpkg` *or*
  `haiku-*.hpkg`, so a guest must carry **exactly one** candidate. Two copies means
  the one that wins is filesystem-order dependent.
- Check which `system_time()` a package actually contains, rather than trusting its
  version string:

  ```
  package extract .../haiku.hpkg lib/libroot.so \
    && objdump -d lib/libroot.so | grep -A1 '<system_time>:'
  ```

  `cntvct_el0` is correct; `cntpct_el0` is the stale, broken one.

## What is here

| script | role |
|---|---|
| `mkguest.sh` | create a QEMU Haiku build guest, seeded from `hpkg-out/arm64/` |
| `prepguest.sh` | make one guest ready for a bottom-up rebuild: refuse it if its chroot `haiku` is still `_dirty`, quarantine every non-input hpkg out of `packages/`, install the edited recipes with pinned mtimes, install the wget/decompress shims and `patch(1)` |
| `rebuild.sh` | rebuild ports in one guest against the repaired non-dirty chroot, pushing the shared pool in and harvesting out, so several guests can cooperate on one dependency graph |
| `boot-qemu.sh`, `runboot.sh` | boot a guest with the forwarded-ssh setup |
| `cwork.sh`, `gworker.sh` | run `haikuporter` per port in a guest, then harvest + S3 sync |
| `worker.sh`, `worker-full.sh`, `fanout2.sh`, `dolt.sh`, `dolt2.sh` | port fan-out across guests |
| `make-gpt-image.sh` | build the bootable GPT disk image |
| `build-*.sh` | cross-tools, minimum image, A/B image builds |
| `validate-ssh.sh` | check a guest's sshd is actually usable |
| `bstat.sh` | build status snapshot |
| `jumbo.sh` | jumbo-frame test helper |

One-off investigation aids that lived beside these (`clock-*.sh`, `probe*.sh`,
`*-setup*.sh`) are deliberately **not** captured — they were throwaway diagnostics, not
pipeline logic.

## Conventions these encode, learned the hard way

- **Verify a port by `ls` of its hpkg, never by exit code**, and sync to S3 after
  *every* port — a guest swap once lost a finished `diffutils`.
- **`ssh` inside a `while read` loop eats the loop's stdin** — use `ssh -n` there. But
  drop `-n` when feeding a script by heredoc, or it receives nothing.
- **A probe whose empty output means "error", not "nothing to do"**: `haikuporter`
  dependency resolution printed nothing when it had *failed*, and reading that as "all
  satisfied" caused a fan-out onto six ports that were all blocked.
- **Never diagnose from `tail -18` of a build log.** That destroyed the decisive
  evidence on `libtool` more than once.
- **Verifying a port with `ls | grep "^<port>[-_]"` is not enough**, because the guest's
  `packages/` also holds the cross-built bootstrap set. `ls | grep '^m4[-_]'` matched
  `m4-1.4.19_bootstrap-1-arm64.hpkg` and reported a successful `m4` rebuild that had never
  happened; it was only caught by diffing the finished set against the list of names being
  replaced. `rebuild.sh` now appends `| grep -v _bootstrap`.
- **Do not overwrite one of these scripts while a copy of it is running.** bash reads a
  script incrementally by byte offset, so replacing the file mid-run makes the running
  shell resume at a shifted offset: a live `rebuild.sh` ended up executing
  `remove-destination: command not found` and `syntax error near unexpected token 'done'`
  out of the middle of its own loop. Install to a new name, or wait for the `.done` marker.

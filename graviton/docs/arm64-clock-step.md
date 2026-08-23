# The 18-hour clock step inside the haikuporter build chroot

Status: **root-caused and reproduced to the second**. The cause is a stale artifact,
not a bug in this tree; the cure is to refresh two packages in the build guests
(below). This branch adds the hardening that would have made it loud.

## Symptom

Ports built by `haikuporter` on the arm64 build guests printed, over and over:

```
make: Warning: File 'conftest.mk' has modification time 66159 s in the future
make: warning:  Clock skew detected.  Your build may be incomplete.
```

66159 s is 18 h 22 m 39 s. Other ports on other guests reported 64825, 64790,
82393. The same quantity showed up as "the clock steps backwards by about 18
hours" during a `libtool` build, and as `touch` complaining that files were in
the future.

This matters far more than a wrong clock, because `haikuporter`'s
`Repository.py:_partiallyExtractSourcePackageIfNeeded` re-extracts a recipe from
its source hpkg whenever `mtime(recipe) <= mtime(sourcePackage)`. With the clock
behind, every hand-edited recipe was silently reverted before `BUILD()` ran — the
md5 of the recipe still matched pristine, which is what made three successive
fixes to one recipe appear to vanish.

## Verdict: chroot-only. The guests' own clocks are correct.

Outside the chroot a freshly created file's mtime equals `time()` exactly, and the
guest agrees with the metal to within 2 s after 24 h of uptime:

```
guest 2227/2229/2230  date -u +%s = 1787527000      host = 1787527002
dir=/tmp   now=1787527483  mtime=1787527483   mtime-now=0
```

So nothing writes the clock backwards. Instead **two different clocks are in play,
and only one of them is inside the chroot**:

* `make`'s "now" comes from `time()` in whichever `libroot.so` is loaded.
* the mtime it compares against is stamped by the **kernel**, from
  `real_time_clock()`.

They can only disagree if userland's `libroot.so` is not the one the kernel was
built with. Inside a haikuporter chroot, it isn't.

## Root cause

`setupChrootScript` (`HaikuPorter/ShellScriptlets.py`) symlinks the resolved
dependency packages into `<workdir>/boot/system/packages` and mounts a fresh
packagefs over `<workdir>/boot/system`. One of those packages is the Haiku base
system itself, and on these guests it resolves to
`/boot/home/haikuports/packages/haiku.hpkg` — **a different file from the
`/boot/system/packages/haiku-*.hpkg` the running system booted from**:

| | version | `lib/libroot.so` mtime | md5 of `libroot.so` |
|---|---|---|---|
| `/boot/system/packages/haiku-r1~beta6_hrev59996-1-arm64.hpkg` (outer) | `r1~beta6_hrev59996-1` | Aug 22 23:02 | `1b4f57f2…` |
| `/boot/home/haikuports/packages/haiku.hpkg` (**chroot**) | `r1~beta6_hrev59996_dirty-1` | **Aug 20 18:10** | `daa195b5…` |

The chroot's copy predates the `system_time()` fix. Disassembling both settles it:

```
outer, fixed:                        chroot, stale:
  mrs  x1, cntvct_el0                  mrs  x0, cntpct_el0
  mrs  x2, cntfrq_el0                  mrs  x1, cntfrq_el0
  udiv x3, x1, x2                      mov  x2, #1000000
  mov  x0, #1000000                    mul  x0, x0, x2
  msub x1, x3, x2, x1                  udiv x0, x0, x1
  mul  x1, x1, x0                      ret
  udiv x1, x1, x2
  madd x0, x3, x0, x1
  ret
```

The stale one carries **both** of the bugs fixed in
`6e3eb83bd2 arm64: fix system_time() overflow and read the virtual counter`:

1. it reads `CNTPCT_EL0`, the **physical** counter, which a hypervisor does not
   rebase — under KVM that is the *host's* counter, running since the metal
   powered on; and
2. `ticks * 1000000` overflows `uint64` after `2**64 / 10**6` ticks, so the
   result is a sawtooth with period `P = 2**64 / (CNTFRQ_EL0 * 10**6)`
   = **17568.327689 s** (4 h 52 m 48 s) at the 1.05 GHz Graviton counter.

Every process in the chroot — `make`, `configure`, `gcc`, and the `touch` that
sets a recipe's mtime — therefore reads a clock built from a counter that belongs
to another machine, folded into a 4 h 53 m sawtooth. The kernel, correctly using
`CNTVCT_EL0` and a two-step conversion, keeps stamping mtimes with the true time.

### How the stale package keeps coming back

It is a closed loop, which is why refreshing one guest is not enough:

* `mkguest.sh` seeds each new guest with
  `scp /opt/haiku/hpkg-out/arm64/*.hpkg → /boot/home/haikuports/packages/`;
* `cwork.sh` / `gworker.sh` harvest **back** with
  `scp guest:/boot/home/haikuports/packages/*.hpkg → /opt/haiku/hpkg-out/arm64/`
  after every port, and `aws s3 sync` that directory to the hpkg bucket.

`/opt/haiku/haikuports/packages/haiku.hpkg` and
`/opt/haiku/hpkg-out/arm64/haiku.hpkg` are byte-identical (`c89d946c…`): the
Aug 20 `_dirty` build has been laundering itself through the guests, the host and
S3 ever since, and no rebuild of the guest *image* replaces it, because it does
not live in the image's package set.

## The arithmetic, reproducing 66159 s

Let

* `offset = firmware - init_u` — the commpage offset the kernel installs, where
  `firmware` is the wall clock the loader sampled from EFI `GetTime()` and
  `init_u` the uptime at which it did. Both are printed at boot:
  `arch_rtc_init: firmware time 1787441314 s at 6763771 us uptime`;
* `T0'` = the wall time the **metal** powered on, expressed on the guest's own
  clock, so the physical counter reads `T - T0'`;
* `u(T) = T - offset` — the guest's true uptime, from `CNTVCT_EL0`.

The stale `libroot` adds its sawtooth to the same commpage offset the kernel
uses, so the deficit `make` sees is

```
D  =  u(T)  -  ((T - T0') mod P)                            P = 17568.327689 s
   =  (T - offset) - (T - T0' - k*P)
   =  T0' + k*P - offset
```

`T` cancels: **within one wrap of the sawtooth `D` is a constant**, which is why
it stayed pinned at 66159 across three consecutive `libtool` runs eight minutes
apart. A rough wall time only has to be good enough to fix `k`, the number of
wraps.

`T0'` is measured, not fitted — read `CNTPCT_EL0` against `time()` from inside a
guest, since that register is precisely the metal's counter:

```
guest 2229:  time() = 1787527934   cntpct = 188912768186157 (179916.922 s)
             ->  T0' = 1787527934 - 179916.922 = 1787348017.08
```

Against the four observations — three guests, three different `firmware` seeds:

| log | guest | `offset` | `k` | `D` predicted | `D` observed |
|---|---|---|---|---|---|
| `lt-full.log` (libtool) | run8 / 2227 | 1787439973.62 | 9 | **66158.4** | **66159** |
| `hp-2229-patch.log` | run9 / 2229 | 1787441307.25 | 9 | 64824.8 | 64824 |
| `hp-2230-pkgconf.log` | run10 / 2230 | 1787441307.24 | 9 | 64824.8 | 64825 |
| `c-gettext-2230.log` | run10 / 2230 | 1787441307.24 | 10 | 82393.1 | 82393 |

The `libtool` row is the one worth dwelling on, because its guest is an output of
the model rather than an input: `lt-full.log` does not record which guest it ran
on, and 2227 had wedged by the time this was investigated. 66159 fits run8's
`offset` and no other guest's, and run8 is the one whose `firmware` seed is
1334 s earlier — the same 1334 s that separates run8's and run9's `qemu` start
times. That is what identifies it.

All four land within a second — the residual is the truncation in `make`'s
whole-second arithmetic plus the fact that each guest's `firmware` seed is
rounded differently (`T0'` was measured on run9; the metal's own clock puts
power-on at 1787348019.71, i.e. every guest's wall clock sits about 2.6 s behind
the metal's, an artefact of seeding from a whole-second EFI `GetTime()`).

Three features of the table are worth keeping:

* **`D` is not the same on two guests, and it is not a round number**, because
  `offset` differs per guest while the counter term is shared. That is exactly
  why the figure looked like noise rather than a constant, and why it is not a
  timezone: 66159 is not even a whole number of minutes.
* **`D` jumps by exactly one `P` when the sawtooth wraps.** `64825 → 82393` on
  the same guest is `+17568`: the same bug, one wrap later. That step is the "the
  clock jumped backwards 18 hours mid-build" report.
* Solving the model backwards from just those four numbers, before any counter was
  read, gives `T0' ≡ 1787348017` — which is what promoted this from a plausible
  story to a measurement.

And a live probe on guest 2229 closes it. Running the stale formula next to the
fixed one, in the same process — by then the sawtooth had reached `k = 10`, so the
prediction for that guest is the 82393.1 of the last table row, not the 64824.8 of
its own:

```
cntpct        = 188912768186157  (179916.922 s)   <- the metal's counter
cntvct        =  90958739934281  ( 86627.371 s)   <- this guest's uptime
system_time() = 86627371365 us                   [fixed]
buggy(cntpct) =  4233645189 us  (4233.645 s)      <- sawtooth
stale wall    = 1787445540   deficit = 82394 s
```

An actual `haikuporter -y -G -f patch` on that guest minutes later printed
`modification time 82392 s in the future`.

The `cntpct - cntvct` difference is the KVM `CNTVOFF` for that VM, and it is a
third independent check: 93289.55 s on guest 2229 against 94953.66 s on guest
2231, a difference of 1664.1 s — exactly the gap between the two `qemu`
processes' start times.

## The fix

**The cure is to stop shipping the Aug 20 package.** This tree is already
correct; nothing in it produces that `libroot.so` any more.

On the metal, the packages built from the fixed tree are at

```
/opt/haiku/run8/haiku/generated.arm64/objects/haiku/arm64/packaging/repositories/Haiku/packages/
    haiku-r1~beta6_hrev59996-1-arm64.hpkg          (libroot md5 1b4f57f2…, cntvct)
    haiku_devel-r1~beta6_hrev59996-1-arm64.hpkg
```

so the loop has to be broken in all three places at once, or the stale copy will
be harvested straight back:

1. replace `/opt/haiku/hpkg-out/arm64/haiku.hpkg` and `haiku_devel.hpkg` (the
   seed `mkguest.sh` uses, and the S3 sync source) with the two files above;
2. replace `/opt/haiku/haikuports/packages/haiku{,_devel}.hpkg` likewise;
3. on each live guest, overwrite `/boot/home/haikuports/packages/haiku.hpkg` and
   `haiku_devel.hpkg` — there must be exactly one candidate, because
   `BuildPlatform.py` picks the first entry matching `haiku.hpkg` **or**
   `haiku-*.hpkg` in `os.listdir` order, which is not deterministic if both are
   present.

Then re-run one small port and confirm no `in the future` line survives. Longer
term the harvest in `cwork.sh` / `gworker.sh` should exclude `haiku*.hpkg`
outright: those are build *inputs*, and copying them out of a guest can only ever
re-import whatever that guest happened to have.

### Hardening carried on this branch

None of the above stops it happening again, because the failure was **silent**: a
stale binary read a counter that does not belong to this machine and got a
plausible number back. Two changes make that a fault instead.

`arch_init_timer()` granted EL0 access to both counters:

```c
WRITE_SPECIALREG(CNTKCTL_EL1, CNTKCTL_EL0VCTEN | CNTKCTL_EL0PCTEN);
```

`CNTKCTL_EL0PCTEN` is now dropped, as Linux does — `CNTFRQ_EL0` stays readable at
EL0 because either bit permits it, and nothing in the tree reads `CNTPCT_EL0`.
The boot loader's `WRITE_SPECIALREG(CNTKCTL_EL1, 0b11)` is spelled with the same
constant so the two cannot drift apart.

That only helps if a trapped `MRS` produces a diagnosable signal, and it did not:
`do_sync_handler()` decoded five exception classes and left `exceptionType`,
`signalNumber` and `signalCode` **uninitialized** for every other one, so a
trapped system-register access — or an undefined instruction, or a data abort
with a fault status the page-fault path does not translate — raised whatever
signal number happened to be on the stack. They now default to
`SIGILL`/`ILL_ILLOPC` at the faulting PC, `EXCP_MSR` reports `ILL_PRVREG`, and an
undecoded data abort reports `SIGSEGV`/`SEGV_MAPERR` at `FAR`.

**These two changes must be baked together with the package refresh.** With
`EL0PCTEN` clear, any guest still carrying the Aug 20 `haiku.hpkg` will `SIGILL`
on the first `time()` call inside a chroot instead of building with an 18-hour
skew. That is the intended behaviour — it fails at the start rather than
producing silently corrupted packages — but it means a guest with the old package
stops working entirely rather than working badly.

## Checking for it in one line

From the metal, for any guest port:

```
ssh -n -p <port> baron@127.0.0.1 \
  'rm -rf /tmp/hx && mkdir /tmp/hx && cd /tmp/hx &&
   package extract /boot/home/haikuports/packages/haiku.hpkg lib/libroot.so &&
   objdump -d lib/libroot.so | grep -A1 "<system_time>:"'
```

`cntvct_el0` is correct. `cntpct_el0` means every build in that guest's chroots is
running on the metal's clock.

## Not this bug

Two other arm64 clock defects are already fixed and are *not* what this was, in
case the next wrong timestamp looks familiar:

* the same `system_time()` overflow in the **kernel and outer libroot**, which
  stepped the whole system back 4 h 52 m 48 s per wrap
  (`6e3eb83bd2`) — a different signature, and guest-wide rather than chroot-only;
* `arch_rtc_get_hw_time()` returning 0, which left the wall clock at 1970 until
  the loader started sampling EFI `GetTime()`.

Guest 2222, still running the pre-fix image, was measured 105414 s behind the
metal — exactly `6 x 17568.33`, the outer-libroot form of the same overflow.

# Stop, start, and no sshd

A Haiku arm64 instance works on first boot. Stop it and start it again and it
answers ping in 0.17 ms with nothing listening on port 22 -- a TCP RST, not a
timeout, so the host is up and the network stack is fine. 5301 and 80 are closed
too. First boot on the same AMI brings sshd up every time.

The kernel, the ENA driver, DHCP and the network stack are all innocent. The
whole failure is two bugs meeting.

## Root cause

**The sshd host key is on disk with the right size, mode and mtime, and its
contents are 411 zero bytes.**

```
  BROKEN volume (post stop/start)
  -rw-r--r--  1495 Aug 23 18:36 ssh_config                    nonzero_bytes=1495
  -rw-------   411 Aug 23 18:45 ssh_host_ed25519_key          nonzero_bytes=0
  -rw-r--r--    96 Aug 23 18:45 ssh_host_ed25519_key.pub      nonzero_bytes=0
  -rw-------  4006 Aug 23 18:45 ssh_host_mldsa44-ed25519_key  nonzero_bytes=0
  -rw-r--r--  1892 Aug 23 18:45 ssh_host_mldsa44-ed25519_key.pub  nonzero_bytes=0
  -rw-r--r--  2321 Aug 23 18:36 sshd_config                   nonzero_bytes=2321
```

Note which files survived. `ssh_config` and `sshd_config` were written by the
image build (mtime 18:36) and are intact. Everything written at *runtime* by the
first boot (mtime 18:45) is zeroed. Same on `/boot/system/var/log/syslog`, on
`sshd_boot.stamp` and on `remote_desktop.stamp` -- which is also why there is no
syslog to read from either volume, and why the first hour of this investigation
produced nothing.

`sshd_config` names exactly one host key:

```
HostKey /boot/system/settings/ssh/ssh_host_ed25519_key
```

so that one unreadable file is the whole story. On the real platform, with the
real binary:

```
$ /boot/system/bin/sshd -t -h /boot/home/hktest/zerokey
Unable to load host key "/boot/home/hktest/zerokey": invalid format
Unable to load host key: /boot/home/hktest/zerokey
```

sshd exits with no host keys at all. `sshd_boot.sh` then does not recover,
because it generated the key `if [ ! -f ]` -- and the broken file very much
exists, so it is never regenerated. launch_daemon restarts the service, the
script skips keygen again, sshd exits again, forever. Host up, port 22 refused.

### Why the data is missing: the page writer never writes

Not "lost the last few seconds". **On this build file data is never flushed at
all.** `src/system/kernel/vm/vm_page_writer.cpp`, `ModifiedPageQueue::_PageWriter`:

```c
	while (fWriterThread >= 0) {
		if (queue.Count() < kNumPages) {
			// wait the full amount when no one triggers us
			if (!fPageWriterCondition.Wait(PAGES_FLUSH_DURATION_LOCAL_QUOTA, true))
				continue;
		}
```

`BinarySemaphore::Wait()` returns `true` when notified and `false` on timeout
(`headers/private/kernel/util/BinarySemaphore.h`, `return entry.Wait(B_RELATIVE_TIMEOUT,
timeout) == B_OK;`). The timeout *is* the periodic flush -- "nobody triggered us,
write what is queued anyway" -- and it is the branch that `continue`s. A modified
queue holding fewer than `kNumPages` (256) pages is therefore never even looked
at. A 411-byte key is one page.

Nothing else covers the gap:

- `NotifyWriter()` is only reached from `full_scan_inactive_pages()` under real
  memory pressure, and from `msync()`. An idle instance with free RAM hits
  neither.
- The back-pressure path cannot fire either. `IsOverQuota()` is computed from
  `fLastAveragePageWriteDuration`, which starts at 0 in `StartWriter()` and is
  only assigned *after* a successful write run. No write, no estimate, no quota,
  no wake-up, no write. The failure sustains itself.

Introduced upstream in `3bfaa91290` "kernel/vm: Per-KDiskDevice modified queues
and page writers." The `while (fWriterThread >= 0)` loop condition needed a
`continue` for clean thread shutdown; attaching it to the wait result destroyed
the 3-second periodic flush. It is not arch-gated -- arm64 is not special here,
it is just where it was noticed.

Metadata survives because BFS journals it and the block cache flusher has the
*correct* timeout polarity (`block_cache.cpp`, `block_notifier_and_writer`:
`continue` on the event branch, fall through to write on timeout), plus BFS's
`TRANSACTION_IDLE` listener. `bfs_write` states the split outright:

```c
	Transaction transaction;
		// We are not starting the transaction here, since
		// it might not be needed at all (the contents of
		// regular files aren't logged)
```

So the inode, its size, its mode, its mtime and the block run pointing at the
data are all durable, and the blocks they point at still hold whatever was there
before. That is exactly the on-disk signature above.

#### Proof

A/B on a live instance. Write two files, `sync` one of them, wait 30 s,
snapshot the EBS volume from outside, attach the copy elsewhere and grep the raw
device:

| file | `sync` called? | bytes on the physical disk |
|---|---|---|
| `durability-test.txt` | yes | 32 of 32 nonzero, content correct |
| `nosync-test.txt` | no | 24 allocated, **0 nonzero** |
| `ssh_host_ed25519_key` | flushed by that same `sync` | 411 of 411 nonzero, valid PEM |

The host key had been sitting unflushed for six minutes and became durable only
because a `sync` happened to catch it.

#### The single-variable confirmation

The same experiment, run forwards. Launch from the canonical AMI, let it boot,
run **one `sync`**, then stop and start it -- changing nothing else, no new
image, no code:

```
=== SECOND BOOT of a box whose host key was synced to disk ===
RESULT: port 22 is OPEN on the second boot (try 1, ~15s)
logged in as baron
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIB55D3gIErhaluWNIfghsIn2dmZ26ibb/uLuJJ4YBwxY
```

and that is the *same* host key the first boot generated. Two other instances of
the same AMI and type, stopped and started without that `sync`, refuse port 22
permanently. One `sync` is the entire difference between the working and broken
cases, which is as direct a confirmation of the diagnosis as this system allows.

### Why every stop is unclean

On Nitro, `StopInstances` makes the hypervisor send an ACPI shutdown request
equivalent to a single power-button press, and gives the guest roughly 3-4
minutes before a hard power-off. `TerminateInstances` sends nothing at all.

Haiku arm64 had no inbound path for that event, so the grace period always
expired. `system_shutdown()` -- the only thing that calls `sync()` before
`arch_cpu_shutdown()` -- was never reached. Combined with a page writer that
never flushes on its own, the steady-state amount of unflushed file data is
*everything written since boot*, and every stop threw all of it away.

## The fixes

### 1. `kernel/vm: don't skip the page writer's periodic flush on timeout`

Ignore the wait result and let the loop body run. Being woken early means either
that there is work or that `~ModifiedPageQueue` is tearing the queue down after
setting `fWriterThread = -1`; both want the body, the latter so that what is
still queued is drained before the loop condition ends it.

Also stop `fLastAveragePageWriteDuration` being pinned at zero, so the quota
mechanism can arm itself: a noisy sample from a short run beats an accurate zero
that disables back-pressure entirely.

This is the root fix for the data loss, and it is not arm64-specific.

### 2. `graviton/ssh: make the sshd host key survive an unclean boot`

Resilience, and it still matters with (1) fixed, because terminate and genuine
crashes are always unclean. Test whether the key *parses* (`ssh-keygen -y`)
rather than whether the file exists, and regenerate it if it does not, so the
boot self-heals. Then `sync` once the keys are written, so the ordinary case
keeps the key it started with instead of recovering onto a new one and tripping a
host-key-mismatch warning on every client that had already connected.

The restart-throttle stamp is read the same defensive way -- it is also a
runtime-written file that can come back as NULs, and `expr` on a non-number is a
spurious failure. A stamp from the future is discarded too, since this platform
has had the clock step backwards.

### 3. `arm64/acpi: receive the platform power button through the PL061`

The root fix for the *unclean* part: make an EC2 stop shut Haiku down cleanly.

On a hardware-reduced platform (`HW_REDUCED_ACPI`, i.e. every ARM server) there
is no PM1 event block, so the power button cannot be an ACPI fixed feature. It is
a control-method device instead. The real chain, from a DSDT dumped off a
`c7g.large`:

```
Device (GPO0)
    Name (_HID, "ARMH0061")               // ARM PL061 GPIO controller
    Name (_CRS, ...  Memory32Fixed (0x09030000, 0x1000)
                     Interrupt (Level, ActiveHigh) { 0x27 })
    Name (_AEI, ...  GpioInt (Edge, ActiveHigh, ...) { 0x0003 }
                     GpioInt (Edge, ActiveHigh, ...) { 0x0004 })
    Method (_E03) { Notify (PWRB, 0x80) }  // pin 3 -> power button
    Method (_E04) { Notify (SLPB, 0x80) }  // pin 4 -> sleep button

Device (PWRB)  _HID "PNP0C0C"
```

so:

```
pin toggles -> PL061 raises its interrupt
  -> run the pin's _Exx method
    -> its AML does Notify(PWRB, 0x80)
      -> acpi_button's notify handler marks /dev/power/button/power
        -> power_daemon reads it and asks the registrar to shut down
```

Haiku had **no GPIO support of any kind** (no `pl061`, no `ARMH0061`, no GPIO bus
manager, and `_AEI` appears only inside vendored ACPICA, never used), so the
middle two steps did not exist. And `acpi_button` was gated to `x86,x86_64`, so
`/dev/power` was empty and `power_daemon` -- which *is* in the arm64 image -- had
nothing to watch even if a notify could have arrived.

Measured on a running arm64 instance from the canonical AMI, before the fix:

```
$ ls -la /dev/power/
total 0
drwxr-xr-x 1 baron root 0 Aug 23 23:51 .
drwxr-xr-x 1 baron root 0 Aug 23 23:51 ..          <- empty, no button/

$ ls -la /boot/system/add-ons/kernel/drivers/power/
ls: cannot access ...: No such file or directory    <- the list was empty, so
                                                      the directory is absent
$ ps | grep power
/boot/system/servers/power_daemon   37   1           <- running, with nothing to watch

$ ls /dev/acpi*
call
namespace                                           <- the bus manager is alive
```

So every ingredient was present except the two steps in the middle.

Three changes:

- **New driver `pl061_acpi_event`** (`src/add-ons/kernel/drivers/power/pl061_acpi_event/`).
  Matches `_HID "ARMH0061"`, reads `_CRS` for the registers and the interrupt,
  walks `_AEI` for the pins, arms exactly those pins, and on an interrupt
  evaluates the matching `_Exx`/`_Lxx` from a DPC -- AML cannot be evaluated in
  interrupt context, and `queue_dpc()` is documented to be callable from an
  interrupt handler. It publishes no device (hence `B_KEEP_DRIVER_LOADED`, or the
  device manager would drop it as unused) and claims no PL061 that has no `_AEI`.

  **Nothing is hardcoded, deliberately.** These differ across instance types of
  one family:

  | | c7g.metal | c7g.large |
  |---|---|---|
  | power-button method | `_E00` | `_E03` (pin 3) |
  | PL061 interrupt | 23 | 0x27 = 39 |
  | `ACPI0013` GED | absent | present, PCI hotplug only |

  Two ordering details are load-bearing and commented as such in the source: the
  pins are masked before they are configured and the latched state is discarded
  before the enable, because an edge firmware left pending would otherwise shut
  the machine down just as it finished booting; and the interrupt is acknowledged
  in the controller *before* the method is dispatched, since the GIC treats this
  as level-triggered and the method can take milliseconds.

- **Un-gate `acpi_button` for arm64** in `build/jam/images/definitions/minimum`.
  It is architecture-neutral; it only talks to the ACPI bus manager, which was
  already built for arm64.

- **Stop registering the fixed-button nodes when `HW_REDUCED_ACPI` is set**
  (`bus_managers/acpi/Module.cpp`). The `ACPI_FPB`/`ACPI_FSB` pseudo-HIDs
  represent buttons in the PM1 event block; the existing test is only
  `(FADT.Flags & ACPI_FADT_POWER_BUTTON) == 0`, and those flags are 0 here, so
  `acpi_button` would attach to a device that can never report anything and
  publish `/dev/power/button/power_fixed` for `power_daemon` to sit on while the
  real control-method button went unwatched.

## What is verified and what is not

Verified:

- The zeroed host key on the broken volume, against a first-boot control, by
  read-only `befs` mounts of EBS snapshot copies. (The Linux `befs` driver reads
  file data correctly -- `.hpkg` magic and the build-time configs come back
  intact -- so the zeros are real and not a driver artefact.)
- `Unable to load host key ... invalid format` from the actual Haiku arm64 sshd
  binary given a zero-filled key of the right size.
- The page-writer polarity bug, by reading `BinarySemaphore::Wait()`.
- The durability A/B against the raw physical disk (table above).
- The self-heal logic in `sshd_boot.sh`: keeps a good key, regenerates a
  zero-filled one, final key valid.
- All four code changes compile for arm64, and both `acpi_button` and
  `pl061_acpi_event` are present in the built `haiku.hpkg`:
  ```
  add-ons/kernel/drivers/power/acpi_button
  add-ons/kernel/drivers/power/pl061_acpi_event
  ```
  (that directory was empty on arm64 before).

**Not** verified: that the PL061 path actually fires on EC2. It cannot be tested
locally -- QEMU's `virt` machine delivers the power button through the ACPI
**GED**, EC2 through the **PL061**, so a GED implementation passes under QEMU and
does nothing on real hardware. Same divergence class as the PL011-vs-16550 serial
trap. It has to be tested on a real instance.

Also unverified: the PSCI `SYSTEM_OFF` that `arch_cpu_shutdown()` now issues has
never actually executed its SMC branch under Haiku. EC2 uses SMC; QEMU only ever
advertises HVC. Interestingly a `t4g.medium` reports `conduit=hvc`
(`discovered psci from acpi: conduit=hvc` in its boot log), so t4g exercises the
HVC branch and c7g will be the first SMC user.

## Notes for the next person

- ~~The serial console works on t4g.medium and is blank on c7g.~~ **DISPROVEN —
  the console works fine on c7g.** A live `c7g.large` returns the full boot log
  (775 lines, byte-comparable to `t4g.medium` from the same image). The original
  conclusion came from calling `get-console-output` about 90 seconds after launch
  and giving up. **Pass `--latest` and allow several minutes.**
  For the record, the captured device *is* a PCI 16550 (`1d0f:8250` at
  `0000:00:01.0`) and there is no PL011 anywhere — but the SPCR alias at
  `0x090a0000` reaches that same console, verified by booting with it as the only
  console path, and there is **no DBG2 table** on any Graviton instance. Do not
  chase the BAR: it moves with device population (t4g `0x80008000`, c7g.large
  `0x80048000`, metal `0xe2f00000`) and is reassignable mid-boot, whereas the SPCR
  alias is fixed. See `arm64-serial-console-c7g.md`.
- launch_daemon prints only on *failure* (`Launching %s failed: %s`), so the
  absence of an sshd line in a console log means the script ran, not that it
  didn't.
- A failing launch_daemon job recursively aborts *and deletes* everything that
  depends on it (`JobQueue::_RemoveDependantJobsOf`). Not what happened here --
  our jobs are top-level with no target -- but worth knowing.
- `sshd_config` line 53 sets `PrintLastLog`, which this OpenSSH build rejects:
  `Unsupported option PrintLastLog`. Harmless, but it is noise on every start.
- There is no `sync` in the Haiku source tree (`src/bin` has none, and `minimum`
  does not list one); `/boot/system/bin/sync` on these images comes from the
  haikuports `coreutils` package. That is why `sshd_boot.sh` tolerates its
  absence.
- `/boot/system/var/log/syslog` is written by syslog_daemon with plain `write()`
  and no `fsync`, so after a hard stop its tail is whatever the cache happened to
  have flushed -- which, before fix (1), was nothing. Do not plan a forensic
  approach around it.

## What to bake and test

Branch `fix/haiku-ec2-stop-start` (three commits on top of `graviton`).

`sshd_boot.sh` ships *inside* the OpenSSH hpkg
(`build-openssh-arm64.sh` line 168 installs it to `bin/sshd_boot.sh`), so fix (2)
needs `graviton/ssh/build-openssh-arm64.sh` re-run -- it is not picked up by a
plain `jam` re-run.

1. **The regression test that matters:** boot, confirm sshd, `stop`, `start`,
   confirm sshd again. That currently fails 100% of the time and should now pass.
2. **Clean shutdown:** watch how long `stop` takes. Before, the instance always
   burned the full 3-4 minute grace period and was cut. If the PL061 path works
   it should stop in seconds. `get-console-output` on a t4g should show the
   `pl061:` lines at boot:
   ```
   pl061: pin 3 -> _E03 (edge triggered, active high)
   pl061: \_SB_.GPO0: 2 event pin(s) armed on irq 39
   ```
   and `/dev/power/button/power` should now exist.
3. **Test on both a c7g.large and a c7g.metal**, because the pin, method name and
   interrupt differ between them and the discovery code is exactly what that
   table above is testing. A t4g.medium as well, for the boot log.
4. Watch for the trap the code guards against: if the box shuts *itself* down a
   few seconds after boot, a latched pin is firing and the mask/clear ordering in
   `pl061_init_driver` is wrong for that firmware.

Also worth a check, since fix (1) changes global write-back behaviour: build
throughput on the metal builder should be unchanged or better, and `df` should
stop lying about free space after large writes.

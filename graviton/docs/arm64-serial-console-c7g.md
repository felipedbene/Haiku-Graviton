# arm64 serial console on c7g: which UART carries `get-console-output`

Investigation of the report *"`aws ec2 get-console-output` returns Haiku's boot log
on t4g but comes back completely empty on c7g"*.

**Headline: the symptom does not reproduce, and the UART selection is not at
fault.** The current canonical image produces a complete, essentially identical
boot log on `c7g.large`, `c7g.4xlarge` and `t4g.medium`. The thing that actually
returns an empty log is calling `get-console-output` **without `--latest`** — and
for a Haiku node that is not a transient condition, it is permanent. Details in
[The real blind spot](#the-real-blind-spot).

Everything below is measured on real EC2 in us-west-2, not derived.


## Which UART carries the captured console

Two separate facts, each established by experiment rather than by reading tables.

### 1. The captured device is the PCI UART `1d0f:8250`

On a `c7g.large` Linux control instance, a marker string was written one byte at a
time to the THR of PCI function `0000:00:01.0`, mapped through
`/sys/bus/pci/devices/0000:00:01.0/resource0` (no `/dev/mem` on AL2023 arm64).
It came back verbatim from `aws ec2 get-console-output --latest`:

```
>>>>>>>>>   HHHAAAIIIKKKUUU---PPPRRROOOBBBEEE   PPPCCCIII---BBBAAARRR000   WWWRRRIIITTTEEE   ttt444ggg...mmmeeedddiiiuuummm   <<<<<<<<<
```

(Each character is tripled because the write raced with Linux's own 8250 driver
still owning the port; irrelevant to the conclusion.)

This matches the platform description: EC2's console is *"an emulated UART
8250/16550 serial device"* and the console daemon *"caches ttyS0 output"* — i.e.
whatever the guest drives as ttyS0 is what is captured.

`1d0f:8250` is class `0x070003` (16550-compatible). **There is no PL011
anywhere** — Linux loads the AMBA PL011 driver and it binds nothing.

### 2. The SPCR-advertised address `0x090a0000` reaches the *same* captured console

This is the part that was previously assumed to be a problem. It is not.

`0x090a0000` appears nowhere in `/proc/iomem`, no DSDT device claims it, and
Linux abandons it after early boot (`printk: legacy console [ttyS0] disabled`,
then re-enabled on the PCI device). That makes it look like a dead legacy alias.
It is not dead. Booting with the PCI console removed so that `0x090a0000` was the
*only* console path:

```
grubby --update-kernel=ALL --remove-args="quiet console=ttyS0,115200n8 console=tty0"
grubby --update-kernel=ALL --args="earlycon=uart8250,mmio,0x090a0000,115200 keep_bootcon loglevel=7"
```

the **entire** boot log — kernel banner through login prompt — was captured on
**both** `c7g.large` and `t4g.medium`. So `0x090a0000` is a live, byte-strided
16550 aliasing the same console the PCI BAR reaches.

Note `mmio` (not `mmio32`) in that earlycon spec: byte register stride, matching
the SPCR access size. Consistent with the earlier x4-stride fix.


## Firmware tables, as measured

**There is no `DBG2` table on any Graviton instance tested** — virtualized or
bare metal. `SPCR` is the only debug-port table present, so reading DBG2 "instead
of just SPCR" was not the answer; there is nothing there to read.

`SPCR` on `c7g.large` and `t4g.medium` is **byte-for-byte identical**, which by
itself rules SPCR out as the explanation for any t4g/c7g divergence:

| SPCR field | c7g.large / t4g.medium | c7g.metal |
|---|---|---|
| OEM table id | `AMZNSPCR` | `GRVTN003` |
| Interface type | 0 (full 16550) | 0 (full 16550) |
| Base address | `0x090a0000` | `0xe2f00000` |
| GAS bit width / access size | 8 / 1 (byte) → `reg_shift` 0 | 8 / 1 (byte) → `reg_shift` 0 |
| Interrupt type / GSIV | 0x08 (GIC) / 37 | 0x08 (GIC) / 408 |
| Baud enum | 7 (115200) | 7 (115200) |
| Flow control | 2 (RTS/CTS) | 0 (none) |
| PCI vendor:device | `1d0f:8250` | `1d0f:8250` |
| PCI segment/bus/dev/fn | 0 / 0 / 1 / 0 | 1 / 3 / 0 / 0 |
| Clock | 0 → assume 1843200 | 0 → assume 1843200 |

Two things worth keeping in mind:

- **SPCR names the exact PCI BDF of the console.** So if a BAR-based lookup were
  ever needed, no PCI *enumeration* is required — SPCR plus `MCFG` (whose structs
  already exist in `headers/private/kernel/acpi.h`) is enough for a direct ECAM
  config read at a known function. Not needed today; recorded so nobody rebuilds
  that reasoning.
- **The SPCR clock of 0 is handled correctly.** `arch_acpi_get_uart_8250()`
  substitutes 1843200, and 1843200 / 16 = 115200 = the `base_baud` Linux reports.

### The PCI BAR moves, the SPCR address does not

| | UART BAR0 | SPCR base |
|---|---|---|
| `t4g.medium` | `0x80008000` | `0x090a0000` |
| `c7g.large` | `0x80048000` | `0x090a0000` |
| `c7g.metal` | `0xe2f00000` | `0xe2f00000` |

The BAR base is a function of how many devices are populated, not of the instance
family: on `c7g.large` the ENA has three BARs including a 256 KB one at
`0x80000000`, which pushes the UART up to `0x80048000`; on `t4g.medium` the ENA
has a single 16 KB BAR and the UART lands at `0x80008000`. Attaching more ENIs or
volumes would move it again.

**This is a positive argument for keeping the current behaviour.** Haiku's kernel
does enumerate this device — the boot log shows
`vendor 1d0f, device 8250 … base reg 0: host 80048000` — so a BAR-based debug
console is implementable. It would also be *worse*: the BAR is writable and can
be reassigned by Haiku's own PCI bus manager mid-boot, whereas the SPCR alias is
a fixed platform address that cannot move. On bare metal SPCR points at the BAR
anyway, so SPCR is the strictly better of the two everywhere tested.

**Conclusion: Haiku's existing SPCR-based discovery is correct and portable
across virtualized and bare-metal Graviton. Do not switch it to the PCI BAR.**


## What Haiku actually does today

`arch_handle_acpi()` in `src/system/boot/platform/efi/arch/arm64/arch_acpi.cpp`
prefers SPCR and falls back to DBG2 only when SPCR is absent. It takes the base
address, derives `reg_shift` from the GAS access size, sets `gUARTSkipInit` (the
firmware has already programmed the port), and hands the values to the kernel in
`kernel_args`. `arch_debug_console_init()` rebuilds a `DebugUART8250` from them.

On every instance type tested the loader logs:

```
discovered uart from acpi: base=90a0000, irq=37, clock=0, reg_shift=0
```

which is right, and it works.


## Boot results from one image (`ami-0cff6f999ca5003ff`, hrev59996)

| Instance type | Console output | Result |
|---|---|---|
| `t4g.medium` | 38751 B / 775 lines | full boot to first-login |
| `c7g.large` | 38938 B / 775 lines | full boot to first-login |
| `c7g.4xlarge` | 46981 B | full boot to first-login |
| `c7g.metal` | 42040 B | kernel log fine; **loader log missing**; ~~panics in GICv3~~ — **STALE, see note below** |

> **Table row corrected 2026-08-24.** `c7g.metal` **no longer panics in GICv3, and it
> now boots to userland** — 53 PCI devices, NVMe root mounted. Two fixes closed it:
> arm64 GICv3 redistributor discovery (verified across multiple metal redistributor
> layouts) and the PCI ECAM multi-region fix (`f5367b3602`, merged via `e270548f33`).
> See `metal-gicv3-panic.md` and `metal-pci-segment.md`. The byte count and the
> missing-loader-log observation are still accurate; the panic is not.

With timestamps stripped, the `c7g.large` and `t4g.medium` logs differ only in
values that *must* differ: heap/object pointers, EBS volume ids, PCI BAR
addresses, RAM size, `PMUVer` 5 vs 4, generic timer 1050000000 vs 121875000 Hz,
and the RTC seconds. **No content is missing on c7g and nothing is garbled.**
Neither log is near the 65535 B API cap.


## The real blind spot

`get-console-output` **without `--latest` returns nothing** for these nodes:

| | `--latest` | default |
|---|---|---|
| Haiku `c7g.large` (fresh launch) | 38938 B | **1 B (empty)** |
| Haiku `t4g.medium` (fresh launch) | 38751 B | **1 B (empty)** |
| Linux `c7g.large` (after reboot) | 63736 B | 63736 B |
| Linux `t4g.medium` (after reboot) | 65538 B | 65536 B |

The default form returns only the 64 KB following an instance *lifecycle event*
(stop/start/reboot); `--latest` returns the live buffer. A freshly launched
instance has no such event, so the default form is empty.

For Haiku this is permanent, not transient, because of two existing constraints:
a Haiku node **must never be stopped** (it does not survive stop/start), and
~~Haiku's arm64 ACPI reboot is still a no-op inbound.~~ **Corrected 2026-08-24: an
inbound path is now IMPLEMENTED and merged** — `e6c9102f8c` receives the platform
power button through the **PL061 GPIO** (not the GED; a QEMU proxy misleadingly
suggested GED and diverged from EC2 here). Register base, interrupt and pin numbers
are all read from the ACPI namespace because they differ between instance types of
one family.

**How far this is verified, stated precisely:** nodes **can** now be stopped and
started — `128a3f1761` makes the hardware gate stop the instance, wait for
`stopped`, start it and require sshd to answer, and that gate passes. Whether the
**PL061 event itself fires** (a graceful ACPI shutdown) as opposed to EC2 falling
back to a forced stop is **UNVERIFIED as of 2026-08-24** from the evidence I could
find: the commit implementing it claims no hardware confirmation, and reaching the
`stopped` state does not by itself distinguish the two. The wall-clock time of the
stop would distinguish them; I did not find that recorded.

As written: so a Haiku node never
generates a lifecycle event, and the non-`--latest` form will *never* return
anything for one — while any Linux node that has been rebooted once will happily
return output either way. That asymmetry is a very good match for
"works on t4g, empty on c7g" arising from how the two were queried rather than
from what they were running.

**Always pass `--latest`.** This is the actual fix for the reported blind spot and
it needs no code change.

> **The advice above is still right; its stated *reason* no longer holds (2026-08-24).**
> The argument ran: a Haiku node never generates a lifecycle event, therefore the
> non-`--latest` form never returns anything. That premise is gone — nodes are now
> stopped and started routinely, because the hardware perf gate cycles the instance
> on every run. **Keep passing `--latest`** (it is free and it removes a whole class
> of empty-result confusion); just do not rely on the "no lifecycle event" reasoning,
> and above all **do not conclude from this section that a Haiku node cannot be
> stopped** — it can, and there is a pipeline stage that depends on it.


## Secondary finding: no loader output on c7g.metal

On `c7g.metal` the console jumps straight from the EDK2 banner to the kernel's
first line; every loader `dprintf` is missing, including `acpi_init:` and
`discovered uart from acpi:`. The kernel's output on the same port is fine, so
the SPCR address (`0xe2f00000`) is good — it is the loader's plumbing that fails.

The loader prefers **EFI `serial_io`** and only falls back to `gUART`, and
`serial_putc()` latches `gUART = NULL` permanently on a single `PutChar`
timeout. On virtualized instances `serial_io` exists and works, which is why
loader output appears there. On metal it evidently does not carry to the captured
console, and the `gUART` fallback is one failed character away from being
switched off for the rest of the boot.

This matters because that missing loader log is exactly what you would want in
order to debug ~~the GICv3 redistributor panic metal currently dies in~~ **a metal
boot problem** — but note (2026-08-24) that **metal no longer dies in a GICv3 panic**;
that specific panic is fixed and metal reaches userland. The loader-log gap remains a
real diagnostic gap for whatever comes next.

### Change made

`src/system/boot/platform/efi/serial.cpp`: once firmware has told us where the
console UART is, prefer that port over EFI `serial_io`, and on a `PutChar`
failure fall back to `serial_io` for subsequent characters instead of going
silent.

This **discovers, it does not hardcode** — it changes only *which of two already
discovered sinks is preferred*, and adds no addresses. It is a no-op on x86
(`gUART` is left NULL there whenever `serial_io` is present) and a no-op on the
early loader path on arm64, because `serial_init()` runs before `acpi_init()` and
so `gUART` is still NULL for the first few lines.


## Is QEMU a valid test bed for this?

**No.** QEMU `virt` presents a **PL011** and Haiku finds it via the device tree;
EC2 presents a **16550 on PCI** with no PL011 at all and no device tree. The two
exercise different drivers via different discovery paths, so a QEMU pass says
nothing about EC2 here. This is the same divergence class already recorded for
the GED vs PL061 power button and the PL031 RTC. Validate on real EC2.


## Reproducing the measurements

Linux control instances (AL2023 arm64) need the SSM instance profile
`AWSSupportPatchwork-SSMRoleForInstances` and security group
`sg-008114891fd207df1`. `/dev/mem` is not available, so use
`/sys/bus/pci/devices/*/resource0` for BAR access and `grubby` + `earlycon` to
test an arbitrary MMIO address.

```bash
# tables, PCI resources, ttyS0 binding
ls /sys/firmware/acpi/tables/            # note: no DBG2 on Graviton
hexdump -C /sys/firmware/acpi/tables/SPCR
cat /proc/tty/driver/serial
dmesg | grep -iE 'SPCR|ttyS|16550|pl011'
```

Note `graviton/scripts/ssm-run` takes the **instance id first**; any option
before it is consumed as the instance id and the script then reads stdin and
hangs.

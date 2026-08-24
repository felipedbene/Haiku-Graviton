# `mimeset` is a silent no-op on a headless Haiku — and it is not alone

**Status 2026-08-24.** The defect is understood and the `mimeset` fix is **built for arm64**
but **NOT yet deployed or runtime-verified**. This document is a checkpoint written ahead of
a session restart; read the "What is measured / inferred / untested" table before acting on
anything here.

Branch: `fix/mimeset-headless`. Fix commit: `3eb457b35e`.

---

## 1. The defect

`src/kits/app/Application.cpp`, end of `BApplication::_InitData()`:

```
	if (_error != NULL) {
		*_error = fInitError;
	} else if (fInitError != B_OK) {
		DBG(OUT("BApplication::InitData() failed: %s\n", strerror(fInitError)));
		exit(0);
	}
```

`exit(0)` — a **success** status on an init failure. On a headless system
`_InitGUIContext()` → `_ConnectToServer()` fails (no app_server), so `fInitError != B_OK`,
so **any program that constructs a `BApplication` without passing the trailing
`status_t*` terminates in the constructor, reporting success.**

For `mimeset` the construction is at `src/bin/mimeset.cpp:232` and the file loop is at
`:234`. It therefore never looks at a file. `HaikuPorter/Package.py:236-242` runs
`mimeset --all` under `check_call`, sees rc 0, and moves on. **145 of 262 packages in the
pool have zero `BEOS:` attributes** — no MIME types, and no resource→attribute icon copy.

Every QEMU build guest in this fleet is headless (`-display none`, no GPU), and
[[graviton-has-no-video-device]] establishes there is no framebuffer on Graviton at all, so
this fires on every guest, every build, always.

### Why `mimeset` does not need the GUI (all four points read from source)

1. **`be_app` is assigned *before* the GUI init.** In `_InitData()`, `be_app = this` and
   `be_app_messenger` are set inside the `if (fInitError == B_OK)` block, and
   `_InitGUIContext()` is called *at the end of that same block*. So on a headless failure
   the application object and its **registrar registration are already complete**; only the
   GUI context is missing. This is the load-bearing fact and it is what makes the cheap fix
   correct rather than merely non-crashing.
2. **The storage kit contains no `be_app` and no `AppServerLink` reference at all**
   (`grep -rn 'be_app\|AppServerLink' src/kits/storage/` → zero hits), and `src/kits/storage/mime/`
   likewise has zero `be_app` hits.
3. **Every `BBitmap` on mimeset's path is `B_BITMAP_NO_SERVER_LINK`** —
   `AppMetaMimeCreator.cpp:126,135`, `MimeInfoUpdater.cpp:194,204`,
   `database_support.cpp:194`. That flag exists precisely to build a bitmap with no
   app_server connection.
4. **`~BApplication()` now runs where it previously could not**, and is safe:
   `fServerLink` is unconditionally initialized to `PortLink(-1, -1)` in `_InitData()`, so
   the destructor's `AppServerLink` flush and its `delete_port(fServerLink->ReceiverPort())`
   both act on port `-1`, return `B_BAD_PORT_ID`, and the result is discarded
   (`LinkSender::Flush()` returns the `write_port` error; nothing dereferences a null).
   `be_app` is non-NULL, so the `if (be_app)` branch taken is the already-exercised one.

## 2. The fix

`BApplication(signature, status_t*)` instead of `BApplication(signature)`, plus a one-line
stderr warning. The full rationale is in the commit message of `3eb457b35e`.

**Why this cannot regress the app_server-present case** (which cannot be tested here —
nothing in this fleet runs app_server, so this is reasoning, not measurement): the two
constructors differ *only* in the trailing branch of `_InitData()`, and that branch is
reached only when `fInitError != B_OK`. When app_server is present the init succeeds, so
`*_error` is set to `B_OK`, the warning does not print, and control flow is byte-identical.
If the **registrar** is also unavailable, the per-file `update_mime_info()` calls fail, are
reported to stderr, and `exit(1)` follows — strictly better than the old `exit(0)`.

The change is deliberately at the call site, not in `Application.cpp`. Changing `exit(0)`
semantics in `Application.cpp` would be correct in the abstract but has a blast radius of
every GUI application in the tree; see §3 for why that is a separate, larger decision.

## 3. Blast radius — Task 2

### First, the scoping fact that bounds all of this

**The build-platform copies of these tools are immune.** `build/jam/BuildSetup:354` sets
`HOST_LIBBE = libbe_build.so`, and `src/build/libbe/app/Application.cpp:9` is an **empty
stub constructor**. So `<build>mimeset`, `<build>xres`, `<build>copyattr`, `<build>addattr`,
`<build>catattr`, `<build>listattr`, `<build>keymap`, `<build>unzip` (all in
`src/tools/Jamfile`) never trip this, and everything `jam` invokes via
`build/scripts/build_haiku_package` and `build/jam/BeOSRules` is safe. **MEASURED.**

That means the blast radius is exactly the **target** binaries — i.e. what haikuporter runs
*inside a Haiku chroot on the headless instance*. Which is precisely our situation, and
also why the Haiku image build itself has never shown this.

### Counts (MEASURED)

- **161** `BApplication` construction sites in non-test code.
- **1** safe: `src/tools/cppunit/TestApp.cpp:41`. **Zero** sites anywhere in the tree pass
  `initGUI=false` via a `BApplication` constructor directly (the 5-arg `initGUI` form is
  **private**, friend-only to `BApplication::Private` and `BServer` — so it is not available
  to a CLI tool without going through `BServer`).
- **160** vulnerable.
- **29** in class (b) — 21 command-line tools + 8 non-GUI daemons.
- In `src/bin/` specifically, the number of tools passing a `status_t*` was **ZERO** before
  this change. `mimeset` is now the first.
- Separately, **13** `BServer`-derived daemons: **9 safe** (pass `&error`: registrar,
  launch_daemon, package_daemon, net_server, debug_server, notification_server, midi_server,
  media_server, print_server); **4** pass `NULL` — `syslog_daemon/SyslogDaemon.cpp:36`,
  `power/power_daemon.cpp:47`, `mount/AutoMounter.cpp:374` (all `initGUI=false`, so only a
  registrar-side failure reaches `exit(0)`), and `mail/MailDaemonApplication.cpp:157`
  (`initGUI=true` → fully exposed to the app_server path).

### Class (b) — does NOT need a GUI. Killed for no reason. **These are the real defects.**

Line numbers and construction position are MEASURED; the "what it needs" column is INFERRED
from the includes and calls in each file.

| Tool | Site | Position | What it actually needs |
|---|---|---|---|
| `mimeset` | `src/bin/mimeset.cpp:232` | before loop (234) | registrar MIME db — **FIXED on this branch**. With `--mimedb` it needs *nothing* (purely local `Mime::Database`) |
| **`setmime`** | `src/bin/setmime.cpp:1154` | first stmt of `main()` | registrar MIME db. **MIXED case — see below** |
| `version` | `src/bin/version.cpp:125` | first stmt | **nothing at all** — only `BFile` + `BAppFileInfo` |
| `hey` | `src/bin/hey.cpp:268` | first stmt | `be_roster` + `BMessenger` scripting (registrar) |
| `keymap` (CLI) | `src/bin/keymap/main.cpp:155` | before the mode switch (158) | **nothing** for `-c`/compile and save-source-from-file; only `SetToCurrent`/`SaveAsCurrent`/`RestoreSystemDefault` want app_server |
| `clipboard` | `src/bin/clipboard.cpp:51` (inst. 340) | before work | registrar only — Haiku's clipboard is the registrar's `ClipboardHandler` |
| `notify` | `src/bin/notify.cpp:74` (inst. 287) | total no-op | notification_server. **`app.InitCheck()` at 288 is dead code** |
| `waitfor` | `src/bin/waitfor.cpp:62` (inst. 209) | total no-op | registrar + net_server. **Reports success *instantly* instead of waiting** — a silently broken synchronisation primitive |
| `mountvolume` | `src/bin/mountvolume.cpp:333` (inst. 580) | total no-op | `BDiskDeviceRoster` (kernel); the looper exists only for `ArgvReceived` |
| `keystore` (CLI) | `src/bin/keystore/keystore.cpp:330` | first stmt | keystore_server |
| `setvolume` | `src/bin/setvolume.cpp:38` | first stmt | media_server |
| `installsound` | `src/bin/installsound.cpp:44` | total no-op | media_server |
| `media_client` | `src/bin/media_client/media_client.cpp:30` | total no-op | media_server |
| `cddb_lookup` | `src/bin/cddb_lookup/cddb_lookup.cpp:76` (inst. 396) | total no-op | network + volume attributes |
| `mail` | `src/bin/mail_utils/mail.cpp:25` | first stmt | mail_daemon |
| `mail2mbox` | `src/bin/mail_utils/mail2mbox.cpp:302` | first stmt | **nothing** — file + attribute I/O only |
| `mbox2mail` | `src/bin/mail_utils/mbox2mail.cpp:509` | total no-op | **nothing** — file + attribute I/O only |
| `spamdbm` | `src/bin/mail_utils/spamdbm.cpp:2416` (inst. 7839) | total no-op | `BResources` + settings files. Has an explicit `g_CommandLineMode` (7828); **`InitCheck()` at 7841 is dead code** |
| `urlwrapper` | `src/bin/urlwrapper.cpp:42` (inst. 614) | total no-op | `be_roster` launch; `if (be_app)` at 615 unreachable |
| `checkitout` | `src/bin/checkitout.cpp:40` (inst. 245) | total no-op | `be_roster` launch |
| `netfs_server_prefs` | `src/add-ons/kernel/file_systems/netfs/netfs_server_prefs/NetFSServerPrefs.cpp:414` | first stmt | settings-file I/O + a port |

### Class (b) daemons — same defect, no GUI need

- `src/add-ons/kernel/file_systems/userlandfs/server/UserlandFSServer.cpp:39` — **this
  silently kills userlandfs mounts headless.**
- `src/servers/keystore/KeyStoreServer.cpp:62`
- `src/servers/index/IndexServer.cpp:78`
- `src/servers/bluetooth/BluetoothServer.cpp:63`
- `src/servers/media_addon/MediaAddonServer.cpp:206`
- `src/add-ons/kernel/file_systems/netfs/server/NetFSServer.cpp:191`
- `src/add-ons/kernel/file_systems/netfs/authentication_server/AuthenticationServer.cpp:260`
- `src/servers/mail/MailDaemonApplication.cpp:157` — `BServer(sig, true, NULL)`: the **only**
  `BServer` subclass passing `initGUI=true` *and* a NULL error.

> **`mountvolume` + `userlandfs_server` together mean headless volume management is broken
> end to end**, not just MIME tagging. That is plausibly relevant to the chroot/harvest
> loops in the builder scripts and is worth a look independently of this fix.

### Class (a) — genuinely needs a GUI. Exiting is defensible; **exiting 0 still is not.**

In `src/bin`: `WindowShade.cpp:134`, `alert.cpp:56`, `desklink/desklink.cpp:39`,
`dpms.cpp:23`, `draggers.cpp:28`, `dstcheck.cpp:142`, `ffm.cpp:16`, `filepanel.cpp:47`,
`listfont.cpp:82`, `screen_blanker/ScreenBlanker.cpp:43`, `screeninfo.cpp:92`,
`screenmode/screenmode.cpp:248`, `setcontrollook.cpp:28`, `setdecor.cpp:83`,
`translate.cpp:488`, `network/ppp_up/PPPUpApplication.cpp:30`.

Bulk of (a): all of `src/preferences/**` (25 sites), `src/apps/**` (~55), the ~20 standalone
translator `main()`s in `src/add-ons/translators/**`, `src/kits/tracker/Tracker.cpp:246`,
`src/servers/input/InputServer.cpp:144`,
`src/servers/print_addon/PrintAddOnServerApplication.cpp:16`, `src/libs/glut/glutInit.cpp:220`,
`src/tools/translation/inspector/InspectorApp.cpp:48`,
`src/add-ons/tracker/zipomatic/ZipOMatic.cpp:37`.

Worth flagging inside (a): **`dstcheck.cpp:142`** constructs the app only in the
DST-changed branch, so `exit(0)` also skips the `write()` of `time_dststatus` at the end of
`main()` — it re-prompts every boot forever.

### Critical path (haikuporter) — checked individually

| Tool | Verdict |
|---|---|
| `mimeset` | **VULNERABLE** — `src/bin/mimeset.cpp:232`. Fixed here. |
| **`setmime`** | **VULNERABLE — `src/bin/setmime.cpp:1154`, and it is a MIXED case.** See below. |
| `xres` | **SAFE** — `src/bin/xres.cpp` has no `BApplication` at all. |
| `catattr`, `listattr`, `copyattr`, `addattr`, `rmattr`, `mvattr`, `resattr` | **SAFE** — no `BApplication` in any of them. |
| `rc` / rdef compiler | **SAFE** — no `BApplication`. |
| `package`, `package_repo` | **SAFE** — no `BApplication`; the hpkg kit is app-kit-free. |
| `unzip` | **latent only** — `src/bin/unzip/beosmain.cpp:36` is compiled solely into `<build>libunzip.a` against the stub libbe; `src/bin/unzip/Jamfile` is empty. Target `unzip` comes from a port. |
| all `<build>` host tools | **SAFE** — stub `libbe_build` constructor, see the scoping note above. |

**`setmime` is the second critical-path casualty and it is not a one-liner.** Its
`BApplication` sits at the very top of `main()` with the comment *"AppServer link is
required to work with bitmaps"*, and unlike the mime kit its bitmaps are
`new BBitmap(rect, B_COLOR_8_BIT)` at `setmime.cpp:763,768,854,860` — **without**
`B_BITMAP_NO_SERVER_LINK`. So the comment is accurate: its `-mimeicon`/`-appicon` paths
genuinely need app_server, while its type / extension / sniffer-rule / description /
preferred-app paths do not. Fixing it properly means either adding
`B_BITMAP_NO_SERVER_LINK` to those two allocations or failing only on the icon paths — a
real change, not a constructor swap. **Not attempted here.**

### The wider question this raises — and the evidence that settles it

The per-call-site fix is right for *this task*, but the honest reading is that `exit(0)` in
`_InitData()` is the actual bug, inherited by 160 sites.

**Three pieces of dead code are direct evidence of that**, not merely an argument for it:
`notify.cpp:288` and `spamdbm.cpp:7841` both call `InitCheck()` immediately after
constructing the app, and `urlwrapper.cpp:615` / `checkitout.cpp:246` both test
`if (be_app)`. **All four are unreachable on failure** — `exit(0)` in the base constructor
preempts every one of them. Four separate authors wrote failure handling that the base class
silently deletes. That is a defect in `Application.cpp`, not in four call sites.

A tree-wide repair (set `*_error`/return, or at absolute minimum `exit(1)` so that callers
checking status codes stop being lied to) is the better fix, and changing `exit(0)` →
`exit(1)` alone would be a one-line change that fixes the *reporting* half for all 160 sites
without changing any control flow. **That is a decision for the owner, not something to slip
into a mimeset fix** — but it is a cheap and high-value one, and it is the recommendation
this investigation ends on.

## 4. What is measured / inferred / untested

| Claim | Status |
|---|---|
| `exit(0)` on init failure at `Application.cpp:533-539` | **MEASURED** (read in source, this tree) |
| `mimeset`'s `BApplication` precedes its file loop (232 vs 234) | **MEASURED** |
| `be_app` is set before `_InitGUIContext()` | **MEASURED** (read `_InitData()`) |
| No `be_app`/`AppServerLink` in `src/kits/storage/` | **MEASURED** (grep, zero hits) |
| All mime-path bitmaps are `B_BITMAP_NO_SERVER_LINK` | **MEASURED** (grep + read) |
| `~BApplication()` is safe with a failed GUI init | **INFERRED** from reading `LinkSender::Flush()` and the `PortLink(-1,-1)` init. Not executed. |
| Zero `status_t*` uses in `src/bin/` before this change | **MEASURED** (grep) |
| 161 sites / 160 vulnerable / 29 class (b) | **MEASURED** (tree-wide sweep) |
| Host `<build>` tools immune via the `libbe_build` stub ctor | **MEASURED** (`BuildSetup:354`, `src/build/libbe/app/Application.cpp:9`) |
| The 5-arg `initGUI` ctor is private (friend-only) | **MEASURED** (`headers/os/app/Application.h:108`) |
| Four dead `InitCheck()`/`if (be_app)` guards | **MEASURED** (line numbers read) |
| Blast-radius GUI/non-GUI classification | **INFERRED** per tool from its includes and calls |
| `setmime` bitmaps lack `B_BITMAP_NO_SERVER_LINK` | **MEASURED** (`setmime.cpp:763,768,854,860`) |
| Fixed `mimeset` cross-builds for arm64 | **MEASURED** — `jam -q mimeset` rc=0, binary differs from stock, and the new warning string is present in the fixed binary and absent from the stock one (the artifact announces itself) |
| **Fixed `mimeset` actually writes `BEOS:TYPE` on a guest** | **UNTESTED — NOT DEPLOYED.** This is the gap. |
| **One-package end-to-end (`vim`) rebuild carrying `BEOS:` attrs** | **UNTESTED — not started.** |
| Remediation cost for the 145 packages | **NOT ESTIMATED — not started.** |

The prior session's report that a compiled probe calling
`update_mime_info(path, true, true, FORCE_UPDATE_ALL)` with **no** `BApplication` returned
`B_OK` and wrote `BEOS:TYPE` is **inherited, not re-verified here.** It is consistent with
everything measured above.

## 5. Build artifacts left on the metal

On `i-0f7f6f3e8922acffd` (c7g.metal builder), in `/opt/haiku/mimeset-fix/`:

- `mimeset.FIXED` — arm64 binary with the fix, 284135 B
- `mimeset.ORIG` — the stock arm64 binary for A/B, 284063 B
- `mimeset.patch` — the one-file diff, applied and reverted

It was built by borrowing `/opt/haiku/haiku-opt` (the only tree with a prebuilt
`libbe.so` + `mimeset`), which was **restored afterwards**: `src/bin/mimeset.cpp` is back
to blob `263bdefe32c8bdeee076143ed3338886a8fc5d83` and the stock binary relinked, leaving
only that tree's two pre-existing dirty files (`build/jam/ArchitectureRules`,
`src/add-ons/kernel/file_systems/btrfs/Jamfile`). **Guest 2235 was never touched.**

Note the source blob for `src/bin/mimeset.cpp` is identical (`263bdefe32…`) across
`graviton@8e365e576d` and every tree on the metal, so the binary built in `haiku-opt`
corresponds exactly to the committed source.

## 6. Next steps for whoever picks this up

1. `scp` `/opt/haiku/mimeset-fix/mimeset.FIXED` into guest **2235** (port-forward; see
   [[metal-builder-guest-topology]]) — `ssh -p 2235 -i /home/ubuntu/.ssh/haiku-ed25519 baron@127.0.0.1`,
   run as `ubuntu` on the metal.
2. **Positive control first.** Run `catattr BEOS:TYPE <file known to have it>` and show a
   populated row *before* interpreting any empty result. An empty `catattr` proves nothing
   on its own; this project has lost three sessions to zero-row filters read as absence
   (see [[verification-discipline-five-rules]]).
3. Then A/B on a file with no `BEOS:TYPE`: stock `mimeset` → still empty, rc 0;
   `mimeset.FIXED` → populated `BEOS:TYPE`. Expect the stderr warning to appear — its
   absence would mean the binary did not run.
4. One package end-to-end (`vim` is a good pick: it is one of the 145, and its GUI cut was
   priced on this false cause).
5. **Then stop and price** (a) unpack→mimeset→repack vs (b) rebuild. Do not remediate 145
   packages; the choice is the owner's.

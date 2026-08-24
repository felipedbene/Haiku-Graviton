# `mimeset` is a silent no-op on a headless Haiku — and it is not alone

**Status 2026-08-24 (updated).** The defect is understood, the `mimeset` fix is built for
arm64, and it is now **runtime-verified on guest 2235** — including one package end-to-end
(`vim`: **0 of 2454 files → 2454 of 2454** carrying a `BEOS:` attribute). Remediation path
(a) is priced and proven to round-trip. What remains is a single decision that is the
owner's, not an agent's: see section 7.

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
| **Fixed `mimeset` actually writes `BEOS:TYPE` on a guest** | **MEASURED** — see section 6. Stock: rc 0, zero attributes. Fixed: rc 0, `BEOS:TYPE` on every file. |
| **One-package end-to-end (`vim`) carrying `BEOS:` attrs** | **MEASURED** — 0/2454 → 2454/2454, via haikuporter's exact argv. |
| Remediation cost for the 145 packages | **MEASURED** for path (a) — ~9.5 s for `vim`; see section 7. |
| Headless type assignments match the working reference | **MEASURED** — controlled against host-built `mimeset` output; see section 6.3. |
| Attributes survive `package create` | **MEASURED** — repack → re-extract → 2454/2454. |

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
`src/add-ons/kernel/file_systems/btrfs/Jamfile`).

Guest 2235 has since been used for the verification in section 6. Nothing in its
`/boot/system` was modified — packagefs is read-only and the binaries were run from
`/boot/home`. The scratch dirs it left are `/boot/home/{mstest,vimcheck,pricea,mdb-none,mdb-sys,ctl}`
plus `/boot/home/mimeset.{ORIG,FIXED}`; all are disposable.

Note the source blob for `src/bin/mimeset.cpp` is identical (`263bdefe32…`) across
`graviton@8e365e576d` and every tree on the metal, so the binary built in `haiku-opt`
corresponds exactly to the committed source.

## 6. Runtime verification on guest 2235 (MEASURED)

Guest 2235 is `Haiku shredder R1~beta6+development hrev59996 arm64`. `ps` confirms the
predicted configuration exactly: **`registrar` is running, `app_server` is not.**

### 6.1 Positive control, then A/B

The control ran first, because an empty `catattr` proves nothing on its own
([[verification-discipline-five-rules]]): `/boot/system/bin/bash` returns
`'MIMS' : text/plain`, so `catattr` demonstrably prints a populated row on this system.

Four fresh files on the writable BFS volume, `0 bytes total in attributes` each:

| | `plain.txt` | `page.html` | `script.sh` | `code.c` |
|---|---|---|---|---|
| baseline | — | — | — | — |
| stock `mimeset --all .`, **rc 0** | — | — | — | — |
| fixed `mimeset --all .`, **rc 0** | `text/plain` | `text/html` | `text/plain` | `text/plain` |

The stock binary is the silent no-op at runtime, confirmed: **exit status 0 and not one
attribute written.** The fixed binary emits
`mimeset.FIXED: warning: application init failed (Bad port ID); continuing without it.`
on stderr — the artifact announces itself, so a silent run would have meant the wrong
binary executed.

`Bad port ID` is the concrete failure behind `fInitError`: no `app_server` port to find.

### 6.2 One package end-to-end — `vim`

`vim-9.1.1618-1-arm64.hpkg` (14.5 MB, 2454 files) is one of the 145. Extracted, then
re-run through **haikuporter's exact argv** (`Package.py:238-242`,
`mimeset --all --mimedb data/mime_db --mimedb /boot/system/data/mime_db .`):

```
SHIPPED:      0 of 2454 files carry a BEOS: attribute
AFTER-FIXED:  2454 of 2454 files carry a BEOS: attribute
```

`bin/vim` and its seven siblings (`ex`, `rview`, `rvim`, `vi`, `view`, `vimdiff`, `xxd`)
are typed `application/x-vnd.be-elfexecutable` — the type Tracker and Deskbar need. This
is the direct, positive confirmation that **vim's GUI cut was priced on a false cause**:
its `xres` + `mimeset` + `catattr BEOS:ICON` chain was correct all along, and `mimeset`
simply never ran.

`data/mime_db` came out empty for `vim`, so haikuporter's `rmdir` branch is the one that
applies; `vim` declares no new MIME types.

### 6.3 Control: are the headless type assignments *correct*?

Worth asking, because a remediation that stamps 145 packages with **wrong** types is worse
than leaving them empty. `code.c` → `text/plain` looked wrong: `c` **is** in
`text/x-source-code`'s `META:EXTENS` list, and shipped `.h` files carry
`text/x-source-code`.

Two things resolve it, and the concern does not survive:

1. `--mimedb` makes **no difference** to any assignment (A/B run, identical output with and
   without both flags). So the earlier no-`--mimedb` run was not the confound.
2. What actually decides is the **sniffer rule**, not the extension.
   `text/x-source-code`'s rule is
   `0.30 ([0]"//" | [0]"/*" | [0:32]"#include" | [0:32]"#ifndef" | [0:32]"#ifdef" | [0]"SUMMARY=")`.
   `hdr.h` (`#ifndef H`) matches it and is typed `text/x-source-code`. `code.c`
   (`int main(){return 0;}`) matches nothing and falls back to `text/plain`.

The decisive control is a shipped file whose type could only have come from its extension.
**Shipped `.py` files under `/boot/system/lib/python3.10/` carry `text/plain`** — and those
were typed at image-build time by the *working* host-built `mimeset`, even though `py` is
in the extension list. So extension lookup is subordinate to sniffing in the reference
implementation too.

**Our headless output agrees with the working reference. There is no type-quality
regression** — the fix restores exactly the behaviour the image build already gets. The
extension-vs-sniffer precedence is a pre-existing upstream trait, out of scope here.

> One measurement in this area was confounded and is discarded: re-typing a *copy* of a
> shipped `.c` file. Haiku's `cp` preserves attributes, so the copy arrived already typed
> and `mimeset` had nothing to do. The `.py` control above is unconfounded and sufficient.

## 7. Remediation — path (a) is priced and proven; the choice is yours

The pool is **262 packages, 370.7 MiB**, of which 145 have no `BEOS:` attributes.

### Path (a) unpack → mimeset → repack — MEASURED on `vim`, the worst case in the pool

| step | time |
|---|---|
| `package extract -i .PackageInfo` | 2.4 s |
| `package extract` (2454 files) | 2.8 s |
| `mimeset` | 0.4 s |
| `package create -i .PackageInfo` | 3.8 s |
| **total** | **≈ 9.5 s** |

Round-trip integrity is verified, not assumed: repack → re-extract → **2454 of 2454** files
still carry `BEOS:`, and `bin/vim` still reads `application/x-vnd.be-elfexecutable`. Size
grows 14512906 → 14535981 B (**+0.16%**).

`vim` at 14.5 MB in 9.5 s is ≈0.65 s/MB, so the whole 370.7 MiB pool is **single-digit
minutes**, and the 145-package subset less. Faithfulness is good: the *only* haikuporter
step between `mimeset` and `package create` is normalising `data/mime_db` mtimes to
2001-08-18 (`Package.py:249-252`), and that branch is skipped whenever `data/mime_db` is
empty — as it was for `vim`.

**The cost that is not time:** every repacked hpkg gets a new hash
(`13528bae…` → `ec7ea9c3…`) and is no longer the artifact its build produced. That is a
provenance change, and it is the real argument against (a).

### Path (b) rebuild

Correct by construction and keeps provenance intact, but it re-runs the package chain that
took days across six guests — three to four orders of magnitude more expensive than (a) —
and the chain still has open blockers, so a clean 145-package sweep is not currently a
button anyone can press.

### If (a) is chosen, one detail makes it cheap

`Configuration.py:373` resolves the tool with `which("mimeset")`, and the guest's `PATH`
puts `/boot/home/config/non-packaged/bin` **first**, ahead of `/boot/system/bin`. Dropping
the fixed binary there makes every future haikuporter run pick it up with no image rebake —
consistent with [[kernel-module-hotswap-no-bake]]. **Doing that fixes the pipeline going
forward and is independent of whether the existing 145 are remediated at all.**

### Still open, and not an agent's call

1. **(a) vs (b) vs neither** for the existing 145 packages.
2. `exit(0)` → `exit(1)` in `Application.cpp`. One line; stops all 160 sites reporting
   success on failure; changes no control flow. Recommended in the commit message,
   deliberately not done.
3. The 29 class-(b) tools that need no GUI and die anyway — `setmime` is the one on the
   critical path, and it is **not** a constructor swap (its bitmaps at
   `setmime.cpp:763,768,854,860` lack `B_BITMAP_NO_SERVER_LINK`, so its icon paths
   genuinely need `app_server` while its type/extension/sniffer paths do not).

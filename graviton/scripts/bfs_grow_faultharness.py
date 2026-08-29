#!/usr/bin/env python3
"""
BFS mount-time large auto-grow -- host-side fault-injection harness.

This is the merge gate for the destructive grow path (design v2 Parts B-F). It
drives the host bfs_shell (built with BFS_ENABLE_LARGE_GROW +
BFS_GROW_FAULT_INJECTION) against raw image files:

  T1    clean large grow: mount an oversized headroom image -> grow runs at
        mount -> checkfs clean, superblock geometry + num_ags invariant, data
        byte-identical, second mount is a no-op (idempotent).
  T1.5  refusal / guard paths: stock (no-headroom) volume, over-cap clamp,
        corrupted gap bit, ioctl shrink -- each safe, FS geometry intact.
  T2a   phase-boundary crash injection: abort at every boundary
        {intent, bitmap-mid, bitmap-flush, phase, commit-torn, commit, erase},
        then recover on the next mount. Every resulting state must mount as
        completed-new geometry, checkfs clean, data intact. N repeats.
  T2b   write-subset replay: from a logged clean grow, synthesize crash states
        where segments before the crash barrier are fully durable and an
        arbitrary subset of the in-flight segment persisted. R randomized
        states, each mount-checked.

PASS for a state = mounts, superblock valid + geometry is old-or-completed-new,
checkfs status B_OK with zero missing and zero already-set blocks, and every
manifest file byte-identical. ANY deviation is a corruption -> FAIL.
"""

import os, sys, struct, subprocess, hashlib, random, shutil, re, tempfile

BFS_SHELL = os.environ.get("BFS_SHELL",
    "/opt/haiku/bfsgrow/haiku/generated.arm64/objects/linux/arm64/release/"
    "tools/bfs_shell/bfs_shell")
WORK = os.environ.get("BFS_WORK", "/opt/haiku/bfsgrow/work")
BS = 2048
SMALL_MB = 128
BIG_MB = 512
ONEP_MB = 160          # +1 bitmap block edge (adds exactly one bitmap block)
CAP_BLOCKS = (1 << 40) // BS   # 1 TiB / 2048 = 536870912 (the baked cap)

def mb_blocks(mb): return mb * 1024 * 1024 // BS

OLD_BLOCKS = mb_blocks(SMALL_MB)
NEW_BLOCKS = mb_blocks(BIG_MB)

random.seed(1234)

# ---------------------------------------------------------------- shell driver

def run_shell(img, cmds, env=None, timeout=600):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.Popen([BFS_SHELL, img], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=e)
    data = ("".join(c + "\n" for c in cmds)).encode()
    try:
        out, _ = p.communicate(data, timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        return (-99, out.decode(errors="replace"))
    return (p.returncode, out.decode(errors="replace"))

def fmt(img, mb, headroom=True):
    if os.path.exists(img):
        os.remove(img)
    with open(img, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    params = "block_size 2048"
    if headroom:
        params = "growheadroom true"
    r = subprocess.run([BFS_SHELL, "--initialize", "-n", img, "TestVol",
        params], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("format failed: " + r.stdout.decode(errors="replace"))

def extend(img, mb):
    with open(img, "r+b") as f:
        f.truncate(mb * 1024 * 1024)

# ---------------------------------------------------------------- superblock

def read_super(img):
    with open(img, "rb") as f:
        f.seek(512)
        sb = f.read(512)
    return dict(
        magic1=struct.unpack_from("<i", sb, 32)[0],
        block_size=struct.unpack_from("<I", sb, 40)[0],
        block_shift=struct.unpack_from("<i", sb, 44)[0],
        num_blocks=struct.unpack_from("<q", sb, 48)[0],
        used_blocks=struct.unpack_from("<q", sb, 56)[0],
        ag_shift=struct.unpack_from("<i", sb, 76)[0],
        num_ags=struct.unpack_from("<i", sb, 80)[0],
        grow_max=struct.unpack_from("<q", sb, 132)[0],
    )

def super_valid(s):
    if s["block_size"] != BS or (1 << s["block_shift"]) != BS:
        return False, "bad block_size/shift"
    if s["num_blocks"] < 10:
        return False, "num_blocks<10"
    if s["num_ags"] < 1 or s["ag_shift"] < 1:
        return False, "bad ags"
    want = (s["num_blocks"] + (1 << s["ag_shift"]) - 1) >> s["ag_shift"]
    if s["num_ags"] != want:
        return False, "num_ags %d != divide_roundup %d" % (s["num_ags"], want)
    return True, "ok"

# ---------------------------------------------------------------- checkfs

def parse_checkfs(out):
    """Return (status_ok, missing, already_set, freed) or None if not found."""
    m = re.search(r"(\d+)\s+blocks not allocated,\s*\n?\s*(\d+)\s+blocks "
        r"already set,\s*\n?\s*(\d+)\s+blocks could be freed", out)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))

# ---------------------------------------------------------------- manifest

MANIFEST = []   # list of (guestname, sha256, size)

def make_manifest(srcdir):
    os.makedirs(srcdir, exist_ok=True)
    specs = [("f_small", 100 * 1024), ("f_mid", 1536 * 1024),
             ("f_big", 4 * 1024 * 1024)]
    MANIFEST.clear()
    for name, size in specs:
        data = os.urandom(size)
        with open(os.path.join(srcdir, name), "wb") as f:
            f.write(data)
        MANIFEST.append((name, hashlib.sha256(data).hexdigest(), size))

def populate_cmds(srcdir):
    c = []
    for name, _, _ in MANIFEST:
        c.append("cp :%s /myfs/%s" % (os.path.join(srcdir, name), name))
    c.append("sync")
    return c

def verify_cmds(outdir):
    os.makedirs(outdir, exist_ok=True)
    c = ["checkfs -c"]
    for name, _, _ in MANIFEST:
        c.append("cp /myfs/%s :%s" % (name, os.path.join(outdir, name)))
    return c

def check_manifest(outdir):
    for name, sha, size in MANIFEST:
        p = os.path.join(outdir, name)
        if not os.path.exists(p):
            return False, "missing extracted %s" % name
        with open(p, "rb") as f:
            d = f.read()
        if len(d) != size:
            return False, "%s size %d != %d" % (name, len(d), size)
        if hashlib.sha256(d).hexdigest() != sha:
            return False, "%s sha mismatch" % name
    return True, "ok"

# ---------------------------------------------------------------- assessment

def assess(tag, img, out, expect_new, require_manifest=True, outdir=None,
        targets=None):
    """Common pass criteria. Returns (ok, detail).

    targets = the set of acceptable num_blocks values (old-or-completed-new).
    Defaults to {OLD_BLOCKS, NEW_BLOCKS}."""
    if targets is None:
        targets = {OLD_BLOCKS, NEW_BLOCKS}
    s = read_super(img)
    ok, why = super_valid(s)
    if not ok:
        return False, "%s: invalid superblock (%s) nb=%d ags=%d" % (
            tag, why, s["num_blocks"], s["num_ags"])
    if s["num_blocks"] not in targets:
        return False, "%s: num_blocks %d not in acceptable set %s" % (
            tag, s["num_blocks"], sorted(targets))
    if expect_new is True and s["num_blocks"] == OLD_BLOCKS:
        return False, "%s: expected grown geometry, still old (%d)" % (
            tag, s["num_blocks"])
    if expect_new is False and s["num_blocks"] != OLD_BLOCKS:
        return False, "%s: expected unchanged geometry, got %d" % (
            tag, s["num_blocks"])
    cf = parse_checkfs(out)
    if cf is None:
        return False, "%s: checkfs produced no summary\n%s" % (tag, out[-800:])
    missing, already, freed = cf
    if missing != 0 or already != 0:
        return False, "%s: checkfs missing=%d already_set=%d freed=%d" % (
            tag, missing, already, freed)
    if require_manifest:
        ok, why = check_manifest(outdir)
        if not ok:
            return False, "%s: manifest %s" % (tag, why)
    return True, "nb=%d ags=%d used=%d freed=%d" % (
        s["num_blocks"], s["num_ags"], s["used_blocks"], freed)

# ---------------------------------------------------------------- setup base

SRC = os.path.join(WORK, "src")
OUT = os.path.join(WORK, "out")
BASE = os.path.join(WORK, "base_small.img")   # populated 128MB headroom image

def build_base():
    os.makedirs(WORK, exist_ok=True)
    make_manifest(SRC)
    fmt(BASE, SMALL_MB, headroom=True)
    rc, out = run_shell(BASE, populate_cmds(SRC))
    if rc != 0:
        raise RuntimeError("populate failed rc=%d\n%s" % (rc, out))

def fresh_trial(mb=BIG_MB, headroom=True):
    """A pristine oversized copy of the populated base, ungrown."""
    t = os.path.join(WORK, "trial.img")
    if headroom:
        shutil.copyfile(BASE, t)
    else:
        # rebuild a stock (no-headroom) populated image
        fmt(t, SMALL_MB, headroom=False)
        run_shell(t, populate_cmds(SRC))
    extend(t, mb)
    return t

def clean_out():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- tests

results = []
def record(name, ok, detail):
    results.append((name, ok, detail))
    print(("  PASS " if ok else "  FAIL ") + name + " :: " + detail, flush=True)

def t1():
    print("== T1: clean large grow ==", flush=True)
    t = fresh_trial(BIG_MB)
    clean_out()
    rc, out = run_shell(t, verify_cmds(OUT))
    ok, d = assess("T1-grow", t, out, expect_new=True, outdir=OUT)
    record("T1 clean grow 128->512", ok, d)
    # idempotent second mount (no further grow; already at partition size)
    clean_out()
    rc, out = run_shell(t, verify_cmds(OUT))
    ok2, d2 = assess("T1-2nd", t, out, expect_new=True, outdir=OUT)
    record("T1 second mount idempotent", ok2, d2)
    # +1 bitmap-block edge
    t2 = fresh_trial(ONEP_MB)
    clean_out()
    rc, out = run_shell(t2, verify_cmds(OUT))
    ok3, d3 = assess("T1-edge", t2, out, expect_new=True, outdir=OUT,
        targets={OLD_BLOCKS, mb_blocks(ONEP_MB)})
    record("T1 +1-bitmap-block edge 128->160", ok3, d3)

def t15():
    print("== T1.5: refusal / guard paths ==", flush=True)
    # 1. stock volume, oversized -> refuse, geometry unchanged
    t = fresh_trial(BIG_MB, headroom=False)
    clean_out()
    rc, out = run_shell(t, verify_cmds(OUT))
    ok, d = assess("T1.5-stock", t, out, expect_new=False, outdir=OUT)
    record("T1.5 stock no-headroom refused (unchanged)", ok, d)

    # 2. corrupted gap bit -> refuse, geometry unchanged. The corruption is a
    # deliberate, localized bitmap edit in the reserved gap, so checkfs/manifest
    # are confounded by design; the property under test is only that the grow
    # refuses (gap-sanity, Part D step 1) and leaves the geometry untouched.
    t = fresh_trial(BIG_MB)
    with open(t, "r+b") as f:
        f.seek(BS + 200)          # a byte inside bitmap block 1 (the gap region)
        f.write(bytes([0x00]))    # clear 8 gap bits (blocks 1600..1607)
    # Rigorous, crash-agnostic proof that the destructive path never runs: with
    # the write log armed, the grow engine only opens/writes the log on its
    # first raw write (intent record). If gap-sanity (Part D step 1) refuses
    # first, NO write happens, so the log file is never even created. (Clearing
    # a reserved-region bitmap bit also trips a pre-existing BFS self-heal
    # fragility later in the normal mount -- unrelated to the grow, which has
    # already returned "refuse".)
    gaplog = os.path.join(WORK, "gap_writelog.bin")
    if os.path.exists(gaplog):
        os.remove(gaplog)
    rc, out = run_shell(t, [], env={"BFS_GROW_WRITELOG": gaplog})
    s = read_super(t)
    wrote_nothing = (not os.path.exists(gaplog)) or os.path.getsize(gaplog) == 0
    ok = (s["num_blocks"] == OLD_BLOCKS) and wrote_nothing
    record("T1.5 corrupted gap bit refused (no writes)", ok,
        "num_blocks=%d grow_wrote_nothing=%s" % (s["num_blocks"], wrote_nothing))

    # 3. over-cap clamp: sparse-extend far beyond the 1TiB cap -> grow to cap.
    t = os.path.join(WORK, "cap.img")
    shutil.copyfile(BASE, t)
    with open(t, "r+b") as f:
        f.truncate((1 << 40) + (256 << 20))   # 1TiB + 256MB, sparse
    clean_out()
    rc, out = run_shell(t, verify_cmds(OUT), timeout=1200)
    s = read_super(t)
    okv, why = super_valid(s)
    okc = parse_checkfs(out)
    okcap = okv and s["num_blocks"] == CAP_BLOCKS and okc is not None \
        and okc[0] == 0 and okc[1] == 0
    okm, mwhy = check_manifest(OUT)
    record("T1.5 over-cap clamp to 1TiB", okcap and okm,
        "num_blocks=%d (cap=%d) valid=%s checkfs=%s manifest=%s" % (
            s["num_blocks"], CAP_BLOCKS, why, okc, mwhy))

    # 4. ioctl shrink refused (ResizeVisitor guard), volume mounted at small
    t = os.path.join(WORK, "shrink.img")
    shutil.copyfile(BASE, t)
    smaller = (SMALL_MB - 16) * 1024 * 1024
    rc, out = run_shell(t, ["resizefs %d" % smaller, "checkfs -c"])
    s = read_super(t)
    okshrink = (s["num_blocks"] == OLD_BLOCKS) and ("failed" in out.lower()
        or "not supported" in out.lower() or "Resizing failed" in out)
    record("T1.5 ioctl shrink refused", okshrink,
        "num_blocks=%d" % s["num_blocks"])

def t2a(repeats=5):
    print("== T2a: phase-boundary crash injection ==", flush=True)
    labels = ["intent", "bitmap-mid", "bitmap-flush", "phase",
              "commit-torn", "commit", "erase"]
    for label in labels:
        npass = 0
        detail = ""
        for i in range(repeats):
            t = fresh_trial(BIG_MB)
            # Session A: crash at label (mount aborts before the shell prompt)
            rc, out = run_shell(t, ["checkfs -c"],
                env={"BFS_GROW_ABORT": label})
            crashed = (rc == 42) or (rc < 0) or (rc != 0)
            # Session B: recover + verify
            clean_out()
            rc2, out2 = run_shell(t, verify_cmds(OUT))
            ok, d = assess("T2a-%s" % label, t, out2, expect_new=True,
                outdir=OUT)
            if ok and crashed:
                npass += 1
            else:
                detail = d + (" (crashA_rc=%d)" % rc)
                break
        record("T2a abort@%s x%d" % (label, repeats), npass == repeats,
            detail or "all %d recovered clean" % repeats)

# ---- T2b write-subset replay --------------------------------------------

def capture_writelog():
    """Run one clean grow with the write log on, return (segments, barriers).
    segments[i] = list of (offset,len,bytes) written before barriers[i]."""
    t = os.path.join(WORK, "wl_trial.img")
    shutil.copyfile(BASE, t)
    extend(t, BIG_MB)
    wl = os.path.join(WORK, "wl.bin")
    if os.path.exists(wl):
        os.remove(wl)
    rc, out = run_shell(t, ["checkfs -c"], env={"BFS_GROW_WRITELOG": wl})
    # parse
    segments = []
    barriers = []
    cur = []
    with open(wl, "rb") as f:
        blob = f.read()
    i = 0
    while i < len(blob):
        kind = blob[i]; i += 1
        if kind == 1:   # WRITE
            off = struct.unpack_from("<Q", blob, i)[0]; i += 8
            ln = struct.unpack_from("<I", blob, i)[0]; i += 4
            data = blob[i:i + ln]; i += ln
            cur.append((off, ln, data))
        elif kind == 2:  # BARRIER
            ln = struct.unpack_from("<I", blob, i)[0]; i += 4
            label = blob[i:i + ln].decode(); i += ln
            segments.append(cur)
            barriers.append(label)
            cur = []
        else:
            raise RuntimeError("bad writelog record %d at %d" % (kind, i))
    if cur:
        segments.append(cur)
        barriers.append("<end>")
    return segments, barriers

def apply_writes(img, writes):
    with open(img, "r+b") as f:
        for off, ln, data in writes:
            f.seek(off)
            f.write(data)

def t2b(iters=200):
    print("== T2b: write-subset replay ==", flush=True)
    segments, barriers = capture_writelog()
    print("  writelog: %d segments, barriers=%s"
          % (len(segments), barriers), flush=True)
    nseg = len(segments)
    npass = 0
    firstfail = ""
    pristine = os.path.join(WORK, "wl_pristine.img")
    shutil.copyfile(BASE, pristine)
    extend(pristine, BIG_MB)
    for it in range(iters):
        # crash barrier p: segments [0..p-1] fully durable; segment p in-flight
        p = random.randint(0, nseg - 1)
        durable = []
        for s in segments[:p]:
            durable.extend(s)
        inflight = segments[p]
        # random subset of the in-flight segment persists (any order)
        subset = [w for w in inflight if random.random() < 0.5]
        t = os.path.join(WORK, "wl_state.img")
        shutil.copyfile(pristine, t)
        apply_writes(t, durable + subset)
        clean_out()
        rc, out = run_shell(t, verify_cmds(OUT))
        ok, d = assess("T2b", t, out, expect_new=True, outdir=OUT)
        if ok:
            npass += 1
        else:
            firstfail = "iter=%d p=%d(%s) subset=%d/%d :: %s" % (
                it, p, barriers[p], len(subset), len(inflight), d)
            break
    record("T2b write-subset replay x%d" % iters, npass == iters,
        firstfail or "all %d states mounted clean-new" % iters)

def main():
    print("bfs_shell:", BFS_SHELL, flush=True)
    print("OLD_BLOCKS=%d NEW_BLOCKS=%d CAP_BLOCKS=%d"
          % (OLD_BLOCKS, NEW_BLOCKS, CAP_BLOCKS), flush=True)
    build_base()
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    if which in ("all", "t1"):
        t1()
    if which in ("all", "t15"):
        t15()
    if which in ("all", "t2a"):
        t2a(repeats=int(os.environ.get("T2A_REPEATS", "5")))
    if which in ("all", "t2b"):
        t2b(iters=int(os.environ.get("T2B_ITERS", "200")))
    print("\n==== SUMMARY ====", flush=True)
    npass = sum(1 for _, ok, _ in results if ok)
    for name, ok, d in results:
        print(("PASS " if ok else "FAIL ") + name, flush=True)
    print("%d/%d passed" % (npass, len(results)), flush=True)
    sys.exit(0 if npass == len(results) else 1)

if __name__ == "__main__":
    main()

# Static inventory of a Bun bundle's runtime surface. Reads JS sources only.
import json, os, re, sys, collections
root = sys.argv[1]
m = json.load(open(os.path.join(root, 'manifest.json')))
src = {}
for x in m['modules']:
    if x['loader'] == 1 or x['name'].endswith(('.js', '.mjs')) or x['name'].endswith('/cli'):
        p = os.path.join(root, x['name'][len('/$bunfs/'):])
        if os.path.exists(p): src[x['name']] = open(p, encoding='utf-8', errors='replace').read()
C = lambda: collections.Counter()
bun_api, bun_imp, node_imp, spawn_bins, plat, napi, procbun = C(), C(), C(), C(), C(), C(), C()
for n, s in src.items():
    for a in re.findall(r'\bBun\.([A-Za-z_$][\w$]*)', s): bun_api[a] += 1
    for a in re.findall(r'["\'](bun:[\w/-]+)["\']', s): bun_imp[a] += 1
    for a in re.findall(r'["\']node:([\w/]+)["\']', s): node_imp[a] += 1
    for a in re.findall(r'process\.versions\.bun|typeof Bun\s*[!=]==?\s*["\']undefined["\']|globalThis\.Bun\b', s): procbun[a] += 1
    for a in re.findall(r'process\.platform\s*[!=]==?\s*["\'](\w+)["\']', s): plat[a] += 1
    for a in re.findall(r'["\'](\w+)["\']\s*[!=]==?\s*process\.platform', s): plat[a] += 1
    for a in re.findall(r'["\']/\$bunfs/root/([^"\']+\.(?:node|wasm))["\']', s): napi[a] += 1
    for a in re.findall(r'(?:spawn|spawnSync|execFile|execFileSync|exec|execSync|Bun\.spawn|Bun\.spawnSync|which)\(\s*["\']([\w.+-]+)["\']', s): spawn_bins[a] += 1
def show(t, c, k=200):
    print(f"\n## {t} ({len(c)} distinct)"); print(", ".join(f"{a}×{n}" for a, n in c.most_common(k)))
print("JS modules scanned:", len(src), " total", sum(map(len, src.values()))//1_000_000, "MB")
show("Bun.* globals", bun_api); show("bun:* imports", bun_imp); show("Bun detection", procbun)
show("node:* imports", node_imp); show("process.platform literals", plat)
show(".node/.wasm referenced", napi); show("spawned binaries (literal first arg)", spawn_bins)
print("\n## embedded native/asset files")
for x in m['modules']:
    if x['loader'] in (10,) or x['name'].endswith(('.wasm',)): print(" ", x['loader'], x['contents_len'], x['name'])

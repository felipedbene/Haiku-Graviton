#!/usr/bin/env python3
"""Answer one question about the native arm64 package chain: for a given port, is
what stands in front of it a genuine dependency cycle, or a set of unbuilt leaves?

The chain's history is full of confident wrong cycle diagnoses (`zstd -> cmd:cmake`
was called "the one genuine cycle edge" for a day after it had dissolved), so this
reads haikuporter's own graph rather than the prose.

Method: **saturation**, not a backward walk. Start from the provides that the built
hpkgs actually supply, then repeatedly promote any recipe whose every build
dependency is already satisfied, adding what it provides to the satisfied set. Run to
a fixpoint over the whole recipe tree.

That settles the cycle question with no guesswork:

  * if the target is reached, it is NOT behind a cycle -- it is behind however many
    waves of leaves the saturation took, and the wave number is a real lower bound on
    build depth;
  * if the target is never reached, the ports still unreached at the fixpoint are the
    genuinely stuck set, and the requirements none of them can satisfy name the cause.

Saturation also avoids the trap of a backward walk: when several ports provide the
same name (jasper *and* jasper7 provide devel:libjasper; python3.10 *and* python3.14
provide cmd:python3) only one is needed, and a backward walk that unions the
providers inflates the answer.

Usage:
    depclosure.py --repo <haikuports> --built <hpkg dir> [--built <dir>...] \
        [--base <provides file>...] TARGET...
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

HPKG_RE = re.compile(r'^(.+)-([^-]+)-(\d+)-(arm64|any|source)\.hpkg$')

# Provides that the Haiku BASE image already supplies but that no haikuports
# recipe builds -- so saturation must credit them from the start or it will name
# them as blockers that no wave can ever clear. These packages ship in every
# standard (non-bootstrap, non-minimum) image and are installed in every native
# builder chroot; the bake harvest deliberately sweeps their hpkgs OUT of the
# built pool (rebuild.sh, prepguest.sh) as chroot inputs, so satisfied_from_disk
# never sees them. `makefile_engine` alone is build-required by the whole family
# of classic Haiku GUI-app ports; before this credit, ~110 such ports escalated
# to needs_human with esc_note NOPROV:makefile_engine (issue #174).
#
# This is a floor of KNOWN base packages that have no recipe. For a builder's
# exact installed provides (base commands like cmd:xres, libraries, etc.), pass
# --base pointing at that host's `pkgman list-installed`/`package list` output;
# the two combine. Version constraints are irrelevant here -- only names gate
# reachability -- so bare names suffice.
BASE_IMAGE_PROVIDES = {
	'makefile_engine',
	'netfs',
	'userland_fs',
}


def entry_name(spec):
	"""A provides/requires entry is "<name>[ <op> <ver>][ compat >= <v>]".

	Only the name is used for reachability. A version constraint is a different and
	much cheaper problem than a missing provider, so it is never grounds here for
	calling something unreachable -- that would manufacture a blocker.
	"""
	s = spec.strip()
	return s.split()[0] if s else ''


def vkey(v):
	return [int(p) if p.isdigit() else p for p in re.split(r'[._~+-]', v) if p]


def newer(a, b):
	try:
		return vkey(a) > vkey(b)
	except TypeError:
		return str(a) > str(b)


class Tree:
	"""The recipe tree, as haikuporter already parsed it into repository/*.DependencyInfo."""

	def __init__(self, repo):
		self.repo = repo
		self.ports = {}            # port -> info dict (highest version only)
		self.providers = defaultdict(set)   # provides-name -> {port, ...}
		self.pkg_provides = {}     # exact package name -> [provides names]
		# port -> everything it supplies once built, base package AND subpackages.
		# Keeping this separate from ports[p]['provides'] is load-bearing: a port's
		# base package does not provide its own subpackage names, so crediting only
		# the base makes things like flit_core_python310 or jasper_devel look as
		# though nothing in the tree supplies them, and saturation then reports a
		# reachable port as stuck.
		self.port_all_provides = defaultdict(set)
		self._load()

	def _load(self):
		repodir = os.path.join(self.repo, 'repository')
		markers, infos = set(), {}
		for fn in os.listdir(repodir):
			if fn.endswith('.DependencyInfoMarker'):
				markers.add(fn[:-len('.DependencyInfoMarker')])
			elif fn.endswith('.DependencyInfo'):
				infos[fn[:-len('.DependencyInfo')]] = os.path.join(repodir, fn)

		raw = {}
		for key, path in infos.items():
			try:
				with open(path) as f:
					raw[key] = json.load(f)
			except Exception as e:
				print('warn: unreadable %s: %s' % (key, e), file=sys.stderr)

		# A marker marks a base package, i.e. one recipe. Build dependencies live there.
		for key in sorted(markers):
			d = raw.get(key)
			if not d:
				continue
			name, version = d.get('name'), d.get('version', '')
			if not name:
				continue
			prev = self.ports.get(name)
			if prev and not newer(version, prev['version']):
				continue
			self.ports[name] = {
				'version': version,
				'build': sorted({entry_name(x) for x in
					list(d.get('buildRequires', [])) + list(d.get('buildPrerequires', []))} - {''}),
				'provides': sorted({entry_name(x) for x in d.get('provides', [])} - {''}),
				'requires': sorted({entry_name(x) for x in d.get('requires', [])} - {''}),
			}

		# Eight ports (measured: 8 of 3667 markers) have a marker but NO base
		# DependencyInfo -- pure-Python ARCHITECTURES="any" ports such as pygments,
		# pytest and sip, which emit only per-flavour subpackages. For those the build
		# requirements live in the *subpackage* files instead, so synthesise the port
		# from them. Without this the port looks absent and everything downstream is
		# misreported as having no provider at all: pygments_python310 read as
		# "nothing in the tree provides this", which wrongly made gtk_doc -> libidn2
		# -> libpsl a hard stop and so declared a browser unreachable.
		for key in sorted(markers):
			if key in raw:
				continue
			subs = [d for k, d in raw.items()
				if k.startswith(key.rsplit('-', 1)[0] + '_') and
					d.get('version') == key.rsplit('-', 1)[1]]
			if not subs:
				continue
			name = key.rsplit('-', 1)[0]
			version = key.rsplit('-', 1)[1]
			build, prov, req = set(), set(), set()
			for d in subs:
				build |= {entry_name(x) for x in
					list(d.get('buildRequires', [])) + list(d.get('buildPrerequires', []))}
				prov |= {entry_name(x) for x in d.get('provides', [])}
				req |= {entry_name(x) for x in d.get('requires', [])}
			self.ports[name] = {
				'version': version,
				'build': sorted(build - {''}),
				'provides': sorted(prov - {''}),
				'requires': sorted(req - {''}),
				'synthesised': True,
			}

		# Attribute every package's provides -- base and subpackage alike -- to the port
		# that builds it, so devel:libjasper resolves to the jasper port.
		byver = defaultdict(list)
		for name, inf in self.ports.items():
			byver[inf['version']].append(name)

		for key, d in raw.items():
			pkg, version = d.get('name', ''), d.get('version', '')
			plist = sorted({entry_name(x) for x in d.get('provides', [])} - {''})
			self.pkg_provides[pkg] = plist
			owner = self._owner(pkg, version, byver)
			if owner is None:
				continue
			# Only credit the port if this DependencyInfo is for the version we kept.
			if self.ports[owner]['version'] != version:
				continue
			# The subpackage's own name is a provides in its own right.
			self.port_all_provides[owner].add(pkg)
			self.providers[pkg].add(owner)
			for p in plist:
				self.providers[p].add(owner)
				self.port_all_provides[owner].add(p)

	def _owner(self, pkg, version, byver):
		if pkg in self.ports:
			return pkg
		best = None
		for cand in byver.get(version, []):
			if pkg.startswith(cand + '_') and (best is None or len(cand) > len(best)):
				best = cand
		if best:
			return best
		for cand in self.ports:
			if pkg.startswith(cand + '_') and (best is None or len(cand) > len(best)):
				best = cand
		return best


def satisfied_from_disk(tree, dirs):
	"""What is already available, derived from hpkg files that exist.

	Keyed off files on disk and their recorded provides, never off a build log: a
	success code is not an artifact.
	"""
	pkgs, ports = set(), set()
	for d in dirs:
		if not os.path.isdir(d):
			print('warn: no such built dir: %s' % d, file=sys.stderr)
			continue
		for fn in os.listdir(d):
			if not fn.endswith('.hpkg'):
				continue
			m = HPKG_RE.match(fn)
			pkgs.add(m.group(1) if m else fn[:-len('.hpkg')])

	sat = set(pkgs)
	for pkg in pkgs:
		for p in tree.pkg_provides.get(pkg, []):
			sat.add(p)
	# A built hpkg whose name matches a port counts that port as built.
	for pkg in pkgs:
		if pkg in tree.ports:
			ports.add(pkg)
		else:
			for cand in tree.ports:
				if pkg.startswith(cand + '_'):
					ports.add(cand)
	return sat, ports, pkgs


def base_provides(files):
	"""Names the base image supplies for free: the known no-recipe floor plus any
	provide-tokens listed in --base files (one per line; '#' comments allowed).

	Only the leading name of each token is kept -- a version constraint is not a
	missing-provider problem and must never manufacture a blocker (same rule as
	entry_name)."""
	names = set(BASE_IMAGE_PROVIDES)
	for path in files:
		try:
			with open(path) as f:
				for line in f:
					s = line.strip()
					if s and not s.startswith('#'):
						names.add(entry_name(s))
		except Exception as e:
			print('warn: unreadable --base %s: %s' % (path, e), file=sys.stderr)
	return names


def saturate(tree, sat, already):
	"""Promote every recipe whose build deps are satisfied, to a fixpoint.

	Returns wave[port] = the iteration at which it became buildable. Ports absent
	from wave are unreachable no matter what order anything is built in.

	A port is promoted only when BOTH its build requirements and its own runtime
	`requires` are satisfied. The runtime half is not pedantry: haikuporter has to
	*install* every build dependency into the chroot, so a dependency that builds but
	cannot be installed is not usable. Measured the hard way -- `meson` failed with

	    requires "packaging_python314" of package "build_python3.14-1.5.0-1" could not be resolved
	    build-requires "build_python314" of package "meson-1.11.1" could not be resolved

	*after* `build` had itself built cleanly at RC=0. Walking build edges alone said
	meson was ready; it was not, because `build_python3.14` needs `packaging` at
	runtime. Note which line is which: haikuporter prints the real cause first and the
	misleading summary last, so a tail(1) blames `build_python314` instead of naming
	`packaging`.

	The same shape hides a costlier one: `psutils` builds fine but requires
	`puremagic_python310` and `pypdf_python310`, so `cmd:psselect` cannot be installed
	for groff until those two exist. Before this fix the tool reported groff's distance
	without them, i.e. it under-counted.
	"""
	sat = set(sat)
	wave = {p: 0 for p in already}
	n = 0
	while True:
		n += 1
		promoted = []
		for port, inf in tree.ports.items():
			if port in wave:
				continue
			if not all(r in sat for r in inf['build']):
				continue
			# Installability: a build dependency must be installable, not merely built.
			if not all(r in sat for r in inf['requires']):
				continue
			promoted.append(port)
		if not promoted:
			return wave, n - 1
		for port in promoted:
			wave[port] = n
			sat |= tree.port_all_provides.get(port, set())
			sat |= set(tree.ports[port]['provides'])


def minimal_set(tree, target, wave, sat):
	"""Backward walk restricted to strictly-lower waves, choosing one provider per
	requirement -- the reachable one with the lowest wave, i.e. the cheapest. Because
	every chosen provider has a strictly smaller wave, the result is acyclic by
	construction.

	Walks build requirements AND runtime `requires`, because a build dependency has to
	be installable into the chroot, not merely built. Omitting the runtime half is what
	made an earlier run of this tool leave `packaging`, `puremagic` and `pypdf` out of
	groff's build set entirely."""
	chosen, order = {}, []
	stack = [target]
	seen = set()
	while stack:
		port = stack.pop()
		if port in seen:
			continue
		seen.add(port)
		inf = tree.ports.get(port)
		if not inf:
			continue
		deps = set()
		for req in inf['build'] + inf['requires']:
			if req in sat:
				continue
			cands = [c for c in tree.providers.get(req, ()) if c in wave and wave[c] < wave[port]]
			if not cands:
				continue
			pick = min(cands, key=lambda c: (wave[c], c))
			deps.add(pick)
			stack.append(pick)
		chosen[port] = deps
	return chosen


def report(tree, sat, already, wave, rounds, targets):
	print('recipes indexed:          %d' % len(tree.ports))
	print('distinct provides names:  %d' % len(tree.providers))
	print('provides satisfied now:   %d' % len(sat))
	print('ports already built:      %d' % len(already))
	print('saturation rounds:        %d' % rounds)
	print('ports reachable at all:   %d of %d' % (len(wave), len(tree.ports)))
	print()

	for target in targets:
		print('=' * 72)
		inf = tree.ports.get(target)
		if inf is None:
			print('%s: NO RECIPE in this tree' % target)
			continue
		if target not in wave:
			print('%s %s: UNREACHABLE at the saturation fixpoint.' % (target, inf['version']))
			blockers = [r for r in inf['build'] if r not in sat and
				not any(c in wave for c in tree.providers.get(r, ()))]
			print('  Its own unreachable build requirements:')
			for r in sorted(blockers):
				owners = sorted(tree.providers.get(r, ()))
				print('    %-34s providers: %s' % (r, ', '.join(owners) or 'NONE IN TREE'))
			continue

		w = wave[target]
		chosen = minimal_set(tree, target, wave, sat)
		tobuild = {p: d for p, d in chosen.items() if p not in already}
		print('%s %s: REACHABLE -- wave %d, NOT behind a cycle.' % (target, inf['version'], w))
		print('  Minimal build set: %d ports (target included).' % len(tobuild))
		print('  Acyclic by construction: every chosen provider sits in a strictly')
		print('  lower wave than its consumer, so no ordering conflict can exist.')
		print()
		waves = defaultdict(list)
		for p in tobuild:
			waves[wave[p]].append(p)
		for k in sorted(waves):
			print('    wave %-2d (%2d): %s' % (k, len(waves[k]), ' '.join(sorted(waves[k]))))
		print()
		print('  Ports in the set with NO unbuilt dependency (start here):')
		ready = sorted(p for p, d in tobuild.items() if not (d & set(tobuild)))
		for p in ready:
			print('    %-28s %s' % (p, tree.ports[p]['version']))


def explain(tree, sat, wave, port):
	"""Per-requirement status for one port. The habit this enforces: read the *first*
	failure and the whole list, never the last line."""
	inf = tree.ports.get(port)
	print('=' * 72)
	if inf is None:
		print('%s: NO RECIPE' % port)
		return
	print('%s %s -- build requirements, one line each:' % (port, inf['version']))
	for r in inf['build']:
		if r in sat:
			print('  OK        %s' % r)
			continue
		owners = sorted(tree.providers.get(r, ()))
		if not owners:
			print('  NOPROV    %-32s nothing in the tree provides this' % r)
			continue
		desc = ', '.join('%s@%s' % (o, wave.get(o, 'unreachable')) for o in owners)
		reach = [o for o in owners if o in wave]
		print('  %-9s %-32s %s' % ('NEED' if reach else 'STUCK', r, desc))


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('--repo', default='/opt/haiku/haikuports')
	ap.add_argument('--built', action='append', default=[])
	ap.add_argument('--base', action='append', default=[],
		help='file of provide-tokens the base image already supplies (one per '
			'line); credited as satisfied before saturation. A known no-recipe '
			'floor (makefile_engine, netfs, userland_fs) is always credited.')
	ap.add_argument('--why', action='append', default=[],
		help='explain which ports provide this requirement name')
	ap.add_argument('--explain', action='append', default=[],
		help='list one port\'s build requirements with per-requirement status')
	ap.add_argument('--assume', action='append', default=[],
		help='pretend this port is built, to measure what it would unlock')
	ap.add_argument('targets', nargs='*')
	args = ap.parse_args()

	tree = Tree(args.repo)
	sat, already, pkgs = satisfied_from_disk(tree, args.built or ['/opt/haiku/hpkg-out/arm64'])

	# Credit what the base image supplies but no recipe builds, so a base-only
	# provider (makefile_engine et al.) is never misreported as an unreachable
	# blocker. See BASE_IMAGE_PROVIDES / --base (issue #174).
	sat |= base_provides(args.base)

	# A counterfactual: mark a port built and re-saturate. This is how to price a
	# chokepoint -- "what does python3.14 actually unlock" is answerable without
	# building it, and the answer is a measurement of the graph, not a guess.
	for port in args.assume:
		inf = tree.ports.get(port)
		if inf is None:
			print('warn: --assume %s: no such recipe' % port, file=sys.stderr)
			continue
		already = already | {port}
		sat |= set(inf['provides'])
		sat |= tree.port_all_provides.get(port, set())

	wave, rounds = saturate(tree, sat, already)

	for req in args.why:
		owners = sorted(tree.providers.get(req, ()))
		print('%s: satisfied=%s providers=%s' % (req, req in sat,
			', '.join('%s(wave %s)' % (o, wave.get(o, 'UNREACHABLE')) for o in owners) or 'NONE'))
	if args.why:
		print()

	for port in args.explain:
		explain(tree, sat, wave, port)
	if args.explain:
		print()

	if args.targets:
		report(tree, sat, already, wave, rounds, args.targets)


if __name__ == '__main__':
	main()

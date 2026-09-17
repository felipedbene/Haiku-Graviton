# Playbook-parity hook — enablement

`graviton/scripts/haiku-playbook-parity` keeps three things in lockstep (#90/#136):

- the **classifier** ruleset (`graviton/scripts/haiku-triage-failures` `RULES`),
- the **playbook** (`graviton/docs/porting-playbook.md`),
- the mined failure **frontier** (`graviton/scripts/haiku-pattern-miner`).

It runs two checks:

1. **Hard gate (offline, no AWS).** Every class the classifier recognises must have a
   matching playbook section/anchor. Fails (**exit 3**) if a ruleset class points at a
   missing or renamed heading — otherwise a stamped `triage_fix_ref` is a dead link.
   Also prints an INFO line for playbook classes the ruleset does not yet detect
   (e.g. classes 18–24 until their regex is added; 15–17 are PR #309’s).
2. **Soft nudge (opt-in, needs AWS read).** If a mined UNMATCHED cluster is ≥ threshold
   (default 5 ports) with no class, it warns you are probably missing a pattern. Warning
   only (**exit 0**) unless `--strict` (**exit 4**).

**This hook is not force-installed.** Enable it one of the ways below.

## Option A — native git pre-commit hook (per clone)

Fast, offline gate on every commit. From the repo root:

```sh
cat > .git/hooks/pre-commit <<'SH'
#!/bin/sh
# DeBeOS: playbook <-> classifier parity (offline gate only; no AWS on commit).
exec graviton/scripts/haiku-playbook-parity
SH
chmod +x .git/hooks/pre-commit
```

Or, to keep hooks in-tree and shared, point git at a tracked hooks dir:

```sh
git config core.hooksPath graviton/githooks   # then add the pre-commit shim there
```

## Option B — `pre-commit` framework

If you use [pre-commit](https://pre-commit.com), add to `.pre-commit-config.yaml`:

```yaml
repos:
  - repo: local
    hooks:
      - id: haiku-playbook-parity
        name: playbook <-> classifier parity
        entry: graviton/scripts/haiku-playbook-parity
        language: system
        files: '^graviton/(docs/porting-playbook\.md|scripts/haiku-(triage-failures|pattern-miner|playbook-parity))$'
        pass_filenames: false
```

(The offline gate runs on commit; the mined nudge stays a CI concern — see below.)

## Option C — CI job stub (the mined nudge belongs here)

The `--mine` half needs AWS read access and the live DDB/S3 backlog, so run it in CI
(nightly or on PRs that touch the playbook/scripts), not on every local commit. GitHub
Actions stub:

```yaml
# .github/workflows/playbook-parity.yml
name: playbook-parity
on:
  pull_request:
    paths:
      - 'graviton/docs/porting-playbook.md'
      - 'graviton/scripts/haiku-triage-failures'
      - 'graviton/scripts/haiku-pattern-miner'
      - 'graviton/scripts/haiku-playbook-parity'
  schedule:
    - cron: '0 7 * * 1'          # Monday, to catch a growing UNMATCHED cluster
jobs:
  parity:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: '3.11' }
      - run: pip install boto3
      # Offline gate on every run (never needs AWS):
      - run: graviton/scripts/haiku-playbook-parity
      # Mined nudge only when AWS read creds are configured (assume-role step omitted):
      - if: ${{ env.AWS_ROLE_ARN != '' }}
        run: graviton/scripts/haiku-playbook-parity --mine --threshold 5
        # add --strict to make a big unclassified cluster fail the build.
```

Keep the mined nudge non-`--strict` at first: the current backlog already has clusters
≥ 5 (classes 15/18/20 are documented but the *ruleset* does not detect them yet, so they
stay UNMATCHED and the miner keeps flagging them). Make it `--strict` only once the
classifier ruleset is extended to recognise the documented classes and the standing
clusters drain.

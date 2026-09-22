"""pkgkey -- the `debeos-package-state` primary-key contract, in one place (#487).

WHY THIS EXISTS. The state table's partition key `pkg` is one port name. Nothing
enforced that, and in the #136 campaign a requeue wrote a key that was *thirteen*
space-joined port names:

    pkg = "opencc polyclipping robin_map recastnavigation portsmf mdate musicpc
           mm_common primesieve qhull pystring minisign nesalizer"

That row is a phantom: no such port exists, so it can never build, never fail, and
never be marked anything -- while the thirteen real ports it names go untracked by
it. It sat in `queued` for months looking like work in progress. The cost is not the
one bad row, it is that a shell expansion accident (`"$pkgs"` instead of `$pkgs`, or
a `$(...)` that returned a list) silently becomes durable state.

So every writer validates the key before the write, and the validation lives HERE
so there is exactly one definition of "a legal pkg key" for the lambdas
(ClaimBatch/RecordOutcome), state-sync, and the operator scripts.

It lives in `ops-lambdas/` because `lambda.Code.fromAsset(ops-lambdas)` bundles this
directory verbatim, so the lambdas can `import pkgkey` with no packaging work; the
repo-side callers reach it by path (see `load_pkgkey()` in the scripts).

WHAT IS REJECTED. Whitespace of any kind, because every legitimate HaikuPorts port
name is a single shell word: `[A-Za-z0-9._+-]`-ish, never a space. A space in the
key means a caller passed a *list* where the contract wants one name, which is the
#487 defect exactly. Empty/non-string keys are rejected for the same reason (they
are always a caller bug, never a real port).

Deliberately NOT rejected: unusual-but-real characters. `python3.14`, `libgpg_error`,
`rust_bin`, `1.0~git`-style suffixes and `+` all occur, so this is a whitespace and
emptiness guard, not an allowlist. An allowlist that guessed wrong would refuse a
real port, which is worse than the bug it prevents.
"""

_WS = frozenset(" \t\n\r\v\f")


class BadPkgKey(ValueError):
    """A `pkg` primary key that violates the one-name-per-row contract."""


def validate_pkg_key(pkg, where=""):
    """Return `pkg` unchanged, or raise BadPkgKey loudly.

    `where` is a caller label that ends up in the message -- when this fires in a
    lambda the CloudWatch line has to say which writer it was, or the next operator
    is back to guessing.
    """
    tag = " [%s]" % where if where else ""
    if not isinstance(pkg, str):
        raise BadPkgKey("pkg key must be a string, got %s: %r%s"
                        % (type(pkg).__name__, pkg, tag))
    if not pkg:
        raise BadPkgKey("pkg key is empty%s" % tag)
    if any(c in _WS for c in pkg):
        n = len(pkg.split())
        raise BadPkgKey(
            "pkg key contains whitespace -- the table's partition key is ONE port "
            "name, not a list. Got %d whitespace-separated words: %r%s. If you meant "
            "to write several ports, loop over them (one UpdateItem each); a joined "
            "key creates a phantom row that can never build (#487)." % (n, pkg, tag))
    return pkg


def is_valid_pkg_key(pkg):
    """Non-raising form, for auditing a table you did not write."""
    try:
        validate_pkg_key(pkg)
        return True
    except BadPkgKey:
        return False


def split_pkg_key(pkg):
    """The port names a whitespace-joined key was trying to name.

    Used only by the repair path (`haiku-state-guard split-key`): it is how a
    phantom row is turned back into the list it should always have been.
    """
    return [p for p in str(pkg).split() if p]

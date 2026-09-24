#!/usr/bin/env python3
"""Apply one textual mutation to a file, refusing anything ambiguous.

Used by mutation_test.sh. The site must match exactly once: a mutation that
matched twice, or zero times, would silently test something other than the rule
it names -- which is the failure mode the whole exercise exists to avoid.

    mutate.py <file> <from-file> <to-file>
"""
import sys


def main():
    path, from_path, to_path = sys.argv[1:4]
    src = open(path).read()
    frm = open(from_path).read()
    to = open(to_path).read()

    # The shell cannot pass a trailing newline reliably; compare without one.
    frm = frm.rstrip('\n')
    to = to.rstrip('\n')

    count = src.count(frm)
    if count != 1:
        sys.stderr.write('mutation site matched %d times, need exactly 1:\n---\n%s\n---\n'
                         % (count, frm))
        return 2

    open(path, 'w').write(src.replace(frm, to))
    return 0


if __name__ == '__main__':
    sys.exit(main())

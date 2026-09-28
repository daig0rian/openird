#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""trace2cmd - turn a yb1-ioctl "trace" log into a yb1-cmd command file.

  trace2cmd.py <trace.log> [out.cmd]

yb1-ioctl with a "trace" line prints, after every vendor ioctl, the AV
sub-CPU command-stack entries the call produced (entry, command ID, sequence
number, status, request and response words).  This writes them back out as
one yb1-cmd line each, in the order they were issued, so that yb1-cmd can post
the very same commands -- same entries, sequence numbers and data-ring
addresses -- with no vendor kernel module in the path, and check every status
and response against what the vendor path got.
(docs/specs/poe-br100-yb1-reception-stack.md §80)
"""
import re
import sys

ENTRY = re.compile(r'\s+=> entry (\d+) cmd 0x([0-9a-f]+) seq 0x([0-9a-f]+) w0 [0-9a-f]+ '
                   r'status (-?\d+) req (\d+) B resp (\d+) B @([0-9a-f]+)')


def words(line):
    body = line.split(':', 1)[1].split()
    if '...' in body:
        raise ValueError('a request or response longer than the trace prints: ' + line)
    return ['0x' + w for w in body]


def main():
    lines = open(sys.argv[1]).read().splitlines()
    out = []
    call = ''
    for i, ln in enumerate(lines):
        if ln.startswith('nr='):
            call = ln.split('  buf:')[0]
            continue
        m = ENTRY.match(ln)
        if not m:
            continue
        entry, cmd, seq, status, reqlen, resplen, addr = m.groups()
        req = words(lines[i + 1])
        resp = words(lines[i + 2])
        out.append('# %s' % call)
        out.append(' '.join([entry, '0x%04x%04x' % (int(cmd, 16), int(seq, 16)),
                             '0x' + addr, reqlen, resplen, status] + req +
                            (['='] + resp if resp else [])))
    text = '\n'.join(out) + '\n'
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w').write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write('%d commands\n' % sum(1 for l in out if not l.startswith('#')))


if __name__ == '__main__':
    main()

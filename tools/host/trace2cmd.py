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
    groups = []                 # one list of commands per vendor call
    for i, ln in enumerate(lines):
        if ln.startswith('nr='):
            groups.append((ln.split('  buf:')[0], []))
            continue
        m = ENTRY.match(ln)
        if not m or not groups:
            continue
        entry, cmd, seq, status, reqlen, resplen, addr = m.groups()
        groups[-1][1].append((int(entry), int(cmd, 16), int(seq, 16), status, reqlen,
                              resplen, addr, words(lines[i + 1]), words(lines[i + 2])))

    # The trace lists a call's entries in TABLE order; the sub-CPU takes them in
    # SEQUENCE order (the low 8 bits count, and wrap), and a call whose commands
    # straddle the end of the table (entry 31, then 1) must keep that order.
    # Entry 0 is not part of the ring: the vendor posts its status polls there
    # by some other means, so those are kept as comments only.
    out = []
    last = None
    for call, cmds in groups:
        if last is not None:
            cmds.sort(key=lambda c: (c[2] - last - 1) & 0xff)
        else:
            cmds.sort(key=lambda c: c[2] & 0xff)
        for entry, cmd, seq, status, reqlen, resplen, addr, req, resp in cmds:
            line = ' '.join([str(entry), '0x%04x%04x' % (cmd, seq), '0x' + addr, reqlen,
                             resplen, status] + req + (['='] + resp if resp else []))
            out.append('# %s' % call)
            if entry == 0:
                out.append('# (entry 0, not replayed) ' + line)
                continue
            out.append(line)
            last = seq
    text = '\n'.join(out) + '\n'
    if len(sys.argv) > 2:
        open(sys.argv[2], 'w').write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write('%d commands\n' % sum(1 for l in out if not l.startswith('#')))


if __name__ == '__main__':
    main()

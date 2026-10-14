#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tsref - summarise a raw (scrambled, undecoded) TS: PIDs, rates, PSI, and a per-packet
hash list for cross-checking another receiver's capture of the same broadcast.

Only TS headers, clear PSI tables and hashes are reported -- never payload bytes.

  tsref.py <file.ts> [hashes.txt]

Written to check POE-BR100-YB1's raw TS against a PC tuner's capture of the same
station (docs/specs/poe-br100-yb1-reception-stack.md §76).  Capture WITHOUT
descrambling (e.g. recisdb --no-decode); OpenIRD never descrambles.  The hash
list has one 'pid cc crc32' line per packet, the same format and CRC as
`yb1-mem @file 0 <len> hashes`, so tshashcmp.py can line the two up.
"""
import collections
import sys
import zlib

TS = 188
PCR_WRAP = (1 << 33) * 300
# Descriptors that name a CA stream's PID: the conditional access descriptor
# (0x09) and ARIB's access control descriptor (0xF6, ARIB TR-B14 Vol.4 5.2 /
# 30.2.2.2: same layout; EMM when in the CAT, ECM when in the PMT).
CA_TAGS = (0x09, 0xF6)


def packets(buf):
    i = 0
    while i + 3 * TS <= len(buf) and not (
            buf[i] == 0x47 and buf[i + TS] == 0x47 and buf[i + 2 * TS] == 0x47):
        i += 1
    while i + TS <= len(buf):
        if buf[i] != 0x47:
            j = buf.find(b'\x47', i + 1)
            if j < 0:
                break
            i = j
            continue
        yield buf[i:i + TS]
        i += TS


def pcr_of(p):
    if (p[3] >> 4) & 2 and p[4] >= 7 and p[5] & 0x10:
        b = p[6:12]
        base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
        return base * 300 + (((b[4] & 1) << 8) | b[5])
    return None


def crc_ok(sec):
    crc = 0xffffffff
    for b in sec:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04c11db7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xffffffff
    return crc == 0


class Sections:
    """Reassemble the (clear) PSI sections carried on one PID."""

    def __init__(self):
        self.buf = b''
        self.out = []

    def feed(self, p):
        if (p[3] >> 6) & 3 or not (p[3] >> 4) & 1:
            return
        off = 4 + (1 + p[4] if (p[3] >> 4) & 2 else 0)
        pl = p[off:]
        if not pl:  # adaptation field to the end (or a broken length)
            return
        if p[1] & 0x40:
            ptr = pl[0]
            if self.buf:
                self.buf += pl[1:1 + ptr]
                self.drain()
            self.buf = pl[1 + ptr:]
        elif self.buf:
            self.buf += pl
        else:
            return
        self.drain()

    def drain(self):
        while len(self.buf) >= 3 and self.buf[0] != 0xff:
            ln = ((self.buf[1] & 0x0f) << 8) | self.buf[2]
            if len(self.buf) < 3 + ln:
                return
            self.out.append(self.buf[:3 + ln])
            self.buf = self.buf[3 + ln:]
        if self.buf[:1] == b'\xff':
            self.buf = b''


def descriptors(d):
    i = 0
    while i + 2 <= len(d):
        yield d[i], d[i + 2:i + 2 + d[i + 1]]
        i += 2 + d[i + 1]


def scan(buf, psi_pids, hashes=None):
    st = {'n': 0, 'cnt': collections.Counter(), 'scr': collections.defaultdict(collections.Counter),
          'tei': collections.Counter(), 'gap': collections.Counter(), 'pcr': []}
    last = {}
    secs = collections.defaultdict(Sections)
    for p in packets(buf):
        st['n'] += 1
        pid = ((p[1] & 0x1f) << 8) | p[2]
        st['cnt'][pid] += 1
        st['scr'][pid][(p[3] >> 6) & 3] += 1
        if p[1] & 0x80:
            st['tei'][pid] += 1
        cc = p[3] & 15
        if (p[3] >> 4) & 1 and pid != 0x1fff:
            if pid in last and cc not in (last[pid], (last[pid] + 1) & 15):
                st['gap'][pid] += 1
            last[pid] = cc
        pc = pcr_of(p)
        if pc is not None:
            st['pcr'].append((pid, pc))
        if pid in psi_pids:
            secs[pid].feed(p)
        if hashes is not None:
            hashes.append('%04x %x %08x' % (pid, cc, zlib.crc32(p) & 0xffffffff))
    return st, secs


def main():
    path = sys.argv[1]
    buf = open(path, 'rb').read()

    # pass 1: the PAT
    _, secs = scan(buf, {0})
    pat, tsid = {}, None
    for sec in secs[0].out:
        if sec[0] == 0 and crc_ok(sec):
            tsid = (sec[3] << 8) | sec[4]
            body = sec[8:-4]
            for i in range(0, len(body) - 3, 4):
                pat[(body[i] << 8) | body[i + 1]] = ((body[i + 2] & 0x1f) << 8) | body[i + 3]

    # pass 2: everything, with the PMTs, CAT and the low SI PIDs reassembled
    hashes = [] if len(sys.argv) > 2 else None
    psi = {0, 1} | set(pat.values()) | set(range(0x10, 0x30))
    st, secs = scan(buf, psi, hashes)
    n = st['n']
    print('file %s: %d packets (%d bytes)' % (path, n, len(buf)))
    dur = None
    if st['pcr']:
        pp = collections.Counter(pid for pid, _ in st['pcr']).most_common(1)[0][0]
        v = [c for pid, c in st['pcr'] if pid == pp]
        span = (v[-1] - v[0]) % PCR_WRAP
        dur = span / 27e6
        print('duration by PCR (PID 0x%04x): %.3f s  -> %.0f packets/s = %.2f Mbps'
              % (pp, dur, n / dur, n / dur * TS * 8 / 1e6))

    print('\nPAT: TSID 0x%04x  %s' % (tsid or 0, '  '.join(
        'prog 0x%04x->PID 0x%04x' % kv for kv in sorted(pat.items()))))

    announced = {0: 'PAT'}
    for prog, pid in pat.items():
        announced[pid] = 'NIT' if prog == 0 else 'PMT %04x' % prog
    for prog, pmt_pid in sorted(pat.items()):
        if prog == 0:
            continue
        got = [s for s in secs[pmt_pid].out if s[0] == 2 and crc_ok(s) and ((s[3] << 8) | s[4]) == prog]
        if not got:
            print('PMT 0x%04x on 0x%04x: not seen' % (prog, pmt_pid))
            continue
        s = got[-1]
        pcr = ((s[8] & 0x1f) << 8) | s[9]
        pil = ((s[10] & 0x0f) << 8) | s[11]
        cas = ['CA_PID 0x%04x' % (((d[2] & 0x1f) << 8) | d[3])
               for t, d in descriptors(s[12:12 + pil]) if t in CA_TAGS and len(d) >= 4]
        announced.setdefault(0x1fff, 'null')
        for t, d in descriptors(s[12:12 + pil]):
            if t in CA_TAGS and len(d) >= 4:
                announced.setdefault(((d[2] & 0x1f) << 8) | d[3], 'CA %04x' % prog)
        if pcr != 0x1fff:
            announced.setdefault(pcr, 'PCR %04x' % prog)
        es, i, end = [], 12 + pil, len(s) - 4
        while i + 5 <= end:
            stype, epid = s[i], ((s[i + 1] & 0x1f) << 8) | s[i + 2]
            eil = ((s[i + 3] & 0x0f) << 8) | s[i + 4]
            es.append('0x%04x(type %02x)' % (epid, stype))
            announced.setdefault(epid, 'ES %04x t%02x' % (prog, stype))
            for t, d in descriptors(s[i + 5:i + 5 + eil]):
                if t in CA_TAGS and len(d) >= 4:
                    announced.setdefault(((d[2] & 0x1f) << 8) | d[3], 'CA %04x' % prog)
            i += 5 + eil
        print('PMT 0x%04x on 0x%04x: PCR 0x%04x  %s  %s' % (prog, pmt_pid, pcr, ' '.join(es), ' '.join(cas)))
    announced.setdefault(0x1fff, 'null')
    if st['cnt'][1]:
        announced[1] = 'CAT'
    cat_seen = 0
    for s in secs[1].out:
        if s[0] == 1:
            cat_seen += 1
            ok = crc_ok(s)
            pids = [((d[2] & 0x1f) << 8) | d[3] for t, d in descriptors(s[8:-4]) if t in CA_TAGS and len(d) >= 4]
            print('CAT section: crc %s, CA_PID %s' % ('ok' if ok else 'BAD', ' '.join('0x%04x' % x for x in pids) or '-'))
            for x in pids:
                announced.setdefault(x, 'CA (CAT)')
    if not cat_seen:
        print('CAT: no section reassembled (%d packets on PID 1)' % st['cnt'][1])
    tids = {pid: sorted({s[0] for s in secs[pid].out if crc_ok(s)}) for pid in range(0x10, 0x30) if secs[pid].out}
    # PIDs nothing announces: which kind of sections do they carry?  The
    # table_id says (ARIB STD-B10 Table 5-2: 0x3a-0x3f DSM-CC, 0x82-0x83 ECM,
    # 0x84-0x85 EMM ...).  Only the table_id is read, never the contents.
    stray = {pid for pid in st['cnt'] if pid not in announced and pid not in tids}
    if stray:
        _, ssecs = scan(buf, stray)
        for pid in stray:
            if ssecs[pid].out:
                tids[pid] = sorted({s[0] for s in ssecs[pid].out})

    print('\n  PID     packets  pkt/s   scrambled(even/odd)  cc_gaps  tei  role')
    for pid, c in sorted(st['cnt'].items()):
        role = announced.get(pid)
        if role is None:
            kind = 'SI' if 0x10 <= pid < 0x30 else 'NOT ANNOUNCED'
            role = ('%s, table_id %s' % (kind, ','.join('%02x' % t for t in tids[pid]))) \
                if pid in tids else ('null' if pid == 0x1fff else '-- NOT ANNOUNCED --')
        sc = st['scr'][pid]
        print('  0x%04x %8d %7.1f   %7d/%-7d      %5d  %4d  %s' % (
            pid, c, c / dur if dur else 0, sc[2], sc[3], st['gap'][pid], st['tei'][pid], role))
    absent = sorted(pid for pid in announced if pid not in st['cnt'])
    print('\nannounced but absent:', ' '.join('0x%04x(%s)' % (p, announced[p]) for p in absent) or 'none')
    print('present PIDs: %d  announced: %d  PID-table entries needed for all but null: %d'
          % (len(st['cnt']), len(announced), len([p for p in st['cnt'] if p != 0x1fff])))
    if hashes is not None:
        with open(sys.argv[2], 'w') as f:
            f.write('\n'.join(hashes) + '\n')
        print('hashes: %d lines -> %s' % (len(hashes), sys.argv[2]))


if __name__ == '__main__':
    main()

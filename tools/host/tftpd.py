#!/usr/bin/env python3
"""Read/write TFTP server for the YB1 rig (busybox tftp -g and -p).

The device has no wget and pushing megabytes through the telnet helper takes
ten minutes; this moves them at LAN speed in both directions.
"""
import os, socket, struct, sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 69

def opts_of(parts):
    o = {}; rest = parts[2:]
    for i in range(0, len(rest) - 1, 2):
        if rest[i]:
            o[rest[i].decode().lower()] = rest[i + 1].decode()
    return o

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", PORT))
    print("tftpd on %d serving %s (read/write)" % (PORT, ROOT), flush=True)
    while True:
        data, peer = s.recvfrom(65536)
        if len(data) < 4:
            continue
        op = struct.unpack(">H", data[:2])[0]
        if op not in (1, 2):
            continue
        parts = data[2:].split(b"\x00")
        name = os.path.basename(parts[0].decode("utf-8", "replace"))
        opts = opts_of(parts)
        path = os.path.join(ROOT, name)
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.bind(("0.0.0.0", 0)); c.settimeout(5)
        blksize = int(opts.get("blksize", 512))
        blksize = max(8, min(blksize, 65464))
        print("%s %s from %s opts=%s" % ("RRQ" if op == 1 else "WRQ", name, peer, opts), flush=True)
        if opts:
            ack = b""
            if "blksize" in opts:
                ack += b"blksize\x00" + str(blksize).encode() + b"\x00"
            if "tsize" in opts:
                sz = os.path.getsize(path) if (op == 1 and os.path.isfile(path)) else 0
                ack += b"tsize\x00" + str(sz).encode() + b"\x00"
            c.sendto(struct.pack(">H", 6) + ack, peer)
            if op == 1:
                try: _, peer = c.recvfrom(1024)
                except socket.timeout: c.close(); continue
        if op == 1:
            if not os.path.isfile(path):
                c.sendto(struct.pack(">HH", 5, 1) + b"not found\x00", peer); c.close(); continue
            n = 0
            with open(path, "rb") as f:
                while True:
                    buf = f.read(blksize); n = (n + 1) & 0xffff
                    pkt = struct.pack(">HH", 3, n) + buf
                    for _ in range(5):
                        c.sendto(pkt, peer)
                        try:
                            a, peer = c.recvfrom(1024)
                            if len(a) >= 4 and struct.unpack(">HH", a[:4]) == (4, n): break
                        except socket.timeout: continue
                    else:
                        print("  gave up at block", n, flush=True); break
                    if len(buf) < blksize:
                        print("  sent %d blocks" % n, flush=True); break
        else:
            if not opts:
                c.sendto(struct.pack(">HH", 4, 0), peer)
            n = 0; total = 0
            with open(path, "wb") as f:
                while True:
                    try: a, peer = c.recvfrom(blksize + 4)
                    except socket.timeout:
                        print("  timeout after %d bytes" % total, flush=True); break
                    if len(a) < 4 or struct.unpack(">H", a[:2])[0] != 3:
                        continue
                    b = struct.unpack(">H", a[2:4])[0]
                    if b == ((n + 1) & 0xffff):
                        n = b; f.write(a[4:]); total += len(a) - 4
                    c.sendto(struct.pack(">HH", 4, b), peer)
                    if len(a) - 4 < blksize:
                        print("  received %d bytes" % total, flush=True); break
        c.close()

serve()

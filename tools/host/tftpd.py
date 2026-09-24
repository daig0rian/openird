#!/usr/bin/env python3
"""Read-only TFTP server, just enough for busybox 'tftp -g' on the device.

The YB1 has no wget and only busybox tftp, and pushing a megabyte through the
telnet helper takes ten minutes.  This serves files from one directory at LAN
speed instead.  RRQ only; anything else is refused.
"""
import os, socket, struct, sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 69

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", PORT))
    print("tftpd on %d serving %s" % (PORT, ROOT), flush=True)
    while True:
        data, peer = s.recvfrom(65536)
        if len(data) < 4 or struct.unpack(">H", data[:2])[0] != 1:
            continue
        parts = data[2:].split(b"\x00")
        name = parts[0].decode("utf-8", "replace")
        opts = {}
        rest = parts[2:]
        for i in range(0, len(rest) - 1, 2):
            if rest[i]:
                opts[rest[i].decode().lower()] = rest[i + 1].decode()
        path = os.path.abspath(os.path.join(ROOT, os.path.basename(name)))
        print("RRQ %s from %s opts=%s" % (name, peer, opts), flush=True)
        c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        c.bind(("0.0.0.0", 0))
        c.settimeout(5)
        if not os.path.isfile(path):
            c.sendto(struct.pack(">HH", 5, 1) + b"not found\x00", peer)
            c.close(); continue
        blksize = 512
        if "blksize" in opts:
            blksize = max(8, min(int(opts["blksize"]), 65464))
            ack = b""
            if "blksize" in opts:
                ack += b"blksize\x00" + str(blksize).encode() + b"\x00"
            if "tsize" in opts:
                ack += b"tsize\x00" + str(os.path.getsize(path)).encode() + b"\x00"
            c.sendto(struct.pack(">H", 6) + ack, peer)
            try:
                a, peer = c.recvfrom(1024)
            except socket.timeout:
                c.close(); continue
        n = 0
        with open(path, "rb") as f:
            while True:
                buf = f.read(blksize)
                n = (n + 1) & 0xffff
                pkt = struct.pack(">HH", 3, n) + buf
                for _ in range(5):
                    c.sendto(pkt, peer)
                    try:
                        a, peer = c.recvfrom(1024)
                        if len(a) >= 4 and struct.unpack(">HH", a[:4]) == (4, n):
                            break
                    except socket.timeout:
                        continue
                else:
                    print("  giving up at block", n, flush=True); break
                if len(buf) < blksize:
                    print("  sent %d blocks" % n, flush=True); break
        c.close()

serve()

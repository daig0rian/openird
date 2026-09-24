import struct, zlib, io
def crc32_mpeg(b):
    c = 0xffffffff
    for byte in b:
        c ^= byte << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04c11db7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c
def section(tid, body):
    s = bytes([tid]) + struct.pack(">H", 0xb000 | (len(body) + 4)) + body
    return s + struct.pack(">I", crc32_mpeg(s))
def pkt(pid, cc, payload, pusi=1):
    h = bytes([0x47, (0x40 if pusi else 0) | (pid >> 8), pid & 0xff, 0x10 | (cc & 0x0f)])
    d = h + payload
    return d + b"\xff" * (188 - len(d))
# PAT: program 1 -> PMT PID 0x1000
pat = section(0x00, struct.pack(">HBBB", 0x0001, 0xc1, 0, 0) + struct.pack(">HH", 1, 0xe000 | 0x1000))
# PMT: PCR 0x0100, one MPEG-2 video stream on 0x0100
pmt = section(0x02, struct.pack(">HBBB", 0x0001, 0xc1, 0, 0) +
              struct.pack(">HH", 0xe000 | 0x0100, 0xf000) +
              bytes([0x02]) + struct.pack(">HH", 0xe000 | 0x0100, 0xf000))
out = io.BytesIO(); cc = {}
def emit(pid, payload, pusi=1):
    c = cc.get(pid, 0); out.write(pkt(pid, c, payload, pusi)); cc[pid] = (c + 1) & 0x0f
for i in range(1024):
    if i % 32 == 0:
        emit(0x0000, b"\x00" + pat)
        emit(0x1000, b"\x00" + pmt)
    # a recognisable payload so a pass-through output is obvious
    emit(0x0100, bytes([0x5a, i & 0xff, (i >> 8) & 0xff, 0xa5]) + bytes(range(256))[:180], pusi=(i % 8 == 0))
    emit(0x1fff, b"", pusi=0)
data = out.getvalue()
open("in.ts", "wb").write(data)
print("wrote in.ts", len(data), "bytes =", len(data) // 188, "packets")

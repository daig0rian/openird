import struct, io, sys
def crc32_mpeg(b):
    c = 0xffffffff
    for x in b:
        c ^= x << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04c11db7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c
def section(tid, body):
    s = bytes([tid]) + struct.pack(">H", 0xb000 | (len(body) + 4)) + body
    return s + struct.pack(">I", crc32_mpeg(s))
pat = section(0x00, struct.pack(">HBBB", 1, 0xc1, 0, 0) + struct.pack(">HH", 1, 0xe000 | 0x1000))
pmt = section(0x02, struct.pack(">HBBB", 1, 0xc1, 0, 0) + struct.pack(">HH", 0xe000 | 0x0100, 0xf000) +
              bytes([0x02]) + struct.pack(">HH", 0xe000 | 0x0100, 0xf000))
out = io.BytesIO(); cc = {}; ts = 0
# 4 byte timestamp in front of every 188 byte packet = 192 byte TTS, the form
# blur's SendTTSData implies and the 0x00c0 (192) in the TSOUT blocks matches.
def emit(pid, payload, pusi=1):
    global ts
    c = cc.get(pid, 0)
    p = bytes([0x47, (0x40 if pusi else 0) | (pid >> 8), pid & 0xff, 0x10 | (c & 0x0f)]) + payload
    p = p + b"\xff" * (188 - len(p))
    out.write(struct.pack(">I", ts) + p)
    cc[pid] = (c + 1) & 0x0f
    ts = (ts + 4000) & 0x3fffffff
for i in range(1024):
    if i % 32 == 0:
        emit(0x0000, b"\x00" + pat); emit(0x1000, b"\x00" + pmt)
    emit(0x0100, bytes([0x5a, i & 0xff, (i >> 8) & 0xff, 0xa5]) + bytes(range(256))[:180], pusi=(i % 8 == 0))
    emit(0x1fff, b"", pusi=0)
d = out.getvalue()
open("in192.ts","wb").write(d)
print("wrote in192.ts", len(d), "bytes =", len(d)//192, "TTS packets of 192")

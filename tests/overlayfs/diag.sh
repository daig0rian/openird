#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
# 動作中の QEMU ゲストを QMP 経由で診断する: SysRq-w → PC 採取 → __log_buf 吸い出し。
#   使い方: diag.sh <qmp.sock> <System.map> <kernel .config> <targets/ARCH.env>
# run.sh が「コンソール出力が止まった」ときに自動で呼ぶ。手で QEMU を起動した場合も
# -qmp unix:<sock>,server,nowait を付けておけば同じように使える。
# コンソール初期化前の panic はシリアルに何も出ないが、__log_buf には残っている。
set -u
[ $# -eq 4 ] || { sed -n '5p' "$0" | sed 's/^# //'; exit 1; }
SOCK=$1; MAP=$2; CFG=$3
# shellcheck disable=SC1090
. "$4"
shift_=$(sed -n 's/^CONFIG_LOG_BUF_SHIFT=//p' "$CFG"); [ -n "$shift_" ] || shift_=14

python3 - "$SOCK" "$MAP" "$((1 << shift_))" "$KERNEL_VA_TO_PA" "$QEMU_PC_REGEX" "${QEMU_LR_REGEX:-}" <<'PY'
import bisect, json, os, re, socket, sys, tempfile, time

sock, smap, loglen = sys.argv[1], sys.argv[2], int(sys.argv[3])
va2pa, pc_re, lr_re = int(sys.argv[4], 0), sys.argv[5], sys.argv[6]

syms = []
for line in open(smap):
    p = line.split()
    if len(p) >= 3:
        syms.append((int(p[0], 16), p[2]))
syms.sort()
addrs = [a for a, _ in syms]

def sym(v):
    i = bisect.bisect_right(addrs, v) - 1
    if i >= 0 and v - syms[i][0] < 0x10000:
        return "%s+0x%x" % (syms[i][1], v - syms[i][0])
    return "?"

s = socket.socket(socket.AF_UNIX)
s.settimeout(30)
s.connect(sock)
f = s.makefile("rw")
f.readline()

def cmd(c):
    f.write(json.dumps(c) + "\n")
    f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r

cmd({"execute": "qmp_capabilities"})

print("== SysRq-w を送信（ブロック中タスクのスタックはコンソールログ側に出る）==")
cmd({"execute": "send-key", "arguments": {"keys": [
    {"type": "qcode", "data": k} for k in ("alt", "sysrq", "w")]}})
time.sleep(3)

print("== PC サンプリング ==")
for _ in range(3):
    r = cmd({"execute": "human-monitor-command",
             "arguments": {"command-line": "info registers"}})["return"]
    pc = re.search(pc_re, r)
    if not pc:
        print("PC が読めない（QEMU_PC_REGEX を確認）")
        break
    v = int(pc.group(1), 16)
    out = "PC=%08x %s" % (v, sym(v))
    lr = re.search(lr_re, r) if lr_re else None
    if lr:
        v = int(lr.group(1), 16)
        out += "  LR=%08x %s" % (v, sym(v))
    print(out)
    time.sleep(1)

cmd({"execute": "stop"})
logva = next((a for a, n in syms if n == "__log_buf"), None)
if logva is None:
    print("__log_buf が System.map に無い")
    sys.exit(0)
fd, path = tempfile.mkstemp()
os.close(fd)
cmd({"execute": "pmemsave",
     "arguments": {"val": logva - va2pa, "size": loglen, "filename": path}})
data = open(path, "rb").read()
os.unlink(path)
text = "".join(chr(b) if 32 <= b < 127 or b == 10 else "\n" for b in data)
lines = [l for l in text.split("\n") if l]
print("== __log_buf（リングバッファの物理末尾 60 行。溢れていれば順序が前後する）==")
for l in lines[-60:]:
    print(l)
PY

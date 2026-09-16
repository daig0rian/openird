#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""openird-serial - シリアルコンソールの共有ブリッジ。

1 つのプロセス（serve）がシリアルポートを握り、受信をタイムスタンプ付きでログに
残しつつ、127.0.0.1 の TCP で複数のクライアントに中継する。人は TeraTerm 等で
TCP 接続し、スクリプトや Claude Code は send / run / expect で同じコンソールを使う。

  serve   [--port COM3] [--baud 115200] [--listen 127.0.0.1:7300] [--log PATH]
  send    TEXT [--eol cr|lf|crlf|none] [--char-delay MS] [--escapes]
  run     COMMAND [--prompt REGEX] [--quiet SEC] [--timeout SEC] [--raw]
  expect  REGEX [--timeout SEC] [--send TEXT ...] [--raw]

ポートを開くとき DTR/RTS は立てない（libooter の誤割り込みを避ける）。
"""
import argparse
import datetime
import os
import re
import socket
import sys
import threading
import time

for _s in (sys.stdout, sys.stderr):            # Windows cp932 既定を回避
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_LISTEN = "127.0.0.1:7300"
EOL = {"cr": b"\r", "lf": b"\n", "crlf": b"\r\n", "none": b""}
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Za-z0-9]")


def hostport(s):
    host, _, port = s.rpartition(":")
    return host or "127.0.0.1", int(port)


def now():
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


# ---------------------------------------------------------------- serve
class Bridge:
    def __init__(self, a):
        self.a = a
        self.ser = None
        self.ser_lock = threading.Lock()
        self.clients = []
        self.clients_lock = threading.Lock()
        os.makedirs(os.path.dirname(a.log), exist_ok=True)
        self.log = open(a.log, "ab", buffering=0)
        self.bol = True

    def note(self, msg):
        print("[%s] %s" % (now(), msg), flush=True)
        self.write_log(("%s# %s\n" % ("" if self.bol else "\n", msg)).encode())

    def write_log(self, data):
        out = bytearray()
        for b in data.replace(b"\r", b""):
            if self.bol:
                out += ("[%s] " % now()).encode()
                self.bol = False
            out.append(b)
            if b == 0x0A:
                self.bol = True
        self.log.write(bytes(out))

    def open_serial(self):
        import serial
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.a.port, self.a.baud, 0.1
        s.dtr = False
        s.rts = False
        s.open()
        return s

    def serial_loop(self):
        import serial
        down_reported = False
        while True:
            if self.ser is None:
                try:
                    s = self.open_serial()
                except serial.SerialException as e:
                    if not down_reported:
                        self.note("%s を開けない（1 秒ごとに再試行）: %s" % (self.a.port, e))
                        down_reported = True
                    time.sleep(1)
                    continue
                with self.ser_lock:
                    self.ser = s
                down_reported = False
                self.note("%s を開いた（%d bps）" % (self.a.port, self.a.baud))
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except (serial.SerialException, OSError) as e:
                self.note("%s を失った: %s" % (self.a.port, e))
                with self.ser_lock:
                    try:
                        self.ser.close()
                    except Exception:
                        pass
                    self.ser = None
                continue
            if not data:
                continue
            self.write_log(data)
            with self.clients_lock:
                dead = []
                for c in self.clients:
                    try:
                        c.sendall(data)
                    except OSError:
                        dead.append(c)
                for c in dead:
                    self.clients.remove(c)

    def client_loop(self, c, addr):
        try:
            while True:
                data = c.recv(4096)
                if not data:
                    break
                with self.ser_lock:
                    if self.ser is not None:
                        try:
                            self.ser.write(data)
                        except Exception:
                            pass
        except OSError:
            pass
        finally:
            with self.clients_lock:
                if c in self.clients:
                    self.clients.remove(c)
            c.close()

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            srv.bind(hostport(self.a.listen))
        except OSError as e:
            sys.exit("✗ %s で待ち受けられない（既に serve が動いている？）: %s" % (self.a.listen, e))
        srv.listen(8)
        self.note("serve 開始: %s <-> tcp://%s  log=%s" % (self.a.port, self.a.listen, self.a.log))
        threading.Thread(target=self.serial_loop, daemon=True).start()
        try:
            while True:
                c, addr = srv.accept()
                c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with self.clients_lock:
                    self.clients.append(c)
                threading.Thread(target=self.client_loop, args=(c, addr), daemon=True).start()
        except KeyboardInterrupt:
            self.note("serve 終了")


def cmd_serve(a):
    if not a.log:
        a.log = os.path.join(ROOT, "temp", "serial", "%s.log" % a.port.replace("/", "_"))
    Bridge(a).run()


# ---------------------------------------------------------------- clients
def connect(a):
    try:
        c = socket.create_connection(hostport(a.listen), timeout=3)
    except OSError as e:
        sys.exit("✗ ブリッジに接続できない（%s）: %s\n  先に serve を起動する" % (a.listen, e))
    c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    c.settimeout(0.05)
    return c


def payload(text, a):
    if getattr(a, "escapes", False):
        text = text.encode("latin-1", "backslashreplace").decode("unicode_escape")
    return text.encode("latin-1", "replace") + EOL[a.eol]


def transmit(c, data, delay_ms):
    if delay_ms <= 0:
        c.sendall(data)
        return
    for i in range(len(data)):
        c.sendall(data[i:i + 1])
        time.sleep(delay_ms / 1000.0)


def show(buf, raw):
    if not raw:
        buf = ANSI.sub(b"", buf).replace(b"\r", b"")
    sys.stdout.write(buf.decode("utf-8", "replace"))
    sys.stdout.flush()


def strip_echo(text, echo):
    """先頭のエコーを除く。シェルが端末幅で折り返した改行は読み飛ばす。"""
    i = j = 0
    while i < len(text) and j < len(echo):
        if text[i] == echo[j]:
            i += 1
            j += 1
        elif text[i] == 0x0A:
            i += 1
        else:
            return text
    return text[i:].lstrip(b"\n") if j == len(echo) else text


def recv_some(c):
    try:
        return c.recv(4096)
    except socket.timeout:
        return b""


def cmd_send(a):
    c = connect(a)
    transmit(c, payload(a.text, a), a.char_delay)
    c.close()


def cmd_run(a):
    c = connect(a)
    transmit(c, payload(a.command, a), a.char_delay)
    prompt = re.compile(a.prompt.encode())
    buf, start, last = b"", time.time(), time.time()
    status = 2
    while time.time() - start < a.timeout:
        data = recv_some(c)
        if data:
            buf += data
            last = time.time()
            continue
        idle = time.time() - last
        tail = ANSI.sub(b"", buf).replace(b"\r", b"")
        if buf and idle >= 0.2 and prompt.search(tail):
            status = 0
            break
        if buf and a.quiet and idle >= a.quiet:
            status = 0
            break
    c.close()
    text = buf
    if not a.raw:
        text = ANSI.sub(b"", text).replace(b"\r", b"")
        text = strip_echo(text, a.command.encode("latin-1", "replace"))
        m = prompt.search(text)
        if m:
            text = text[:m.start()] + b"\n"
    show(text, True)
    if status:
        print("\n✗ timeout（%ss）" % a.timeout, file=sys.stderr)
    sys.exit(status)


def cmd_expect(a):
    c = connect(a)
    pat = re.compile(a.regex.encode())
    buf, start = b"", time.time()
    while time.time() - start < a.timeout:
        data = recv_some(c)
        if not data:
            continue
        buf += data
        show(data, a.raw)
        if pat.search(ANSI.sub(b"", buf).replace(b"\r", b"")):
            for t in a.send or []:
                transmit(c, payload(t, a), a.char_delay)
            c.close()
            print("\n✓ matched: %s" % a.regex, file=sys.stderr)
            return
    c.close()
    print("\n✗ timeout（%ss）: %s" % (a.timeout, a.regex), file=sys.stderr)
    sys.exit(2)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--listen", default=os.environ.get("OPENIRD_SERIAL_LISTEN", DEFAULT_LISTEN),
                   help="ブリッジの TCP アドレス（既定 %(default)s）")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("serve", help="シリアルポートを握ってログと TCP 中継を始める")
    s.add_argument("--port", default="COM3")
    s.add_argument("--baud", type=int, default=115200)
    s.add_argument("--log", help="既定: <repo>/temp/serial/<PORT>.log（追記）")
    s.set_defaults(func=cmd_serve)

    def tx_opts(sp, eol="cr"):
        sp.add_argument("--eol", choices=EOL, default=eol, help="末尾に付ける改行（既定 %(default)s）")
        sp.add_argument("--char-delay", type=float, default=0, metavar="MS",
                        help="1 文字ごとの待ち（libooter の長い行は 1 文字ずつ送る）")
        sp.add_argument("--escapes", action="store_true", help=r"\x03 や \r などのエスケープを解釈する")

    s = sub.add_parser("send", help="文字列を送る（応答は待たない）")
    s.add_argument("text")
    tx_opts(s)
    s.set_defaults(func=cmd_send)

    s = sub.add_parser("run", help="コマンドを送り、プロンプトが戻るまでの出力を表示する")
    s.add_argument("command")
    s.add_argument("--prompt", default=r"(^|\n)[^\n]*[#>$] $", help="出力末尾のプロンプト（正規表現）")
    s.add_argument("--quiet", type=float, default=0, metavar="SEC", help="この秒数出力が止まったら終える（0=使わない）")
    s.add_argument("--timeout", type=float, default=30)
    s.add_argument("--raw", action="store_true", help="エスケープシーケンス・エコーを残す")
    tx_opts(s)
    s.set_defaults(func=cmd_run)

    s = sub.add_parser("expect", help="正規表現が出るまで待ち、出たら --send を送る")
    s.add_argument("regex")
    s.add_argument("--timeout", type=float, default=60)
    s.add_argument("--send", action="append", metavar="TEXT", help="一致したら送る（複数可・順に送る）")
    s.add_argument("--raw", action="store_true")
    tx_opts(s)
    s.set_defaults(func=cmd_expect)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()

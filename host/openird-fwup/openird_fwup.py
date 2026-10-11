#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""openird-fwup - emma3sep 系のベンダイメージへ、更新コンテナを UPnP で投入する。

PC でコンテナを HTTP 配信し、機器の無認証 UPnP action X_UpdateFirmware にその URL を
渡す。機器が自分で取りに来て、検証・書き込み・再起動まで進む。通信は PC と機器の間
だけで、ベンダのサーバには接続しない。鍵は扱わない（コンテナは openird-mkimage が作る）。
仕様は docs/specs/emma3sep-upnp-update.md。標準ライブラリだけで動く。

  discover [--wait 3] [--timeout 6] [--iface IP]
  info     <target> [--timeout 8]
  serve    <file> [--port 8080] [--bind 0.0.0.0] [--serve-ip IP] [-v]
  flash    <target> <file> [--port 8080] [--bind 0.0.0.0] [--serve-ip IP]
           [--settle 60] [--idle 180] [--confirm-flash] [--yes] [-v]

  <target> = ip | ip:port | http://ip:port/description.xml（port の既定は 49152）

flash は既定で dry-run（X_UpdateFirmware を送らない）。--confirm-flash で実際に送り、
FLASH と打って確かめる（--yes で省略）。

終了コード: 0 受理 / 3 拒否（機器が取り直した）/ 4 未到達 / 5 不受理（SOAP が失敗）/
1 その他の誤り。「受理」は機器が書き込みに進んだという意味で、起動までは見ていない。
"""
import argparse
import hashlib
import http.client
import http.server
import os
import select
import socket
import sys
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

UA = "openird-fwup/1"
UPNP_PORT = 49152
CONTROL_SERVICE = "X_AirTunerControl"
SSDP_ADDR = ("239.255.255.250", 1900)
SEARCH_TARGETS = ("urn:schemas-pixela.jp:service:X_AirTunerControl:1",
                  "upnp:rootdevice", "ssdp:all")
MODES = ("download", "immediately", "waitTrigger")
MAX_REPLY = 4 << 20              # 機器記述・SOAP 応答として読む上限
CHUNK = 64 << 10

EXIT_OK, EXIT_ERROR, EXIT_REJECTED, EXIT_NOFETCH, EXIT_REFUSED = 0, 1, 3, 4, 5


class Error(Exception):
    pass


def reason(e):
    if isinstance(e, (TimeoutError, socket.timeout)):      # 3.9 では別の型
        return "タイムアウト"
    if isinstance(e, ConnectionRefusedError):
        return "接続を拒否された"
    return str(e) or e.__class__.__name__


# ---------------------------------------------------------------- 表示
class Console:
    """行の表示と、その場で書き換える 1 行（残り時間）を混ぜないための口。"""

    def __init__(self):
        self.lock = threading.Lock()
        self.live = ""

    def say(self, text=""):
        with self.lock:
            if self.live:
                sys.stdout.write("\r%s\r" % (" " * len(self.live)))
                self.live = ""
            print(text, flush=True)

    def status(self, text):
        if not sys.stdout.isatty():
            return
        with self.lock:
            sys.stdout.write("\r%s" % text.ljust(len(self.live)))
            sys.stdout.flush()
            self.live = text


out = Console()


def stamp():
    return time.strftime("%H:%M:%S")


# ---------------------------------------------------------------- HTTP / XML
def parse_target(t):
    """<target> を (host, port, 機器記述のパス) に直す。"""
    path = "/description.xml"
    try:
        if t.startswith("http://"):
            u = urllib.parse.urlsplit(t)
            if u.path not in ("", "/"):
                path = u.path + ("?" + u.query if u.query else "")
            host, port = u.hostname, u.port or UPNP_PORT
        else:
            host, sep, port = t.rpartition(":")
            host, port = (host, int(port)) if sep else (t, UPNP_PORT)
    except ValueError:
        host = None
    if not host:
        raise Error("<target> が読めない: %r（ip | ip:port | http://ip:port/description.xml）" % t)
    return host, port, path


def http_request(host, port, method, path, headers=(), body=b"", timeout=8.0):
    """1 回の要求を送り (status, reason, 本文) を返す。接続は使い回さない。"""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.putrequest(method, path, skip_accept_encoding=True)
        for k, v in headers:
            conn.putheader(k, v)
        conn.putheader("User-Agent", UA)
        if body:
            conn.putheader("Content-Length", str(len(body)))
        conn.putheader("Connection", "close")
        conn.endheaders(body or None)
        r = conn.getresponse()
        return r.status, r.reason, r.read(MAX_REPLY)
    except (OSError, http.client.HTTPException) as e:
        raise Error("%s:%d と通信できない: %s" % (host, port, reason(e)))
    finally:
        conn.close()


def local(tag):
    return tag.rsplit("}", 1)[-1]


def find_text(node, name):
    """局所名が name の最初の要素の中身（名前空間の接頭辞に依らない）。"""
    for e in node.iter():
        if local(e.tag) == name:
            return (e.text or "").strip()
    return ""


class Device:
    """機器記述から、表示と制御に要る分だけを取り出したもの。"""

    def __init__(self, host, port, path, xml):
        try:
            root = ET.fromstring(xml)
        except ET.ParseError as e:
            raise Error("%s:%d の機器記述を XML として読めない: %s" % (host, port, e))
        self.host, self.port = host, port
        self.base = "http://%s:%d%s" % (host, port, path)
        self.friendly_name = find_text(root, "friendlyName")
        self.manufacturer = find_text(root, "manufacturer")
        self.model = find_text(root, "modelName")
        self.udn = find_text(root, "UDN")
        self.fw_version = find_text(root, "fwVersion")
        self.services = []          # (serviceType, controlURL)
        for s in root.iter():
            if local(s.tag) == "service":
                st = find_text(s, "serviceType")
                if st:
                    self.services.append((st, find_text(s, "controlURL")))

    def control(self):
        """制御サービスの (serviceType, host, port, path)。無ければ None。"""
        for st, url in self.services:
            if CONTROL_SERVICE in st:
                u = urllib.parse.urlsplit(urllib.parse.urljoin(self.base, url))
                return st, u.hostname, u.port or self.port, u.path or "/"
        return None


def fetch_device(host, port, path, timeout):
    status, why, body = http_request(host, port, "GET", path,
                                     headers=[("Accept", "*/*")], timeout=timeout)
    if status != 200:
        raise Error("%s:%d%s: HTTP %d %s" % (host, port, path, status, why))
    return Device(host, port, path, body)


def soap_body(service_type, action, args=()):
    inner = "".join("<%s>%s</%s>" % (k, escape(v, {'"': "&quot;", "'": "&apos;"}), k)
                    for k, v in args)
    return ('<?xml version="1.0" encoding="utf-8"?>\r\n'
            '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\r\n'
            '<s:Body>\r\n'
            '<u:%s xmlns:u="%s">%s</u:%s>\r\n'
            '</s:Body>\r\n'
            '</s:Envelope>\r\n' % (action, service_type, inner, action)).encode("utf-8")


def call(dev, action, args=(), timeout=8.0):
    """制御サービスの action を呼ぶ。(HTTP status, 出力引数, fault) を返す。

    fault は失敗のとき (errorCode, errorDescription)、成功なら None。
    """
    st, host, port, path = dev.control()
    status, why, body = http_request(
        host, port, "POST", path, timeout=timeout,
        headers=[("Content-Type", 'text/xml; charset="utf-8"'),
                 ("SOAPACTION", '"%s#%s"' % (st, action))],
        body=soap_body(st, action, args))
    result, fault = {}, None
    try:
        root = ET.fromstring(body) if body.strip() else None
    except ET.ParseError:
        root = None
    if root is not None:
        code, desc = find_text(root, "errorCode"), find_text(root, "errorDescription")
        if code or desc:
            fault = (code, desc)
        elif any(local(e.tag) == "Fault" for e in root.iter()):
            fault = ("", find_text(root, "faultstring"))
        for e in root.iter():
            if local(e.tag) == action + "Response":
                result = {local(c.tag): (c.text or "").strip() for c in e}
    if fault is None and status >= 400:
        fault = ("HTTP %d" % status, why)
    return status, result, fault


# ---------------------------------------------------------------- SSDP
def source_ip(host, port):
    """host へ向かう経路の送信元アドレス。UDP の connect は何も送らない。"""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((host, port))
            return s.getsockname()[0]
    except OSError:
        return None


def local_ipv4s():
    """この PC の、LAN に使える IPv4 アドレス（ループバックとリンクローカルは除く）。"""
    found = []
    try:
        cand = [i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)]
    except OSError:
        cand = []
    for ip in cand + [source_ip(*SSDP_ADDR)]:
        if ip and not ip.startswith(("127.", "169.254.", "0.")) and ip not in found:
            found.append(ip)
    return found


def msearch(st, mx=2):
    return ("M-SEARCH * HTTP/1.1\r\n"
            "HOST: %s:%d\r\n"
            'MAN: "ssdp:discover"\r\n'
            "MX: %d\r\n"
            "ST: %s\r\n\r\n" % (SSDP_ADDR[0], SSDP_ADDR[1], mx, st)).encode("ascii")


def ssdp_location(datagram):
    """SSDP 応答の LOCATION。無ければ None。"""
    for line in datagram.decode("utf-8", "replace").split("\r\n")[1:]:
        k, sep, v = line.partition(":")
        if sep and k.strip().lower() == "location":
            return v.strip() or None
    return None


def ssdp_search(ifaces, wait):
    """NIC ごとに M-SEARCH を送り、wait 秒のあいだに返った LOCATION を集める。"""
    socks = []
    for ip in ifaces or [""]:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind((ip, 0))
            if ip:      # bind だけでは送出 NIC が決まらない OS がある
                s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
        except OSError:
            s.close()
            continue
        socks.append(s)
    if not socks:
        raise Error("SSDP を送るソケットを作れない（--iface を確かめる）")
    try:
        for s in socks:
            for st in SEARCH_TARGETS:
                for _ in range(2):          # マルチキャストは落ちることがある
                    try:
                        s.sendto(msearch(st), SSDP_ADDR)
                    except OSError:
                        pass
        found = []
        deadline = time.monotonic() + wait
        while (left := deadline - time.monotonic()) > 0:
            for s in select.select(socks, [], [], left)[0]:
                try:
                    loc = ssdp_location(s.recvfrom(8192)[0])
                except OSError:
                    continue
                if loc and loc not in found:
                    found.append(loc)
        return found
    finally:
        for s in socks:
            s.close()


# ---------------------------------------------------------------- 配信
class Feed:
    """配るファイルと、機器からのリクエストの記録。flash の判定がこれを読む。"""

    def __init__(self, data, name, verbose=False):
        self.data, self.name, self.verbose = data, name, verbose
        self.lock = threading.Lock()
        self.last = time.monotonic()    # 最後に動きがあった時刻
        self.requests = 0
        self.completed = 0              # 送り切った GET の数
        self.refetched = False          # 送り切った後に、また取りに来た

    def touch(self):
        with self.lock:
            self.last = time.monotonic()

    def request(self):
        with self.lock:
            self.last = time.monotonic()
            self.requests += 1
            if self.completed:
                self.refetched = True

    def complete(self):
        with self.lock:
            self.last = time.monotonic()
            self.completed += 1
            return self.completed

    def snapshot(self):
        with self.lock:
            return time.monotonic() - self.last, self.requests, self.completed, self.refetched


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 60

    def log_message(self, *args):
        pass

    def do_HEAD(self):
        self.reply(False)

    def do_GET(self):
        self.reply(True)

    def head(self, status, ctype, length):
        # 実機で通っている応答の形（仕様 §6.1）。Server / Date は足さない。
        self.send_response_only(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(length))
        if status == 200:
            self.send_header("Accept-Ranges", "none")
        self.send_header("Connection", "close")
        self.end_headers()

    def reply(self, with_body):
        feed = self.server.feed
        peer = "%s:%d" % self.client_address[:2]
        feed.request()
        out.say("[%s] %s %s %s %s" % (stamp(), peer, self.command, self.path,
                                      self.headers.get("User-Agent", "")))
        if feed.verbose:
            out.say("    | %s" % self.requestline)
            for k, v in self.headers.items():
                out.say("    | %s: %s" % (k, v))
        self.close_connection = True
        want = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).rsplit("/", 1)[-1]
        if want != feed.name:
            body = b"404 Not Found"
            self.head(404, "text/plain", len(body))
            if with_body:
                self.wfile.write(body)
            return
        self.head(200, "application/octet-stream", len(feed.data))
        if not with_body:
            return
        view, sent = memoryview(feed.data), 0
        try:
            while sent < len(view):
                self.wfile.write(view[sent:sent + CHUNK])
                sent += CHUNK
                feed.touch()
            self.wfile.flush()
        except OSError as e:
            out.say("[%s] %s 送信が途中で切れた（%d / %d bytes）: %s"
                    % (stamp(), peer, min(sent, len(view)), len(view), reason(e)))
            return
        out.say("[%s] %s %d bytes を送り切った（GET #%d）"
                % (stamp(), peer, len(view), feed.complete()))


class FeedServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    # Windows の SO_REUSEADDR は使用中のポートにも bind できてしまう（相乗りになる）。
    allow_reuse_address = os.name != "nt"

    def handle_error(self, request, client_address):
        out.say("[%s] %s:%d 接続の誤り: %s"
                % (stamp(), client_address[0], client_address[1], reason(sys.exc_info()[1])))


def start_server(bind, port, feed):
    try:
        server = FeedServer((bind, port), Handler)
    except OSError as e:
        raise Error("%s:%d で待ち受けられない: %s" % (bind, port, reason(e)))
    server.feed = feed
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def stop_server(server):
    server.shutdown()
    server.server_close()


def load(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise Error("%s を読めない: %s" % (path, reason(e)))
    if not data:
        raise Error("%s は空" % path)
    return data, os.path.basename(path)


def feed_url(ip, port, name):
    return "http://%s:%d/%s" % (ip, port, urllib.parse.quote(name))


def process_image():
    """このプロセスの実行ファイル。venv の python.exe は起動役で、待ち受けるのは別の実体。"""
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.kernel32.GetModuleFileNameW(None, buf, len(buf)):
            return os.path.realpath(buf.value)      # 規則に載るのはリンクを辿った先
    except (AttributeError, OSError):
        pass
    return sys.executable


def firewall_hint(port):
    if os.name != "nt":
        return
    out.say("  Windows のファイアウォールが受信を落としていないか確かめる。受信規則は")
    out.say("  プログラム単位で、待ち受けているのは次の実行ファイル:")
    out.say("    %s" % process_image())
    out.say("  TCP %d を許可する規則でもよい（管理者の PowerShell）:" % port)
    out.say('    New-NetFirewallRule -DisplayName "openird-fwup" -Direction Inbound '
            "-Protocol TCP -LocalPort %d -Action Allow" % port)
    out.say("  このプログラムを Block する規則が残っていると、そちらが優先される。")
    out.say("  焼く前の疎通は `serve` で確かめられる。")


# ---------------------------------------------------------------- サブコマンド
def show_device(dev):
    out.say("%s:%d" % (dev.host, dev.port))
    for label, value in (("friendlyName", dev.friendly_name), ("manufacturer", dev.manufacturer),
                         ("model", dev.model), ("fwVersion", dev.fw_version), ("UDN", dev.udn)):
        if value:
            out.say("  %-12s : %s" % (label, value))
    names = []
    for st, _ in dev.services:
        parts = st.split(":")
        name = parts[-2] if len(parts) >= 2 else st
        names.append(name + " [control]" if CONTROL_SERVICE in st else name)
    if names:
        out.say("  %-12s : %s" % ("services", ", ".join(names)))


def show_state(dev, timeout):
    """X_GetStateVersatile。機種によっては無いので、失敗しても先へ進める。"""
    try:
        _, state, fault = call(dev, "X_GetStateVersatile", timeout=timeout)
    except Error as e:
        out.say("  state        : %s" % e)
        return
    if fault:
        out.say("  state        : fault %s %s" % fault)
        return
    for k in ("FwVersion", "StorageState", "RecordState"):
        if state.get(k):
            out.say("  %-12s : %s" % (k, state[k]))


def cmd_discover(a):
    ifaces = [a.iface] if a.iface else local_ipv4s()
    out.say("SSDP で探す（%g 秒）: %s" % (a.wait, ", ".join(ifaces) or "既定の NIC"))
    n = 0
    for loc in ssdp_search(ifaces, a.wait):
        try:
            dev = fetch_device(*parse_target(loc), a.timeout)
        except Error:
            continue                    # 他の UPnP 機器や、読めない機器記述は黙って飛ばす
        if dev.control():
            n += 1
            out.say("%s:%d  %s  fw %s" % (dev.host, dev.port, dev.model or "?",
                                          dev.fw_version or "?"))
    if n:
        out.say("%d 台見つかった" % n)
    else:
        out.say("見つからない。IP が分かっていれば: openird-fwup info <ip>")
    return EXIT_OK


def cmd_info(a):
    dev = fetch_device(*parse_target(a.target), a.timeout)
    show_device(dev)
    if dev.control():
        show_state(dev, a.timeout)
    else:
        out.say("  （%s サービスが無い）" % CONTROL_SERVICE)
    return EXIT_OK


def cmd_serve(a):
    data, name = load(a.file)
    feed = Feed(data, name, a.verbose)
    server = start_server(a.bind, a.port, feed)
    port = server.server_address[1]
    out.say("配信: %s（%d bytes）  待ち受け %s:%d" % (a.file, len(data), a.bind, port))
    for ip in [a.serve_ip] if a.serve_ip else local_ipv4s():
        out.say("  %s" % feed_url(ip, port, name))
    out.say("SOAP は送らない。止めるのは Ctrl+C")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        return EXIT_OK
    finally:
        stop_server(server)


def watch(feed, settle, idle, port):
    """機器の取りに来かたから結果を決める（仕様 §6.4）。"""
    told = False
    while True:
        quiet, requests, completed, refetched = feed.snapshot()
        if refetched and not told:
            told = True
            out.say("✗ 機器がまた取りに来た＝コンテナは拒否された。静かになるまで配信を続ける")
        limit = settle if completed else idle
        if quiet >= limit:
            break
        out.status("%s  残り %3d 秒" % ("取り直しが止むのを待つ" if refetched else
                                       "取り直しが無いことを確かめる" if completed else
                                       "機器が取りに来るのを待つ", limit - quiet + 0.999))
        time.sleep(0.2)
    if refetched:
        out.say("✗ 拒否: 機器が取り直した（送り切った GET は %d 回）。何も書かれていない" % completed)
        return EXIT_REJECTED
    if completed:
        out.say("✓ 受理: GET 1 回のあと %g 秒取りに来ない＝機器は書き込みに進んだ" % settle)
        out.say("  起動と中身は別に確かめる（シリアルのブートログ、イメージに入れた目印）")
        return EXIT_OK
    if requests:
        out.say("✗ 未到達: リクエストは来たが、%g 秒のあいだに GET を送り切れていない" % idle)
    else:
        out.say("✗ 未到達: %g 秒待ったが、機器が取りに来ない" % idle)
        out.say("  配信 URL の IP に機器から届くか（--serve-ip）を確かめる。")
        firewall_hint(port)
    return EXIT_NOFETCH


def cmd_flash(a):
    host, port, path = parse_target(a.target)
    dev = fetch_device(host, port, path, a.timeout)
    show_device(dev)
    if not dev.control():
        raise Error("%s サービスが無い＝この機器には投入できない" % CONTROL_SERVICE)
    show_state(dev, a.timeout)

    data, name = load(a.file)
    serve_ip = a.serve_ip or source_ip(dev.host, dev.port)
    if not serve_ip:
        raise Error("配信に使う PC の IP が決められない。--serve-ip <PC の LAN 側 IP> で指定する")
    out.say("計画:")
    out.say("  target    : %s:%d (fw %s)" % (dev.host, dev.port, dev.fw_version or "?"))
    out.say("  container : %s (%d bytes)" % (a.file, len(data)))
    out.say("  sha256    : %s" % hashlib.sha256(data).hexdigest())
    out.say("  url       : %s" % (feed_url(serve_ip, a.port, name) if a.port else
                                  feed_url(serve_ip, 0, name).replace(":0/", ":<空きポート>/", 1)))
    out.say("  mode      : %s" % a.mode)

    if not a.confirm_flash:
        out.say("dry-run: SOAP は送っていない。書き込むには --confirm-flash を付ける")
        return EXIT_OK
    if not a.yes:
        sys.stderr.write("警告: 機器の NAND を書き換える。進めるなら FLASH と打つ: ")
        sys.stderr.flush()
        if sys.stdin.readline().strip() != "FLASH":
            out.say("中止（SOAP は送っていない）")
            return EXIT_ERROR

    # 機器は SOAP の応答より先に取りに来ることがある＝先に待ち受ける（仕様 §5.2）。
    feed = Feed(data, name, a.verbose)
    server = start_server(a.bind, a.port, feed)
    try:
        bound = server.server_address[1]
        url = feed_url(serve_ip, bound, name)
        out.say("配信を開始: %s（待ち受け %s:%d）。X_UpdateFirmware を送る" % (url, a.bind, bound))
        feed.touch()
        try:
            status, _, fault = call(dev, "X_UpdateFirmware",
                                    [("UpdateMode", a.mode), ("FirmwareURL", url)], a.timeout)
        except Error as e:
            if not feed.snapshot()[1]:
                raise
            out.say("⚠ SOAP の応答を読めなかったが、機器は取りに来ている＝続ける（%s）" % e)
        else:
            if fault:
                out.say("✗ 不受理: HTTP %d  fault %s %s" % (status, fault[0], fault[1]))
                return EXIT_REFUSED
            out.say("SOAP 受理（HTTP %d）。止めるなら Ctrl+C" % status)
        return watch(feed, a.settle, a.idle, bound)
    finally:
        stop_server(server)


def build_parser():
    p = argparse.ArgumentParser(
        prog="openird-fwup", formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[1], epilog="\n\n".join(__doc__.split("\n\n")[2:]))
    sub = p.add_subparsers(dest="cmd", metavar="<cmd>")

    def serving(q):
        q.add_argument("--port", type=int, default=8080, help="配信の待ち受けポート（既定 8080）")
        q.add_argument("--bind", default="0.0.0.0", help="配信の待ち受けアドレス（既定 0.0.0.0）")
        q.add_argument("--serve-ip", help="機器に渡す URL の IP（既定は機器へ向かう経路の送信元）")
        q.add_argument("-v", "--verbose", action="store_true",
                       help="機器が送ってきたリクエストのヘッダをすべて表示する")

    q = sub.add_parser("discover", help="SSDP で機器を探す（読み取りのみ）")
    q.add_argument("--wait", type=float, default=3, help="応答を待つ秒数（既定 3）")
    q.add_argument("--timeout", type=float, default=6, help="機器記述を読むときの秒数（既定 6）")
    q.add_argument("--iface", help="この IP の NIC だけから探す（既定はすべての NIC）")
    q.set_defaults(func=cmd_discover)

    q = sub.add_parser("info", help="機器記述と状態を表示する（読み取りのみ）")
    q.add_argument("target")
    q.add_argument("--timeout", type=float, default=8)
    q.set_defaults(func=cmd_info)

    q = sub.add_parser("serve", help="コンテナを配信するだけ（SOAP は送らない。疎通の確認用）")
    q.add_argument("file")
    serving(q)
    q.set_defaults(func=cmd_serve)

    q = sub.add_parser("flash", help="配信して X_UpdateFirmware を送る（書き込み。既定は dry-run）")
    q.add_argument("target")
    q.add_argument("file")
    serving(q)
    q.add_argument("--mode", choices=MODES, default="download",
                   help="UpdateMode（既定 download。実機で確かめてあるのはこれだけ）")
    q.add_argument("--settle", type=float, default=60,
                   help="GET を送り切ってから、取り直しが無いと見なすまでの秒数（既定 60）")
    q.add_argument("--idle", type=float, default=180,
                   help="GET を送り切れないまま、あきらめるまでの秒数（既定 180）")
    q.add_argument("--timeout", type=float, default=10, help="機器との SOAP / HTTP の秒数（既定 10）")
    q.add_argument("--confirm-flash", action="store_true", help="実際に X_UpdateFirmware を送る")
    q.add_argument("--yes", action="store_true", help="FLASH の入力を省く")
    q.set_defaults(func=cmd_flash)
    return p


def main(argv=None):
    for s in (sys.stdout, sys.stderr):          # Windows cp932 既定を回避
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8", errors="replace")
    p = build_parser()
    a = p.parse_args(argv)
    if not a.cmd:
        p.print_help()
        return 2
    try:
        return a.func(a)
    except Error as e:
        out.say("✗ %s" % e)
        return EXIT_ERROR
    except KeyboardInterrupt:
        out.say("中断")
        return 130


if __name__ == "__main__":
    sys.exit(main())

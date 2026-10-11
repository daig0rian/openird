#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""openird-fwup の自己完結テスト。

仕様（docs/specs/emma3sep-upnp-update.md）のとおりに振る舞う偽の機器を 127.0.0.1 に
立て、発見後の全経路（機器記述 → SOAP → 機器からの HEAD/GET → 終わり方の判定）を通す。
実機もベンダ資材も要らない。待ち受けはループバックだけなので外には何も出ない。

単独でも走る（tests/fwup/run.sh）。仕様の Status 検査に引用を渡すため、
tests/mkimage/test_mkimage.py からも run(check) で呼ばれる。
"""
import contextlib
import hashlib
import http.server
import importlib.util
import io
import os
import socket
import sys
import tempfile
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

_spec = importlib.util.spec_from_file_location(
    "openird_fwup", os.path.join(ROOT, "host", "openird-fwup", "openird_fwup.py"))
fwup = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fwup)

SPEC = "emma3sep-upnp-update"
SERVICE_TYPE = "urn:schemas-pixela.jp:service:X_AirTunerControl:1"
CONTROL_PATH = "/ctl/X_AirTunerControl"      # 実機と違うパス＝決め打ちなら通らない
DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0" xmlns:at="urn:schemas-pixela.jp:device-1-0/">
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
    <friendlyName>Fake Tuner</friendlyName>
    <manufacturer>OPENIRD TEST</manufacturer>
    <modelName>FAKE-MODEL</modelName>
    <UDN>uuid:00000000-0000-0000-0000-000000000000</UDN>
    <at:fwVersion>9.8.7</at:fwVersion>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>
        <controlURL>/upnp/control/ContentDirectory</controlURL>
      </service>
      <service>
        <serviceType>%s</serviceType>
        <controlURL>%s</controlURL>
      </service>
    </serviceList>
  </device>
</root>
""" % (SERVICE_TYPE, CONTROL_PATH)


def soap_ok(action, args=""):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><u:%sResponse xmlns:u="%s">%s</u:%sResponse></s:Body></s:Envelope>'
            % (action, SERVICE_TYPE, args, action)).encode()


def soap_fault(code, desc):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>'
            '<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>%s</errorCode>'
            '<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body>'
            '</s:Envelope>' % (code, desc)).encode()


def wget(url, method):
    """機器の取り方（仕様 §5.1）で 1 回取る。(応答の先頭行, ヘッダ, 本文) を返す。"""
    u = urllib.parse.urlsplit(url)
    req = ("%s %s HTTP/1.0\r\n"
           "User-Agent: Wget/1.12 (linux-gnu)\r\n"
           "Accept: */*\r\n"
           "Host: %s\r\n"
           "Connection: Keep-Alive\r\n\r\n" % (method, u.path, u.netloc)).encode()
    with socket.create_connection((u.hostname, u.port), timeout=10) as s:
        s.sendall(req)
        raw = b""
        while chunk := s.recv(65536):
            raw += chunk
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    return lines[0], [tuple(x.strip() for x in ln.split(":", 1)) for ln in lines[1:]], body


class FakeDevice:
    """偽の機器。scenario で X_UpdateFirmware の後の振る舞いを選ぶ。

    accept  : 200 を返してから HEAD・GET を 1 回ずつ（受理）
    early   : 200 を返す前に取りに来る
    reject  : HEAD・GET の後、GET を 3 回やり直す（拒否）
    nofetch : 200 は返すが取りに来ない
    fault   : 801 の Fault を返す
    """

    def __init__(self, scenario="accept", state=True):
        self.scenario, self.state = scenario, state
        self.posts = []         # (action, path, ヘッダ, 本文)
        self.fetches = []       # (method, 先頭行, ヘッダ, 本文)
        self.errors = []
        self.threads = []
        dev = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def send(self, status, body, ctype="text/xml"):
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/description.xml":
                    self.send(200, DESCRIPTION.encode())
                else:
                    self.send(404, b"")

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                action = self.headers.get("SOAPACTION", "").strip('"').rpartition("#")[2]
                dev.posts.append((action, self.path, dict(self.headers.items()), body))
                if self.path != CONTROL_PATH:
                    self.send(404, b"")
                elif action == "X_GetStateVersatile":
                    if dev.state:
                        self.send(200, soap_ok(action, "<RecordState>READY</RecordState>"
                                                       "<StorageState>NONE</StorageState>"
                                                       "<FwVersion>9.8.7 r1</FwVersion>"))
                    else:
                        self.send(500, soap_fault(401, "Invalid Action"))
                elif action == "X_UpdateFirmware":
                    url = [e.text for e in ET.fromstring(body).iter() if e.tag == "FirmwareURL"][0]
                    if dev.scenario == "fault":
                        self.send(500, soap_fault(801, "Failed to Download"))
                        return
                    if dev.scenario == "early":
                        dev.pull(url)
                        time.sleep(0.3)
                    self.send(200, soap_ok(action))
                    if dev.scenario in ("accept", "reject"):
                        dev.pull(url)
                else:
                    self.send(500, soap_fault(401, "Invalid Action"))

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def pull(self, url):
        def work():
            try:
                self.fetches.append(("HEAD",) + wget(url, "HEAD"))
                self.fetches.append(("GET",) + wget(url, "GET"))
                if self.scenario == "reject":
                    for _ in range(3):
                        time.sleep(0.2)
                        self.fetches.append(("GET",) + wget(url, "GET"))
            except Exception as e:                      # noqa: BLE001
                self.errors.append(repr(e))
        t = threading.Thread(target=work, daemon=True)
        self.threads.append(t)
        t.start()

    def close(self):
        for t in self.threads:
            t.join(10)
        self.server.shutdown()
        self.server.server_close()

    def updates(self):
        return [p for p in self.posts if p[0] == "X_UpdateFirmware"]


def run_cli(argv, stdin=""):
    """main を呼び、(終了コード, 表示) を返す。"""
    buf, old = io.StringIO(), sys.stdin
    sys.stdin = io.StringIO(stdin)
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            try:
                code = fwup.main(argv)
            except SystemExit as e:
                code = e.code
    finally:
        sys.stdin = old
    return code, buf.getvalue()


def flash(dev, path, *extra, stdin=""):
    return run_cli(["flash", "127.0.0.1:%d" % dev.port, path, "--bind", "127.0.0.1",
                    "--port", "0", "--settle", "1", "--idle", "2", *extra], stdin)


def run(check):
    def ck(name, cond, sec, detail=""):
        check(name, cond, detail, spec="%s#%s" % (SPEC, sec))

    tmp = tempfile.TemporaryDirectory()
    data = os.urandom(300_000)                      # 送信の 1 かたまりより大きくする
    sha = hashlib.sha256(data).hexdigest()
    path = os.path.join(tmp.name, "factory test.bin")
    with open(path, "wb") as f:
        f.write(data)

    print("[fwup] 発見（SSDP・機器記述）")
    m = fwup.msearch("upnp:rootdevice", 2)
    ck("M-SEARCH の形", m == b'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n'
       b'MAN: "ssdp:discover"\r\nMX: 2\r\nST: upnp:rootdevice\r\n\r\n', "2.1")
    reply = (b"HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\n"
             b"Location: http://192.0.2.7:49152/description.xml\r\nST: upnp:rootdevice\r\n\r\n")
    ck("SSDP 応答から LOCATION を取る（大文字小文字を問わない）",
       fwup.ssdp_location(reply) == "http://192.0.2.7:49152/description.xml", "2.1")
    ck("LOCATION の無い応答は捨てる", fwup.ssdp_location(b"HTTP/1.1 200 OK\r\nST: x\r\n\r\n") is None,
       "2.1")
    ck("<target> の 3 つの書き方",
       fwup.parse_target("192.0.2.7") == ("192.0.2.7", 49152, "/description.xml")
       and fwup.parse_target("192.0.2.7:8000") == ("192.0.2.7", 8000, "/description.xml")
       and fwup.parse_target("http://192.0.2.7:8000/d.xml") == ("192.0.2.7", 8000, "/d.xml"), "2.2")

    dev = FakeDevice()
    d = fwup.fetch_device("127.0.0.1", dev.port, "/description.xml", 5)
    ck("機器記述の要素を読む", (d.friendly_name, d.manufacturer, d.model) ==
       ("Fake Tuner", "OPENIRD TEST", "FAKE-MODEL"), "2.2")
    ck("fwVersion は接頭辞つきでも読む", d.fw_version == "9.8.7", "2.2")
    ck("制御サービスを serviceType で見分け、controlURL は機器記述から取る",
       d.control() == (SERVICE_TYPE, "127.0.0.1", dev.port, CONTROL_PATH), "2.2")

    print("[fwup] 制御（SOAP）")
    body = fwup.soap_body(SERVICE_TYPE, "X_UpdateFirmware",
                          [("UpdateMode", "download"), ("FirmwareURL", "http://h/a?b=1&c=<2>")])
    ck("SOAP 本文の形（CR LF・名前空間）", body == (
        '<?xml version="1.0" encoding="utf-8"?>\r\n'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">\r\n'
        '<s:Body>\r\n'
        '<u:X_UpdateFirmware xmlns:u="%s"><UpdateMode>download</UpdateMode>'
        '<FirmwareURL>http://h/a?b=1&amp;c=&lt;2&gt;</FirmwareURL></u:X_UpdateFirmware>\r\n'
        '</s:Body>\r\n'
        '</s:Envelope>\r\n' % SERVICE_TYPE).encode(), "3.1")
    status, state, fault = fwup.call(d, "X_GetStateVersatile", timeout=5)
    action, ppath, hdr, pbody = dev.posts[-1]
    ck("controlURL へ POST する", ppath == CONTROL_PATH, "3.1")
    ck("SOAPACTION は引用符つきの <serviceType>#<action>",
       hdr.get("SOAPACTION") == '"%s#X_GetStateVersatile"' % SERVICE_TYPE, "3.1")
    ck("Content-Type と Content-Length", hdr.get("Content-Type") == 'text/xml; charset="utf-8"'
       and hdr.get("Content-Length") == str(len(pbody)), "3.1")
    ck("成功の応答から出力引数を読む", status == 200 and fault is None
       and state.get("FwVersion") == "9.8.7 r1" and state.get("RecordState") == "READY", "3.2")
    code, text = run_cli(["info", "127.0.0.1:%d" % dev.port])
    ck("info が機器記述と FwVersion を出す", code == 0 and "FAKE-MODEL" in text
       and "9.8.7 r1" in text and "X_AirTunerControl [control]" in text, "3.3")
    dev.close()

    dev = FakeDevice(state=False)
    d = fwup.fetch_device("127.0.0.1", dev.port, "/description.xml", 5)
    status, _, fault = fwup.call(d, "X_GetStateVersatile", timeout=5)
    ck("Fault から errorCode と errorDescription を読む",
       status == 500 and fault == ("401", "Invalid Action"), "3.2")
    code, text = run_cli(["info", "127.0.0.1:%d" % dev.port])
    ck("X_GetStateVersatile が無い機種でも info は止まらない",
       code == 0 and "401 Invalid Action" in text, "3.3")

    print("[fwup] 誤操作への備え")
    code, text = flash(dev, path)
    ck("既定は dry-run＝X_UpdateFirmware を送らない",
       code == 0 and "dry-run" in text and not dev.updates(), "6.3")
    ck("dry-run でも計画（sha256・URL）は出す", sha in text and "/factory%20test.bin" in text, "6.3")
    code, text = flash(dev, path, "--confirm-flash", stdin="yes\n")
    ck("確認の語が違えば送らない", code == 1 and not dev.updates(), "6.3")
    dev.close()

    print("[fwup] 受理")
    dev = FakeDevice("accept", state=False)
    code, text = flash(dev, path, "--confirm-flash", stdin="FLASH\n")
    dev.close()
    ck("確認の語を打てば送る", len(dev.updates()) == 1, "6.3")
    args = {e.tag: e.text for e in ET.fromstring(dev.updates()[0][3]).iter()}
    url = args.get("FirmwareURL", "")
    ck("UpdateMode は download", args.get("UpdateMode") == "download", "4.2")
    ck("FirmwareURL は PC の IP・待ち受けポート・ファイル名",
       url.startswith("http://127.0.0.1:") and url.endswith("/factory%20test.bin"), "6.2")
    ck("偽の機器の取得に誤りが無い", not dev.errors, "5.1", "; ".join(dev.errors))
    kinds = [f[0] for f in dev.fetches]
    ck("HEAD 1 回・GET 1 回で終わる", kinds == ["HEAD", "GET"], "5.3", str(kinds))
    if kinds == ["HEAD", "GET"]:
        (_, hline, hhdr, hbody), (_, gline, ghdr, gbody) = dev.fetches
        ck("応答は実機で通っている形のまま", hline == "HTTP/1.1 200 OK" and hhdr == [
            ("Content-Type", "application/octet-stream"), ("Content-Length", str(len(data))),
            ("Accept-Ranges", "none"), ("Connection", "close")], "6.1", "%s %s" % (hline, hhdr))
        ck("HEAD に本文を付けない", hbody == b"", "6.1")
        ck("GET は同じヘッダで、中身がファイルと一致", ghdr == hhdr and gline == hline
           and hashlib.sha256(gbody).hexdigest() == sha, "6.1")
    ck("GET 1 回のあと取りに来なければ受理（終了コード 0）", code == 0 and "受理" in text, "6.4",
       "code=%r" % code)

    dev = FakeDevice("early")
    code, text = flash(dev, path, "--confirm-flash", "--yes", "--serve-ip", "127.0.0.1")
    dev.close()
    ck("SOAP の応答より先に取りに来ても配れる", code == 0 and not dev.errors
       and [f[0] for f in dev.fetches] == ["HEAD", "GET"], "5.2", "code=%r %s" % (code, dev.errors))
    ck("--yes で確認の入力を省ける", len(dev.updates()) == 1, "6.3")

    print("[fwup] 拒否・未到達・不受理")
    dev = FakeDevice("reject")
    code, text = flash(dev, path, "--confirm-flash", "--yes")
    dev.close()
    ck("取り直しに毎回同じ中身を返す", len(dev.fetches) == 5 and not dev.errors and all(
        hashlib.sha256(f[3]).hexdigest() == sha for f in dev.fetches[1:]), "5.4")
    ck("送り切った後にまた取りに来たら拒否（終了コード 3）", code == 3 and "拒否" in text, "6.4",
       "code=%r" % code)

    dev = FakeDevice("nofetch")
    t0 = time.monotonic()
    code, text = flash(dev, path, "--confirm-flash", "--yes")
    dev.close()
    ck("取りに来なければ未到達（終了コード 4）", code == 4 and "未到達" in text
       and time.monotonic() - t0 >= 2, "6.4", "code=%r" % code)

    dev = FakeDevice("fault")
    code, text = flash(dev, path, "--confirm-flash", "--yes")
    dev.close()
    ck("SOAP が Fault なら不受理（終了コード 5）", code == 5 and "801" in text, "6.4",
       "code=%r" % code)

    print("[fwup] 配信")
    feed = fwup.Feed(data, "factory test.bin")
    server = fwup.start_server("127.0.0.1", 0, feed)
    base = "http://127.0.0.1:%d" % server.server_address[1]
    with contextlib.redirect_stdout(io.StringIO()):         # 配信側のログは出さない
        miss = wget(base + "/other.bin", "GET")
        hit = wget(base + "/sub/dir/factory%20test.bin", "GET")
    ck("ファイル名が違えば 404", miss[0].startswith("HTTP/1.1 404"), "6.1", miss[0])
    ck("パスの最後の要素がファイル名なら返す", hit[0] == "HTTP/1.1 200 OK" and hit[2] == data, "6.1")
    quiet, requests, completed, refetched = feed.snapshot()
    ck("送り切った GET だけを数える", (requests, completed, refetched) == (2, 1, False), "6.4")
    fwup.stop_server(server)
    tmp.cleanup()


def main():
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8")
    fails = []

    def check(name, cond, detail="", spec=None):
        print("  %s %s%s" % ("✓" if cond else "✗", name, ("  — " + detail) if detail and not cond else ""))
        if not cond:
            fails.append(name)

    run(check)
    print()
    if fails:
        print("✗ %d 件失敗: %s" % (len(fails), "; ".join(fails)))
        return 1
    print("✓ すべて通過")
    return 0


if __name__ == "__main__":
    sys.exit(main())

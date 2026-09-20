# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""秘密（AES 鍵・ZIP パスワード）の読み込み。

方針は CONTRIBUTING.md §0 と update-v1b-container.md §0:

  - リポジトリ・配布物・ツールのいずれにも値を置かない。既定値も持たない。
  - 所有者が自機から回収した値を **ファイルのパス**で渡す。値そのものは
    コマンドライン引数にも環境変数にも載せない（`ps` と shell history に残るため）。
  - ツールは値を出力しない。どれを使ったかは指紋（SHA-256 の先頭 4B）だけ示す。
"""
import hashlib
import os
import sys

DEFAULT_DIR = os.path.join(os.path.expanduser("~"), ".openird", "v1b")
KEY_FILE = "aes-128-ecb.key"
PW_FILE = "zip.pass"
KEY_ENV = "OPENIRD_V1B_KEY_FILE"
PW_ENV = "OPENIRD_V1B_ZIPPW_FILE"
KEY_LEN = 16


def fingerprint(data):
    """値を晒さずに「どれを使ったか」を示すための指紋。"""
    return hashlib.sha256(data).hexdigest()[:8]


def _resolve(explicit, env_name, default_name, what):
    if explicit:
        if not os.path.exists(explicit):
            sys.exit("✗ %s が見つかりません: %s" % (what, explicit))
        return explicit
    env = os.environ.get(env_name)
    if env:
        if not os.path.exists(env):
            sys.exit("✗ %s=%s が指すファイルがありません。" % (env_name, env))
        return env
    path = os.path.join(DEFAULT_DIR, default_name)
    if os.path.exists(path):
        return path
    sys.exit(
        "✗ %s がありません。次のどれかで渡してください:\n"
        "    既定の置き場   %s\n"
        "    環境変数       %s=<path>\n"
        "    コマンドライン --%s <path>\n"
        "  値は所有者が自機から回収したものを使います。リポジトリには置かないでください\n"
        "  （CONTRIBUTING.md §0 / update-v1b-container.md §0）。"
        % (what, path, env_name, "key-file" if default_name == KEY_FILE else "zip-pw-file"))


def load_key(explicit=None):
    """AES-128 鍵を読む。16 進 32 文字（空白・':'・'0x'・'#' コメントは無視）か生 16B。"""
    path = _resolve(explicit, KEY_ENV, KEY_FILE, "AES 鍵ファイル")
    with open(path, "rb") as f:
        raw = f.read()
    text = "".join(ln.split("#", 1)[0] for ln in raw.decode("ascii", "ignore").splitlines())
    cleaned = text.replace("0x", "").replace("0X", "")
    for ch in " \t\r\n:-_,":
        cleaned = cleaned.replace(ch, "")
    if len(cleaned) == KEY_LEN * 2 and all(c in "0123456789abcdefABCDEF" for c in cleaned):
        return bytes.fromhex(cleaned), path
    if len(raw) == KEY_LEN:
        return raw, path                      # 生バイナリ
    sys.exit(
        "✗ 鍵ファイルの形式が読めません: %s\n"
        "  16 進 32 文字（AES-128 = 16 バイト）か、生 16 バイトにしてください。\n"
        "  得: %d バイト / 16 進に見える文字 %d 個" % (path, len(raw), len(cleaned)))


def load_zip_password(explicit=None):
    """ZIP パスワードを読む。末尾の改行だけ落とす（行末スペースは意味を持ちうるので残す）。"""
    path = _resolve(explicit, PW_ENV, PW_FILE, "ZIP パスワードファイル")
    with open(path, "rb") as f:
        data = f.read()
    if data.endswith(b"\r\n"):
        data = data[:-2]
    elif data.endswith(b"\n"):
        data = data[:-1]
    if not data:
        sys.exit("✗ ZIP パスワードが空です: %s" % path)
    if b"\n" in data:
        sys.exit("✗ ZIP パスワードが複数行あります: %s（1 行にしてください）" % path)
    return data, path


def report(label, value, path):
    """読んだことだけを示す。値は出さない。"""
    print("  %s: fp=%s len=%d  (%s)" % (label, fingerprint(value), len(value), path))

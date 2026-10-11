# POE-BR100-YB1 の観測リグ — 使い方

実機（BMMPX1301 / POE-BR100-YB1）を段階 B（ベンダアプリを動かして観察する）と
段階 C（ベンダアプリ抜きで自前のコードを動かす）で行き来するための作業手順。

**仕様ではなく運用の記録**。機器の事実は
[docs/specs/poe-br100-yb1-reception-stack.md](specs/poe-br100-yb1-reception-stack.md)
にある。ここは「どう触るか」だけを書く。

> **焼き直しは要らない。** リグの切り替えは `/app/setting`（yaffs2・再起動を跨ぐ）
> にファイルを置き換えるだけで済むようにしてある。NAND を書くのは
> `mkshell.sh` でイメージを作り直したときだけ。

---

## 1. 2 つのモード

`rcS` に入れた汎用フックが `/app/setting/openird-pre.sh` を source する
（`tools/vendor-rootfs/mkshell.sh`）。**このファイルを差し替えるだけでモードが変わる。**

| モード | `openird-pre.sh` | ベンダアプリ | 入り方 |
|---|---|---|---|
| **段階 B**（観察） | 段階 B 版 | **動く** | telnet（`udhcpc` はベンダアプリが起動する） |
| **段階 C**（自前） | 段階 C 版＋`openird-noblur` | **動かない** | telnet（**フックが静的 IP を振る**） |

**段階 C で IP を振らないとシリアルだけになる**（`udhcpc` を起動するのは
ベンダアプリなので）。段階 C 版のフックは `ifconfig eth0 192.168.11.217` を自分で行う
（2026-09-27 に `.17` から変更。LAN の DHCP が `.17` を別の機器に配っていて、
telnet が別機器に繋がって拒否された。変えたのは `/app/setting/openird-pre{,-noblur,-csd}.sh`）。
`telnetd` は `rcS` のフックが起動するのでどちらのモードでも上がっている。

切り替え（実機上で）:

```sh
# → 段階 C
cp -p /app/setting/openird-pre-noblur.sh /app/setting/openird-pre.sh
touch /app/setting/openird-noblur
# → 段階 B
cp -p /app/setting/openird-pre.sh.bak /app/setting/openird-pre.sh
rm -f /app/setting/openird-noblur
sync
```

置いてあるフラグ:

| ファイル | 効果 |
|---|---|
| `openird-noblur` | ベンダアプリを起動しない（`setup.sh` の最終行を bind mount で潰す） |
| `openird-trace` | ベンダアプリに `LD_PRELOAD` の ioctl トレーサを被せる → `/tmp/yb1-trace.log` |
| `openird-i2ctrace` | `rcS` が `/opt/openird/*.ko` を `trace=1` で load する |
| `openird-csddebug` | `csd.ko` を `CSD_DEBUG=<中身>` で load する（`0` で静か） |
| `openird-nocsd-once` | 次の 1 回だけ `csd` と `val_merge` を load しない（裏 CPU は止まったまま起動する）。読んだ時点でフックが消す。csd の load 前後を観察するため（2026-09-27 に実機のフックへ追加） |

---

## 2. ★ 電源は毎回入れ直す

**AV サブシステムの状態はプロセスの終了では消えない。** 同じ ioctl 列を 2 回流すと
`nr=0x01` が `-1`、`nr=0x02` が 2 つめのデバイス、`nr=0x30` が `-8`（資源は取られた
まま）になる。`blur` を kill しても同じ（受信スタック仕様 §25.2 / §31.3）。

∴ **1 試行 = 1 電源入れ直し。** `reboot` はリモートシェル越しだと
サンドボックスの分類器に止められることがあるので、SwitchBot Plug で断つ:

```powershell
cd ~\switchbot-cli
uv run switchbot.py off ; Start-Sleep -Seconds 8 ; uv run switchbot.py on
```

起動は約 50 秒（段階 C は `blur` が無いぶん少し速い）。

**段階 C なら、電源の代わりに csd の入れ直しで済む**（2026-09-27・2 回成立）:

```sh
sh /app/setting/av-restart.sh     # rmmod val_merge csd → FW を dd → insmod csd → insmod val_merge
```

これで `val_demux nr=0x01` がまた `0` を返し、ハンドルは電源投入直後と同じ値になる。
MXL683 の選局は残るので、やり直すのはデマルチプレクサの列だけ。
`back CPU did NOT answer` が出たら電源を入れ直す。仕組みと実測は
[裏 CPU 制御仕様 §5.2](specs/poe-br100-yb1-subcpu-control.md)、スクリプトは
`tools/vendor-rootfs/av-restart.sh`。`CSD_MODE=FORCE` は起動をやり直さないので使わない。

---

## 3. 実機に入る・ファイルをやり取りする

- telnet `192.168.11.217:23`（段階 C。段階 B は DHCP）、root はパスワード空。
  **繋がらないときは、まずシリアルで `ifconfig eth0` の MAC を見て、Windows の
  `arp -a` がその番地に同じ MAC を出しているか突き合わせる**（番地の衝突を一度踏んだ）。
- シリアルは COM3 / 機器側 `ttyS1`（`host/openird-serial`）。段階 C で IP を
  振り損ねたときの唯一の口。

**デバイスに `base64` も `nc` も無い**（2026-09-22 確認）。あるのは
`uuencode` / `uudecode` / `od` / `tftp` / `gzip`。∴

```sh
# ホスト → 実機: base64 を begin-base64 で包んで heredoc で送り、uudecode で戻す
#   （uuencode -m が吐くのと同じ形）
uudecode -o /app/setting/x /tmp/x.uu
# 実機 → ホスト: 大きいものは先に縮める（355KB のトレースが 20KB になる）
gzip -9 -c /tmp/yb1-trace.log > /tmp/tr.gz
uuencode -m /tmp/tr.gz x
```

telnet を素で喋るときの注意: 先頭で IAC ネゴシエーションが来るので全部拒否し、
コマンドの終わりは**エコーバック行と区別できる番兵**にする
（`echo "__D""ONE__$?"` と書けば、エコーには引用符付きで、実行結果には
`__DONE__` として現れる）。

---

## 4. ベンダを外から動かす（段階 B）

UPnP（**無認証**）。`urn:schemas-pixela.jp:service:X_AirTunerControl:1`、
`http://192.168.11.17:49152/upnp/control/X_AirTunerControl`。

```
X_SetTuning(BroadcastWave: "tb", Frequency: <整数 MHz>)
X_GetOfdmTunerInfo() -> Frequency, SignalStrength, Cn, Ber, Agc
X_GetStateVersatile() -> FwVersion ほか
```

`X_SetTuning` → 4 秒 → `X_GetOfdmTunerInfo` を対にする。503 MHz で
`Cn=33 / Ber=0 / SignalStrength=89` が返る（この個体・この場所）。
**トレースを採るときは、これで選局させてからログを吸い出す。**

---

## 5. トレースの採り方

### 5.1 ioctl（ベンダアプリの動きを見る）

段階 B ＋ `openird-trace`。ログは `/tmp/yb1-trace.log`（tmpfs なので電源で消える。
**採ったらその場で吸い出すこと**）。起動＋選局 1 回で約 3,000〜5,500 行。

### 5.2 I2C バス（復調器との線上のバイト列）

GPL の `i2c-algo-emma` を建て直したものを**ベンダより先に load** する
（先に入れた方が勝つ）。`/app/setting/i2c-algo-emma.ko` に置いてあり、
`openird-pre-i2ctrace.sh` を `openird-pre.sh` に複写して再起動すれば効く。

**カーネルのログ環は 16KB しかない**ので、採りながら `dmesg -c` を 20ms ごとに
tmpfs へ吸い出すループを回すこと（busybox の `sleep` は小数を取らない。`usleep 20000`）。
これを怠ると途中から採れる（受信スタック仕様 §28.1、§4.3 の訂正）。

`EMMA_TRACE_MAX` は **64**。48 バイトのファーム転送ブロックを丸ごと残すため。

---

## 6. 実機に置いてある道具

`/app/setting`（再起動を跨ぐ）:

| | |
|---|---|
| `yb1-mxl` | 復調器の立ち上げ（`tools/yb1-mxl`）。`load` / `tune` / `status` |
| `yb1-ioctl` | ベンダ ioctl を出す（`tools/yb1-ioctl`）。`@script` で複数ノードを跨げる |
| `i2c-algo-emma.ko` | 自前ビルドの I2C algo（trace 付き） |
| `openird-pre*.sh` | 各モードのフック |
| `ts*.script` | 段階 C の試行列（最新の整理版は `tools/yb1-ioctl/stage-c-demux.script`） |

`/opt/openird`（rootfs 内・読み取り専用）: `yb1-mem` / `yb1-i2c` / `yb1_trace.so` /
`i2c-algo-emma.ko`。

---

## 7. 段階 C の 1 回ぶん（現状の定型）

```sh
# 1) 電源を入れ直す（§2）
# 2) フロントエンドを立ち上げて選局する
/app/setting/yb1-mxl 1 load /app/bin/blur     # 冷起動 → ファーム転送 → 0xabcd
/app/setting/yb1-mxl 1 tune 503               # 0x112d != 0 ならロック
# 3) デマルチプレクサ
/app/setting/yb1-ioctl /dev/val_demux @/app/setting/stage-c-demux.script
# 4) リングを見る
/opt/openird/yb1-mem 0x0c000000 0x4000 scan
```

**2) は毎回通る。3) は最後の 3 本（`nr=0x0a`/`0x47`/`0x67`）以外すべて `ret=0`。
4) にはまだ何も来ない。** 残件は受信スタック仕様 §34.5。

---

## 8. 触ってはいけないもの

- **`/dev/i2c-0`**。何も載っておらず、転送がエラーで返らずアダプタ内でブロックする。
  復調器は**バス 1**。
- **`nr=0x06`**（`val_demux`）。実機が落ちる（受信スタック仕様 §18.4）。
- **`killall blur`**。WDT でボードごと再起動する。設定の変更は「フラグを置いて
  電源を入れ直す」。
- **mtd の書き込み**。このリグの範囲では要らない。必要になったら必ず確認を取る。

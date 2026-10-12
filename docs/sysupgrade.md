# sysupgrade — OpenIRD から OpenIRD への更新

OpenIRD を入れた機器には、メーカーの更新機構がもう無い。次の版に上げる手段は、いま動いている
OpenIRD 自身が持つ。それが `sysupgrade` で、作りは OpenWrt のものを踏襲している。

**共通にするのは枠で、フラッシュへの書き方は機種ごとの部品。** ブートローダに頼むことは無く、
ブートローダの違いは、ビルドのときのイメージの形（`openird-mkimage`）と、機種ごとの数行に閉じる。

| 層 | 場所 | すること |
|---|---|---|
| 共通の入口 | `/sbin/sysupgrade` | イメージの検査、RAM への用意、init への引き継ぎ。自分ではフラッシュを書かない |
| 共通の stage2 | `/lib/upgrade/upgraded`・`stage2` | init から PID 1 を受け取り、RAM の上だけで動いて、機種の部品を呼び、再起動する |
| 機種ごとの部品 | `/lib/upgrade/platform.sh` | `platform_check_image` と `platform_do_upgrade` |

共通の 2 層は `ird-base-files`、機種の部品は各機種の `rootfs_overlay` にある。部品の無い機種では、
`sysupgrade` は「この機種では未実装」と言って終わる。

## 1. 使い方

```
sysupgrade [-n] [-T] [--dry-run] <更新イメージのファイル | http(s) の URL>
```

| オプション | 意味 |
|---|---|
| （なし） | 更新する。設定とアドオン（overlay）は残る |
| `-n` | 設定を引き継がない。overlay を空にして、新しい版の既定から始める |
| `-T` | イメージを検査して終わる。何も書かない |
| `--dry-run` | 引き継ぎと再起動まで通すが、フラッシュには書かない（新しい機種で枠を確かめるため） |

- 始めると、動いているものは全部止まる（SSH のセッションも）。書いている間は LED が橙になり
  （ある機種では）、終わると自分で再起動する。**その間、電源を切らない。**
- URL から取るときは BusyBox の `wget` を使う。HTTPS の証明書は検証されない（アドオンの索引と
  同じ・[addon.md](addon.md) §4）。中身が壊れていないことは、イメージの中の SHA-256 で分かるが、
  **誰が作ったものかは保証しない**（署名は無い）。

## 2. 更新イメージの形式

`openird-<版>-<family>-<device>-sysupgrade.bin` は無圧縮の tar。

```
sysupgrade-<board>/CONTROL
sysupgrade-<board>/<名前>      機種が書くファイル（emma3sep では kernel と root）
```

`<board>` は defconfig の名前（機器の `/etc/openird_release` の `OPENIRD_BOARD`）。メンバは、この
1 つのディレクトリと、その直下の通常のファイルだけ。`CONTROL` は ASCII の `KEY=値`:

| キー | 内容 |
|---|---|
| `BOARD` | 対象の機種。機器の `OPENIRD_BOARD` と違えば、受け取らない |
| `VERSION` | 版（表示用） |
| `COMPAT` | 設定を引き継げる世代。機器の値（`/lib/upgrade/common.sh` の `OPENIRD_COMPAT`）と違えば、`-n` を求める |
| `FILES` | 入っているファイルの名前（空白区切り） |
| `SHA256_<名前>` | そのファイルの SHA-256 |

`COMPAT` は、新しい base が古い overlay をもう使えなくなる変更を入れたときにだけ上げる。

鍵は関わらない（factory イメージと違い、メーカーの容器に包まない）。同じ入力からは同じバイト列に
なる。作るのは `openird-mkimage pack-sysupgrade`、中身を見るのは `probe-sysupgrade`。

## 3. 機器の中の流れ

1. **検査と用意**（`/sbin/sysupgrade`・動いている系の中）: tar の形 → `CONTROL` → 機種 → 各ファイルの
   SHA-256 → 世代 → `platform_check_image`。通ったら、`/tmp/sysupgrade` に、イメージのファイル・
   BusyBox・機種の道具（`RAMFS_COPY_BIN`）・更新のスクリプトを写す。
2. **引き継ぎ**: init に SIGQUIT を送る。BusyBox の init は、停止処理（`rcK`・`umount -a -r`）を
   走らせ、ほかの全プロセスを止めてから、inittab の `restart` の行（`/lib/upgrade/upgraded`）を
   **自分の代わりに PID 1 として**実行する。`upgraded` は、用意ができていれば RAM の BusyBox で
   `stage2` を始め、できていなければ再起動する。
3. **書き込み**（`stage2`・PID 1・RAM の上だけ）: `platform_do_upgrade` を呼び、終わったら再起動する。

フラッシュの上の rootfs から動いているものが残っていると、書き換えた後のブロックを読んで倒れる。
PID 1 まで RAM に移すのはそのため。OpenWrt は同じことを procd の `upgraded` でしている。

`stage2` と `platform_do_upgrade` の決まり:

- **rootfs の上のものを使わない。** `PATH` は `/tmp/sysupgrade/bin` だけ。
- **終わらない。** PID 1 なので、終わるとカーネルが止まる。失敗しても、再起動まで進む。

## 4. 機種ごとの部品

`/lib/upgrade/platform.sh` は、次を持つ。

| 名前 | 内容 |
|---|---|
| `platform_check_image <dir>` | `<dir>` の中身がこの機種のものか（区画の並び・マジック・大きさ）。違えば、理由を言って 1 を返す |
| `platform_do_upgrade <dir>` | 書く。成功なら 0。`exit` しない |
| `RAMFS_COPY_BIN` | 書くのに要る、BusyBox 以外のプログラム |

`/lib/upgrade/common.sh` の道具を使える: `file_size`・`file_magic`・`mtd_size`・`mtd_erasesize`・
`control_get`、そして `upgrade_parts <file> <mtdN> [...]`。

**`upgrade_parts`** は、並べた順に各区画へ書いて読み戻す。書く前に、新しいイメージが覆う分の古い中身を
RAM に取っておき、照合が 2 回合わなければ、**それまでに書いた区画も含めて全部を元に戻す**
（次に起動するものを、版の混ざった状態にしないため）。

### emma3sep（POE-BR100-YB1）

[platform.sh](../br-ext-chip-renesas/board/emma3sep/rootfs_overlay/lib/upgrade/platform.sh)。
カーネル（uImage）を mtd2、rootfs（squashfs）を mtd3 に、`mtd` で書く。順は rootfs → カーネル。
検査は、128MB 機の区画の並び、uImage と squashfs のマジック、大きさの上限（カーネル `0x280000`＝
ブートローダの展開の余裕、rootfs `0x680000`）。

## 5. 失敗と限界

| 起きること | どうなるか |
|---|---|
| イメージが違う・壊れている | 検査で止まる。何も書かれない |
| 書いた区画の照合が合わない | 書き直す。それでも合わなければ、全部を元に戻して再起動する（前の版が起動する） |
| **書いている最中に電源が落ちる** | **起動しなくなることがある。** 面が 1 つしか無い機種（emma3sep）では、戻す手段は書き込み器かブートローダの解錠だけ。書いている時間は約 2 秒（§8） |
| 新しい版が起動するが、動きがおかしい | 前の版の更新イメージで、もう一度 `sysupgrade` する（入れるなら `-n` も） |
| 新しい版が起動しない | 上の電源断と同じ。**出す前に実機で確かめたイメージだけを配る**（[release.md](release.md)） |

電源断に強くしたい機種は、その機種の `platform_do_upgrade` の中で別の書き方にできる（2 面を持つ
機種で、使っていない面に書くなど）。枠は変えない。

## 6. 作り方

```
make BOARD=<board> && make kernel BOARD=<board>
make sysupgrade BOARD=<board>     # → output/<board>/images/openird-<版>-…-sysupgrade.bin
```

検査は factory イメージと同じもの（uImage の形・展開の余裕・squashfs の版・書き込み固定長）。
機種が更新イメージに対応しているかは、`host/openird-mkimage/boards/<機種>.json` の `sysupgrade` で
決まる。リリースでは CI の build ジョブが作り、配布物に入る（鍵のジョブは関わらない）。

## 7. テスト

- `make test-sysupgrade`（[tests/sysupgrade/run.sh](../tests/sysupgrade/run.sh)）: イメージの生成と
  検査、RAM への用意、`upgrade_parts` の書く・照合する・戻す、overlay の初期化を、偽の機器ツリーで
  確かめる。`BUSYBOX=<パス>` を渡すと、機器側のスクリプトを BusyBox だけで走らせる。**機器と同じ
  設定の BusyBox で走らせること**（道具のオプションの有無が違う。`stat -c` の無い設定で、それを
  使ったコードがホストのテストだけ通ったことがある）。
- [tests/sysupgrade/selftest-device.sh](../tests/sysupgrade/selftest-device.sh): 同じ「書く・照合する・
  戻す」を、**機器の上で、その機器の BusyBox で**確かめる。`/tmp` の普通のファイルが相手で、
  フラッシュには触れない。新しい機種に部品を足すときは、まずこれを通す。
- init から PID 1 を引き継ぐところは、実機でしか確かめられない。`sysupgrade --dry-run` が、書かずに
  そこまでを通す。

## 8. 実装状態

| 項目 | 状態 |
|---|---|
| 形式・`pack-sysupgrade`・`probe-sysupgrade` | 実装済み・テストあり |
| 共通の入口と stage2 | 実装済み・テストあり・実機で確認 |
| emma3sep の部品 | 実装済み・実機で確認 |
| mb86h61 の部品 | 未実装（`sysupgrade` は「未実装」と言って終わる） |
| 署名 | 無い |

**実機での確認**（POE-BR100-YB1・2026-10-12）:

| したこと | 結果 |
|---|---|
| `selftest-device.sh`（機器の BusyBox） | 8 項目とも通過。最初は `stat -c` が無くて落ちた（直した） |
| `sysupgrade -T` | 正しいイメージを受け取り、1 バイト壊したものは `checksum mismatch` で断る |
| `sysupgrade --dry-run` | init が停止処理を走らせて全プロセスを止め、PID 1 が RAM の BusyBox に移る → mtd3 の 9 ブロックと mtd2 の 16 ブロックを保存 → 書かずに再起動 → 起動 |
| `sysupgrade <イメージ>`（2 回） | rootfs とカーネルを書いて再起動し、新しい版が起動する。起動後、両区画の読み戻しがイメージの SHA-256 と一致。overlay（SSH のホスト鍵）は残り、受信は連続性エラー 0・同期エラー 0 |

- かかる時間: コマンドを打ってから新しい版が起動するまで約 20 秒。そのうち、フラッシュを
  書いているのは約 2 秒（rootfs 1.1MB に 0.6 秒、カーネル 2.1MB に 1.1 秒。照合を含む）。
- 最初の `--dry-run` で、stage2 が `/tmp` を書き込み可能に戻せていないことが分かった（`/proc` を
  戻す前に `mount` を呼んでいた）。古い中身を保存できず、何も書かずに再起動した＝安全な側に
  倒れた。直してから本番を通した。
- **実機で確かめていないもの**: `-n`（実機の overlay は消していない。偽のツリーで確認）、URL からの
  取得、照合が合わないときの巻き戻し（実機のフラッシュでは起こしていない。偽の `mtd` で確認）、
  書いている最中の電源断。

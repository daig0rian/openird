# POE-BR100-YB1 の factory 経路 — 確定事項と未解明点

このファイルは `openird-mkimage` の emma3sep バックエンドの実装方針に効く
「残っている不確実性」を管理する。確定した形式・機構は `docs/specs/` にあるので、
ここには重複を置かない。

## 経路の位置づけ

SB-TV02-WFPL と違い、**この機種には開発者向けの直接書き込み経路が無い**
（u-boot はインタラクティブ入力にロックがかかる）。∴ 純正の更新コンテナ
`BLUR_UPDATE_v1b` を UPnP で流し込む factory 経路が本線になる。

形式は [update-v1b-container.md](../../docs/specs/update-v1b-container.md)、
機器側の書き込み機構は
[poe-br100-yb1-nand-update.md](../../docs/specs/poe-br100-yb1-nand-update.md)。

## 実装の段取り

| 段 | 内容 | 状態 |
|---|---|---|
| P0 | ボード定義 ＋ `probe-factory`（鍵不要のヘッダ観察） | **完了** |
| P1 | `unpack-factory`（AES-128-ECB → ZipCrypto → 無暗号 ZIP） | **実装済み**。合成コンテナで往復を確認。純正コンテナでの検証は鍵ファイル設置待ち |
| P1.5 | 平文から V1/V2 を仕様化（`docs/specs/` を更新） | 未着手（P1 の実行待ち） |
| P2a | `ird/zipcrypto.py`（ZipCrypto 付き ZIP の書き出し） | **完了**。V1/V2 に依存しないので先に片付けた |
| P2b | `repack-factory`（純正の組み直し・ペイロードの差し替え） | **完了** |
| P2c | ベンダ rootfs を 3.1BE で往復する道具（`scripts/fetch-squashfs3.sh` ＋ `tools/vendor-rootfs/mkshell.sh`） | **完了** |
| P2d | `pack-factory`（OpenIRD 自身の kernel/rootfs から factory 像を作る） | 未着手 |
| P3 | UPnP 投入クライアント（別ツール・鍵を扱わない） | 未着手 |

**unpack を pack より先に作る。** 仕様 §7 の U1/U2 は「鍵回収後に復号平文で検算すれば
即決着」と仕様自身が書いており、かつ pack に必要な V1（下記）は平文を見ないと
仕様化できないため。

## 未解明点（推測で埋めない）

| # | 不明点 | 状態 | 影響 |
|---|---|---|---|
| V3b | OpenIRD の rootfs を squashfs 3.1BE で作るか、mainline 4.0 をベンダカーネルに backport して 4.0LE にするか | **未決（設計判断）** | 高。Buildroot の mksquashfs は 4.x ＝ 3.1BE を作れない。overlayfs を 2.6.31 に移植したのと同じ手口で 4.0 を 2.6.27 に持ち込むのが素直だが、未着手 |
| V7 | h20/h30 の算出式（仕様 §7 U1/U2） | **推定は反証された**（平文 MD5 / CRC32 ではない） | 低。**pack のブロッカーではない** — 純正から複写すれば「無改変 repack」は成立する。任意値でも通るかは仕様 §7 U3 の実験で決める |
| V8 | 自作コンテナが純正アプリに受理されるか | **一度実機で試して原因特定・修正済み。再試行待ち** | 下記「2026-09-21 の失敗」を参照 |

### 投入の初弾は「純正を無改変で repack」にする

V6 があるため、自作コンテナの初回投入では **中身を変えない**。純正コンテナを
`unpack-factory` して、そのままのペイロードを `pack-factory` で包み直したものを
投入する。受理されれば容器の実装が正しいと確定し、中身の差し替えを次の一手に
分離できる。OEM 形式を実装するときの定石。

仕様 §7 U3（h20/h30 が適用可否の判定に使われるか）の決定的実験は、**その後**に行う。
初弾が通っていない状態で拒否されても、原因を切り分けられないため。

## 容器の各層をどう裏取りしたか（実機投入なし・2026-09-21）

`repack-factory` で純正 BLUR v2.1.17 を組み直し、層ごとに独立した手段で確かめた。

| 層 | 確かめ方 | 結果 |
|---|---|---|
| ヘッダ 332B ＋ AES-128-ECB | `--rebuild none` で平文をそのまま再暗号化 | **原本と 17,371,468 バイト完全一致**（md5 も同一）＝仕様どおりと確定 |
| ZipCrypto（中間 ZIP） | 自前生成を **Python stdlib の復号器**（独立実装）で読み返す | 全エントリ復号・csum 一致 |
| ペイロード ZIP（無暗号） | 自前生成を **Info-ZIP `unzip -t`**（独立実装・純正の書き手と同系）で検査 | 9 エントリ全数 CRC OK。名前・サイズ・日時も保持 |

∴ **実機に流して初めて分かることは V8 だけ**に絞り込めた。バイト一致した方
（`--rebuild none`）を投入する意味は無い（原本そのもの）。投入するなら
`--rebuild all` の方で、これは ZIP 層を自前で組んだ別バイト列（+9,504B）。

## 手順 1b の像（root シェル入りベンダ rootfs）— 2026-09-21

段階 B の観察環境を、純正との差が最小の状態で用意する。差が大きいほどベンダアプリが
落ちる要因が増えて観察にならない（SB-TV02 での教訓）。

ベンダ rootfs には **シリアル getty の行が最初からコメントアウトで入っており、root は
パスワード空**（`/etc/shadow` が `root::`）。busybox の telnetd も同梱されている。
∴ 必要な改変は **`/etc/inittab` の `#` を 1 つ外すだけ**。

```
scripts/fetch-squashfs3.sh                          # mksquashfs 3.4（-be が要る）
tools/vendor-rootfs/mkshell.sh <in.img> <out.img>   # fakeroot 下で展開→1行外す→3.1BE で梱包
openird_mkimage.py repack-factory <純正.bin> <out.bin> --replace rootfs.img=<out.img>
```

**`fakeroot` 必須**。一般ユーザで `unsquashfs` するとデバイスノード 223 個が黙って
作られず、`/dev/console` の無い rootfs ができて起動が無言で死ぬ。`mkshell.sh` は
生成後に char/block/symlink の数と mode/owner を元イメージと突き合わせて検証する。

検証（生成コンテナをほどいて純正 rootfs と全ファイル比較）:
**差分は `/etc/inittab` の 1 ファイル・1 行のみ**。特殊ファイル 223 個・シンボリック
リンク 291 個・setuid 1 個はすべて保たれている。

## 2026-09-21 の失敗と原因（ファイルモード）

手順 1b の像を実機に投入 → **更新が何も起こらず普通に起動**。シリアルログには
`using update environment on mtd1` が出ず、u-boot の更新段に入っていなかった。
族違いのときの症状（UPnP の GET が 20 回ほど繰り返される）は出ていない＝
ダウンロードとアプリ側の受理は通っている。

原因は**ペイロード ZIP のファイルモード**。ベンダの平文スクリプト `/app/update.sh`
は展開したスクリプトを `./write.sh` の形で**実行する**のに、自前生成の ZIP は

- 全エントリの external attributes が 0（＝モード 0000）
- 「作成した版」が MS-DOS 扱い（＝展開側が Unix モードを見ない）

だったため実行ビットが落ち、`./write.sh` が Permission denied → `update()` が
`return 1` → env が書かれない → 何も起きない、という経路をたどった。**どの段でも
エラーは表に出ない**のが厄介なところ。

対処: `ird/zipcrypto.py` にエントリ単位の external attributes と
`version made by = Unix 3.0` を持たせ、`.sh` は 0755 / `.img` は 0644 にした。
機器と同じ Info-ZIP で展開して `-rwxr-xr-x write.sh` になることを確認済み。
tests/mkimage に回帰検査を追加した。

**教訓**: 容器の形式だけを仕様化していたが、機器側が**何をどう実行するか**まで
追わないと足りない。ベンダの平文スクリプトを読むのが最短だった。

## 解決済み（記録）

- **旧 V1 — `write.sh` の呼び出し契約**（2026-09-20・純正 BLUR v2.1.17 の復号平文より）…
  決着。cwd は `/flash/tmp_update`、source される、env は
  `/app/bin/nand_flash_write -u /dev/mtd1` で書く、RAM ステージングは 1Gb=`0x83000000` /
  512Mb=`0x86000000`。全文は
  [poe-br100-yb1-nand-update.md](../../docs/specs/poe-br100-yb1-nand-update.md) §3.2。
  **`/app/bin/nand_flash_write` は呼んでよい**（factory 実行時点の機器はまだベンダ状態で、
  そのツールは機器上にある。配布物に含めるわけではない）。
- **旧 V2 — ペイロード ZIP の圧縮方式**… 全エントリ **deflate**。`store` は使われない。
- **旧 V5b — 512Mb 機の書き込み固定長**… `write512Mb.sh` の実測で確定（同 §5）。
  rootfs の上限は 1Gb 機と同じ 6.5MB、kernel だけ 3.0MB → 2.0MB。
- **旧 V6 — `/flash/tmp_update` への展開**… 展開先パスが確定し、**展開は純正アプリ側が行う**
  ＝ OpenIRD 側で再現する必要が無いと判明。pack のブロッカーではなくなった。
- **ZIP 末尾の詰め物**… `0x00` を 16B 境界まで。純正は余分に足していることもあるが、
  ちょうど最小の純正コンテナが複数あるので最小で足りる。

- **旧 V3 — rootfs の squashfs 世代とエンディアン**（2026-09-20）… **3.1 ビッグエンディアン**。
  GPL の `fs/squashfs/{inode.c,squashfs2_0.c}` と `include/linux/squashfs_fs.h`
  （`SQUASHFS_MAJOR 3` / `SQUASHFS_MINOR 1`）＝ mainline in-tree（4.0・2.6.29〜）ではなく
  patch 版。実機 NAND ダンプの mtd3/4/5 はいずれも BE マジック `sqsh` を持つ。
  → 残る判断は V3b。
- **旧 V4 — ダウングレード拒否の有無**（実機検証済）… **拒否されない**。純正イメージで確認。
  ∴ `--fw-version` に版数の順序制約を設ける必要はない。
- **旧 V5 — 512Mb 機の区画表**… GPL `drivers/mtd/nand/nand_emma_ptt_512Mb.h` に全数あり。
  Kernel 起点 `0x380000` は仕様 §5 の部分観察と一致。**書き込み固定長だけが未確定**（V5b）。
- **族タグの綴りと LINDBERG の実在**（2026-09-20）… `probe-factory` で手元 29 本を全数検査し、
  族は `BLUR` / `BR100-YB5` / `BR310L` / **`LINDBERG`**（`LINDBERGH` ではない）の 4 系統、
  h38 = SHA-1(本体) が 29/29 一致、本体長も全数 16B 境界。

> ベンダコンテナ・NAND ダンプ（`temp/`）は解析専用。commit しない。
> 鍵・ZIP パスワードはリポジトリにもツールにも置かない（CONTRIBUTING.md §0）。

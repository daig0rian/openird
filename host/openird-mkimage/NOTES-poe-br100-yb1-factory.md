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
| P2b | ペイロード生成（`write.sh` 系・csum）＋ `pack-factory` ＋ `verify-factory` | 未着手（V1/V2 待ち） |
| P3 | UPnP 投入クライアント（別ツール・鍵を扱わない） | 未着手 |

**unpack を pack より先に作る。** 仕様 §7 の U1/U2 は「鍵回収後に復号平文で検算すれば
即決着」と仕様自身が書いており、かつ pack に必要な V1（下記）は平文を見ないと
仕様化できないため。

## 未解明点（推測で埋めない）

| # | 不明点 | 状態 | 影響 |
|---|---|---|---|
| V3b | OpenIRD の rootfs を squashfs 3.1BE で作るか、mainline 4.0 をベンダカーネルに backport して 4.0LE にするか | **未決（設計判断）** | 高。Buildroot の mksquashfs は 4.x ＝ 3.1BE を作れない。overlayfs を 2.6.31 に移植したのと同じ手口で 4.0 を 2.6.27 に持ち込むのが素直だが、未着手 |
| V7 | h20/h30 の算出式（仕様 §7 U1/U2） | **推定は反証された**（平文 MD5 / CRC32 ではない） | 低。**pack のブロッカーではない** — 純正から複写すれば「無改変 repack」は成立する。任意値でも通るかは仕様 §7 U3 の実験で決める |
| V8 | 自作コンテナが純正アプリに受理されるか | 未検証 | **高。ここだけが本当の関門として残る**。P3 の初弾（無改変 repack）で決める |

### 投入の初弾は「純正を無改変で repack」にする

V6 があるため、自作コンテナの初回投入では **中身を変えない**。純正コンテナを
`unpack-factory` して、そのままのペイロードを `pack-factory` で包み直したものを
投入する。受理されれば容器の実装が正しいと確定し、中身の差し替えを次の一手に
分離できる。OEM 形式を実装するときの定石。

仕様 §7 U3（h20/h30 が適用可否の判定に使われるか）の決定的実験は、**その後**に行う。
初弾が通っていない状態で拒否されても、原因を切り分けられないため。

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

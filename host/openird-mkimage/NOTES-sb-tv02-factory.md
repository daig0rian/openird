# SB-TV02-WFPL の導入経路 — 確定事項と未解明点

このファイルは `openird-mkimage` の実装方針に効く「残っている不確実性」を管理する。
確定した形式・機構は `docs/specs/` にあるので、ここには重複を置かない。

## 確定している経路（開発者向け・実機で検証済み）

libooter のシリアルシェルから root で起動し、機器上のツールで MTD を直接書く。
暗号ゲートは無い。

1. `openird_mkimage.py pack-kernel` … 生カーネル `Image` を RDHB で梱包 → mtd5
   （形式: [rdhb-kernel-container.md](../../docs/specs/rdhb-kernel-container.md)）
2. `openird_mkimage.py pack-rootfs` … squashfs を検査して rootfs 区画の像に → mtd6
3. 機器上で `mtd write`（OpenIRD の rootfs に同梱）で書き込む
   — 区画レイアウトと導入順は
   [board/mb86h61/README.md](../../br-ext-chip-fujitsu/board/mb86h61/README.md)

区画切り直し前のベンダ状態では、機器に mtd ツールが無いのでベンダ app 層の
書き込みコマンドを使う（`flash-script --via dione` が生成する）。

## 未解明点（推測で埋めない）

| # | 不明点 | 状態 | 影響 |
|---|---|---|---|
| H2 | `updater.full` の外側ラッパの変換（`Updater_decode`） | 未解明。純正 `.full` の入手待ち | 高。**一般ユーザ向け USB 導入経路の唯一の関門**。開発者向けの直接書き込みには不要 |
| H3 | `Updater_Header` の byte レイアウト | H2 と一体で解明する | 高（同上） |

`full` サブコマンドは H2/H3 が未解明のうちは実装せず、理由を示して停止する。

## 解決済み（記録）

- **RDHB カーネル容器**（旧 H1）… 実機ダンプの構造読みで同定し、`pack-kernel` を実装。
  実機 mtd5 の像を展開 → 再梱包 → 再展開して元と byte 一致、ヘッダの size/load/name も一致。
- **RDHB `0x08` のチェックサム**（旧 H1b）… **libooter は検証しない**。`0x08=0` で梱包した
  自前カーネルが実機で起動することを確認（2026-09-13）。アルゴリズムの特定は不要になった。
- **rootfs が区画に収まるか**（旧 H7）… 収まる。実測で本番 rootfs 2.02MB（区画 5MB）、
  救助用 612KB（区画 1MB）。区画は切り直し済み。
- **libc の選択**（旧 H6）… musl（Bootlin の外部 toolchain）。2.6.31 上での動作を QEMU と
  実機で確認。ベンダ blob を併用する段階 B では、blob 側の libc に合わせる判断が別途要る。
- **カーネル設定**（旧 H4）… `board/mb86h61/kernel/linux.config` として確定。GPL の
  devkit defconfig からの差分は `CONFIG_MEM_SIZE_RAMIFS=0`（load アドレスが実機と一致する）、
  squashfs / overlayfs / jffs2 の有効化、区画表の差し替え（パッチ 0015）。

> ダンプ（`temp/`）は解析専用。commit しない。GPL tarball は `dl/` に置く。

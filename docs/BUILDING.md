# ビルド手順（再現ビルド）

clone した人が、検証済みの像を再現するための手順。現状は **カーネル**（`mtd5.img`）と
**ファーストエイド rootfs**（区画切り直しの足場）まで。本番 rootfs は今後。

## 対応状況
| 機種 (BOARD) | カーネル再現ビルド | rootfs | 実機起動確認 |
|---|---|---|---|
| `mb86h61_softbank_sb-tv02-wfpl` (SB-TV02-WFPL) | ✅ `make kernel`（区画切り直し済み＝パッチ 0015） | ✅ `make BOARD=…`（overlay root + SSH） | ✅ 実機で起動・overlay root・SSH ログインまで確認 |
| `mb86h61_softbank_sb-tv02-wfpl_firstaid` | （同上のカーネルを使う） | ✅ `make BOARD=…_firstaid` | ✅ mtd6 / mtd8 の両方で起動確認（`board/mb86h61/firstaid/README.md`） |
| その他 | 未 | 未 | 未 |

## 前提環境
- **Linux（ネイティブ）または WSL2**。Windows ネイティブ不可（Git Bash 等では ARM カーネルをビルドできない）。
- **WSL の場合はリポジトリを ext4 側（`~/`）に置いてビルドすること。** `/mnt/c`（Windows ドライブ＝DrvFs）
  上では `tar`/`patch`/`make` が Unix の timestamp・permission を設定できず `Operation not permitted` で
  失敗する。ext4（`~/openird` 等に clone）なら問題なし。
- ホストに必要なパッケージ（Ubuntu 例）:
  ```
  sudo apt install -y build-essential bc bison flex libncurses-dev wget xz-utils \
                      cpio unzip rsync file python3 git
  ```
  ARM クロスツールチェーン（gcc 4.9.4）と旧 `libmpfr.so.4` は `build-kernel.sh` が、
  Buildroot 本体と rootfs 用 toolchain は `make` が、**公開URLから sha256 検証つきで自動取得**する。
- **Ubuntu 26.04 など `install` が uutils coreutils の環境**では Buildroot が既知の不具合
  （uutils#12166）を検出して停止する。GNU 版に切り替える:
  ```
  sudo update-alternatives --install /usr/bin/install install /usr/bin/gnuinstall 100
  ```
- WSL は Windows 側の PATH を引き継ぐが、Buildroot は空白入りの PATH を拒否する。
  トップの `Makefile` が空白を含む要素を落として渡すので、追加の対処は不要。

## GPL カーネルソースの入手（各自）
配布物にベンダ由来物を含めない方針のため、GPL カーネル tarball はリポジトリに同梱しない。
各自 **Pixela の GPL/OSS 提供**で入手し、`dl/` に置く（整合性は sha256 で検証される）:
- ファイル名: `Linux_kernel_2.6.31.6-rt19_r631_01.00.0002.tar.bz2`
- sha256: `befbcf187cb815674a168d90225a14c47601455f33b5658209d36253c9790906`
- 置き場所: リポジトリ直下の `dl/`（git 管理外）

pin の全体は `br-ext-chip-fujitsu/board/mb86h61/kernel/build.env` を参照。

## 実行
```
# リポジトリ直下で（Linux/WSL）
make kernel BOARD=mb86h61_softbank_sb-tv02-wfpl
# 直接叩く場合:
#   scripts/build-kernel.sh mb86h61_softbank_sb-tv02-wfpl
```
`scripts/build-kernel.sh` が行うこと:
1. クロスツールチェーン（kernel.org crosstool 4.9.4）取得・検証・展開
2. ホスト lib `libmpfr.so.4`（Debian archive）取得・検証・展開
3. `dl/` の GPL カーネル tarball を sha256 検証（無ければ入手手順を表示して停止）
4. 展開（Fujitsu proprietary FAPex シンボリックリンクは除外）
5. `patches/*.patch` 適用 → `linux.config` 適用 → `oldconfig` → `make Image`
6. `openird-mkimage pack-kernel` で RDHB 梱包 → `output/<BOARD>/mtd5.img`

## 出力と実機への適用
- `output/<BOARD>/mtd5.img` … libooter が起動できる RDHB カーネル像。
- 実機 mtd5 への書き込みは `docs/devices`（Wiki）と `host/openird-mkimage`（`flash-script` /
  `dione_server -w`）を参照。**mtd5 は単一面のため SPI クリップの復旧手段を用意してから**。
  overlayfs 入りの像は実機での起動がまだ確認されていない（上表）。

## rootfs（Buildroot）
Buildroot 本体はリポジトリに含めない。`scripts/buildroot.env` の pin（版・URL・sha256）から
`make` が `dl/` に取得・検証して `buildroot/` に展開する（git 管理外）。
```
make BOARD=mb86h61_softbank_sb-tv02-wfpl            # 本番（overlay root + SSH）→ mtd6 用
make BOARD=mb86h61_softbank_sb-tv02-wfpl_firstaid   # 救助用（mtd ツールのみ）→ mtd8 用
# → output/<BOARD>/images/openird-*-rootfs.squashfs（+ .sha256）
```
`make` が行うこと:
1. `scripts/fetch-buildroot.sh` が Buildroot を取得・展開
2. defconfig 適用 → Buildroot が外部 toolchain（Bootlin musl・sha256 は
   `board/mb86h61/patches/toolchain-external-custom/`）と各ソースを取得（全て hash 検証）
3. BusyBox ＋ `openwrt-mtd`（OpenWrt の `mtd`。commit 固定・ファイル単位で取得）を静的リンクでビルド
4. squashfs 4.0/gzip を生成し、`openird-mkimage pack-rootfs` が
   **カーネルが読める形式か（gzip・xattr 無し）と旧 mtd6 容量 1MB に収まるか**を検査

ファーストエイド rootfs の中身と実機への入れ方は
`br-ext-chip-fujitsu/board/mb86h61/firstaid/README.md`。

## カーネルパッチの機能テスト（QEMU）
同じ GPL ツリーとパッチを QEMU の汎用ボード向けにビルドし、overlayfs の動作を確かめる
（実機は焼かない）。`make kernel` を一度通した後に:
```
make test-overlayfs BOARD=mb86h61_softbank_sb-tv02-wfpl
```
詳細は `tests/overlayfs/README.md`、移植の経緯は `docs/kernel-overlayfs.md`。

## 再現性について
同一の pin（toolchain / patches / linux.config / GPL ソース）から**機能的に等価な起動可能カーネル**を
再現する。ビルド identity（`KBUILD_BUILD_USER/HOST/TIMESTAMP`）は固定を試みるが、2.6.31 が
尊重しない場合は banner にビルダの identity が入る（起動挙動は不変）。

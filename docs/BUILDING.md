# ビルド手順（再現ビルド）

clone した人が、検証済みのカーネル像（`mtd5.img`）を再現するための手順。
現状は **カーネルの再現ビルド**まで（フル Buildroot イメージ＝rootfs 込みは今後）。

## 対応状況
| 機種 (BOARD) | カーネル再現ビルド | 実機起動確認 |
|---|---|---|
| `mb86h61_softbank_sb-tv02-wfpl` (SB-TV02-WFPL) | ✅ `make kernel` | ✅ 実機で起動確認済（H1b 解決） |
| その他 | 未 | 未 |

## 前提環境
- **Linux（ネイティブ）または WSL2**。Windows ネイティブ不可（Git Bash 等では ARM カーネルをビルドできない）。
- **WSL の場合はリポジトリを ext4 側（`~/`）に置いてビルドすること。** `/mnt/c`（Windows ドライブ＝DrvFs）
  上では `tar`/`patch`/`make` が Unix の timestamp・permission を設定できず `Operation not permitted` で
  失敗する。ext4（`~/openird` 等に clone）なら問題なし。
- ホストに必要なパッケージ（Ubuntu 例）:
  ```
  sudo apt install -y build-essential bc bison flex libncurses-dev wget xz-utils
  ```
  ARM クロスツールチェーン（gcc 4.9.4）と旧 `libmpfr.so.4` は `build-kernel.sh` が
  **公開URLから sha256 検証つきで自動取得**するので、事前導入は不要。

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
git submodule update --init buildroot   # ← フルビルド用。kernel だけなら不要
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

## 再現性について
同一の pin（toolchain / patches / linux.config / GPL ソース）から**機能的に等価な起動可能カーネル**を
再現する。ビルド identity（`KBUILD_BUILD_USER/HOST/TIMESTAMP`）は固定を試みるが、2.6.31 が
尊重しない場合は banner にビルダの identity が入る（起動挙動は不変）。

# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD top-level orchestrator.
#   make list-boards                 … ビルド可能な機種一覧
#   make BOARD=<device>              … 当該機種をビルド（共通+ベンダの外部ツリーを自動合成）
#   make BOARD=<device> menuconfig   … 設定編集
#   make BOARD=<device> clean        … 当該機種の output を掃除
#   make test-overlayfs BOARD=<device> … overlayfs カーネルパッチの QEMU 機能テスト（TEST_ARGS="-n 5" 等）
#   make test-kabi BOARD=<device>      … ベンダ製 .ko と構造体レイアウトが互換かを確かめる
#   make sdk BOARD=<device>          … アドオン作者向け SDK（toolchain＋sysroot＋ird-mkaddon）
#   make release BOARD=<device>      … リリース物（SDK・ソース一式・SHA256SUMS）を揃える（docs/release.md）
#   make factory BOARD=<device>      … factory イメージ（純正の更新機構に渡すコンテナ。要 鍵・docs/release.md）
#   make test-addon                 … アドオン機構（ird-mkaddon / ird-addon）のテスト
#   make test-fwup                   … openird-fwup（UPnP 投入クライアント）のテスト
#   make hooks                       … git フック/テンプレートを有効化
#
# 外部ツリーは常に「br-ext-common : 該当ベンダツリー」を重ねる。ベンダツリーは
# <device>_defconfig を持つ br-ext-chip-* を自動検出する（登録表を二重に持たない）。

ROOT        := $(CURDIR)
BR          := $(ROOT)/buildroot
COMMON_TREE := $(ROOT)/br-ext-common

BOARD ?=
DEFCONFIG_MATCH := $(wildcard $(ROOT)/br-ext-chip-*/configs/$(BOARD)_defconfig)
VENDOR_TREE     := $(patsubst %/configs/$(BOARD)_defconfig,%,$(DEFCONFIG_MATCH))
BR2_EXTERNAL    := $(COMMON_TREE):$(VENDOR_TREE)
O               := $(ROOT)/output/$(BOARD)

# WSL は Windows 側の PATH（空白入り）を引き継ぐが、Buildroot は空白を含む PATH を拒否する。
# 空白を含む要素だけ落として渡す。ダウンロードキャッシュは kernel ビルドと同じ dl/ を共有する。
BR_PATH := $(shell printf '%s' "$$PATH" | tr ':' '\n' | grep -v '[[:space:]]' | paste -sd: -)
BR_MAKE = env PATH="$(BR_PATH)" BR2_DL_DIR="$(ROOT)/dl" \
	$(MAKE) -C $(BR) BR2_EXTERNAL="$(BR2_EXTERNAL)" O="$(O)"

.DEFAULT_GOAL := $(if $(BOARD),all,help)

.PHONY: help list-boards all kernel sdk release factory test-overlayfs test-kabi test-mkimage test-fwup test-tuner test-addon menuconfig linux-menuconfig savedefconfig clean distclean hooks check-buildroot check-board

help:
	@echo "OpenIRD — make list-boards | make BOARD=<device> [menuconfig|clean]"
	@echo "例: make BOARD=emma3sep_softbank_poe-br100-yb1"
	@echo "     make kernel BOARD=<device> … 検証済みカーネル(mtd5イメージ)を再現ビルド (要 Linux/WSL)"
	@echo "     make test-overlayfs BOARD=<device> … overlayfs パッチの QEMU 機能テスト"
	@echo "     make test-kabi BOARD=<device> … ベンダ製 .ko との構造体レイアウト互換テスト"
	@echo "     make test-mkimage … openird-mkimage の自己完結テスト (BOARD 不要・uv のみ)"
	@echo "     make test-fwup … openird-fwup の自己完結テスト (BOARD 不要・偽の機器を 127.0.0.1 に立てる)"
	@echo "     make test-tuner … tuner-core の PID 列挙のホスト単体テスト (BOARD 不要)"
	@echo "     make sdk BOARD=<device> … アドオン作者向け SDK (docs/addon.md)"
	@echo "     make release BOARD=<device> … リリース物を揃える (版のタグを打った clean な clone で・docs/release.md)"
	@echo "     make factory BOARD=<device> … factory イメージを作る (先に make BOARD=… と make kernel。要 鍵・docs/release.md)"
	@echo "     make test-addon … アドオン機構のテスト (BOARD 不要・要 Linux/WSL)"
	@$(MAKE) -s list-boards

list-boards:
	@echo "ビルド可能な機種 (BOARD):"
	@for f in $(ROOT)/br-ext-chip-*/configs/*_defconfig; do \
		[ -e "$$f" ] || continue; \
		b=$$(basename "$$f" _defconfig); \
		v=$$(basename $$(dirname $$(dirname "$$f"))); \
		printf "  %-40s (%s)\n" "$$b" "$$v"; \
	done

# pin（scripts/buildroot.env）どおりの Buildroot を dl/ に取得・検証して buildroot/ に展開
check-buildroot:
	@$(ROOT)/scripts/fetch-buildroot.sh

check-board:
	@test -n "$(BOARD)" || { echo "✗ BOARD= を指定してください（make list-boards）"; exit 1; }
	@test -n "$(strip $(DEFCONFIG_MATCH))" || { \
		echo "✗ 未知の BOARD: '$(BOARD)'。make list-boards で確認してください。"; exit 1; }

# BOARD 指定時の既定ターゲット＝ビルド（defconfig 適用 → world）
all: check-buildroot check-board
	@echo "→ $(BOARD)  外部ツリー: $(BR2_EXTERNAL)"
	$(BR_MAKE) $(BOARD)_defconfig
	$(BR_MAKE)

# 検証済みカーネル(mtd5 イメージ)を再現ビルド（Buildroot 不使用・要 Linux/WSL）。詳細は docs/BUILDING.md
kernel: check-board
	@$(ROOT)/scripts/build-kernel.sh $(BOARD)

# アドオン作者向け SDK＝再配置できる cross toolchain＋sysroot＋ird-mkaddon（docs/addon.md §6）。
# 再配置は Buildroot の仕組み（prepare-sdk が relocate-sdk.sh を host に置く）に任せ、
# OpenIRD の分（sdk.env・ird-mkaddon・README）を重ねて固める。
sdk: all
	$(BR_MAKE) prepare-sdk
	@$(ROOT)/scripts/make-sdk.sh $(BOARD)

# リリース物を output/release/<版>/ に揃える: SDK・対応するソース一式（Buildroot の legal-info）・
# SHA256SUMS。版のタグを打った、変更の無い git checkout でだけ通る。増分ビルドの残りを入れない
# よう、その機種の output を消して最初からビルドする（docs/release.md）。
release: check-board
	@$(ROOT)/scripts/make-release.sh --check
	rm -rf "$(O)"
	$(MAKE) BOARD=$(BOARD) sdk
	$(BR_MAKE) legal-info
	@$(ROOT)/scripts/make-release.sh $(BOARD)

# factory イメージ＝純正の更新機構に渡すコンテナ（docs/release.md）。先に make BOARD=…（rootfs）と
# make kernel を通しておく。鍵は openird-mkimage の決まりどおり、ファイルのパスで渡す。
# FACTORY_ARGS で scripts/make-factory.sh の引数を足せる（--payload-only＝鍵不要、--outdir など）。
factory: check-board
	@$(ROOT)/scripts/make-factory.sh $(BOARD) $(FACTORY_ARGS)

# アドオン機構（host/ird-mkaddon と ird-addon）の自己完結テスト（要 Linux/WSL・python3）
test-addon:
	@$(ROOT)/tests/addon/run.sh

# カーネルパッチの QEMU 機能テスト（要 Linux/WSL・qemu-system-<arch>）。詳細は tests/overlayfs/README.md
test-overlayfs: check-board
	@$(ROOT)/tests/overlayfs/run.sh $(TEST_ARGS) $(BOARD)

# ベンダ製 .ko とのカーネル内部 ABI 互換テスト（要 Linux/WSL）。詳細は tests/kabi/README.md
test-kabi: check-board
	@$(ROOT)/tests/kabi/run.sh $(BOARD)

# openird-mkimage のボード定義・容器形式の自己完結テスト（秘密もベンダ資材も不要）
test-mkimage:
	@$(ROOT)/tests/mkimage/run.sh

# openird-fwup の自己完結テスト（偽の機器を 127.0.0.1 に立てる。実機もベンダ資材も不要）
test-fwup:
	@$(ROOT)/tests/fwup/run.sh

# tuner-core の PID 列挙（ird_psi）のホスト単体テスト（BOARD 不要・ホストの cc のみ）
test-tuner:
	@$(ROOT)/tests/tuner/run.sh

menuconfig linux-menuconfig savedefconfig: check-buildroot check-board
	$(BR_MAKE) $@

clean: check-board
	rm -rf "$(O)"

distclean:
	rm -rf "$(ROOT)/output"

hooks:
	@$(ROOT)/tools/setup-hooks.sh

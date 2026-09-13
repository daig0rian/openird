# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD top-level orchestrator.
#   make list-boards                 … ビルド可能な機種一覧
#   make BOARD=<device>              … 当該機種をビルド（共通+ベンダの外部ツリーを自動合成）
#   make BOARD=<device> menuconfig   … 設定編集
#   make BOARD=<device> clean        … 当該機種の output を掃除
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

BR_MAKE = $(MAKE) -C $(BR) BR2_EXTERNAL="$(BR2_EXTERNAL)" O="$(O)"

.PHONY: help list-boards all kernel menuconfig linux-menuconfig savedefconfig clean distclean hooks check-buildroot check-board

help:
	@echo "OpenIRD — make list-boards | make BOARD=<device> [menuconfig|clean]"
	@echo "例: make BOARD=emma3sep_softbank_poe-br100-yb1"
	@echo "     make kernel BOARD=<device> … 検証済みカーネル(mtd5像)を再現ビルド (要 Linux/WSL)"
	@$(MAKE) -s list-boards

list-boards:
	@echo "ビルド可能な機種 (BOARD):"
	@for f in $(ROOT)/br-ext-chip-*/configs/*_defconfig; do \
		[ -e "$$f" ] || continue; \
		b=$$(basename "$$f" _defconfig); \
		v=$$(basename $$(dirname $$(dirname "$$f"))); \
		printf "  %-40s (%s)\n" "$$b" "$$v"; \
	done

check-buildroot:
	@test -f "$(BR)/Makefile" || { \
		echo "✗ buildroot/ がありません。submodule を取得してください:"; \
		echo "    git submodule update --init buildroot"; exit 1; }

check-board:
	@test -n "$(BOARD)" || { echo "✗ BOARD= を指定してください（make list-boards）"; exit 1; }
	@test -n "$(strip $(DEFCONFIG_MATCH))" || { \
		echo "✗ 未知の BOARD: '$(BOARD)'。make list-boards で確認してください。"; exit 1; }

# BOARD 指定時の既定ターゲット＝ビルド（defconfig 適用 → world）
all: check-buildroot check-board
	@echo "→ $(BOARD)  外部ツリー: $(BR2_EXTERNAL)"
	$(BR_MAKE) $(BOARD)_defconfig
	$(BR_MAKE)

# 検証済みカーネル(mtd5 像)を再現ビルド（Buildroot 不使用・要 Linux/WSL）。詳細は docs/BUILDING.md
kernel: check-board
	@$(ROOT)/scripts/build-kernel.sh $(BOARD)

menuconfig linux-menuconfig savedefconfig: check-buildroot check-board
	$(BR_MAKE) $@

clean: check-board
	rm -rf "$(O)"

distclean:
	rm -rf "$(ROOT)/output"

hooks:
	@$(ROOT)/tools/setup-hooks.sh

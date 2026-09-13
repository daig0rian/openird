# SPDX-License-Identifier: GPL-2.0-only
################################################################################
# ird-tuner-core — px4_drv 互換 chardev ABI（契約の本体）
################################################################################
IRD_TUNER_CORE_VERSION = 0.1.0
IRD_TUNER_CORE_SITE = $(BR2_EXTERNAL_IRD_COMMON_PATH)/package/ird-tuner-core/src
IRD_TUNER_CORE_SITE_METHOD = local
IRD_TUNER_CORE_LICENSE = GPL-2.0-only

# NOTE(Milestone 2 / 段階C): 実装は docs/specs の chardev 契約に基づき clean-room で
# 追加する。現状は構造を確定するためのプレースホルダ。
# $(eval $(kernel-module))
# $(eval $(generic-package))

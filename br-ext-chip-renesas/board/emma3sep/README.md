# board/emma3sep — Renesas EMMA3SEP

MIPS32r2 **ビッグエンディアン** / linux-2.6.27.39（GPL から再ビルド・非 DT）。
- `kernel/linux.config` … カーネル設定（GPL .config を出発点に）。
- `kernel/patches/` … 自前パッチのみ（pristine tarball は dl/、`*.hash` で検証）。
- uImage: Load/Entry `0x82280000` / name `emma3se`（CLAUDE.md §7）。
- u-boot は純正温存（UPnP 経路では触らない）。

対象機: POE-BR100-YB1（BMMPX1301）, POE-BR310L-SB1（SB-TV06-WRIP, 板サポート GPL 未収録）。

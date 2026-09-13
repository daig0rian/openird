# board/mb86h61 — Fujitsu MB86H61 (MB86H610H)

ARM1176（ARMv6, VFPv2）**リトルエンディアン** / linux-2.6.31.6-rt19（PREEMPT_RT）。
- `kernel/linux.config` … カーネル設定。
- `kernel/patches/` … 自前パッチのみ。
- ブートローダは libooter（ロック無し・純正温存）。kernel は SPI-NOR mtd5。
- カードリーダは Dione(MB86H58A) の下＝当面 deferred（CONTRIBUTING.md §0）。

対象機: SB-TV02-WFPL。

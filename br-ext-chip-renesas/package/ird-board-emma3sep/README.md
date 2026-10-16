# ird-board-emma3sep

EMMA3SEP の基板ごとの支援。GPL のカーネルが SoC までしか知らない分を足す。

カーネルに組み込む（`src/` を `scripts/kernel-overlay.sh` が `drivers/ird/` へ重ねる。
機種の `kernel/build.env` の `KERNEL_DRIVER_DIRS` に並べる）。rootfs に置くものは無い。

| ファイル | 内容 |
|---|---|
| `src/airtuner.c` | AirTuner 基板の LED（`/sys/class/leds/ird:<色>:<power\|status>`）と、効く再起動（自己リセット線）。`PIX_AIRTUNER_BR310L`（SB-TV06-WRIP）では LED の 2 組の入れ替えと USB の電源（GPIO36）も |

割り当ての出どころと実機での確認は
[docs/specs/emma3sep-airtuner-gpio.md](../../../docs/specs/emma3sep-airtuner-gpio.md)。
LED を点ける側（`ird-led`・起動スクリプト）は `ird-base-files` と `ird-tuner-emma3sep` にある。

# AirTuner 基板（EMMA3SEP）の LED と再起動

<!-- openird-spec
Status: verified
As-of:  GPL の U-Boot（2013.5.27.00-emma3sep-01）と GPL のカーネル 2.6.27.39 のソース / 実機 POE-BR100-YB1 (2026-10-12)
Impl:   br-ext-chip-renesas/package/ird-board-emma3sep/src/airtuner.c,
        br-ext-common/package/ird-base-files/files/usr/sbin/ird-led
Tests:  tests/mkimage/
Note:   実機で確かめたのは POE-BR100-YB1 だけ。LED の色は、所有者が実物を見て確かめた (2026-10-13)
-->

<!-- Open 層: 出どころは、機器のメーカーが公開している GPL のソース（ブートローダとカーネル）と、
     動いている実機の GPIO レジスタの読み書き・シリアルのログ。ベンダのバイナリは見ていない。 -->

前面の 2 つの LED（電源・ステータス）と、基板を再起動させる線の割り当て。GPL のカーネルは
SoC までしか知らず、この基板の LED を持たない。再起動も効かない（§4）。割り当ては、GPL の
ブートローダが基板ごとのファイルに書いている。

## 1. 出どころ

| 何 | どこ |
|---|---|
| GPIO の割り当て（基板ごと） | GPL の U-Boot `board/renesas/emma3sep/setup_gpio_<基板名>.h` の `_setup_gpio()` のコメント |
| LED の点け方 | 同じファイルの `_set_led()`・`_set_led_pattern()`、呼び出しは `arch/mips/lib/board.c` |
| GPIO のレジスタ | GPL のカーネル `drivers/gpio/emma-gpio.c`（`zpio`・gpiolib に登録・番号は 0 から） |
| カーネルの再起動 | GPL のカーネル `arch/mips/emma/emma3sx/setup.c` |
| ウォッチドッグ | GPL のカーネル `drivers/watchdog/emma_wdt.c` |

基板名は、ブートローダがカーネルの起動引数 `PRODUCT_FWIDS=` で渡す名前と同じ。

## 2. ピンの割り当て

次の 5 つの基板で同じ: `PIX_AIRTUNER_LITE`（POE-BR100-YB1）・`PIX_AIRTUNER_R`・
`PIX_AIRTUNER_W`・`NEC_AIRTUNER_R`・`TOSHIBA_AIRTUNER_R`。`PIX_XT046` には、この 5 本が無い。

| GPIO | 信号名 | 向き | 役割 |
|---|---|---|---|
| 19 | `LED1_A` | 出力 | ステータス LED の緑。**L で点灯** |
| 20 | `LED1_B` | 出力 | ステータス LED の赤。L で点灯 |
| 21 | `LED2_A` | 出力 | 電源 LED の緑。L で点灯 |
| 22 | `LED2_B` | 出力 | 電源 LED の赤。L で点灯 |
| 30 | `SELF_RST_EMMA_L` | 出力 | **L にすると基板がリセットされる。** ふだんは H |

- 緑と赤を両方点けると橙。
- 5 本とも 1 本目の語（GPIO 0〜31）にある。レジスタは `zpio` の先頭から、データ `+0x00`・
  マスク `+0x18`・向き `+0x30`（2 本目の語はそれぞれ `+4`）。向きは 1 が出力。マスクは、0 の
  ビットだけがデータへの書き込みを受ける。カーネルのドライバは、書くビットだけマスクを開け、
  書いたらマスクを戻す。
- SB-TV06-WRIP（`PIX_AIRTUNER_BR310L`）の基板のファイルは GPL に無い。同じかどうかは未確認。

## 3. ブートローダの点け方

ブートローダは、GPIO の初期化では 4 本とも消灯にし、そのあとで状態に応じたパターンにする。

| パターン | 電源 LED | ステータス LED |
|---|---|---|
| `PON_NORMAL`（通常の起動） | 赤 | 消灯 |
| `PON_ACTIVE` | 緑 | 緑 |
| `PON_ACTIVE_NO_SESSION` | 緑 | 橙 |
| `PON_FWUP`（更新の書き込み） | 橙 | 橙 |

∴ 通常の起動では、カーネルに制御が渡った時点で**電源が赤・ステータスが消灯**。

## 4. 再起動

- **GPL のカーネルの再起動は、この基板では効かない。** `emma3sx_machine_restart()` は
  リセットベクタ `0xbfc00000` へ飛ぶだけで、NAND から起動するこの基板ではそこに何も無い。
- **ウォッチドッグも再起動にならない。** ドライバは開いた時点で 58 秒のタイマを始め、満了で
  NMI を起こす設定（`NMI_EN`）にする。NMI の飛び先も同じベクタ。
- **`SELF_RST_EMMA_L`（GPIO30）を L にすると、基板ごとリセットされる。** ブートローダからの
  起動になり、AV の裏 CPU もリセットされる。

## 5. 実機での確認（POE-BR100-YB1・2026-10-12）

| したこと | 見えたこと |
|---|---|
| 起動後にデータレジスタを読む | GPIO19〜22 = H・H・H・L（§3 の `PON_NORMAL`＝電源が赤）、GPIO30 = H |
| GPIO19〜22 に L・H・L・H を書く（マスクは 4 ビットだけ開ける） | 読み戻しが書いた値どおり |
| `reboot -f` | `EMMA3Sx restart.` のあと `Reserved instruction in kernel code`・`epc : bfc00000`。`reboot` のプロセスだけが落ち、系は動いたまま |
| そのあと `/dev/watchdog` を開いて放置 | 数分のうちに、シリアルもネットワークも無応答。ブートローダの出力は出ない |
| GPIO30 に L を書く | すぐに落ち、15 秒後にはネットワークに戻った（カーネルの稼働時間 11 秒）。チューナは ready、受信は連続性エラー 0・同期エラー 0。LED のピンは `PON_NORMAL` に戻っていた |

上の 5 行は、手を入れる前のカーネル（OpenIRD 26.10-rc2）での観察。§6 の実装を入れたカーネルと
rootfs では:

| したこと | 見えたこと |
|---|---|
| 起動 | カーネルが LED 4 本を登録。起動が終わると `ird:green:power`・`ird:green:status` が 255、赤は 2 本とも 0。GPIO19〜22 = L・H・L・H |
| `S99ready stop` → `start` | 電源が赤（緑 0・赤 255）→ 緑（緑 255・赤 0）。ステータスは変わらない |
| `ird-led status orange` → `green` | ステータスの緑と赤が 255 → 緑だけ 255 |
| `reboot` | 停止処理 → `Restarting system.` → `AirTuner restart (self-reset line).` → ブートローダ → 起動。19 秒後にはネットワークに戻り、チューナは ready、LED は上と同じ |
| `reboot -f` | 同じく再起動する（rootfs とカーネルを書いた直後に、tmpfs の BusyBox から） |

色は、所有者が実物を見て確かめた（2026-10-13・起動後に電源が緑、チューナが ready でステータスが緑）。

## 6. OpenIRD の実装

**カーネル**（`ird-board-emma3sep` の `airtuner.c`・組み込み）。起動引数の `PRODUCT_FWIDS=` が
§2 の 5 つの名前のどれかのときだけ働く。

- LED は 4 本を `leds-gpio` に登録する。名前は `/sys/class/leds/ird:<色>:<power|status>`
  （色は `green`・`red`）。`brightness` に 0 を書くと消灯、0 以外で点灯。
  **名前は 19 文字まで**: このカーネルはデバイス名を 19 文字で切る。20 文字の
  `openird:green:status` で登録したら末尾の 1 文字が切れ、`ird-led` がその LED を見つけ
  られなかった（実機で踏んだ）。
- 登録すると LED はいったん消えるので、**電源の赤だけ**トリガ `default-on` で点け直す
  ＝カーネルの起動中は、ブートローダのまま電源が赤。
- 再起動は、GPIO30 を L にする関数に差し替える。起動時にこのピンを要求するときは H を書く
  （L にしない）。1 秒待ってもリセットされなければ、元の関数へ渡す。

**ユーザランド**（`ird-base-files`）。

- `ird-led <power|status> <off|green|red|orange>`: 色を 2 本の LED に直して書く。トリガは
  外す。その LED の無い基板では、何もせず 0 で終わる。
- 起動の最後（`S99ready`）に電源を緑にする。停止のときは赤に戻す。
- チューナの起動スクリプト（`S30tuner`）が、ready になったらステータスを緑に、起動できな
  かったら赤にする。

∴ 通常の起動: 電源が赤（ブートローダ〜カーネル〜起動スクリプト）→ チューナが ready で
ステータスが緑 → 起動が終わって電源が緑。

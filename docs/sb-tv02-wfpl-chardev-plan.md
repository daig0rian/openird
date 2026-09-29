# SB-TV02-WFPL — chardev に向けた観察計画（2026-09-29）

段階 C（`ird-tuner-*` の px4_drv 互換 chardev で生 TS を出す）に要る観察を、
「何が分かっていて、何が分からないと実装に入れないか」から組み立てた計画。
**仕様ではない**。機器の事実は `docs/specs/sb-tv02-wfpl-*.md` にあり、ここは順番と判断だけを書く。
土台は vendor-base（`tools/vendor-base/README.md`）で、**どのフェーズもフラッシュを書かない**。

## 0. 完成条件

- chardev から地上（ISDB-T）と BS/CS（ISDB-S）の生 TS（スクランブルのまま）が読め、
  recpt1 / recisdb で録れる。YB1 と同じ契約（CLAUDE.md §4）。
- 観察の完了＝実装が見る仕様だけで、起動・選局・信号品質・TS の取り出しが書ける状態。

## 1. 分かっていること

| 領域 | 分かっていること | 根拠 |
|---|---|---|
| フロントエンド | 復調器 TC90532（I2C0 slave `0x20`/`0x21`＝T、`0x22`/`0x23`＝S）、チューナは復調器の `0xfe` ゲートの奥（T＝XC5200、S＝D2818）。mainline の `tc90522.c` / `xc5000.c` が出発点。XC5200 のファームは公開の `xc5000c` と同型 | stage-b-fapex §5.5、`temp/spec_SB-TV02-WFPL.md` §5.2 |
| 電源 | GPIO 77/81/78/49/48 → 約 50ms → 74 解除、リセット 8/7/11。GPIO は 1 操作ごとに open/close | stage-b-fapex §5.5 |
| Dione | ファームは `/dev/h57fmb0` への `write()` 512B 単位・計 1,092,480B（idle＋trc）。ioctl はレジスタ R/W（約 1,500 回） | stage-b-fapex §5.5 |
| 選局の手順 | 工場テストシェルの `forceinit` → `dpower 1 1` → `settuner <0/1> <Hz> 0 0`。ロック時の復調器レジスタのダンプあり | vendor-boot §5、stage-b-fapex §5.5 |
| TS の口 | SoC の TSD（`/dev/fapi` の `0x90` 系）。ベンダは CH_A（地上）と CH_B（衛星）を `iflags=1`（直列＋PS）・`pid=0`・`flags=0x04`・`bufferSize=0x3ac000` で開く。TS 入力は TSD のブロックに結び付いている公算が高い | stage-b-fapex §5.6 |
| USB | Dione は `2-1.4`、USB ホストのエンドポイントは 6 本 | vendor-boot §7 |
| 部品のライセンス | FAPex のうち GPL は `fapex_cfdl`（ラッパ）だけ。TSD・I2C・GPIO・CLKPWR の実体（`fapi_core` 6.1MB ほか）は proprietary。GPL カーネルにはベースアドレス（GPIO `0xC3000000`・I2C0/1/2 `0xD1000000`〜・TSD `0xC8000000`・HDMAC `0xE6000000`）しか無い | stage-b-fapex §1、GPL `mach-mb86hxx` |

## 2. 分からないと実装に入れないこと

| # | 問い | なぜ効くか |
|---|---|---|
| **U1** | **SoC の CH_A/CH_B に TS を送っているのは復調器か Dione か**。ベンダは TS 入力ピンから読むが、TS はトランスコーダの設定後にしか来ず、アプリを止めると消える | Dione を経るなら、chardev は Dione のファーム（所有者の blob）と THROUGH 設定を抱える |
| **U2** | **復調器が Dione の完全起動なしでは I2C に応答しない理由**（クロック供給か、バスの切り替えか、ファームか） | Dione を「起こすだけ」で済むのか、「動かし続ける」必要があるのかが決まる |
| **U3** | **TSD で全 PID を 1 本に取れるか**（`flags=0x04` の意味、1 ブロックに並べられる PID の数） | YB1 のような PID の列挙が要るかどうか |
| **U4** | **衛星の選局**（D2818 の I2C 列、TSID の選択）と **LNB 電源の実体**（GPIO か I2C か Dione か） | BS/CS を chardev で出すのに要る |
| **U5** | **FAPex を使わずに TSD・I2C・GPIO を動かす手がかり**（レジスタの水準） | 下の判断 J1 で FAPex を外すと決めたときだけ要る |
| **U6** | **Dione の USB の手順**（ファーム転送とレジスタ R/W が USB 上でどう見えるか） | U1/U2 で Dione が要ると分かったときだけ要る |

## 3. 判断してほしいこと（観察の範囲が変わる）

- **J1: FAPex（所有者の機器にある proprietary の .ko）を実行時に使ってよいか。**
  - 使う：chardev は所有者の `fapi_*`・`fapex_fdl*` を load した上に載る。U5 は不要。YB1 の
    「起動時の `csd.ko` だけ」より依存は大きい（TSD・I2C・GPIO の実体をすべて借りる）。
  - 使わない：U5 のレジスタ観察（フェーズ D）が要る。I2C は Fujitsu 系の既知の IP である
    可能性があり、そうなら公開ドライバの意味論で書ける（D2 で検証）。
  - **勧め：まず FAPex の上で端から端まで通し（フェーズ A〜C）、U1/U2 の結果を見てから決める。**
    観察の順番はどちらでも同じで、フェーズ D だけが増減する。
- **J2（U1 の結果次第）: Dione のファームを実行時に流すことを受け入れるか。** ファームは
  所有者の機器上の `/slot/firm/` にあり、配布はしない（§1-1 と整合）。

## 3.5 進捗（2026-09-29）

- **A（地上）済み**：起動・選局・信号品質・Dione のレジスタ値まで（stage-b-fapex §5.7）。
  衛星（A3）は BS/CS のアンテナがつながってから。
- **B：U1 は「Dione」でほぼ確定**（stage-b-fapex §5.7「SoC の TS 入力に TS を送っているのは
  Dione」）。復調器のロックを保ったままアプリを外すと、SoC の入力 A/B/C のどれにも TS が来ない。
  ∴ 生 TS には Dione の THROUGH が要る＝**J2 の判断が必要**。U2（復調器が Dione の完全起動を
  要する理由）は、Dione を常に起こすなら実装上の意味が薄れた。
- **判断（2026-09-29 合意）**：J2＝受け入れる（所有者の機器の Dione のファームを実行時に流す。
  WFPL で生 TS を得る道はこれしかない）。J1＝まず所有者の FAPex と `h57fmb.ko` の上で chardev から
  生 TS が出るところまで通し、置き換え（TSD／I2C／GPIO のレジスタ観察、Dione の USB ドライバ）は
  その後に 1 つずつ決める。**1 回の試行で変えるのは 1 点**（大きく動かすと失敗の場所が見えない）。
- 次：THROUGH の設定を割り出す（ベンダ自身の設定経路を文字列と usage から探す → 無ければ選局後の
  パラメータの塊を 1 語ずつ変えて CH_A のレートと中身を見る）。
- 道具：`fapex-ts` は直し済み（既定 `iflags=1`・段ごとのタイムアウト）。トレーサは Dione の
  レジスタ値を記録する。アプリを外してハードを引き継ぐ手順（ウォッチドッグの扱い）が分かった。

## 4. フェーズ

### A. ベンダの基準列を取り切る（vendor-base＋トレーサ）

ベンダ像に戻したので、以前の観察で差として入っていたもの（chroot サンドボックスで `fdlclkpwr` を外したこと、
旧 rig の自前カーネル・書き換えた slot）が無い。
USB ディスクに `openird/trace` を置けば `dione_server` が `fapex_trace.so` の下で起動する。

| | 操作（COM3 の工場テストシェル） | 取るもの |
|---|---|---|
| A1 | 起動だけ | CLKPWR・GPIO・I2C（復調器の初期化）・TSD・`h57fmb0` の ioctl／`write()` の全列 |
| A2 | `settuner 0 <Hz> 0 0`、`sfreq 1 <Hz>` | 地上の選局列（復調器と XC5200。XC5200 のファームを公開版と突き合わせる） |
| A3 | `settuner 1 <Hz> 0 0`、`stsid`、`slnbpwr 1`／`0` | 衛星の選局列（D2818・TSID）と LNB 電源の実体（U4） |
| A4 | `gsignal`／`chktunerstat` を繰り返す | 信号品質の値と、そのとき読まれるレジスタ（C/N・BER の換算の材料） |
| A5 | `playstop`／`source`／`setDd`／`playstart`（引数の意味は未確認＝応答から探る） | Dione のモード切り替えが `h57fmb0` の何になるか（U1 の材料） |

出力：stage-b-fapex に地上・衛星の選局列と LNB 電源の節。ログは `openird-save` で USB ディスクへ。

### B. TS の送り手を決める（U1・U2）＝全体の分岐点

| | 方法 | 判定 |
|---|---|---|
| B1 | `noapp` で起動し、自前の道具だけで立ち上げる：GPIO で電源 → `h57fmb.ko` を insmod し、A1 の `h57fmb0` の列（レジスタ書き＋ファームの `write()`）を再生 → 復調器を A2 の列で選局 → `fapex-ts` で TSD の CH_A を読む（`iflags=1`） | TS が来れば**復調器→SoC 直結**。来なければ Dione が経路にいる |
| B2 | B1 の途中の段ごとに復調器の ACK を見る（USB 列挙だけ／idle ファームまで／A1 のレジスタ書きのどこまで） | 復調器が Dione の何を必要としているか（U2） |
| B3 | B1 で TS が来なければ、A5 の THROUGH 相当の列を足す | Dione 経由でも生 TS が取れるか |
| B4 | 取れた TS の PID と `transport_scrambling_control` を数える | 生 TS（スクランブルのまま・全番組）か、トランスコード後か |

出力：stage-b-fapex §5.6 の経路の確定。**J2 の判断材料**。

### C. TSD の能力（U3）

- C1：`flags=0x04` で 1 本の channel に何が載るか（PID の分布）。全 PID なら PID の列挙は要らない。
- C2：1 ブロックに並べられる channel/PID の数。地上と衛星を同時に（CH_A＋CH_B）。
- C3：全レートでの ring・level・overflow の振る舞いと CPU 負荷。

出力：chardev のドレイン部の仕様（YB1 の `poe-br100-yb1-ts-fifo.md` に当たるもの）。

### D. FAPex を外すためのレジスタ観察（J1 で「使わない」と決めたときだけ）

- D1：TSD（`0xC8000000`）と DMA（HDMAC `0xE6000000`）を、FAPex TSD の各 ioctl
  （OPEN／SET_INPUT／SET_PID_FILTER／START）の前後で `/dev/mem` から読み、変わる語を特定する。
- D2：I2C（`0xD1000000`〜）のレジスタの並びと初期値を読み、Fujitsu 系の公開ドライバの
  レジスタ配置と一致するかを確かめる（一致すれば、その意味論が仕様の出発点になる）。
- D3：GPIO（`0xC3000000`）で、`fapex-gpio` による 1 本ずつの切り替え前後の差を取る。
- 読むのは小さな窓から始める（YB1 で「読むと止まる」番地を踏んだ）。**書き込みは都度確認する。**

### E. Dione の最小手順（U1/U2 で Dione が要ると分かったときだけ）

- E1：ファーム転送とレジスタ R/W の USB 上の形。出荷 .config は `CONFIG_USB_MON` が無効で、ベンダの `usbcore` は usbmon のフックを持たないので、
  GPL から usbmon 付きの `usbcore`＋`usbmon.ko` を作り、`noapp` の回に USB スタックごと差し替えて取る。
- E2：B2 で分かった「復調器を起こすのに要る最小の列」と THROUGH の列を仕様にする。

### F. 検証

- F1：参照受信機（PX-M1UR）とビット一致を取る。手順は YB1 と同じ（`docs/poe-br100-yb1-ts-drain.md` §6）。
- F2：C/N を `gsignal` の値と較正する。

## 5. 道具

| 道具 | 状態 | 用途 |
|---|---|---|
| `fapex-trace`（`openird/trace`） | 済 | A 全般 |
| `fapex-ts` | 済（既定 `iflags=1`・段ごとのログとタイムアウト） | B・C |
| `fapex-i2c`・`fapex-gpio`・`fapex-poke` | 済 | B |
| Dione の立ち上げ（`h57fmb0` へファームを `write()`） | **作る** | B1 |
| メモリの窓を読む道具（`yb1-mem` の ARM 版） | **作る** | D |
| usbmon 付き `usbcore` | **作る**（E に進むときだけ） | E |

## 6. 触らないもの

- CAS 系の工場コマンド（`setdionekey`・`chkdionekey`・`deldionekey`・`chkotpstat`）、
  `dionekeyEraser`、`testmode 1`（アプリが終了して WDT で再起動する）。
- 名前が `updater.full` / `dionekeyEraser.full` のファイル。
- 素性の分からないレジスタへの書き込み（都度確認）。フラッシュ（この計画では書かない）。

## 7. 順番

A → **B（分岐点）** → C →（J1・J2 の判断）→ D／E → F。
A と B が済んだ時点で、U1・U2 の答えを持って J1・J2 を相談する（2026-09-29 にこの方針で合意）。
衛星（A3 と B の衛星側）は BS/CS のアンテナがつながっているときに行う。地上の部分を先に進めてよい。

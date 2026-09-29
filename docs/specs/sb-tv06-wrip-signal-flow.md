# SB-TV06-WRIP 信号・制御フロー俯瞰図

<!-- openird-spec
Status: observing
As-of:  実機 SB-TV06-WRIP (2026-09-26) ＋ 3機種アーキ比較 (2026-09-30)
Impl:   -
Tests:  -
Note:   物理IC/論理単位と放送データ・制御の流れの俯瞰。詳細は sb-tv06-wrip-bringup.md（§1/§5/§9）。
        2026-09-30 に3機種比較を「補足」へ追記（WFPL 生TS不可の確定=stage-b-fapex §5.8/§5.10 を反映。WRIP 実機観察は 2026-09-26 のまま）。
-->

<!-- Open 層: 基板写真の型番シルク、実機の lsmod / /proc/kallsyms のシンボル名 /
     LD_PRELOAD ioctl トレース / /dev/mem の TS 走査（0x47 同期・スクランブル統計）/
     公開 GPL(r557) の静的読み から構成。逆アセ由来の内部ロジック・コード由来の数式・
     限定受信の鍵/手順は含まない。【確定】は実機または写真で確認、（推測）は状況証拠からの推定。 -->

## 凡例

- **ノード**: 実線の枠＝**物理 IC（1 チップ）**。点線の枠＝**IC 内の論理単位**。
- **エッジ**: 実線 `──▶` ＝**放送データの流れ**。点線 `┄┄▶` ＝**制御の流れ**。
  ラベルに**バス種別・制御内容・データ内容**を表記。
- **確度**: **【確定】**＝実機/写真で確認。**（推測）**＝状況証拠からの推定。

## ブロック図

```mermaid
flowchart LR
  ANT["アンテナ入力<br/>地上/BS/CS 【確定】"]

  MXL["Tuner: MaxLinear<br/>MxL601RF / MxL135RF<br/>（地上波）【確定】"]
  TDA["Tuner: Philips<br/>TDA8263HN<br/>（BS/CS）【確定】"]

  subgraph DEMOD["Demod IC: Toshiba TC90532XBG 【確定】"]
    OFDM["OFDM コア ISDB-T<br/>slave_addr_ofdm<br/>（推測: 別コア）"]
    PSK["PSK コア ISDB-S<br/>slave_addr_psk<br/>（推測: 別コア）"]
    DGATE["I2C ゲート/リピータ<br/>→ チューナ 【確定: topology】"]
  end

  subgraph SOC["SoC: Renesas EMMA3SEP (uPD61425/D61425F1) 【確定】"]
    MAIN["メイン CPU (MIPS32r2 BE)<br/>userland: lindberg 【確定】"]
    SUB["裏 CPU: AV コプロセッサ<br/>（推測: MIPS 4KE）"]
    EI2C["ei2c_ofdm: GPIO ビットバング I2C<br/>（プロプラ .ko）【確定】"]
    ZPIO["zpio GPIO<br/>0x10110d00 (GPL) 【確定】"]
    DEMUX["demux HW<br/>PID フィルタ / バッファ<br/>【確定: ABI=YB1 §49-72】"]
    DSC["MULTI2 復号 HW<br/>val_m2dsc / val_dscram<br/>【確定: 存在】"]
    ENC["DES 変換器 val_dcnv<br/>ローカル鍵で再暗号化<br/>【確定: 存在】（推測: 録画/DTCP の暗号化）"]
  end

  DIONE["Transcoder IC: Fujitsu<br/>MB86H58A (Dione)<br/>H.264 変換 【確定】"]
  BCAS["B-CAS カード<br/>【確定】"]
  DDR["DDR3: Samsung K4B2G1646Q 256MB<br/>平文 TS: 0x0a000000 / 0x0b000000 【確定】"]
  HDD["USB HDD: /dev/sda (約1TB)<br/>録画=ローカル再暗号化・非TS 【確定】"]
  ETH["Ethernet PHY → LAN<br/>（place-shift 配信）【確定】"]

  %% ===== 放送データの流れ（実線） =====
  ANT -->|"RF 地上"| MXL
  ANT -->|"RF BS/CS"| TDA
  MXL -->|"IF アナログ"| OFDM
  TDA -->|"IF アナログ"| PSK
  OFDM -->|"TS スクランブル 【確定】"| DEMUX
  PSK -->|"TS スクランブル"| DEMUX
  DEMUX -->|"PID フィルタ後 TS スクランブル<br/>（推測: HW内でDSCへ）"| DSC
  DSC -->|"平文 TS 【確定: DRAMは平文のみ §9.2】"| DDR
  DDR -->|"平文 TS"| DIONE
  DIONE -->|"H.264（推測: DTCPは ENC 経由）"| ETH
  DDR -->|"平文 TS"| ENC
  ENC -->|"暗号化(非TS)・機器紐付け 【確定 §9.4】"| HDD

  %% ===== 制御の流れ（点線） =====
  MAIN -.->|"/dev/fe_main type=0xb0<br/>nr=0x08=周波数MHz 【確定】<br/>nr=0x0a=TSID（推測・BS）"| EI2C
  ZPIO -.->|"GPIO 提供 (GPL gpiolib)"| EI2C
  EI2C -.->|"I2C bitbang<br/>SCL=GPIO29 / SDA=GPIO0<br/>レジスタ書込 【確定】"| OFDM
  EI2C -.->|"I2C bitbang"| PSK
  OFDM -.->|"I2C リピータ 【確定: topology】"| DGATE
  DGATE -.->|"I2C: チューナ設定"| MXL
  MAIN -.->|"/dev/val_demux 40Bコマンド<br/>nr=0x0c=PID / 0x0d=有効化 / 0x2f=書込ポインタ<br/>【確定: ABI=YB1】"| SUB
  SUB -.->|"BHIF_SW_INT / コマンドスタック<br/>（推測: §72）"| DEMUX
  MAIN -.->|"/dev/val_scard: PC/SC"| BCAS
  BCAS -.->|"MULTI2 鍵（スコープ外・触れない）"| DSC
  MAIN -.->|"/dev/val_record + /dev/sda<br/>録画制御"| ENC

  %% 論理単位（IC内）を点線ノードに
  style OFDM stroke-dasharray: 5 5
  style PSK stroke-dasharray: 5 5
  style DGATE stroke-dasharray: 5 5
  style MAIN stroke-dasharray: 5 5
  style SUB stroke-dasharray: 5 5
  style EI2C stroke-dasharray: 5 5
  style ZPIO stroke-dasharray: 5 5
  style DEMUX stroke-dasharray: 5 5
  style DSC stroke-dasharray: 5 5
  style ENC stroke-dasharray: 5 5
```

## 補足（確度の内訳と OpenIRD の関わり）

- **【確定】の主なもの**: 全 IC の型番（写真・lsmod）、ei2c が zpio の GPIO ビットバング I2C で
  SCL=GPIO29/SDA=GPIO0（実機観察・§7.2）、チューナが demod の I2C ゲート奥（demod driver の
  シンボル・§5.2）、`fe_main` の周波数が MHz（§9.1）、demux の ABI が YB1 と一致（§5.1）、
  復号がインラインで DRAM には平文 TS しか持続しない（§9.2）、録画が非 TS のローカル再暗号化（§9.4）。
- **（推測）の主なもの**: 裏 CPU が MIPS 4KE（YB1 由来・WRIP で未直接確認）、demod の OFDM/PSK が
  別コア（slave_addr が 2 つあることから）、demux→descramble が HW 内で連結（散在片しか無いことから）、
  裏 CPU の合図が §72 と同形式（未検証）、BS の nr=0x0a=TSID。
- **★ OpenIRD の生 TS タップ点**: `DEMUX`（PID フィルタ後・スクランブル TS）を、`DSC`（復号）を
  **バイパス**して DRAM に書かせる（§72 のクリーン demux 制御）＝スクランブル TS を chardev で出す。
  復号は下流の正規 B-CAS+PC/SC に委ねる（scope 準拠）。`tsout_demux`/StreamOut は視聴・録画とも
  未使用（§9.3/§9.4）＝生 TS の出口は自前で demux 出力を確保するのが本線。
- **触れない**: `DSC`（MULTI2）と `BCAS` の鍵・手順（限定受信＝永久スコープ外）。

## 3 機種アーキテクチャの比較（なぜ WRIP が生 TS の本命か・2026-09-30 追記）

同じ B-CAS＋Dione(MB86H58) を持つ WFPL と、B-CAS 無しの YB1 を並べると WRIP の位置づけが
はっきりする。**分かれ目は「復号がどこで起きるか」**。WFPL は今セッション（2026-09-30）に
「ソフトから生 TS は取れない」が**確定**した（`sb-tv02-wfpl-stage-b-fapex.md` §5.8/§5.10。カード
有無に依らず素通し無し）ので、対比が確かなものになった。

| 機種 | SoC | 復号の場所 | Dione(MB86H58) の役割 | 生（スクランブル）TS の所在 | ソフトで生 TS |
|---|---|---|---|---|---|
| YB1 (BMMPX1301) | Renesas EMMA3SEP | **無し**（B-CAS 無し） | 非搭載 | 復調後そのまま SoC の AV SRAM 入力 FIFO | **可・実証**（reception-stack §87） |
| WFPL (SB-TV02) | Fujitsu MB86H61 | **Dione の中**（インライン別チップ・カードも Dione 配下） | 復号＋トランスコード | demod→Dione の基板配線上のみ・SoC は復号後しか見えない | **不可・確定**（stage-b-fapex §5.8/§5.10） |
| **WRIP (BR310L)** | Renesas EMMA3SEP | **SoC の HW**（`val_dscram`/`val_m2dsc`・カードは SoC 側 `val_scard`） | **トランスコードのみ**（DRAM の平文 TS を受け H.264 化＝§9.3） | demux→復号ステージの間（SoC 内 HW） | **（推測）タップ可**：demux（§72 の YB1 互換 ABI）を自前駆動し復号ステージをバイパス |

- ★ **要点**: WFPL は復号が**触れない別チップ(Dione)にインライン**で入るため、SoC は復号後しか
  見えずソフトから生 TS を取れない（確定）。**WRIP は復号が SoC 自身の HW にあり、しかもその
  demux は YB1 と同一 ABI（§5.1）で自前駆動できる。Dione は DRAM の平文を受ける下流の
  トランスコーダに過ぎない**（WFPL の Dione＝復号器、とは役割が逆）。∴ **「demux で PID フィルタ
  後の TS を、復号ステージを通さずに DRAM へ落とす」タップがアーキ上成立し得るのは WRIP だけ**
  （WFPL では原理的に不可、YB1 は復号段が無く既に取れている）。これが WRIP を生 TS・カードの
  本命に置く理由。
- **カード（PC/SC）**: WFPL はカードが Dione プロプラ配下で標準リーダとして SoC に見えない
  （実用上 deferred）。**WRIP はカードが SoC 側の `val_scard`**＝透過リーダの余地がある（スコープ内・
  復号は下流）。この点でも WRIP が本命。
- **未検証（要 RF 信号＋実機）**: この「復号バイパス」タップが実機で本当に成立するか（§6-1・§9.2 の
  設計）。裏 CPU の合図が YB1 §72 と同形式か（demux 76 本 ABI は一致済み・§5.1）。

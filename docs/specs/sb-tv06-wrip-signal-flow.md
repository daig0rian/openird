# SB-TV06-WRIP 信号・制御フロー俯瞰図

<!-- openird-spec
Status: observing
As-of:  実機 SB-TV06-WRIP (2026-09-26)
Impl:   -
Tests:  -
Note:   物理IC/論理単位と放送データ・制御の流れの俯瞰。詳細は sb-tv06-wrip-bringup.md（§1/§5/§9）。
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

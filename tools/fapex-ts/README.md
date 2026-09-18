# fapex-ts

段階 B / Milestone 1 の観察ツール。FAPex の **TSD（transport-stream demux）**
chardev（major 244・TSD0 common=`/dev/fapi/fapi000`、その channel sub ノード＝
`fapi004`〜`fapi035`）を、GPL ヘッダ `abst_lib.h`/`abst_libtsd.h` と GPL サンプル
`fapex_pvr_demo`（`tsd.c`/`main.c`）由来の **公開 ioctl ABI だけ**で叩き、TSD が
demux した TS を読み出す。ベンダコードは一切含まない（`fapex-i2c` と同じ clean な
chardev 利用者）。仕様は `docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §5.6。

**配布イメージには入らない。** 所有者が自機を観察するためだけのもの。

## 何のためか

復調器 TC90532 は生の MPEG2-TS（スクランブル済み＝Multi2 前）を出す。それが SoC の
TSD に届く経路は 2 通り:
- **Path H**: demod → SoC TSD の **HW TS 入力 CH（CH_A/B/C）** 直結（Dione 不要）
- **Path D**: demod → Dione(MB86H58, THROUGH) → USB → TSD の **packet-RAM（linear-auto）**

`fapex-ts` は TSD の demux 出力を読むので、(a) どの HW 入力 CH/モードが復調器 TS を
運ぶかを **probe/scan** して Path H を判定でき、(b) 生 TS をファイルに **capture** して
検証（0x47 sync・PID・スクランブルビット）できる。

## ABI（GPL 由来）

```
FAPEX_DEV_TYPE=0xFF  NB_TSD=0x90  NB_SUBID_TSD=0xFD
OPEN       = _IOW(0xFF,0x90, OpenParam[20]={useStc,tsSelect,syncByteDistance,iflags,oflags})
SET_INPUT  = _IOW(0xFF,0x94, 同上)
SET_PID    = _IOW(0xFF,0x95, PidParam[20]={pid,flags,bufferSize,thH,thL})
OPEN_SUBID = _IOW(0xFF,0xFD, {i32 sub_id; i32 channelNo})
GET_READINFO=_IOR(0xFF,0x9C, {fillSize,readOffset,sectionOffset,sectionLength,sectionNo,bufferStatus})
SET_READSIZE=_IOW(0xFF,0x9A, u32)   START=_IO(0xFF,1) STOP=_IO(0xFF,2) CLOSE_SUBID=_IO(0xFF,6)
tsSelect: CH_A=0 CH_B=1 CH_C=2 LINEAR_AUTO=0x29
iflags SP mode: serial+PS=1 / parallel-noPS=2 / parallel+PS=3, |AM_SYNC=0x20 |INV_CLK=0x04
```

読み出しモデル: START 後、`GET_READINFO`→ mmap した ring の `readOffset` から `fillSize`
バイト読む（ラップは linear 末尾でクランプ）→ `SET_READSIZE(consumed)` で read ポインタを
進める。

## ビルド

```
scripts/build-fapex-ts.sh          # Buildroot(musl) ツールチェーンで static ARM
```
→ `tools/fapex-ts/fapex-ts`（gitignore 済み）。ext4 上に Buildroot 出力が要る。

## 使い方（実機）

FAPex を insmod（`fapi_sys→fapi_core→fapex_cfdl→fapex_fdltsd`。TS 面なので `fdltsd` が要る）
した上で:

```sh
# Path H 判定: 復調器を tune+lock させた状態で、HW 入力 CH x mode を総当り
fapex-ts scan /dev/fapi/fapi000 /dev/fapi/fapi004
#  -> "CH_x mode ... <== TS-SYNC LOCKED" が出れば demod はその CH に直結（Path H 成立）

# 単発 probe（既定 input=A, mode=parps, pid=0=PAT を 5 秒）
fapex-ts --input A --mode parps probe /dev/fapi/fapi000 /dev/fapi/fapi004

# 生 TS を捕捉してファイルへ（PID 指定・秒数/バイト上限・出力先）
fapex-ts --input A --mode parps --pid 0x0000 --secs 10 -o /tmp/pat.ts \
         capture /dev/fapi/fapi000 /dev/fapi/fapi004
```

Path D を見る場合は `--linauto`（tsSelect=0x29, packet-RAM）。ただし Path D は Dione を
THROUGH で上げて USB から TS が RAM に入っている前提。

## 限界 / 次

- 現状は **1 PID = 1 channel**（probe は PAT=PID0 で TS 到来を確認するのに十分）。
  **whole-TS（全 PID）** の取得機構（channel 31 の `bm_wrap` か、dione_server の TSD 設定を
  観察して再現）は §5.6 の次段で確定する。
- byte order（swap）と SP mode/AM_SYNC/INV_CLK は復調器の TS 出力設定と対で決まるので、
  `scan` で当たりを取る。復調器側の TS 出力モード設定（tc90522 レジスタ）は fapex-i2c で行う。

# EMMA3SEP のカードリーダー（`/dev/irdcard0` と pcscd）

<!-- openird-spec
Status: specified
As-of:  カードの口の観察 sb-tv06-wrip-card-slot.md (2026-10-13・抜き挿しは 2026-10-15)
Impl:   -
Tests:  -
Note:   OpenIRD がカードスロットを PC/SC のリーダとして見せる作り（カーネルの口・FW との手順・pcscd）。
        FW とのやり取りの事実は sb-tv06-wrip-card-slot.md にあり、この文書はそれをどう使うかを決める
-->

<!-- Open 層のみ: FW とのやり取りはすべて sb-tv06-wrip-card-slot.md の観察から。pcsc-lite の
     IFD ハンドラの API は公開のヘッダ（ifdhandler.h）と文書から。逆アセ由来のものは置かない。
     カードが運ぶコマンドと応答の意味・限定受信には一切触れない。 -->

カードスロットに挿した接触 IC カードを、機器の上の PC/SC のアプリケーションから使えるようにする
ための仕様。実装者はこの文書と [カードの口](sb-tv06-wrip-card-slot.md)・
[AV のコマンド経路](poe-br100-yb1-av-command.md) だけを見て書けることを目指す。

## 0. スコープ

- **運び役だけ**: カードの在否、リセットと ATR、コマンド（APDU）を送って応答を受け取ること。
  PC/SC のリーダが行う範囲で、カードが運ぶ中身の意味は扱わない。限定受信に関わるもの
  （手順・鍵・解除）は一切扱わない（[CONTRIBUTING.md](../../CONTRIBUTING.md) §0）。
- **機器の中だけで使う**: カードをネットワーク越しに単体で貸す口は作らない。使う側は機器の上で
  動くプログラムで、PC/SC の API（pcsc-lite の `libpcsclite`）で pcscd と話す。
- **対象機**: カードスロットのある SB-TV06-WRIP（`PRODUCT_FWIDS=PIX_AIRTUNER_BR310L`）。
  同じファミリーの POE-BR100-YB1 にはスロットが無く、口は現れない（rootfs の中身は同じで、
  pcscd は起動しない）。

## 1. 全体像

```
機器の上のアプリケーション（PC/SC の API・libpcsclite を静的にリンク）
   │  UNIX ソケット
pcscd（pcsc-lite 2.1.0・静的にリンク。OpenIRD の IFD ハンドラを中に持つ・§4）
   │  ioctl
/dev/irdcard0（カーネル・§2）
   │  AV のコマンド経路（チューナと共有・§3.5）
裏 CPU の FW（T=1 のブロックのやり取りは FW が持つ・カードの口 §1）
   │
カード
```

- **T=1 のブロック（NAD・PCB・EDC・連鎖）は FW が処理する。** メイン CPU が扱うのは APDU と
  その応答のバイト列だけ。カーネルの口も IFD ハンドラも、APDU を素通しする。

## 2. カーネルの口 `/dev/irdcard0`

- 機種が BR310L のときだけ、misc デバイス `irdcard0` を登録する。
- **開けるのは、AV のコマンド経路が使えるようになってから**（チューナの起動スクリプトが
  `start` を済ませた後）。それより前の `open` は `EAGAIN`。
- **同時に開けるのは 1 つ**（2 つ目は `EBUSY`）。開くのは pcscd だけの想定。
- ioctl は [`ird_card_ioctl.h`](../../br-ext-common/package/ird-tuner-core/src/ird_card_ioctl.h)
  （型 `'c'`・番号 `0xd0`〜）:

| 名前 | 引数 | 働き | 失敗 |
|---|---|---|---|
| `IRD_CARD_STATUS` | `struct ird_card_status`（出） | `present`（カードがある）・`powered`（リセット済みで、その後抜かれていない）・`changes`（在否が変わった回数） | — |
| `IRD_CARD_RESET` | `struct ird_card_atr`（出） | カードを起こし直して ATR を返す（§3.2）。電源投入とやり直しの両方 | `ENOMEDIUM` カードが無い・`ETIMEDOUT`・`EIO` |
| `IRD_CARD_POWER_OFF` | なし | 「電源を切った」とだけ覚える（`powered` が 0 になる）。FW に切る手順が見つかっていないので、カードには何もしない（§5） | — |
| `IRD_CARD_TRANSMIT` | `struct ird_card_xfer`（入出） | APDU を送り、応答（状態語を含む）を返す（§3.3） | `ENOMEDIUM`・`ENODEV` リセットしていない・`EINVAL` 長さ・`ETIMEDOUT`・`EIO` |

- 長さの上限: ATR は 33 バイト（規格の上限）。APDU は 261 バイト（短い APDU の最大）。応答は
  256 バイト（FW に渡す上限＝ベンダと同じ値・カードの口 §2.4）。拡張 APDU は扱わない。
- 呼び出しは順番に処理する（カードの口の手順は 1 つずつしか進められない）。

## 3. カーネルの中の手順

FW とのやり取りは [カードの口](sb-tv06-wrip-card-slot.md) の §2・§4 のとおり。ここでは順番と、
在否の追い方を決める。

### 3.1 最初の 1 回

最初に `open` されたとき、起動してから 1 回だけ: `0x1100`（初期化）→ `0x1107`（リーダの設定・
ベンダと同じ 6 語）→ `0x1101`（開く・`{0, 1}`）。どれも状態 0 でなければ `EIO`。

### 3.2 リセット（`IRD_CARD_RESET`）

ベンダと同じ順番（カードの口 §2.3）:

1. `0x1106` コード 1 で設定を読む（応答は要求の場所に上書きされる）。
2. 読んだ 22 語の語 12 を `0x0f`、語 15 を `0x03000000` にして、コード 2 で書く。
3. 合図を消して（§3.4）、コード 0。**状態 15 ならカードが無い**（`ENOMEDIUM`）。
4. 合図を最大 1 秒待つ。来なければ `ETIMEDOUT`。
5. コード 6（語 19 ＝ 要求の番地 ＋ 88）。応答の 9 語が ATR、要求の語 20 の最上位バイトが長さ。
   長さが 0 か 33 を超えたら `EIO`。

成功すれば `powered` を 1 にする。

### 3.3 往復（`IRD_CARD_TRANSMIT`）

1. `powered` でなければ `ENODEV`。
2. 合図を消して（§3.4）、`0x1105` を積む: `{0, 要求の番地 ＋ 20, 長さ << 16, 要求の番地 ＋ 20,
   0x100, APDU…}`（4 バイト境界まで 0 で埋める）。状態 0 でなければ `EIO`。
3. 受理の後の要求の語 3 が、FW の記録の番地 **P**（カードの口 §3）。KSEG1 の 4 バイト境界の
   番地でなければ `EIO`。覚えておく（以後の在否の判定に使う・§3.4）。
4. 合図を最大 5 秒待つ。来なければ `ETIMEDOUT`。
5. P − 8 の状態が `4` でなければ、`0` なら `ENOMEDIUM`（抜かれた）、ほかは `EIO`。
6. P − 4 の下位 16 ビットが応答の長さ（256 を超えたら `EIO`）。P から読む。

### 3.4 合図と在否

- **合図は共有メモリの `+0xd500` の語の bit 24 で見る**（カードの口 §3）。消すときはその 1 ビット
  だけを落として書き戻す。BHIF の `SW_INT` には触らず、割り込みも使わない（割り込みを有効に
  すると、ベンダの `csd.ko` が居るのと見分けがつかなくなる・AV のコマンド経路 §1）。待つのは
  1 ms ごとの確かめ。
- 合図はカードの状態が変わるたびに来る（挿す・抜く・リセットの応答・往復）。自分の手順の
  外で立った合図は「抜き挿しがあった」と読む。

在否（`IRD_CARD_STATUS` の `present`）は、次のとおりに決める:

| 知っていること | 決め方 |
|---|---|
| P を知っている | P − 8 の状態: `1`・`2`・`4` ならある。`0`・`7`・ほかは無い |
| P を知らない（起動してから往復が無い） | 手順の外の合図が無ければ、前の判定のまま。最初の 1 回と、合図があった後は、**最後の合図から 0.5 秒待ってから**リセット（§3.2）で確かめる（状態 15 なら無い） |

- 抜き挿しの接点はばたつくので（約 350 ms・カードの口 §3）、合図の後 0.5 秒は「無い」と答える。
- 無いと判定したら `powered` を 0 にする。`present` が変わるたびに `changes` を 1 増やす。
- P を知らない間にリセットで確かめるのは、カードを使っている者がまだ居ない（往復が無い）
  ときだけなので、使用中のカードを起こし直すことはない。

### 3.5 AV のコマンド経路の共有

カードの手順とチューナ（PID フィルタ）は、同じコマンドスタックに積む。AV のコマンド経路の層が
自分で順番を守る（1 つ積んで、終わるまで次を積まない）。カードのコマンドは要求が大きいので
（`0x1106` は要求 22 語＋応答 9 語、`0x1105` は最大 5 ＋ 66 語）、その層は 1 本あたり 96 語まで
運ぶ。要求の中に自分の番地を入れる語（「要求の番地 ＋ N」）と、FW が要求の場所に書き戻す語
（コード 1 の設定・`0x1105` の語 1 と語 3）も、その層が扱う。

## 4. pcscd

- **pcsc-lite 2.1.0 を静的にリンクして入れる**（rootfs は全部静的・`BR2_STATIC_LIBS`）。
  Buildroot の pcsc-lite は静的な構成を受け付けないので、OpenIRD のパッケージで作る
  （`--disable-libudev --disable-libusb --disable-libsystemd --disable-polkit`）。
- **IFD ハンドラ（`ifd-irdcard`）は pcscd の中に組み込む。** pcscd はリーダのドライバを
  `dlopen` で読むが、静的な musl には `dlopen` が無い。そこで pcscd に、`dlopen`・`dlsym`・
  `dlclose`・`dlerror` の小さな代役をリンクする: `dlopen` は名前が `irdcard.builtin` で終わるとき
  だけ成功し、`dlsym` は組み込みの IFD ハンドラの関数を返す。pcsc-lite 本体には手を入れない。
  ほかのドライバ（USB のリーダなど）は読めない。
- リーダの登録は `/etc/reader.conf.d/irdcard.conf`:
  ```
  FRIENDLYNAME "OpenIRD card slot"
  DEVICENAME   /dev/irdcard0
  LIBPATH      /usr/lib/pcsc/drivers/irdcard.builtin
  CHANNELID    0
  ```
  `LIBPATH` のファイルは中身の無い目印（pcscd が在るかを確かめるため）。
- IFD ハンドラ（IFD Handler API v3）の働き:

| 関数 | 働き |
|---|---|
| `IFDHCreateChannelByName`・`IFDHCreateChannel` | `/dev/irdcard0` を開く |
| `IFDHCloseChannel` | 閉じる |
| `IFDHICCPresence` | `IRD_CARD_STATUS` の `present` |
| `IFDHPowerICC` | 投入・やり直しは `IRD_CARD_RESET`（ATR を返す）、切断は `IRD_CARD_POWER_OFF` |
| `IFDHSetProtocolParameters` | T=1 だけ受ける（FW が T=1 を持つ）。ほかは `IFD_PROTOCOL_NOT_SUPPORTED` |
| `IFDHTransmitToICC` | `IRD_CARD_TRANSMIT`。応答が呼び出し側の器に入らなければ `IFD_ERROR_INSUFFICIENT_BUFFER` |
| `IFDHGetCapabilities` | ATR（最後のリセットのもの）・スロット 1・同時アクセス 1 |
| `IFDHControl`・`IFDHSetCapabilities` | 扱わない |

  自前の在否の見張り（polling thread）は持たない＝pcscd が `IFDHICCPresence` を定期的に呼ぶ。
- 起動は、チューナの起動スクリプトの後（カードの口が AV のコマンド経路を使うため）。
  `/dev/irdcard0` が無い機種では起動しない。
- 使う側のプログラムは `libpcsclite.a` を静的にリンクする（SDK に入る）。確かめ用に `ird-pcsc`
  （リーダとカードの状態・ATR を表示し、指定した APDU を 1 本送って応答をそのまま表示する。
  応答の意味は解釈しない）を rootfs に入れる。

## 5. 限界（分かっていないこと）

- **カードの電源を切る手順は無い**（FW の `Close` に当たるコマンドを観察していない・カードの口
  §7）。`IRD_CARD_POWER_OFF` の後も、カードは通電したまま。次の利用はリセットから始める。
- 確かめたのは、T=1 のカードで 5 バイトのコマンドと 2 バイトの応答の往復だけ。長い APDU・
  長い応答・T=0 のカード・往復の最中に抜いたときの FW の振る舞いは、確かめていない。
- 状態 `7` の意味は分かっていない（無いとして扱う）。

## 6. 検証

（実装の後に書く）

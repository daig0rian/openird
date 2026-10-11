# OpenIRD ディレクトリ構造とビルドモデル

## 1 コードベースで複数機種を作る（OpenIPC 流の外部ツリー）

リポジトリ本体は **Buildroot を内蔵しないオーケストレータ**。Buildroot 本体は
`scripts/buildroot.env` で版・URL・sha256 を固定し、`make` が `dl/` に取得して
`buildroot/` に展開する（git 管理外）。ベンダ GPL カーネルは `dl/` にキャッシュ
（自前の差分だけ `patches/` に commit）。

機種ビルドは常に **共通 + ベンダ 1 つ**の外部ツリーを重ねる:

```
YB1 (BMMPX1301):      BR2_EXTERNAL = br-ext-common : br-ext-chip-renesas
SB-TV06-WRIP:         BR2_EXTERNAL = br-ext-common : br-ext-chip-renesas
SB-TV02-WFPL:         BR2_EXTERNAL = br-ext-common : br-ext-chip-fujitsu
```

```
openird/
├─ Makefile                 # make BOARD=<device> で外部ツリーを自動選択
├─ buildroot/               # 上流 Buildroot（pin から自動展開・git 管理外）
├─ dl/                      # tarball キャッシュ（git 管理外・sha256 で検証）
│
├─ br-ext-common/           # ベンダ非依存・全機種共通
│   └─ package/
│       ├─ ird-tuner-core/  # px4_drv 互換 chardev の ABI 骨格（契約の本体）
│       ├─ ird-config/      # UCI 相当 /etc/config/*
│       ├─ ird-webui/       # 管理 UI
│       ├─ ird-base-files/  # 共通 rootfs skeleton / init
│       ├─ ird-addon/       # アドオン（実行時パッケージ）のインストーラ（docs/addon.md）
│       └─ openwrt-mtd/     # OpenWrt の mtd（区画書き込み。commit 固定で取得）
│
├─ br-ext-chip-renesas/     # Renesas EMMA3SEP（MIPS BE, 2.6.27）
│   ├─ board/emma3sep/      # kernel config + patches、rootfs overlay
│   ├─ configs/             # emma3sep_softbank_poe-br100-yb1_defconfig ほか
│   └─ package/ird-tuner-mxl683/
│
├─ br-ext-chip-fujitsu/     # Fujitsu MB86H61（ARMv6 LE, 2.6.31-rt）
│   ├─ board/mb86h61/       # kernel config/patches、firstaid/（区画切り直しの足場）
│   ├─ configs/             # mb86h61_softbank_sb-tv02-wfpl_defconfig ほか
│   └─ package/ird-tuner-tc90532/
│
├─ host/openird-mkimage/    # factory コンテナ生成（鍵は埋めず外から渡す）
├─ host/openird-fwup/       # factory コンテナを UPnP で機器へ投入（鍵を扱わない）
├─ host/ird-mkaddon/        # アドオン（.ird）と索引の生成。SDK にも入る
├─ scripts/build-kernel.sh  # 検証済みカーネル（mtd5 像）の再現ビルド（make kernel）
├─ scripts/fetch-buildroot.sh # pin した Buildroot の取得・検証・展開
├─ scripts/make-sdk.sh      # アドオン作者向け SDK を固める（make sdk）
├─ tests/overlayfs/         # カーネルパッチの QEMU 機能テスト（make test-overlayfs）
├─ tests/addon/             # アドオン機構のテスト（make test-addon）
└─ docs/
```

「**1 機種 = 1 defconfig = 1 バイナリ**」を保ちつつ、chardev 契約と `ird-*` は
`br-ext-common` に集約。機種追加は該当ベンダツリーに defconfig を 1 枚足すだけ。

## 対象機種（現在）

| BOARD (defconfig 名) | 製品 | SoC / ツリー | カードリーダ |
|---|---|---|---|
| `emma3sep_softbank_poe-br100-yb1` | BMMPX1301 | Renesas EMMA3SEP / renesas | 無（LITE） |
| `mb86h61_softbank_sb-tv02-wfpl`   | SB-TV02-WFPL | Fujitsu MB86H61 / fujitsu | 有（Dione 下＝当面 deferred） |
| `emma3sep_softbank_sb-tv06-wrip`  | SB-TV06-WRIP (POE-BR310L-SB1) | Renesas EMMA3SEP / renesas | 有（可能なら PC/SC 対応） |

> SB-TV06-WRIP の板サポートは GPL 未収録のため bring-up は重い（後続マイルストーン）。

## OpenWrt 管理モデルの適用範囲

| OpenWrt | 適用 | OpenIRD での実現 |
|---|---|---|
| target / subtarget / device の3層 | ◎ | `br-ext-chip-<vendor>` / `board/<family>` / `<device>_defconfig` |
| 機種ごと宣言的 profile | ◎ | 1 機種 = 1 defconfig |
| factory / sysupgrade 分離 | ◎ | factory=`openird-mkimage`（生成）＋`openird-fwup`（投入） / sysupgrade=ネイティブ |
| イメージ命名 | ◎ | `openird-<ver>-<target>-<device>-{factory,sysupgrade}.bin` |
| UCI 設定レイヤ | ○（語彙） | `ird-config` を自前実装 |
| package feed | ○（語彙を2分割・feed 語は不採用） | build-time=**ビルドオプション**（`BR2_EXTERNAL` の package・defconfig 選択・像に焼く）／runtime=**アドオン**（`.ird` をアドオンリポジトリから `ird-addon` で overlay に後載せ）。非互換の別機構（[docs/addon.md](addon.md)） |
| DCO / パッチ流儀 | ◎ | CONTRIBUTING.md で採用 |
| OpenWrt ビルド本体 | ✕ | 上流 Buildroot を使う（古い非 DT カーネルのため） |
| LuCI | ✕ | 軽量 `ird-webui`（BE MIPS / 256MB 制約） |
| DT による機種差分 | ✕ | 2.6.27 は非 DT＝C ボードファイルで表現 |

結論: **語彙と管理モデルは輸入、ビルドエンジンは Buildroot**（OpenIPC と同じ立ち位置）。

## ドキュメントの2層（repo / Wiki）

| 層 | 置き場 | 理由 |
|---|---|---|
| Open 層 spec（chardev 契約・区画表・観察事実）＝ clean-room の firewall | **repo `docs/specs/`** | `Spec-Ref` の不変・レビュー済み・provenance/PII 検査済みターゲット。フック/PR/CI が効く |
| 導入・復旧 how-to / UART ピン配 / FAQ / 写真 / コミュニティメモ | **GitHub Wiki** | 頻繁更新・人向け・ビルド非依存。編集しやすさが活きる |

- `Spec-Ref:` は必ず **repo の spec** を指す（Wiki は誰でも後から書換え可能＝監査線にならない）。
- **Wiki は本体のフック/PR/CI の圏外**。ダンプ・鍵・未伏字の識別情報は Wiki に
  置かない。Wiki の "anyone can edit" は off にする。
- 希望すれば `docs/specs/` → Wiki の read-only ミラーを CI で回せる（repo が canonical）。

## 非再配布ファームウェア blob の扱い（fwcutter パターン）

一部の復調/RF チップは動作に実行時ファーム blob を要する。再配布可能なものは
linux-firmware 等から供給できるが、**非再配布の blob は OpenIRD では配布できない**
（ガードレール: 配布物にベンダバイナリ 0 バイト）。そこで b43-fwcutter と同じく、
**抽出ツール（cutter）だけを OpenIRD が提供し、ユーザが自機のファームから blob を
切り出して所定フォルダに置く**。

- **cutter はベンダバイトを 1 バイトも含まない**。blob の所在・形式（＝観察由来の
  構造知識）だけを持つ自前コード。`Origin: clean-room` + blob 位置を記した `Spec-Ref:`。
- **スコープの線は「復号」であって「ファーム抽出」ではない**。生 TS を出すのに必要な
  ハードウェア動作ファーム（RF/復調/チューナ USB の microcode）を抽出・アップロードして
  自機を動かすのは *enablement* ＝中立で範囲内（b43 と同じ）。一方、MULTI2/ECM/鍵処理の
  **復号ロジック**はファームの有無に関わらず OpenIRD では実装しない（契約の上位＝外）。
  どの blob が生 TS 経路に必要か（RF/復調側か H58 USB サブシステムか）と各 blob の
  再配布可否は **Milestone 1 で確定**する。

### なぜ「鍵の抽出ツールは含めない」のに「ファームの cutter は含める」のか

一見すると CONTRIBUTING.md §0 の「鍵を機器から抽出するツールもスコープ外」と
矛盾して見えるが、**対象が違う**。判断は「そのツールで何ができるようになるか」で行う。

| | 更新コンテナの鍵 | ハードウェア動作ファーム |
|---|---|---|
| 何のために置かれているか | メーカーが**更新の真正性を守る**ための仕組み | ハードウェアを**動かす**ための部品 |
| 抽出ツールを配ると | 誰でも保護を外せる道具が出回る | 所有者が自分の機器を動かせるだけ |
| OpenIRD の扱い | **含めない**（値も抽出手段も）。鍵を**消費する側**だけ（`openird-mkimage` 等） | **含める**（cutter）。前例: b43-fwcutter |
| 共通 | 配布物にバイトは 1 バイトも入らない。所有者が自機から取り出した物を各自で使う | 同左 |

つまり線は「所有者の機器の保護機構を外す手段を、OpenIRD が配るかどうか」にある。
cutter は保護を外さない（ファームを解析して必要な部分を切り出すだけ）。鍵抽出は
保護を外すことそのものが目的になるため、機構の如何によらず受け付けない。

- **配布イメージは常に blob 無し**。ユーザがローカルで切り出した blob を置く:
  - 実行時（機器）: `/lib/firmware/ird/<chip>.fw`（ドライバは `request_firmware` で読む）
  - ビルド時ステージング: `dl/firmware-input/`（**git 管理外**）
  - blob を焼き込んだ個体向けイメージは各自のものであり再配布しない。
- **置き場**:
  - ファーム読込の契約（`request_firmware` パス規約）… `br-ext-common/package/ird-tuner-core`
  - チップ別 cutter ＋ ファーム package … `br-ext-chip-<vendor>/package/ird-fw-<chip>/`
    （ドライバ `ird-tuner-<chip>` と隣接。blob が無ければビルド時に手順を案内して停止）

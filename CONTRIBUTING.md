# Contributing to OpenIRD

OpenIRD はネットワーク接続型チューナー機器向けの、OpenWrt の方法論を踏襲した
Buildroot ベースの共通オープンファームウェアです。本書は **コミットを著作権上
クリーンに積み上げるためのルール**を定めます。OpenWrt / Linux / Buildroot /
OpenIPC と同じ作法（DCO・SPDX・clean-room 規律）を採用します。

> ディレクトリ構造とビルドの全体像は [docs/architecture.md](docs/architecture.md)、
> clean-room の具体的な作業手順は [docs/clean-room.md](docs/clean-room.md) を参照。

---

## 0. スコープの線引き

OpenIRD は **中立なトランスポートのみ**を提供する:

- 生 TS を読める **px4_drv 互換 chardev**（チューナの ABI 契約）。
- **PC/SC スマートカードトランスポート**
  （`pcsc-lite` + `ccid`、非 CCID リーダは clean-room な ifd-handler）。

以下は **スコープ外**。該当する PR は受理しない:

- **限定受信の復号そのもの** — MULTI2 / ECM→Ks / keyladder / ARIB スクランブル
  解除の鍵・手順。これらはOpenIRD Projectの外に属する。
- カードに APDU を通すこと自体は復号ではなく中立＝スコープ内。復号ロジックだけが
  スコープ外。この区別を混同しないこと。
- **配布物にベンダ由来バイナリを 1 バイトも含めない**。吸い出したダンプ・blob・
  ファーム・鍵は「解析対象」であって配布素材でも commit 対象でもない。
- **アップデート機構の鍵、および鍵を機器から抽出するツールもスコープ外。** 鍵は
  所有者が自分の機器から回収し、ビルド時に外部入力として渡すものであり、回収手段
  （分解ダンプ、脆弱性を用いた root 取得など）自体は OpenIRD の配布物・リポジトリ
  には含めない。受け付けるのは鍵を**消費する側**のツールに限る:
  - factory.bin 等の書き込みイメージを生成するツール（鍵は引数/ファイルで受け取り、
    埋め込まない）
  - 機器が既に公開しているアップデートエンドポイント（UPnP 等）を叩くだけの
    クライアント（鍵を扱わない）

---

## 1. ライセンスと SPDX

- リポジトリ全体のライセンスは **GPL-2.0-only**（`LICENSE` は public 化時に設置）。
- 新規ソースファイルの先頭に SPDX 識別子と著作権行を置く:

  ```c
  // SPDX-License-Identifier: GPL-2.0-only
  // Copyright (C) 2026 <Your Name>
  ```

  スクリプト/Makefile/Config.in も `#` コメントで同様に。
- GPL カーネル等へのパッチは上流のライセンス（GPL-2.0）を継承し、上流著作者を
  尊重する。自分のパッチ部分にのみ自分の著作権行を足す。

---

## 2. DCO（原産地証明）
DCO採用プロジェクトでは、コミットごとに「権利的にクリーンなコードである」と自ら署名・保証する必要がある。
 - git commit -s の実行：コミット時に -s を付け、Signed-off-by 署名を必ず挿入する。
 - 本人を特定できる名義：匿名やその場限りのハンドル名ではなく、本名または追跡可能な普段使う identityとメールを使う。
 - 権利の確認：プロプライエタリなバイナリの逆アセンブリや勤務先のコードを勝手に混ぜていないか事前に確認する。


---

## 3. clean-room 規律

順序は **観察 → 仕様化 → 実装** で固定。ベンダバイナリの逆アセを見ながら自前
コードを書かない。理想は観察者と実装者を分ける。単独なら時間で分け、**コミットの
trailer と仕様参照がその証跡**になる。詳細は [docs/clean-room.md](docs/clean-room.md)。

各コミットは provenance を trailer で宣言する（機械検査あり）:

| trailer | 意味 |
|---|---|
| `Origin: clean-room` | 仕様だけを根拠に書いた自前コード（ベンダ内部を参照していない） |
| `Origin: gpl-derived` | GPL 上流（kernel / uClibc / busybox 等）の派生・パッチ |
| `Origin: observed` | ブラックボックス観察（strings / usage / ログ）由来の仕様・記録 |
| `Origin: upstream` | 無改変で取り込んだ上流定義 |
| `Spec-Ref: docs/specs/<file>#<section>` | **`Origin: clean-room` の実装では必須**。根拠の仕様節を引く |

- `docs/specs/` に置く仕様は **Open 層のみ**（観察・公開データ由来）。個体識別情報
  （MAC / S/N / Wi-Fi キー等）は投入前に伏字化すること。

### 3.1 仕様の実装状態を宣言する

`docs/specs/*.md` は H1 の直後に機械可読の `openird-spec` ヘッダを置き、
`Status` / `As-of` / `Impl` / `Tests` を宣言する。**付け忘れはテストが落とす。**

```
<!-- openird-spec
Status: verified
As-of:  BLUR v2.1.17 ほか純正 29 本 / 実機 POE-BR100-YB1 (2026-09-21)
Impl:   host/openird-mkimage/ird/v1b.py, host/openird-mkimage/ird/zipcrypto.py
Tests:  tests/mkimage/
-->
```

テスト側は `check(..., spec="<ファイル名>#<節番号>")` で仕様の節を引く（散文の解析では
なく構造化された引数）。検査は **片側だけを縛る**:

- **妨げないもの**: 仕様が先行している状態。仕様を直した直後に実装が古いのは
  「観察 → 仕様化 → 実装」の正常な途中経過であり、未実装であることの裏付けでもある。
  `Status: specified` で実装もテストも無い仕様は無条件で通る。∴ 仕様だけのコミット
  （`Origin: observed`）は何も要求されない。
- **落とすもの**: ①実装していないのに `implemented` / `verified` と宣言している
  （過大申告）。②実装してテストまで書いたのに `specified` のまま（宣言の更新漏れ）。

∴ `Status` は目安ではなく**検証される宣言**になる。値の一覧と運用は
[docs/specs/README.md](docs/specs/README.md)、検査の実体は
[tests/spec_status.py](tests/spec_status.py)。

---

## 4. コミットの形式

- subject は `<area>: <要約>`。**要約は日本語でも英語でもよい**。area 接頭辞のみ
  小文字 ASCII に統一する（grep / 集計のため）。長さは**一行に収まる程度**を目安に、
  厳密な字数制限は設けない（フックも長さは強制しない）。例:
  - `emma3sep: POE-BR100-YB1 の defconfig 骨格を追加`
  - `ird-tuner-mxl683: chardev 骨格の初版`
  - `mkimage: add BLUR family container writer`
- 1 論理変更 = 1 コミット。カーネルパッチは番号付きで分割。
- コミットテンプレートを有効化すると trailer の雛形が出る:

  ```
  git config commit.template .gitmessage
  ```

---

## 5. フックの導入（ローカル検査）

clone 後に一度だけ:

```
tools/setup-hooks.sh
```

これで以下が commit 前後に走る:

- `pre-commit` … blob・鍵・公開しない作業メモ・巨大ファイルの混入を拒否（ガードレール）。
- `commit-msg` … subject 形式・`Signed-off-by`・`Origin`、および `clean-room`
  実装に対する `Spec-Ref` の有無を検査。

仕様の `openird-spec` ヘッダ（§3.1）はフックではなく**テスト**が検査する。
コミット時ではなく `make test-mkimage` で落ちる（秘密もベンダ資材も要らないので
どの環境でも回せる）。

CI（public 化後）でも同じ検査を再実行する。

---

## 6. パッチ提出

- GitHub PR。（format-patch受け取り不可）
- 各パッチが単体でビルドを壊さないこと（`make BOARD=<device>` が通る単位）。
- ベンダ GPL tarball は commit しない（`dl/` にキャッシュ、`*.hash` で検証）。

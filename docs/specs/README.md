# docs/specs — 機器仕様（Open 層のみ）

clean-room のファイアウォール。観察 → 実装の間に挟む「文章の仕様」を置く。

- **Open 層のみ**: 分解写真・起動ログ・GPL ソース読解・公開データ・ブラックボックス
  観察（strings / usage / `/proc` / バス信号）由来の事実のみ。
- **入れないもの**: 逆アセ由来の内部ロジック・具体アドレス・コード由来の数式は
  `*_Close.md`（git 管理外）へ。限定受信の復号に関する記述は一切置かない。
- **伏字化**: 個体識別情報（MAC アドレス・シリアル番号・Wi-Fi キー等）は投入前に
  伏字化する。

ファイル命名: `spec_<board>.md`（例 `spec_bmmpx1301.md`, `spec_sb-tv02-wfpl.md`,
`spec_sb-tv06-wrip.md`）。実装コミットは `Spec-Ref:` でこれらの節を引く。

**形式・機構の仕様**は対象を表す kebab-case 名にする（機種仕様 `spec_<board>.md` と区別）。
機種を跨ぐ形式は機種名を冠さない。

| ファイル | 対象 | 層 |
|---|---|---|
| [rdhb-kernel-container.md](rdhb-kernel-container.md) | SB-TV02-WFPL mtd5 のカーネル容器（RDHB） | 形式 |
| [update-v1b-container.md](update-v1b-container.md) | emma3sep 系の更新コンテナ（UPnP factory 経路） | 形式 |
| [sb-tv02-wfpl-usb-update.md](sb-tv02-wfpl-usb-update.md) | SB-TV02-WFPL の USB 更新（end-user factory 経路） | 機構 |
| [poe-br100-yb1-nand-update.md](poe-br100-yb1-nand-update.md) | POE-BR100-YB1 の NAND 書き込み（factory の書き込み側） | 機構 |

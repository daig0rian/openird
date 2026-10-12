# docs/specs — 機器仕様（Open 層のみ）

clean-room のファイアウォール。観察 → 実装の間に挟む「文章の仕様」を置く。

- **Open 層のみ**: 分解写真・起動ログ・GPL ソース読解・公開データ・ブラックボックス
  観察（strings / usage / `/proc` / バス信号）由来の事実のみ。
- **入れないもの**: 逆アセ由来の内部ロジック・具体アドレス・コード由来の数式
  （OpenIRD では扱わない）。限定受信の復号に関する記述は一切置かない。
- **伏字化**: 個体識別情報（MAC アドレス・シリアル番号・Wi-Fi キー等）は投入前に
  伏字化する。

ファイル命名: `spec_<board>.md`（例 `spec_bmmpx1301.md`, `spec_sb-tv02-wfpl.md`,
`spec_sb-tv06-wrip.md`）。実装コミットは `Spec-Ref:` でこれらの節を引く。

**形式・機構の仕様**は対象を表す kebab-case 名にする（機種仕様 `spec_<board>.md` と区別）。
機種を跨ぐ形式は機種名を冠さない。

| ファイル | 対象 | 層 | Status |
|---|---|---|---|
| [rdhb-kernel-container.md](rdhb-kernel-container.md) | SB-TV02-WFPL mtd5 のカーネル容器（RDHB） | 形式 | `verified` |
| [update-v1b-container.md](update-v1b-container.md) | emma3sep 系の更新コンテナ（UPnP factory 経路） | 形式 | `verified` |
| [mbin-firmware-container.md](mbin-firmware-container.md) | MaxLinear 復調器のファーム容器（MBIN・段階 C の前提） | 形式 | `implemented` |
| [emma3sep-upnp-update.md](emma3sep-upnp-update.md) | emma3sep 系の UPnP 更新エンドポイント（factory の投入側） | 機構 | `verified` |
| [poe-br100-yb1-nand-update.md](poe-br100-yb1-nand-update.md) | POE-BR100-YB1 の NAND 書き込み（factory の書き込み側） | 機構 | `verified` |
| [emma3sep-airtuner-gpio.md](emma3sep-airtuner-gpio.md) | AirTuner 基板（EMMA3SEP）の LED と再起動（GPIO の割り当て・自己リセット線） | 機構 | `implemented` |
| [sb-tv02-wfpl-usb-update.md](sb-tv02-wfpl-usb-update.md) | SB-TV02-WFPL の USB 更新（end-user factory 経路） | 機構 | `specified` |
| [sb-tv02-wfpl-libooter-env.md](sb-tv02-wfpl-libooter-env.md) | SB-TV02-WFPL の libooter env（mtd1・cmdline・MAC の所在） | 機構 | `specified` |
| [sb-tv02-wfpl-vendor-boot.md](sb-tv02-wfpl-vendor-boot.md) | SB-TV02-WFPL の起動連鎖と工場モード | 機構 | `reference` |
| [sb-tv02-wfpl-stage-b-fapex.md](sb-tv02-wfpl-stage-b-fapex.md) | SB-TV02-WFPL の FAPex chardev/ioctl 契約（段階 B の観察） | 契約 | `observing` |
| [poe-br100-yb1-reception-stack.md](poe-br100-yb1-reception-stack.md) | POE-BR100-YB1 の受信スタック構成と chardev 境界（段階 B の観察） | 契約 | `observing` |
| [poe-br100-yb1-ts-fifo.md](poe-br100-yb1-ts-fifo.md) | POE-BR100-YB1 の生 TS の取り出し（AV SRAM の TS 入力 FIFO・PID の列挙・ドレイナー・chardev への対応） | 機構 | `specified` |
| [poe-br100-yb1-av-command.md](poe-br100-yb1-av-command.md) | POE-BR100-YB1 の AV コマンド経路（ベンダの `.ko` 無しでデマルチプレクサを組む・フィルタの状態遷移） | 機構 | `specified` |
| [poe-br100-yb1-subcpu-control.md](poe-br100-yb1-subcpu-control.md) | POE-BR100-YB1 の裏 CPU 制御インターフェース（BHIF の CPU 制御ブロック・アドレス窓・割り込みの振り分け・デバッグの口） | 機構 | `observing` |

## 実装状態の宣言（`openird-spec` ヘッダ）

各仕様は H1 の直後に機械可読のヘッダを持つ。**付け忘れはテストが落とす。**

```
<!-- openird-spec
Status: verified
As-of:  BLUR v2.1.17 ほか純正 29 本 / 実機 POE-BR100-YB1 (2026-09-21)
Impl:   host/openird-mkimage/ird/v1b.py, host/openird-mkimage/ird/zipcrypto.py
Tests:  tests/mkimage/
Note:   （任意）節ごとの差など、enum で表せない補足
-->
```

1 行に `Key: value` を 1 つ書く。キーは行頭から。値が長いときは、次の行を**字下げして**
続けてよい（字下げした行は、直前の項目の値の続きとして読む）:

```
Note:   物理 IC と論理単位の俯瞰。詳細は別の仕様にある。
        2026-09-30 に比較の節を足した。
```

| Status | 意味 |
|---|---|
| `observing` | 観察中。仕様は未完成 |
| `specified` | 仕様化済み・**未実装**。clean-room の正常な途中経過 |
| `implemented` | 実装済み・実機未検証 |
| `verified` | 実装済み・実機検証済み |
| `reference` | 実装対象ではない観察記録 |
| `frozen` | 対象が消えた／保留 |

### 何を縛り、何を縛らないか

**縛るのは片側だけ。** 「仕様が新しく実装が古い」は観察 → 仕様化 → 実装の正常な
途中経過なので妨げない。仕様だけのコミット（`Origin: observed`）は何も要求されない。
落とすのは次の 2 方向:

- 実装していないのに `implemented` / `verified` と宣言している（**過大申告**）
- 実装してテストまで書いたのに `specified` のまま（**宣言の更新漏れ**）

∴ Status は「目安」ではなく**検証される宣言**になる。書き換えを忘れると落ちる。

### テストからの引用

テストは `check(..., spec="<ファイル名>#<節番号>")` で仕様の節を引く。散文の解析では
なく構造化された引数なので、書式のゆらぎで壊れない。

```python
check("`.sh` のモードが 0755", cond, spec="update-v1b-container#5")
```

**節単位の網羅は強制しない**（§0 の鍵の扱いや §7 の未解明点のように、テストの書き
ようが無い節があるため）。代わりに `make test-mkimage` が節ごとの被引用を報告する
ので、埋めるべき穴は一覧で見える。

検査の実体は [tests/spec_status.py](../../tests/spec_status.py)。

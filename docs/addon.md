# OpenIRD のアドオン（実行時パッケージ）

OpenIRD の像は不変（squashfs の RO ルート）。**アドオン（add-on）は、像を焼き直さずに、
実行中の機器へ後から載せるソフト**。追加分はすべて永続 overlay に入り、base には一切
触れない。アドオンは **アドオンリポジトリ**（HTTPS 上の索引付き静的集合）から
インストーラ `ird-addon` で導入する。

この文書は **OpenIRD 貢献者**（アドオン機構を実装する側）と **アドオン作者**
（アドオンを作って配る側）向けの設計仕様。recpt1 のような chardev 契約の「上」の
ソフトは、アドオンとして載せる。

> **OpenWrt の「feed」という語は使わない（重要）**: OpenWrt は単一の .ipk/opkg の上で
> `y`/`M` を振るだけなので「feed」のパッケージはビルド時にも実行時にも**倒せる（互換が
> ある）**。OpenIRD は Buildroot でパッケージマネージャを持たず、**ビルド時と実行時は
> まったく別の注入機構で互換が無い**。一語「feed」を両者の傘に使うと「片方へ回せる」と
> いう誤解＝事故を招くので、OpenIRD は feed という語を**一切使わず**、別名を与える —
> ビルド時＝**ビルドオプション**、実行時＝**アドオン**、その配布元＝**アドオンリポジトリ**。
> （OpenIPC 等の Buildroot 外部ツリー系は注入経路がビルド時 1 本だけで、そもそも feed の
> 語彙を持ち込んでいない。OpenIRD の実行時アドオンはそれより踏み込んだ領域で、継ぐべき
> 既存語彙が無いので自前で命名する。）

## 0. 2つの注入機構（互換なし・別物）

| 機構 | 名前 | いつ／どこ | 選び方 | 例 |
|---|---|---|---|---|
| build-time | **ビルドオプション** | 像のビルド時に焼く（base の一部） | `BR2_EXTERNAL` の package を defconfig で選択 | `ird-tuner-*`・`ird-webui` |
| runtime | **アドオン** | 実行中の機器・永続 overlay に後載せ | アドオンリポジトリから `ird-addon` で導入 | recpt1 等・chardev の上 |

- **機構はスコープ内**（base／ホストが用意する土台）: アドオン形式（`.ird`）・リポジトリの
  索引形式・インストーラ `ird-addon`・SDK・置き場所の規約。実装も仕様も本リポジトリ。
- **アドオンの中身／アドオンリポジトリはスコープ外**（recpt1 等）: 別リポジトリ。OpenIRD は
  規約だけ定め、何を載せるかは決めない（CLAUDE.md §4.1）。
- 素の OpenIRD 像単体では chardev はあるが読む者がいない。読む者（recpt1 等）はアドオンで
  載せる＝設計どおり。
- ビルドオプションの詳細は Buildroot/`BR2_EXTERNAL` の流儀そのもの（[architecture.md](architecture.md)）。
  本書は**アドオンとアドオンリポジトリ**（実行時側）を規定する。

## 1. 前提＝永続 union（検証済み）

ルートは **squashfs（RO, lower）＋ overlay（RW, upper）の union**。
- emma3sep（2.6.27）は **unionfs ＋ mtd9 の yaffs2** を upper（サブディレクトリ
  `openird/overlay/`）、fujitsu（2.6.31）は **overlayfs**。切り替えは preinit
  （`openird-preinit`）が自動（[kernel-overlayfs.md](kernel-overlayfs.md)）。
- **アドオンは overlay にだけ書く。base（squashfs）には触れない。**
- **忘却しても安全に**: overlay のサブツリーを wipe すれば全アドオンが消え、pristine
  base に戻る（＝ factory reset）。overlay が唯一の可変状態。
- emma3sep の union 永続は cold reboot を跨いで実機確認済み
  （`/etc/init.d`・`/usr/bin` への追加が残存）。

## 2. アドオンの形式（軽量バンドル）

1 アドオン = 1 ファイル **`<name>_<version>_<arch>.ird`**（gzip 圧縮 tar）。中身:

```
manifest                 # key=value のメタデータ
root/                    # / に重ねるツリー（例 root/usr/bin/recpt1, root/etc/init.d/S80recpt1）
postinstall              # 任意: 展開後に走らせる sh
preremove                # 任意: 削除前に走らせる sh
```

`manifest`（1 行 1 項目・`Key: value`）:

| Key | 要/任 | 意味 |
|---|---|---|
| `Package` | 要 | アドオン名 |
| `Version` | 要 | 版（暦 or 上流版＋pin。一意であればよい） |
| `Arch` | 要 | SDK が定める target id（例 `emma3sep-mipseb-musl`）。不一致は拒否 |
| `Requires-OpenIRD` | 任 | base の互換範囲（ABI が変わる更新に備えた予約） |
| `Depends` | 任 | 将来拡張の予約。**当面は使わない**（アドオンは自己完結前提） |
| `Description` | 任 | 一行説明 |

- **自己完結が前提**（静的リンク、または必要な lib を `root/` に同梱）＝当面は依存解決を
  しない。`Depends` は形式に場所だけ用意しておく。
- `root/` は `/` に重ねる。**base のファイルを上書きしない**こと（新規追加のみ）。
  上書き（copy-up）は `remove` の意味が濁る（whiteout と base 復帰の整合）ので避ける。
- 常駐サービスは `root/etc/init.d/SxxNAME` に置く（BusyBox init が次回起動で走らせる。
  即時起動は `postinstall` で `SxxNAME start` を呼ぶ）。

## 3. アドオンリポジトリの索引

アドオンリポジトリ = HTTPS で配る 1 ディレクトリ（静的ファイルでよい。GitHub Pages・S3・
nginx 等）。索引ファイル **`Packages`**（空行区切りのスタンザ、Debian/opkg 風の最小形）:

```
Package: recpt1
Version: 1.2.0-77c7bce
Arch: emma3sep-mipseb-musl
Filename: recpt1_1.2.0-77c7bce_emma3sep-mipseb-musl.ird
SHA256: <64 hex>
Size: 285280
Description: ISDB-T/S recorder (px4_drv/pt1 ABI client)
```

1 スタンザ = 1 アドオン。**`SHA256` が信頼アンカー。** 索引に載るアドオンは 1 個でも多数でも
よい（＝「名前で引ける集合」）。リポジトリとアドオンの中身は OpenIRD が決めない
（スコープ外）。この形式に従って配るだけ。

## 4. 検証（sha256 pin ＋ HTTPS）

- 取得は **HTTPS**。アドオンは索引（または CLI 引数）の **SHA256 と照合してから**展開。
- 署名鍵は持たない（鍵の台帳・配布・失効管理を増やさない）。sha256 pin はリポジトリ
  共通の規律（toolchain・Buildroot 等と同じ）。

## 5. インストーラ `ird-addon`（base 同梱・スコープ内）

アドオンリポジトリからアドオンを導入／削除する道具。

| コマンド | 動作 |
|---|---|
| `ird-addon update` | `addons.conf` の各リポジトリから `Packages` を取得し overlay にキャッシュ |
| `ird-addon install <name>` | 索引から解決→取得→SHA256 照合→Arch 検査→`root/` を `/` に展開→receipt 記録→`postinstall` |
| `ird-addon install <url> <sha256>` | 索引を介さず 1 本を直接（one-off。リポジトリ不要） |
| `ird-addon remove <name>` | `preremove`→receipt の files を消す |
| `ird-addon list` / `info <name>` | 導入済み一覧／詳細 |

- 設定 `/etc/openird/addons.conf`（アドオンリポジトリの URL 群）。**base 既定は空**
  （スコープ外の URL を像に焼かない）。所有者がリポジトリを足す。
- receipt `/var/lib/ird-addon/<name>.{manifest,files}`（overlay 上）。**中央 DB ではなく
  per-addon の受領記録**で、`remove` を正確にするためだけに持つ。overlay wipe で一緒に
  消える＝台帳を増やさない。
- 展開は通常のファイルパスへ書く（union が overlay に回す）。base には書かない。
- Arch 不一致・SHA256 不一致・base ファイルの上書きは拒否。

## 6. SDK（アドオンのビルド土台・スコープ内）

```
make sdk BOARD=<board>   ->  openird-sdk-<board>-<ver>.tar.gz
```

中身:
- 再配置可能な **musl クロス toolchain**（`output/<board>/host`）＋ **target sysroot**
  （headers／libs）。Buildroot の SDK 再配置機構を使う（正確な Buildroot 側の成果物名は
  実装時に確認）。
- `sdk.env`（`CC` prefix・`SYSROOT`・`ARCH`=manifest の `Arch` に使う id）。
- `ird-mkaddon`（staging ディレクトリ → `.ird` アドオンを生成するホストツール）。
- README。

アドオン作者は OpenIRD/Buildroot ツリー無しで、**ABI/libc 整合のまま**アドオンを作れる。
`tools/host/build-recpt1.sh` が使っている toolchain 依存を、これで正式化する
（SDK があれば build-recpt1.sh は SDK を指すだけになる）。Arch id は SDK が定義し、
manifest の `Arch` と一致必須。

## 7. ライフサイクルと sysupgrade

- 像更新（sysupgrade）は overlay を**既定で保持**（設定・アドオンが残る）。ABI 非互換な
  更新は `Arch` / `Requires-OpenIRD` で弾く。
- factory 導入・overlay wipe でアドオンは全消え（safe to forget）。

## 8. ライセンス / clean-room

- 機構（`ird-addon`・`ird-mkaddon`・SDK・`.ird` 形式・索引形式）＝ OpenIRD（GPL-2.0）。
  clean-room の逆アセ対象ではない（独自設計）。
- アドオンの中身・アドオンリポジトリ＝外部・各自のライセンス。OpenIRD 配布物には含めない。

## 9. 実装状態

すべて未実装（**specified**）。実装の段取り:

1. `ird-addon` ＋ `.ird` 形式 ＋ 索引パーサ（base の `ird-addon` ビルドオプション）。
2. `make sdk`（toolchain＋sysroot の再配置 tarball）＋ `ird-mkaddon`。
3. 最初のアドオン例として **recpt1 を別リポジトリ**（アドオンリポジトリ）で `.ird` 化し、
   実機導入→再起動保持→`/dev/irdvideo0` から録画までを検証（§1 の永続 union に載る）。

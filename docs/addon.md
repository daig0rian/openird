# OpenIRD のアドオン（実行時パッケージ）

OpenIRD のイメージは不変（squashfs の RO ルート）。**アドオン（add-on）は、イメージを焼き直さずに、
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
| build-time | **ビルドオプション** | イメージのビルド時に焼く（base の一部） | `BR2_EXTERNAL` の package を defconfig で選択 | `ird-tuner-*`・`ird-webui` |
| runtime | **アドオン** | 実行中の機器・永続 overlay に後載せ | アドオンリポジトリから `ird-addon` で導入 | recpt1 等・chardev の上 |

- **機構はスコープ内**（base／ホストが用意する土台）: アドオン形式（`.ird`）・リポジトリの
  索引形式・インストーラ `ird-addon`・SDK・置き場所の規約。実装も仕様も本リポジトリ。
- **アドオンの中身／アドオンリポジトリはスコープ外**（recpt1 等）: 別リポジトリ。OpenIRD は
  規約だけ定め、何を載せるかは決めない（CLAUDE.md §4.1）。
- 素の OpenIRD イメージ単体では chardev はあるが読む者がいない。読む者（recpt1 等）はアドオンで
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

1 アドオン = 1 ファイル **`<name>_<version>_<arch>.ird`**（gzip 圧縮の tar。ustar）。中身:

```
manifest                 # Key: value のメタデータ
root/                    # / に重ねるツリー（例 root/usr/bin/recpt1, root/etc/init.d/S80recpt1）
postinstall              # 任意: 展開後に走らせる sh
preremove                # 任意: 削除前に走らせる sh
```

メンバはこの 4 種だけ（ほかが入っていれば拒否）。`root/` に入れられるのは通常ファイル・
ディレクトリ・symlink。

`manifest`（1 行 1 項目・`Key: value`・ASCII のみ＝機器のシリアルコンソールに出る）:

| Key | 要/任 | 意味 |
|---|---|---|
| `Package` | 要 | アドオン名。英小文字・数字で始まり、英小文字・数字・`.` `+` `-` |
| `Version` | 要 | 版（暦 or 上流版＋pin）。一意であればよい＝**版に順序は無い**。英数字・`.` `+` `~` `-` |
| `Arch` | 要 | 機器の target id（例 `emma3sep-mipseb-musl`）。不一致は拒否 |
| `Requires-OpenIRD` | 予約 | base の互換範囲（ABI が変わる更新に備える） |
| `Depends` | 予約 | 将来拡張。**当面は使わない**（アドオンは自己完結前提） |
| `Description` | 任 | 一行説明 |

- ファイル名の区切りが `_` なので、`Package`・`Version`・`Arch` は `_` を含められない。
- **Arch id** は「SoC ファミリ（＝カーネル）・CPU・libc」の組。定義は機種の
  `/etc/openird/board.conf` の **`ADDON_ARCH=`** 1 か所で、機器（`ird-addon`）も SDK
  （`sdk.env` の `IRD_ARCH`）もそこから取る。
- **予約キーは場所だけ**。意味が決まるまで、値を持つアドオンを `ird-mkaddon` は作らず、
  `ird-addon` は入れない（黙って無視すると、あとで意味を与えたときに古い base が条件を
  素通しする）。
- **自己完結が前提**（静的リンク、または必要な lib を `root/` に同梱）＝当面は依存解決を
  しない。
- `root/` は `/` に重ねる。**base のファイルを上書きしない**こと（新規追加のみ）。
  上書き（copy-up）は `remove` の意味が濁る（whiteout と base 復帰の整合）ので避ける。
  他のアドオンや所有者が置いたファイルも上書きできない。ディレクトリは共有できる
  （最後の 1 つが消えるときに、空で base に無ければ消える）。
- **置けない場所**: `/dev` `/proc` `/sys` `/tmp` `/run`（揮発・仮想）、`/rom` `/overlay`
  （union の 2 層そのもの）、`/var/lib/ird-addon`（受領記録）。base で symlink になっている
  ディレクトリ（`/var/run` → `/run` など）の下にも置けない。
- 常駐サービスは `root/etc/init.d/SxxNAME` に置く（BusyBox init が次回起動で走らせる。
  即時起動は `postinstall` で `SxxNAME start` を呼ぶ）。
- `postinstall` / `preremove` は機器の `sh` が `/` をカレントにして走らせる。`postinstall` が
  失敗しても展開したファイルは残る（`ird-addon` は非 0 で終わる）。`preremove` が失敗しても
  削除は続ける。
- `ird-mkaddon` が作る `.ird` は**同じ入力から同じバイト列**になる（作り直すたびに
  sha256 が変わらないように）: メンバは `manifest`・スクリプト・`root/` の名前順、所有者は
  0:0、モードは実行ビットの有無で 755 / 644 の 2 通り、時刻は `SOURCE_DATE_EPOCH`
  （無ければ 0）。

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

1 スタンザ = 1 アドオン。**`SHA256` が信頼アンカー**（その前提は §4）。索引に載るアドオンは
1 個でも多数でもよい（＝「名前で引ける集合」）。リポジトリとアドオンの中身は OpenIRD が
決めない（スコープ外）。この形式に従って配るだけ。

- `Filename` はリポジトリの URL からの相対パス（上へ出るパス・別の URL は拒否）。
- **1 つのリポジトリに置けるのは `Package` と `Arch` の組ごとに 1 版だけ。** 版に順序が
  無いので、2 版あっても機器はどちらを入れるべきか決められない。更新は「索引の版が
  導入済みの版と違えば入れ替える」。複数の機種向けは `Arch` の違うスタンザを並べる
  （機器は自分の `Arch` のスタンザしか見ない）。
- 索引は `ird-mkaddon index <dir>` が `<dir>` の `*.ird` から作る（§6）。

## 4. 検証（sha256 pin ＋ HTTPS）

- 機器の外から取るものは **HTTPS だけ**（`http://` は拒否）。ほかに受け付けるのは手元の
  ファイル（パスか `file://`）＝ nc や USB で運んだ `.ird`、手元に置いたリポジトリ。
- アドオンは索引（または CLI 引数）の **SHA256 と照合してから**展開する。索引に `Size` が
  あればそれも照合する。
- 署名鍵は持たない（鍵の台帳・配布・失効管理を増やさない）。sha256 pin はリポジトリ
  共通の規律（toolchain・Buildroot 等と同じ）。

**いまの HTTPS が保証する範囲（未決・§9）**: base の取得は BusyBox `wget` の内蔵 TLS で、
これは**サーバ証明書を検証しない**（期限切れ・ホスト名違い・自己署名の証明書でも通ることを
実測で確認。`wget` 自身が取得のたびに `TLS certificate validation not implemented` と出す）。
したがって、信頼は SHA256 の出どころで決まる:

| 経路 | SHA256 の出どころ | 守られるもの |
|---|---|---|
| `install <url\|file> <sha256>` | 所有者が別経路で得た値 | 取得経路によらず、中身がその値と一致すること（改ざん・破損とも検出） |
| `install <name>` | HTTPS で取った索引 | 破損と、経路の途中での盗み見。**応答を差し替えられる相手（中間者）は索引ごと差し替えられる** |

∴ いまの実装で「索引の SHA256 が信頼アンカー」と言えるのは、リポジトリまでの経路を
信頼できるときだけ。経路を信頼しない場合の確かな道は、sha256 を別経路で得て直接入れる方。

## 5. インストーラ `ird-addon`（base 同梱・スコープ内）

アドオンリポジトリからアドオンを導入／削除する道具。

| コマンド | 動作 |
|---|---|
| `ird-addon update` | `addons.conf` の各リポジトリから `Packages` を取得し overlay にキャッシュ |
| `ird-addon available` | 取得済みの索引のうち、この機器の `Arch` 向けのアドオンを並べる |
| `ird-addon install <name>` | 索引から解決→取得→SHA256 照合→Arch 検査→`root/` を `/` に展開→receipt 記録→`postinstall` |
| `ird-addon install <url\|file> <sha256>` | 索引を介さず 1 本を直接（one-off。リポジトリ不要） |
| `ird-addon remove <name>` | `preremove`→receipt の files を消す |
| `ird-addon list` / `info <name>` | 導入済み一覧／詳細 |

- 設定 `/etc/openird/addons.conf`（アドオンリポジトリの URL 群・1 行 1 つ）。**base 既定は空**
  （スコープ外の URL をイメージに焼かない）。所有者がリポジトリを足す。同じ名前が複数の
  リポジトリにあれば、先に書いた方。
- receipt `/var/lib/ird-addon/<name>.{manifest,files}`（overlay 上。`preremove` があれば
  `<name>.preremove` も）。**中央 DB ではなく per-addon の受領記録**で、`remove` を正確に
  するためだけに持つ。overlay wipe で一緒に消える＝台帳を増やさない。索引のキャッシュは
  同じ場所の `lists/`。
- 展開は通常のファイルパスへ書く（union が overlay に回す）。base には書かない。
- **拒否は、アドオンのファイルを 1 つも置く前に済ませる**: SHA256・Size・Arch の不一致、予約キー、
  形式に無いメンバ、§2 の置き場所の決まり（base・他のアドオン・所有者のファイルの上書き、
  置けない場所）。
- 導入済みの名前をもう一度 `install` すると**入れ替え**（古い方の `preremove` → 削除 → 新しい方の
  展開 → `postinstall`）。名前で入れるとき、索引の版が導入済みと同じなら何もしない。
- **途中で止まっても片づけられる**: `<name>.files` はコピーの前に、`<name>.manifest` は最後に
  書く。`manifest` の無い receipt は未完了の導入で、`list` がそう表示し、`remove`（か入れ直し）
  で消える。コピーに失敗したとき（overlay が満杯など）は、その場で入れた分を取り消す。
- `remove` は receipt にあるパスだけを消す。ディレクトリは空で base に無いときだけ。base が
  後から同じパスを持つようになっていたら（イメージの更新）、そのファイルは消さずに知らせる
  （消すと whiteout が base のファイルを隠す）。
- overlay で起動していない（読み取り専用にフォールバックした）ときは導入も削除もしない。
  overlay が tmpfs のとき（再起動で消える）は警告して続ける。
- 道具は BusyBox の ash とアプレットだけ（`wget`・`tar`・`gunzip`・`sha256sum`・`awk`）。
  `wget` の HTTPS は Buildroot の既定では無効なので、機種の `busybox.fragment` で有効にする
  （無ければパッケージがビルド時に止める）。

## 6. SDK（アドオンのビルド土台・スコープ内）

```
make sdk BOARD=<board>   ->  output/<board>/images/openird-sdk-<ver>-<arch-id>.<host>.tar.xz
                             例 openird-sdk-26.10-emma3sep-mipseb-musl.Linux-x86_64.tar.xz
```

**SDK の単位は機種ではなく Arch id**（＝アドオンの互換の単位。同じ Arch id の機種は 1 本を
共用する）。`<board>` は、その Arch id の toolchain と sysroot を作るビルド元。`<ver>` は版の
タグからの `git describe`、`<host>` は toolchain が動くホストの種類。

中身（展開すると `openird-sdk-<ver>-<arch-id>/`）:
- 再配置可能な **musl クロス toolchain**（C と C++。C++ の標準ライブラリは静的な `libstdc++.a`）＋
  **target sysroot**（headers／libs）＝ Buildroot の
  `output/<board>/host` そのもの。再配置は Buildroot の仕組みに任せる（`prepare-sdk` が
  `relocate-sdk.sh` を置く。展開した先で 1 回走らせる）。
- `sdk.env`（`.` で読む）: `IRD_ARCH`（manifest の `Arch` に使う id）・`CROSS_COMPILE`
  （toolchain の prefix）・`CC`・`CXX`・`SYSROOT`・`OPENIRD_SDK`（SDK の場所）と `PATH`。
  id の変数名を `ARCH` にしないのは、カーネルや多くの Makefile が `ARCH` を別の意味
  （`mips` 等）で読むから。
- `bin/ird-mkaddon`（単一ファイル・Python 3 の標準ライブラリだけ）:
  - `ird-mkaddon build <staging> [-o <dir>] [--arch <id>]` … staging ディレクトリ → `.ird`。
    manifest に `Arch` が無ければ `--arch`、それも無ければ `$IRD_ARCH` を入れる（同じ
    staging を複数の機種向けに作れる）。
  - `ird-mkaddon index <dir>` … `<dir>` の `*.ird` → 索引 `<dir>/Packages`（§3）。
- `README.md`（使い方）。

アドオン作者は OpenIRD/Buildroot ツリー無しで、**ABI/libc 整合のまま**アドオンを作れる。
`tools/host/build-recpt1.sh` が使っている toolchain 依存を、これで正式化する
（SDK があれば build-recpt1.sh は SDK を指すだけになる）。Arch id は機種の `board.conf` の
`ADDON_ARCH=` が定義し（§2）、SDK はそれを `IRD_ARCH` として渡す。manifest の `Arch` と一致必須。

### 6.1 配布（リリース）

SDK は**リリースごとに、本体リポジトリの GitHub Releases に置く**（作り方は
[release.md](release.md)）。1 つのリリース（タグ `v<ver>`）に、Arch id ごとに:

| ファイル | 中身 |
|---|---|
| `openird-sdk-<ver>-<arch-id>.Linux-x86_64.tar.xz` | SDK |
| `openird-<ver>-<arch-id>-sources.tar` | 対応するソース一式（SDK は GCC などの GPL バイナリを含むので、同じ場所に置く。Buildroot の `legal-info`） |
| `SHA256SUMS` | そのリリースの全ファイルの sha256 |

- **使う側（アドオンのリポジトリ）は URL と sha256 を pin する**（本体が Buildroot や
  toolchain を pin するのと同じ規律。署名鍵は持たない）:
  ```
  https://github.com/daig0rian/openird/releases/download/v<ver>/openird-sdk-<ver>-<arch-id>.Linux-x86_64.tar.xz
  ```
  リポジトリが private の間は、読み取り権限のある者が `gh release download v<ver>` で取る。
- **SDK の版は出自の記録で、互換の条件ではない。** アドオンが動くかどうかは Arch id で決まる
  （自己完結＝静的リンクが前提）。新しいリリースが出ても、Arch id が同じなら、古い SDK で
  作ったアドオンを作り直す必要は無い。
- 配るファイルは正本ではなく便宜。正本はタグで、`make sdk` でいつでも作り直せる
  （ビット単位の一致は保証しない）。

## 7. ライフサイクルと sysupgrade

- イメージ更新（sysupgrade）は overlay を**既定で保持**（設定・アドオンが残る）。ABI 非互換な
  更新は `Arch` / `Requires-OpenIRD` で弾く。
- factory 導入・overlay wipe でアドオンは全消え（safe to forget）。

## 8. ライセンス / clean-room

- 機構（`ird-addon`・`ird-mkaddon`・SDK・`.ird` 形式・索引形式）＝ OpenIRD（GPL-2.0）。
  clean-room の逆アセ対象ではない（独自設計）。
- アドオンの中身・アドオンリポジトリ＝外部・各自のライセンス。OpenIRD 配布物には含めない。

## 9. 実装状態

| 段取り | 状態（2026-10-11） | 所在 |
|---|---|---|
| 1. `ird-addon` ＋ `.ird` 形式 ＋ 索引 | **実装済み・実機検証済み**（POE-BR100-YB1） | `br-ext-common/package/ird-addon`（emma3sep の defconfig で有効） |
| 2. `make sdk` ＋ `ird-mkaddon` | **実装済み**（emma3sep で SDK を作り、別の場所へ展開してアドオンを作るところまで確認） | `scripts/make-sdk.sh`・`host/ird-mkaddon` |
| 3. recpt1 を別リポジトリで `.ird` 化し、実機導入→再起動保持→`/dev/irdvideo0` から録画 | 未着手 | （アドオンリポジトリ＝別リポジトリ） |

- テストは `make test-addon`（`tests/addon/`）: 作った `.ird` を偽の機器ツリー（base と、
  その上の見え方）へ入れて、§2〜§5 の決まりを確かめる。`BUSYBOX=<パス>` を渡すと
  `ird-addon` を BusyBox の ash とアプレットだけで走らせる（機器と同じ道具立て）。
- **実機で確かめたこと**（2026-10-11・POE-BR100-YB1・`ird-addon` 入りの base を mtd3 に書いた上で）:
  SDK の toolchain で作ったテスト用アドオンを `install <file> <sha256>` → 実行 → 電源の入れ直しを
  跨いで残る（`S80…` も起動時に走る）→ `remove`。消したファイルの whiteout は残らない
  （base に在るディレクトリの空の骨組みが upper に残るだけ）。機器からの HTTPS 取得
  （BusyBox `wget`）も通る。
- **実機でまだ見ていないこと**: HTTPS のリポジトリから名前で入れる流れ（`update` →
  `install <name>`。ホストのテストでは `file://` で確認済み）、overlay が満杯のときの取り消し。
  段取り 3 で確かめる。
- 実機の時計は起動のたびに 1970 年から始まる（実測。時刻合わせをしていない）。導入・削除の
  際にカーネルが `unionfs: new lower inode mtime` と出すことがあるが、導入・保持・削除の結果には
  影響しなかった（upper が 1970 年・base がイメージのビルド時刻、という時刻の逆転が原因とみられる。未確認）。
- fujitsu（mb86h61）は未対応: `ADDON_ARCH` をまだ決めていない（toolchain の選び方に依る）。
  決めたら `board.conf`・`busybox.fragment`・defconfig の 3 か所。
- **未決**: 索引の真正性（§4）。いまは証明書を検証しない HTTPS で、名前での導入は
  リポジトリまでの経路を信頼している。

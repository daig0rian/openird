# リリースの作り方

保守者向け。リリース＝**版のタグ**と、そのタグからビルドした配布物を GitHub Releases に
置いたもの。置くのは次の 4 つ:

| 配布物 | 中身 |
|---|---|
| SDK | アドオン作者向け（[addon.md](addon.md) §6.1）。Arch id ごとに 1 本 |
| ソース一式 | SDK と rootfs に入るパッケージのソースとライセンス文（Buildroot の `legal-info`） |
| factory イメージ | 純正の更新機構に渡して導入するコンテナ（カーネルと rootfs）。機種ごとに 1 本 |
| カーネルのソース | factory イメージに入れたカーネルの、メーカー公開の GPL ソース（パッチはリポジトリにある） |

sysupgrade イメージはまだ無い。

## 版とタグ

- 版は暦（`YY.MM`）。タグは `vYY.MM`、候補・予行は `vYY.MM-rcN`（GitHub では pre-release）。
- タグは注釈つき（`git tag -a`）で打つ。配布物の名前に入る版は、タグから `v` を取ったもの。

## 手順

ビルドは GitHub Actions（[.github/workflows/release.yml](../.github/workflows/release.yml)）が行う。
人がするのは、タグを打つ・factory ジョブを承認する・実機で確かめて公開する、の 3 つ。

1. **タグを打って push する**（作業リポジトリで。commit を push してから）:
   ```
   git tag -a v26.10 -m "OpenIRD 26.10"
   git push origin v26.10
   ```
2. **build ジョブ**（自動）: タグの commit を、公開しているリポジトリから取り直してビルドする。
   SDK・ソース一式（`make release`）、カーネル（`make kernel`）、rootfs。鍵の要らないところまでの
   factory の検査（`make factory FACTORY_ARGS=--payload-only`）もここで通す。
   このジョブは鍵も、リリースを書く権限も持たない。
3. **factory ジョブを承認する**（人）: Actions の画面に「Review deployments」が出る。
   下の「承認するときに見ること」を確かめてから承認する。承認すると、鍵を使って factory
   イメージを作り、全部の配布物と `SHA256SUMS` を**下書き**のリリースに付ける。
4. **実機で確かめてから公開する**（人）:
   - 下書きから factory イメージを取り、**そのファイルを**実機に書いて、起動と受信を確かめる
     （ビルドはビット単位で再現しないので、確かめたファイルそのものを出す）。
   - リリースの文面を書く（何が入っているか・知っておくべき制約）。
   - 下書きを公開する。
5. **取得して確かめる**（アドオン作者と同じ道筋）: リリースから SDK と `SHA256SUMS` を取り、
   `sha256sum -c` → 展開 → `./relocate-sdk.sh` → `. ./sdk.env` → 何か 1 つビルドして
   `ird-mkaddon build`。
6. **Wiki の対応機種表を更新する**: [対応機種](https://github.com/daig0rian/openird/wiki/Supported-Devices)の
   「確認した版」を、この版で実機確認した機種だけ書き換える。確認していない機種は前の版のまま
   残す（＝どの版で確かめたかが分かる）。

ワークフローそのものを試すときは、Actions の画面から手動で走らせる（`workflow_dispatch`）。
build ジョブだけが動き、リリースも鍵も使わない。

## factory イメージと鍵

factory イメージは、メーカーの更新コンテナの形式で包む。包むには更新用の鍵と ZIP パスワードが
要る（[update-v1b-container.md](specs/update-v1b-container.md)）。**鍵そのものは配布しない。**
リポジトリにも、ツールにも、配布物にも入れない（[CONTRIBUTING.md](../CONTRIBUTING.md) §0）。
公式の factory イメージは、保守者が預けた鍵を CI が使って作る。

**この鍵は、漏れたら戻せない。** メーカーが機器に焼いたもので、機種族で共通、作り直しが
効かない。だから、普通の CI のシークレットより厳しく扱う:

| 守り方 | 中身 |
|---|---|
| 置き場所は 1 つ | environment `factory` のシークレット（`V1B_AES_KEY`・`V1B_ZIP_PASSWORD`）だけ。リポジトリのシークレットには置かない |
| 承認が要る | environment に承認者を設定してある。承認されるまで factory ジョブは始まらず、鍵にも触れない |
| タグからだけ | environment は `v*` のタグからしか使えない。枝や PR からのワークフローは鍵に届かない |
| 鍵のジョブで第三者のコードを動かさない | factory ジョブに `uses:` は 1 つも無い。同じジョブで動くコードは、鍵をランナーのメモリから読めるため。動くのは、タグの commit の自前のスクリプト、ハッシュで固定した pycryptodome（[requirements-factory.txt](../host/openird-mkimage/requirements-factory.txt)）、ランナーに初めから入っている git・gh・python3 だけ |
| ビルドと分ける | 第三者のソースを大量にコンパイルする build ジョブは、鍵もリリースの書き込み権限も持たない。factory ジョブへは、イメージのファイルと sha256 だけを渡す |
| リポジトリ全体の制限 | 使える Action は GitHub 製だけ・コミット SHA での固定が必須（リポジトリの設定） |
| ログに出さない | `openird-mkimage` は鍵の値を出さない。指紋の行も `scripts/make-factory.sh` が落とす |

### 承認するときに見ること

承認は、鍵を使わせる最後の関門。次を確かめてから押す。

- **タグが、自分の打ったもの**で、指す commit が `main` に入っている意図したものであること。
- **その commit の `.github/workflows/release.yml` と `scripts/make-factory.sh`、
  `host/openird-mkimage/` が、意図しない形に変わっていないこと**（鍵のジョブで動くのはこれら）。
  特に factory ジョブに `uses:` が足されていないこと、鍵を表示・送信する行が無いこと。
- **build ジョブが成功している**こと。

覚えのないタグや、説明のつかない変更があれば、承認せずに実行を取り消す。

### 鍵の登録（最初の 1 回・保守者が自分の端末で）

値をコマンドラインに書かない（履歴に残る）。ファイルから渡す:

```
gh secret set V1B_AES_KEY      --env factory -R daig0rian/openird < ~/.openird/v1b/aes-128-ecb.key
gh secret set V1B_ZIP_PASSWORD --env factory -R daig0rian/openird < ~/.openird/v1b/zip.pass
```

`gh` は値を手元で暗号化してから送る。登録した値は、GitHub の画面からも API からも読み戻せない。

### 手元で作るとき

```
make BOARD=emma3sep_softbank_poe-br100-yb1          # rootfs
make kernel BOARD=emma3sep_softbank_poe-br100-yb1   # カーネル
make factory BOARD=emma3sep_softbank_poe-br100-yb1  # → output/<BOARD>/images/openird-<版>-…-factory.bin
```

鍵は `~/.openird/v1b/` に置く（[openird-mkimage の README](../host/openird-mkimage/README.md)）。
`FACTORY_ARGS=--payload-only` を付けると、暗号層の手前まで（鍵不要）を作る。

## Actions を使わずにビルドするとき

リリース物は Actions のものを出す。手元でのビルドは、確かめや比較のため。

- **版のタグを打った commit の、変更の無い git clone で**行う（`make release` が確かめる）。
- **個人名・機械の名前が入らない環境で**行う（下の「決まり」）:
  ```
  git clone https://github.com/daig0rian/openird.git /var/tmp/openird-build/openird
  cd /var/tmp/openird-build/openird
  git checkout --detach v26.10
  cp -al <共有のダウンロードキャッシュ> dl    # 任意。symlink にはしない（元のパスが見える）
  unshare --user --map-root-user --uts sh -c "hostname openird && \
    exec unshare --user --map-user=$(id -u) --map-group=$(id -g) -- \
    make release BOARD=emma3sep_softbank_poe-br100-yb1"
  ```
  その機種の `output` を消して最初からビルドし（16 コアで 10 分ほど）、
  `output/release/<版>/` に SDK・ソース一式・`SHA256SUMS` を揃える。
  **出す前に、SDK とソース一式を展開して、自分のユーザ名とホスト名で grep し、0 件を確かめる。**

## 決まり

- **版のタグを打った commit の、変更の無い git checkout からだけ作る。** `make release` が
  確かめて、そうでなければ止まる。版の名前が中身を指すこと（git でないツリーは `unknown` に
  なる）と、git に入っているとおりのファイルでビルドできること（Windows から写したツリーは
  実行ビットの付け忘れを隠す）を、リリースのたびに確かめるため。
- **個人名・機械の名前が入らない環境でビルドする。** 配布物には、ビルドした場所の絶対パス
  （SDK の toolchain の数百ファイルと、ソース一式の `buildroot.config`・`manifest.csv`）と、
  ビルドした機械のホスト名（SDK の `bin/libtool`）が入る。`/home/<名前>/…` でビルドすると、
  その名前を配ることになる。`relocate-sdk.sh` が書き換えるのは使う側の手元の写しで、
  配るファイルの中身は変わらない。Actions のビルドも、場所とホスト名を手元の手順と同じに
  そろえてある。
- **factory イメージは、実機で確かめたファイルだけを公開する。** 機種によっては、起動しない
  イメージを書くと復旧が難しい（BMMPX1301 は rootfs の区画が 1 面）。Actions が作るのは下書きまで。
- **信頼の単位は sha256。** 署名はしない。出したファイルは差し替えない（直すなら版を上げる）。
  使う側は URL と sha256 を pin している。
- **SDK は Arch id ごとに 1 本。** 同じ Arch id の機種を足しても SDK は増えない。
- **バイナリと同じ場所にソースを置く。** SDK は GCC・binutils などの GPL バイナリを含み、
  factory イメージは Linux カーネルと BusyBox などを含む。ソース一式は Buildroot の `legal-info` の
  出力で、Buildroot 自身と OpenIRD（どちらも `legal-info` が保存しない）は、どの版かを
  `openird-build.txt` に書き添える。カーネルは Buildroot の外でビルドするので、メーカー公開の
  GPL ソースを別に置く（当てているパッチと設定は、タグのリポジトリの
  `br-ext-chip-*/board/*/kernel/` にある）。

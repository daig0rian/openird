# リリースの作り方

保守者向け。リリース＝**版のタグ**と、そのタグからビルドした配布物を GitHub Releases に
置いたもの。いま置くのは、アドオン作者向けの **SDK** と、その**対応するソース一式**
（[addon.md](addon.md) §6.1）。イメージ（factory / sysupgrade）はまだ載せない（factory は所有者の
鍵が要る。配り方は別に決める）。

## 版とタグ

- 版は暦（`YY.MM`）。タグは `vYY.MM`、候補・予行は `vYY.MM-rcN`（GitHub では pre-release）。
- タグは注釈つき（`git tag -a`）で打つ。配布物の名前に入る版は、タグから `v` を取ったもの。

## 手順

1. **タグを打つ**（作業リポジトリで。commit を済ませてから）:
   ```
   git tag -a v26.10 -m "OpenIRD 26.10"
   ```
2. **ビルド用の git clone を、そのタグで作る**（Linux/WSL の ext4 上）。**場所は個人名の
   入らないパスにする**（下の「決まり」）。公開しているリポジトリから取る:
   ```
   git clone https://github.com/daig0rian/openird.git /var/tmp/openird-build/openird
   cd /var/tmp/openird-build/openird
   git checkout --detach v26.10
   cp -al <共有のダウンロードキャッシュ> dl    # 任意。symlink にはしない（元のパスが見える）
   ```
3. **リリース物を作る**（Arch id ごとに、その Arch id の機種を 1 つ指定する）。ビルドの間だけ、
   ホスト名を `openird` に見せる（機械の設定は変えない）:
   ```
   unshare --user --map-root-user --uts sh -c "hostname openird && \
     exec unshare --user --map-user=$(id -u) --map-group=$(id -g) -- \
     make release BOARD=emma3sep_softbank_poe-br100-yb1"
   ```
   その機種の `output` を消して最初からビルドし（16 コアで 10 分ほど）、
   `output/release/<版>/` に SDK・ソース一式・`SHA256SUMS` を揃える。
   **出す前に、SDK とソース一式を展開して、自分のユーザ名とホスト名で grep し、0 件を確かめる。**
4. **公開する**（ここは人が行う）:
   ```
   git push origin main v26.10
   gh release create v26.10 output/release/26.10/* --title "OpenIRD 26.10" --notes-file <notes>
   ```
   候補版は `--prerelease` を付ける。
5. **取得して確かめる**（アドオン作者と同じ道筋）: リリースから SDK と `SHA256SUMS` を取り、
   `sha256sum -c` → 展開 → `./relocate-sdk.sh` → `. ./sdk.env` → 何か 1 つビルドして
   `ird-mkaddon build`。
6. **Wiki の対応機種表を更新する**: [対応機種](https://github.com/daig0rian/openird/wiki/Supported-Devices)の
   「確認した版」を、この版で実機確認した機種だけ書き換える。確認していない機種は前の版のまま
   残す（＝どの版で確かめたかが分かる）。

## 決まり

- **版のタグを打った commit の、変更の無い git checkout からだけ作る。** `make release` が
  確かめて、そうでなければ止まる。版の名前が中身を指すこと（git でないツリーは `unknown` に
  なる）と、git に入っているとおりのファイルでビルドできること（Windows から写したツリーは
  実行ビットの付け忘れを隠す）を、リリースのたびに確かめるため。
- **個人名・機械の名前が入らない環境でビルドする。** 配布物には、ビルドした場所の絶対パス
  （SDK の toolchain の数百ファイルと、ソース一式の `buildroot.config`・`manifest.csv`）と、
  ビルドした機械のホスト名（SDK の `bin/libtool`）が入る。`/home/<名前>/…` でビルドすると、
  その名前を配ることになる。`relocate-sdk.sh` が書き換えるのは使う側の手元の写しで、
  配るファイルの中身は変わらない。
- **信頼の単位は sha256。** 署名はしない。出したファイルは差し替えない（直すなら版を上げる）。
  使う側は URL と sha256 を pin している。
- **SDK は Arch id ごとに 1 本。** 同じ Arch id の機種を足しても SDK は増えない。
- **バイナリと同じ場所にソース一式を置く。** SDK は GCC・binutils などの GPL バイナリを含む。
  ソース一式は Buildroot の `legal-info` の出力で、Buildroot 自身と OpenIRD（どちらも
  `legal-info` が保存しない）は、どの版かを `openird-build.txt` に書き添える。

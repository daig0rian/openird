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
2. **ビルド用の git clone をそのタグに合わせる**（Linux/WSL の ext4 上。`dl` は共有の
   ダウンロードキャッシュへのリンクでよい）:
   ```
   git pull --ff-only --tags        # clone の HEAD がタグの commit になること
   ```
3. **リリース物を作る**（Arch id ごとに、その Arch id の機種を 1 つ指定する）:
   ```
   make release BOARD=emma3sep_softbank_poe-br100-yb1
   ```
   その機種の `output` を消して最初からビルドし（16 コアで 10 分ほど）、
   `output/release/<版>/` に SDK・ソース一式・`SHA256SUMS` を揃える。
4. **公開する**（ここは人が行う）:
   ```
   git push origin main v26.10
   gh release create v26.10 output/release/26.10/* --title "OpenIRD 26.10" --notes-file <notes>
   ```
   候補版は `--prerelease` を付ける。
5. **取得して確かめる**（アドオン作者と同じ道筋）: リリースから SDK と `SHA256SUMS` を取り、
   `sha256sum -c` → 展開 → `./relocate-sdk.sh` → `. ./sdk.env` → 何か 1 つビルドして
   `ird-mkaddon build`。

## 決まり

- **版のタグを打った commit の、変更の無い git checkout からだけ作る。** `make release` が
  確かめて、そうでなければ止まる。版の名前が中身を指すこと（git でないツリーは `unknown` に
  なる）と、git に入っているとおりのファイルでビルドできること（Windows から写したツリーは
  実行ビットの付け忘れを隠す）を、リリースのたびに確かめるため。
- **信頼の単位は sha256。** 署名はしない。出したファイルは差し替えない（直すなら版を上げる）。
  使う側は URL と sha256 を pin している。
- **SDK は Arch id ごとに 1 本。** 同じ Arch id の機種を足しても SDK は増えない。
- **バイナリと同じ場所にソース一式を置く。** SDK は GCC・binutils などの GPL バイナリを含む。
  ソース一式は Buildroot の `legal-info` の出力で、Buildroot 自身と OpenIRD（どちらも
  `legal-info` が保存しない）は、どの版かを `openird-build.txt` に書き添える。

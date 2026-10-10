# ird-mkaddon

OpenIRD の**アドオン**（`.ird`）と、アドオンリポジトリの索引（`Packages`）を作るホストツール。
形式の定義は [docs/addon.md](../../docs/addon.md)（§2 アドオン、§3 索引）。単一ファイル・
Python 3.9+ の標準ライブラリだけで動き、SDK（`make sdk`）には `bin/ird-mkaddon` として入る。

```
ird-mkaddon build <staging> [-o <outdir>] [--arch <id>]
ird-mkaddon index <dir>
```

- `build` … staging ディレクトリ（`manifest`・`root/`・任意で `postinstall` / `preremove`）から
  `<Package>_<Version>_<Arch>.ird` を作り、SHA256 と Size を出す。manifest に `Arch` が無ければ
  `--arch`、それも無ければ環境変数 `IRD_ARCH`（SDK の `sdk.env` が与える）を入れる。
  **Linux/WSL の ext4 上で実行すること**（実行ビットと symlink をそのまま写すため）。
- `index` … `<dir>` の `*.ird` を調べて索引 `<dir>/Packages` を書く。この `<dir>` を HTTPS で
  配れば、それがアドオンリポジトリになる。

同じ入力からは同じバイト列が出る（名前順・所有者 0:0・モードは 755 / 644 に正規化・時刻は
`SOURCE_DATE_EPOCH` か 0）。機器側（`ird-addon`）が拒む形（base へ置けない場所、予約キー、
形式に無いメンバ）は、ここで先に誤りにする。

ツリーの中から使うとき:

```
python3 host/ird-mkaddon/ird_mkaddon.py build <staging> -o <outdir> --arch emma3sep-mipseb-musl
```

テストは `make test-addon`（`tests/addon/run.sh`）。

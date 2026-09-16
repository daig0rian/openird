# カーネル内部 ABI（構造体レイアウト）の互換テスト

段階 B では、所有者の機器に残っているベンダ製の .ko（FAPex ドライバなど）を、OpenIRD が
GPL から再ビルドしたカーネルに読み込む。.ko はベンダのカーネルでコンパイルされているので、
**OpenIRD のパッチや設定でカーネルの構造体のメンバ位置が 1 つでもずれると、読み込めても
誤動作する**。2.6.31 は `CONFIG_MODVERSIONS=n` なので、読み込み時には何も検出されない。

このテストは、次の 2 つのツリーで同じ probe をデバッグ情報付きでコンパイルし、構造体・共用体の
メンバ位置を比べる。

| ツリー | 中身 |
|---|---|
| ref（ベンダ出荷相当） | GPL tarball ＋ `build.env` の `KABI_REF_PATCHES`（ビルドを通すだけのパッチ）＋ tarball 同梱の `.config` |
| new（OpenIRD） | GPL tarball ＋ `patches/*.patch` 全部 ＋ `linux.config` |

設定の差（jffs2 や overlayfs の有効化など）もここで一緒に検査される。

## 使い方

```
make test-kabi BOARD=mb86h61_softbank_sb-tv02-wfpl
```

- 前提は `make kernel` と同じ（Linux か WSL2 の ext4 上。先に `make kernel` を一度通して
  `dl/` に toolchain と GPL tarball を揃える）。
- 成果物は `output/test-kabi/<BOARD>/`（`ref.layout`・`new.layout`・`report.txt`）。
  所要時間は tarball の展開 2 回と `modules_prepare` 2 回で数分。

## 判定

| 結果 | 意味 |
|---|---|
| `MOVED` / `REMOVED` / `RESIZED` | **失敗。** 既存メンバの位置が変わった／消えた／メンバの増減なしに大きさが変わった |
| `ADDED(!)` | **失敗。** メンバが増えたが [allowed-additions.txt](allowed-additions.txt) に無い |
| `ADDED`（note） | 許可リストにある追加。既存メンバの位置は変わっていない |

許可リストに書けるのは、**ベンダ製 .ko が持つ古い形の構造体に対して、カーネルがそのメンバを
読まないと言える追加**だけ。理由をコメントで添える。途中への挿入は許可しない（v4 の overlayfs は
`open_other` を `->open` の直後に足しており、ベンダの文字デバイスでは close 時にカーネルが
`->flush` の位置にある `->release` を呼ぶことになっていた。0003 で末尾に移した）。

## 検査の範囲と限界

- 対象は [probe/kabi_probe.c](probe/kabi_probe.c) が include するヘッダに現れる型
  （文字デバイス・メモリ・割り込み・USB・ネットワークなど。mb86h61 で約 600 型）。
  ベンダの .ko が使う型がここに無ければ足す。
- 見ているのは構造体のメンバ位置と大きさだけ。enum の値、関数の引数・意味の変更、
  export されたシンボルの有無は見ない。
- `KABI_REF_PATCHES` のパッチ自体が構造体を変えていないことは、このテストでは確かめられない
  （基準の側に入るため）。そこに足すパッチはヘッダに触れないものに限る。
- `struct module` の大きさ（mb86h61 では 312 バイト）は、ベンダの .ko の
  `.gnu.linkonce.this_module` の大きさ（`readelf -S`）と手で突き合わせると、コンパイラの
  違いによるずれも無いことを確かめられる（2026-09-17 に一致を確認）。

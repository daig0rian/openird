# fapex-poke

任意の FAPex chardev に任意の ioctl を出す観察ツール。`fapex-i2c` / `fapex-gpio` /
`fapex-ts` はそれぞれ I2C / GPIO / TSD 専用だが、立ち上げの検証にはそれ以外の
サブデバイス（CLKPWR = `fapi199` など）を叩く必要がある。そのたびに単目的の
プログラムを書かずに済ませるためのもの。

**配布イメージには入らない。** 所有者が自機を観察するためだけのもの。ベンダコードは
含まない。組み込みの FAPex 知識は ioctl の TYPE バイト（0xFF・GPL ヘッダ由来）だけで、
コマンド番号・方向・サイズ・ペイロードはすべてコマンドラインから与える＝仕様書か、
操作者が再現しようとしているバス観察から来る。

## 使い方

```
fapex-poke [opts] DEV NR DIR SIZE [BYTE...] [+ NR DIR SIZE [BYTE...]]...
```

`+` で区切ったグループは**同一の開いた fd** で順に実行される。FAPex の設定はハンドルに
紐づくため、これが要る（実測: CLKPWR の `nr=0x21` は、先に同じ fd で `nr=0x00` を
出していないと `-6008` を返す）。

主なオプション:

| オプション | 意味 |
|---|---|
| `--hold` | ioctl 後もデバイスを開いたまま保持（SIGTERM/SIGINT で解放）。ハンドルに紐づく設定を生かすため |
| `--byval` | 引数をポインタではなく**値渡し**にする（`SET_BUFFER_READSIZE` 等・§5.6） |
| `--repeat N` / `--delay-ms N` | 繰り返しと間隔 |
| `--open-rdonly` | `O_RDONLY` で開く |

## 例

```sh
# ベンダデーモンが最初に出す CLKPWR の 2 発を同一 fd で（§5.5）
fapex-poke --hold /dev/fapi/fapi199 0x00 - 0 + 0x21 w 8 0x06 0 0 0 0 0 0 0

# TSD の status を 4 バイト読む
fapex-poke /dev/fapi/fapi004 0x9e r 4
```

FAPex ドライバは自分のステータスを ioctl の戻り値で返すので、非ゼロを致命的と扱わず
そのまま表示する。

## ビルド

```
scripts/build-fapex-poke.sh      # Buildroot(musl) ツールチェーンで static ARM
```

## DIR に `v` を付けると値渡し

FAPex のドライバはポインタ渡しと値渡しを混在させる。1 つの列で両方が必要になるため、
DIR の末尾に `v` を付けるとそのグループだけ値渡しになる。

| DIR | 意味 |
|---|---|
| `-` / `w` / `r` / `rw` | 引数なし / ポインタ渡しの write / read / read+write |
| **`wv`** | write・引数を**値渡し** |
| **`rwv`** | read+write・値渡し |

実例は CLKPWR で、`nr=0x21` はポインタ渡し・`nr=0x23` は値渡しである（仕様 §5.5）。

```sh
# Dione を USB 列挙まで持ち込む CLKPWR 3 発（開いたまま保持する）
fapex-poke --hold /dev/fapi/fapi199 \
    0x00 - 0 + 0x21 w 8 0x06 0 0 0 0 0 0 0 + 0x23 wv 4 0x00 0x30 0 0 &
```

`--byval` はグローバル指定（全グループが値渡しになる）なので、混在する列では使わない。

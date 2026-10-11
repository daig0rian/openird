# openird-fwup

emma3sep 系（POE-BR100-YB1・SB-TV06-WRIP など）の**ベンダ像**へ、更新コンテナを投入する
ホストツール。PC でコンテナを HTTP 配信し、機器の無認証 UPnP action `X_UpdateFirmware` に
その URL を渡す。機器が自分で取りに来て、検証・書き込み・再起動まで進む。

- 通信は PC と機器の間だけ。**ベンダのサーバには接続しない。**
- **鍵は扱わない。** コンテナは [openird-mkimage](../openird-mkimage/) が作る。このツールは
  渡されたファイルを配るだけで、中身は見ない。
- OpenIRD を入れた後の機器には使えない（UPnP を出しているのは純正アプリ）。

手順の仕様は [emma3sep-upnp-update.md](../../docs/specs/emma3sep-upnp-update.md)。単一ファイル・
Python 3.9+ の標準ライブラリだけで動く。

```
uv run host/openird-fwup/openird_fwup.py <cmd> ...
```

| cmd | 内容 |
|---|---|
| `discover` | SSDP で機器を探す（読み取りのみ） |
| `info <target>` | 機器記述と状態を表示する（読み取りのみ） |
| `serve <file>` | コンテナを配信するだけ。SOAP は送らない（疎通の確認用） |
| `flash <target> <file>` | 配信して `X_UpdateFirmware` を送る（**書き込み**。既定は dry-run） |

`<target>` は `ip`・`ip:port`・`http://ip:port/description.xml`（port の既定は 49152）。

```
S="uv run -q host/openird-fwup/openird_fwup.py"
$S discover
$S info 192.168.11.17
$S flash 192.168.11.17 temp/factory.bin                                  # dry-run
$S flash 192.168.11.17 temp/factory.bin --serve-ip 192.168.11.8 --confirm-flash
```

## flash

1. 機器記述と状態を読み、配信の計画（コンテナの大きさと sha256・機器に渡す URL）を表示する。
2. `--confirm-flash` が無ければここで終わる（**dry-run**。`X_UpdateFirmware` は送らない）。
3. `FLASH` と打たせて確かめる（`--yes` で省略）。
4. 配信を始めてから `X_UpdateFirmware(download, URL)` を送り、機器の取りに来かたを見て終わる。

| オプション | 既定 | 内容 |
|---|---|---|
| `--serve-ip IP` | 機器へ向かう経路の送信元 | 機器に渡す URL の IP。NIC が複数あるときは明示する |
| `--port N` / `--bind ADDR` | 8080 / 0.0.0.0 | 配信の待ち受け |
| `--settle SEC` | 60 | GET を送り切ってから、取り直しが無いと見なすまでの時間 |
| `--idle SEC` | 180 | GET を送り切れないまま、あきらめるまでの時間 |
| `-v` | | 機器が送ってきたリクエストのヘッダをすべて表示する |

### 終了コード

機器は受理と拒否を UPnP では返さない。配信側から見える違いは「GET を送り切った後に、
また取りに来るかどうか」だけなので、それで決める（仕様 §5.5・§6.4）。

| code | 結果 | 条件 |
|---|---|---|
| 0 | 受理 | GET 1 回のあと `--settle` 秒取りに来ない。dry-run も 0 |
| 3 | 拒否 | 送り切った後に、また取りに来た。何も書かれていない |
| 4 | 未到達 | `--idle` 秒のあいだに GET を送り切れていない |
| 5 | 不受理 | SOAP が Fault または HTTP エラー |
| 1 | その他 | 機器に届かない、ファイルが読めない、確認で中止した、など |

**「受理」は機器が書き込みに進んだという意味で、起動までは見ていない。** 起動と中身は
シリアルのブートログや、像に入れておいた目印で確かめる。入れた像が OpenIRD なら、機器は
UPnP に戻ってこない。

拒否のとき、機器は同じファイルを何度も取り直す（約 10 秒おき・約 20 回）。止むまで配信を
続けてから終わるので、早く切り上げたければ Ctrl+C。

## Windows のファイアウォール

`flash` と `serve` は機器からの接続を受ける。受信規則は**プログラム単位**で、待ち受けて
いるのは uv が用意した `python.exe`（未到達で終わったときに、そのパスを表示する）。
初回に出る許可のダイアログを通すか、ポートで許可する（管理者の PowerShell）:

```powershell
New-NetFirewallRule -DisplayName "openird-fwup" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow
```

- ダイアログをキャンセルすると、その `python.exe` を Block する規則が残る。Block は
  ポートの許可より優先されるので、残っていたら消す。
- 焼く前に `serve` で配信だけ立て、別の機器から URL を取れるか見ておくと確実。
- `discover` が何も返さないのも同じ原因のことがある（SSDP の応答は UDP の受信）。
  IP が分かっていれば `info <ip>` は外向きの TCP だけなので通る。

## テスト

`make test-fwup`（`tests/fwup/run.sh`）。仕様どおりに振る舞う偽の機器を 127.0.0.1 に立て、
受理・拒否・未到達・不受理の 4 通りと、dry-run で SOAP が出ないことを確かめる。
実機もベンダ資材も要らない。

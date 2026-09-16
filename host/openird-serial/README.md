# openird-serial

実機のシリアルコンソールを、人とスクリプト（Claude Code を含む）で共有するためのブリッジ。
COM ポートは 1 つのプロセスしか開けないので、`serve` がポートを握り、受信を
タイムスタンプ付きのログに残しつつ、`127.0.0.1` の TCP で中継する。

```
実機 ttyAMA0 ── USB シリアル(COM3) ── serve ─┬─ temp/serial/COM3.log（追記）
                                             └─ tcp://127.0.0.1:7300 ─┬─ TeraTerm（人）
                                                                      └─ send / run / expect
```

実行は uv 経由（依存の pyserial はスクリプト冒頭の宣言から uv が用意する）:

```
uv run host/openird-serial/openird_serial.py <cmd> ...
```

## serve（ポートを握る）

```
uv run host/openird-serial/openird_serial.py serve --port COM3
```

- 115200 8N1。**DTR/RTS は立てない**（libooter の誤割り込みを避ける）。
- ログの既定は `temp/serial/<PORT>.log`（追記・行頭に時刻）。`--log` で変えられる。
- USB シリアルが抜けても 1 秒ごとに開き直す。止めるのは Ctrl+C。
- 待ち受けは `127.0.0.1` のみ。認証が無いので外部に開かない。

**TeraTerm から使う**: 新しい接続 → TCP/IP、ホスト `127.0.0.1`、TCP ポート `7300`、
サービス「その他」。COM3 を直接開かないこと（serve と取り合いになる）。

## クライアント

| cmd | 内容 |
|---|---|
| `send TEXT` | 送るだけ。既定で末尾に CR。`--eol none`、`--escapes`（`\x03` で Ctrl+C など） |
| `run COMMAND` | 送って、プロンプト（既定 `# ` / `> ` / `$ `）が戻るまでの出力を表示。エコーとプロンプトは除く。`--timeout`（既定 30 秒）、`--quiet SEC`（出力が止まったら終える） |
| `expect REGEX` | 正規表現が出るまで出力を流し、出たら `--send` を順に送る。起動時の割り込み向け |

`--char-delay MS` で 1 文字ずつ送る（libooter の `setparam` のような長い行向け）。
終了コードは 0＝一致・プロンプト復帰、2＝タイムアウト。

例:

```
S="uv run -q host/openird-serial/openird_serial.py"
$S run 'cat /proc/mtd'
$S expect 'Wait for kbd input' --send ' ' --timeout 120    # 次の起動で libooter のシェルに入る
$S run 'showparam' --quiet 1                                # プロンプトの形が分からないときは無音で終える
$S send '\x03' --escapes --eol none                          # Ctrl+C
```

複数のクライアントが同時につながると、入力は混ざり、出力は全員に届く。
TeraTerm で打っている最中に `run` を使うと、互いの出力が混ざる。

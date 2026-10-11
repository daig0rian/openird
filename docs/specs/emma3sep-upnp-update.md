# emma3sep 系の UPnP 更新エンドポイント（factory 経路の投入側）

<!-- openird-spec
Status: implemented
As-of:  実機 POE-BR100-YB1 fw 1.3.1〜2.1.17 (2026-09-06〜09-21) / SB-TV06-WRIP fw 1.0.2 (2026-09-11・09-26) / SB-TV04-WRIP (2026-10-01・発見のみ)
Impl:   host/openird-fwup/openird_fwup.py
Tests:  tests/fwup/
Note:   実機への投入はこの仕様の元になった旧クライアントで行った。openird-fwup での実機投入は未実施＝verified にしていない
-->

<!-- Open 層: 機器が HTTP で公開する機器記述（description.xml）とサービス記述（SCPD）、
     SOAP の応答、配信サーバ側で見た機器からのリクエスト、シリアルのブートログ。
     すべてネットワークの外側から見える事実で、逆アセ由来の内部ロジックは書かない。
     個体識別情報（UDN・MAC・シリアル番号）は書かない。鍵は扱わない（CONTRIBUTING.md §0）。
     実装（host/openird-fwup）はこの仕様のみを根拠に書く（clean-room）。 -->

[更新コンテナ](update-v1b-container.md) を機器に**渡す**手順。機器は同一 LAN からの UPnP
制御を無認証で受け付け、その中に「この URL からファームを取って書け」という action がある。
OpenIRD の factory 経路はこの口をそのまま使う: PC でコンテナを HTTP 配信し、その URL を
action に渡す。渡したあと機器が何をどこへ書くかは
[poe-br100-yb1-nand-update.md](poe-br100-yb1-nand-update.md)。

## 1. 範囲

- 対象は emma3sep 系のベンダ像（純正アプリが動いている状態）。**OpenIRD 像にこの口は無い**
  （UPnP を出しているのは純正アプリなので、OpenIRD を入れた後は使えない）。
- 投入側は**鍵を扱わない**。コンテナの中身も見ない。渡されたファイルを配るだけ。
- 通信は PC と機器の間だけで完結する。ベンダのサーバには接続しない。

確認した機種:

| 機種 | modelName | friendlyName | 族 | 確認した範囲 |
|---|---|---|---|---|
| BMMPX1301 | `POE-BR100-YB1` | `Chidezi Tuner` | `BLUR` | 発見・状態・投入（純正／自作コンテナとも） |
| SB-TV06-WRIP | `POE-BR310L-SB1` | `PIXELA Digital TV Tuner` | `LINDBERG` | 発見・状態・投入（自作コンテナ）・拒否 |
| SB-TV04-WRIP | `SB-TV04-WRIP` | `PIXELA Digital TV Tuner` | 未確認 | 発見のみ（コンテナ未入手） |

## 2. 発見

### 2.1 SSDP

機器は SSDP（UDP `239.255.255.250:1900`）の `M-SEARCH` に unicast で応答し、`LOCATION`
ヘッダで機器記述の URL を返す。

```
M-SEARCH * HTTP/1.1
HOST: 239.255.255.250:1900
MAN: "ssdp:discover"
MX: <秒>
ST: <検索対象>
```

- 検索対象は `urn:schemas-pixela.jp:service:X_AirTunerControl:1`・`upnp:rootdevice`・
  `ssdp:all` を投げれば応答が得られる（どれに応答しているかは切り分けていない＝§7）。
- 同じ LAN に他の UPnP 機器も応答する。**機器記述に §2.2 の制御サービスがあるものだけ**が対象。
- PC に NIC が複数あると、既定の経路の NIC からしか `M-SEARCH` が出ない。NIC ごとに
  そのアドレスへ bind し、マルチキャストの送出 NIC もそこに指定したソケットから送ると、
  すべての LAN を探せる。
- 応答は受信側のファイアウォールに落とされることがある。SSDP は必須ではなく、機器の
  IP が分かっていれば §2.2 を直接読めばよい。

### 2.2 機器記述

`http://<機器>:49152/description.xml`（3 機種とも port 49152）。UPnP Device Architecture 1.0
の機器記述で、ルート要素の名前空間は `urn:schemas-upnp-org:device-1-0`。

| 要素 | 内容 |
|---|---|
| `deviceType` | `urn:schemas-upnp-org:device:MediaServer:1` |
| `friendlyName` / `manufacturer` / `modelName` / `UDN` | §1 の表。`manufacturer` は `PIXELA CORPORATION` |
| `fwVersion` | ファームの版（例 `2.1.17`）。**名前空間は `urn:schemas-pixela.jp:device-1-0/`**（接頭辞 `at:`） |
| `serviceList/service` | 4 つ。`ContentDirectory`・`ConnectionManager`・`X_ScheduledRecording`・`X_AirTunerControl` |

制御サービスの `service` 要素:

| 要素 | 値 |
|---|---|
| `serviceType` | `urn:schemas-pixela.jp:service:X_AirTunerControl:1` |
| `controlURL` | `/upnp/control/X_AirTunerControl` |
| `SCPDURL` | `X_AirTunerControl.xml` |

クライアントは `serviceType` と `controlURL` を**機器記述から取る**（決め打ちにしない）。
制御サービスの見分けは `serviceType` に `X_AirTunerControl` が含まれること。要素名は
名前空間の接頭辞に依らず、局所名で引く。

## 3. 制御（SOAP）

### 3.1 要求

`controlURL` への HTTP POST。**認証もペアリングも要らない。**

```
POST /upnp/control/X_AirTunerControl HTTP/1.1
Host: <機器>:49152
Content-Type: text/xml; charset="utf-8"
SOAPACTION: "<serviceType>#<action>"
Content-Length: <長さ>
Connection: close

<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
<s:Body>
<u:<action> xmlns:u="<serviceType>"><引数名>値</引数名>…</u:<action>>
</s:Body>
</s:Envelope>
```

- `SOAPACTION` の値は二重引用符で囲む。
- 引数の値は XML の文字参照に直す（`&` `<` `>` `"` `'`）。
- 行末は CR LF。

### 3.2 応答

- 成功: HTTP 200。本文は `<u:<action>Response>` で、出力引数がその子要素に入る。
- 失敗: HTTP 500 と SOAP Fault。`errorCode` と `errorDescription` の 2 要素を読む。
  本文が空の 500 もある。

見た `errorCode`:

| code | 説明 | どこで見たか |
|---|---|---|
| 401 | `Invalid Action` | SB-TV04-WRIP に `X_GetStateVersatile` を送ったとき |
| 402 | 引数が不正 | 許される値の外を渡したとき |
| 801 | `Failed to Download` | `X_UpdateFirmware` に空の URL を渡したとき |

### 3.3 `X_GetStateVersatile`（読み取り）

入力引数なし。出力は `LastObjectID`・`TbObjectID`・`BsObjectID`・`CsObjectID`・
`RecordState`・`BroadcastWave`・`ServiceId`・`StorageState`・`FwVersion`。

- `FwVersion` は機器記述の `fwVersion` より細かい（機器記述 `2.1.17` に対して `2.1.17 r3`）。
- `RecordState` は待機中に `READY`。
- **機種によっては無い**（SB-TV04-WRIP は 401）。∴ 失敗しても投入は止めない。

## 4. `X_UpdateFirmware`

### 4.1 引数

| 引数 | 向き | 内容 |
|---|---|---|
| `UpdateMode` | in | 文字列。サービス記述が許す値は `immediately`・`download`・`waitTrigger` |
| `FirmwareURL` | in | 更新コンテナの URL。平文 HTTP。機器は**任意の URL を受ける**（既定の配信元を持たない） |

出力引数は無い。

### 4.2 `download`

OpenIRD が使うのはこのモードだけ。

- 応答は HTTP 200 と空の `X_UpdateFirmwareResponse`。
- **この 200 は「取りに行く」の合図であって、コンテナを受理したという意味ではない。**
  コンテナの検証は取得の後で、その結果は UPnP では返らない（§5）。
- 名前に反して、取得だけでは終わらない。コンテナが通れば、そのまま書き込みと再起動まで進む。

### 4.3 使わないもの

- `X_UpdateFirmware` に `waitTrigger` を渡すと HTTP 500 で、取得も起きない。
- 同じ引数に出力 `UpdateID` が付く `X_UpdateFirmwareRemote` と、それを受ける
  `X_TriggerUpdate(UpdateID)` がある。`waitTrigger` で取得と保留まではするが、同じ LAN から
  `X_TriggerUpdate` を送っても適用されなかった（HTTP 500）。∴ 使わない。

## 5. 機器側の取得動作

### 5.1 リクエスト

機器は `FirmwareURL` へ **HEAD を 1 回、続けて GET を 1 回**送る。2 つは別の接続。

```
HEAD /<ファイル> HTTP/1.0
User-Agent: Wget/1.12 (linux-gnu)
Accept: */*
Host: <PC>:<port>
Connection: Keep-Alive
```

- HTTP/1.0。`Range` は使わず、全体を 1 回の GET で取る。`Accept-Encoding` は送らない。
- GET のヘッダも同じ。

### 5.2 時間

| 区間 | 実測 |
|---|---|
| SOAP の応答 → HEAD | 0〜約 43 秒（応答より先に HEAD が届いたこともある） |
| GET（約 17MB） | 有線で 1 秒未満〜約 14 秒 |
| GET 完了 → 機器が UPnP に応答しなくなる | 約 30〜80 秒 |
| 全体（SOAP → 書き込み → 起動し直し） | 約 100 秒〜2 分半 |

∴ 配信側は SOAP を送る**前**に待ち受けを始めておく。

### 5.3 受理されたとき

HEAD 1 回・GET 1 回で終わり、**以後は取りに来ない**。機器は自分で検証・書き込み・再起動まで
進む。純正コンテナでも、鍵と族タグの合った自作コンテナでも同じ。版は判定に使われていない
（POE-BR100-YB1 で古い版と同じ版、SB-TV06-WRIP で同じ版が通った）。

### 5.4 拒否されたとき

検証に落ちると、機器は**同じ URL を最初から取り直す**。

- 約 17MB のコンテナで、約 10 秒おきに約 20 回（SB-TV06-WRIP に、通らないコンテナを渡したとき）。
- 約 50MB のコンテナで、取り終えるたびに続けて（別の族のコンテナを渡したとき）。
- 何も書かれず、版も変わらない。機器はそのまま動き続ける。

### 5.5 配信側から分かること

受理と拒否は UPnP の応答では区別できない。配信側から見える違いは 1 つだけ:
**GET を送り切った後に、また取りに来るかどうか**。

- 機器の復帰を待って版を読み直す方法は、いつも使えるわけではない。入れた像が OpenIRD なら
  §1 のとおり UPnP が出ないし、同じ版を入れたなら版は変わらない。
- 中身が実際に入れ替わったかは、この経路の外で確かめる（シリアルのブートログ、
  像に入れておいた目印。[update-v1b-container.md](update-v1b-container.md) §7 の作法）。

## 6. 投入クライアントの要件

### 6.1 配信

- 単一のファイルを HEAD と GET で返す。パスの最後の要素がファイル名に一致するときだけ返し、
  ほかは 404。
- **`Content-Length` を必ず付ける**（chunked にしない）。
- 実機で通っている応答は次の形。これを変えない。

```
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: <長さ>
Accept-Ranges: none
Connection: close
```

- HEAD には本文を付けない。

### 6.2 配信 URL

`http://<PC の IP>:<port>/<ファイル名>`。IP は**機器から届く PC 側のアドレス**でなければ
ならない。NIC が複数あるときは、機器のアドレスへ向かう経路の送信元アドレスを使う。
利用者が明示できるようにもする。

### 6.3 誤操作への備え

書き込みは取り消せない。

- 既定は dry-run。機器の情報と配信の計画を表示するだけで、`X_UpdateFirmware` を送らない。
- 明示のフラグを付けたときだけ送る。その場合も、利用者に確認の語を打たせる（省略のフラグあり）。
- 機器記述と `X_GetStateVersatile` の読み取りは、書き込みではないので制限しない。

### 6.4 終わり方

§5.5 に従って判定する。

| 結果 | 条件 |
|---|---|
| 受理 | GET を 1 回送り切り、その後一定時間（既定 60 秒）取りに来ない |
| 拒否 | GET を送り切った後に、また取りに来た |
| 未到達 | SOAP は受理されたが、一定時間（既定 180 秒）GET を送り切れていない |
| 不受理 | SOAP が Fault または HTTP 4xx/5xx |

- 受理の待ち時間は、拒否のときの取り直し（§5.4・約 10 秒おき）より十分長く取る。
- 拒否と分かっても、機器が取りに来ている間は配信を止めない。静かになってから終える。
- 4 つは終了コードで区別できるようにする。
- 「受理」は機器が書き込みに進んだという意味で、起動まで確かめたという意味ではない（§5.5）。

## 7. 未確認

| # | 項目 | 影響 |
|---|---|---|
| N1 | SSDP でどの検索対象に応答しているか | 無し（3 つとも投げれば見つかる） |
| N2 | `UpdateMode=immediately` での正常系 | 無し（`download` を使う） |
| N3 | 50MB を超えるコンテナが拒否されたとき、取り直しまでの間隔 | §6.4 の待ち時間。取り終えてすぐ取り直すことは見ているが、検証にかかる時間は測っていない |
| N4 | §6.1 以外の応答の形（HTTP/1.0 での応答、ヘッダの追加）で通るか | 無し（通っている形を保つ） |
| N5 | SB-TV04-WRIP での投入 | コンテナ未入手のため未実施 |
| N6 | `https://` やリダイレクトを機器が辿るか | 無し（使わない） |

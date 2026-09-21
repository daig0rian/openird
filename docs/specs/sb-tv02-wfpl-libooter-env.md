# libooter env（SB-TV02-WFPL / mtd1）

<!-- openird-spec
Status: specified
As-of:  実機 SB-TV02-WFPL mtd1 の全ダンプ
Impl:   -
Tests:  -
Note:   cmdline を書き換えるツールは未実装。現状は libooter の setparam/saveparam を手で使う
-->

<!-- Open 層: 実機ダンプの静的読み（逆アセではない）。個体識別情報は伏字。
     実装（cmdline を書き換えるツール等）はこの仕様のみを根拠に書く（clean-room）。 -->

libooter（proprietary・GPL 非収録）が持つ不揮発設定領域。`mtd1 "libooter env"`
＝ `0x100000`（64KB）。シリアルの libooter シェルの `showparam` / `setparam` /
`saveparam` が読み書きするのはここ。**カーネルのコマンドラインもここに入っている。**

## レコード形式（観察）

64KB の中に、可変長レコードが固定的な位置に並ぶ（隙間は消去値 0xff）。

```
+0x00  u32 LE  payload の長さ
+0x04  u16 LE  レコード ID
+0x06  2 バイト  意味未確認。多くのレコードで "xp"(78 70)、cmdline では "AT"(41 54)
+0x08  payload （文字列レコードは NUL 終端 ASCII）
```

観察されたレコード（値は伏字。ID は実測値）:

| off | ID | payload の中身 |
|---|---|---|
| 0x000 | 0x0001 | 製品名（同じ文字列が 2 回）|
| 0x040 | 0x0002 | 製造個体の識別子 ***（伏字）*** |
| 0x400 | 0x0009 | **カーネルコマンドライン**（NUL 終端 ASCII 1 本） |
| 0x810 | 0x0004 | Wi-Fi 2.4GHz の SSID と鍵 ***（伏字）*** |
| 0x890 | 0x0003 | 個体の識別子 ***（伏字）*** |
| 0x8c0 | 0x0005 | Wi-Fi 5GHz の SSID と鍵 ***（伏字）*** |

0x800 には 8 バイトのバイナリがあり（役割未確認）、以降のレコード群の前置きに見える。
ID の番号順とレコードの並び順は一致しない。

## カーネルコマンドライン（ID 0x0009）

実測の形（値は伏字）:

```
console=ttyAMA0,115200 root=/dev/mtdblock6 rw ip=none mac_adrs=XX:XX:XX:XX:XX:XX init=/bin/sh
```

- `setparam <文字列>` でこのレコードを置き換え、`saveparam` で mtd1 に書く。
  **mtd0（libooter 本体）は触らない**ので、この操作は可逆。
- `root=` を変えるだけで起動する rootfs を切り替えられる（OpenIRD の導入・復旧は
  この仕組みに依存している）。

## ⚠ MAC アドレスの所在（重要）

**個体の MAC アドレスは、フラッシュ上ではこの cmdline 文字列の中の `mac_adrs=` に
しか存在しない。** 16MB の全ダンプをバイナリ形式（6 バイト）と ASCII 形式の両方で
検索して、他のコピーは 1 件も無いことを確認した（nvram0/1・slot info・reserved にも無い）。

カーネル側も、Ether ドライバが起動時に cmdline を読むだけ（起動ログの
`macadrs_setup_from_cmdline(): MAC address: …`）。

したがって:

- **`mac_adrs=` を含まない cmdline を `saveparam` すると、その個体の MAC は失われる。**
  復元にはラベル等の外部の記録が要る。
- cmdline を書き換えるツール（将来のインストーラ・sysupgrade を含む）は、
  **文字列を組み立て直さず、既存の 1 行のうち必要なキーだけを差し替える**こと。
- OpenIRD 側の保険として、rootfs は overlay 上の `/etc/openird/network.conf` の
  `MAC=` を起動時に適用する（`br-ext-common/package/ird-base-files`）。

## slot info（mtd2 / mtd9）

区画表では `slot info0`（0x110000）と `slot info1`（0xFC0000）の 2 面。実機ダンプでは
**両者がバイト単位で完全一致**し、先頭 44 バイトだけが有効で残りは 0xff:

```
00000000  54 4f 4c 53 01 00 00 00  ...   |TOLS....|
```

`TOLS` = "SLOT" のリトルエンディアン表記。libooter はここを見てカーネルスロットを
選ぶ（起動ログの `Slot at 0x00140000 is selected.`）。どちらの面を読むかは未確認
なので、OpenIRD の区画レイアウトでは**両方とも温存**している。

`reserved`（mtd10, 0xFD0000, 192KB）は全域 0xff＝未使用。

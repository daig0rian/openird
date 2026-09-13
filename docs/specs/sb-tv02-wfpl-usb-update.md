# SB-TV02-WFPL USB 更新機構（end-user factory 経路）

<!-- Open 層: 実機ダンプ内の平文スクリプト読解（rootfs=mtd6 の /etc/init.d/rcS・/etc/inittab、
     slot0=mtd7 の slot_entry.sh・app/update）と dione_server usage（§8.6）由来。
     逆アセは含まない。PII（MAC/SN/WiFiキー）無し。
     USB factory installer / openird-mkimage pack-full はこの仕様のみを根拠に実装する。 -->

libooter→kernel(mtd5)→rootfs(mtd6)→slot(mtd7/8) で起動した後、ユーザランドの
`slot_entry.sh` が **USB メモリによる更新**を駆動する。**分解・シリアル・root shell 不要**で、
OpenIRD の end-user 向け書き換え手段（factory）として最有力。

## 1. 起動時の USB 検出・トリガ（slot_entry.sh）

次のいずれかで USB マウントを試行する（**リセットボタンは必須ではない**）:

- `dione_server -s` … リセットスイッチ押下 → 試行(押されている間リトライ)
- `update check_usb_flash` … USB が mass-storage class（bInterfaceClass=08/Sub=06/Proto=50）
  と判定されれば自動検出 → 最大30回(1秒毎)試行（refs #29435）
- `update check_just_updated` … 更新完了直後は slot 終端の `JUPD` マーカーで再突入を防止

→ **UX: `updater.full` を入れた FAT USB を挿して電源投入**で走る。完了後は `JUPD` で
ループ防止。

```sh
# slot_entry.sh（要約）
mount -t vfat /dev/sda1 /mnt        # 無ければ /dev/sda
if [ -e /mnt/updater.full ]; then
    dione_server -d /mnt/updater.full   # → /tmp/updater.tar に復号
    umount /mnt
    if update check; then
        update prepare                  # lib/ツールを /tmp へ
        exec /tmp/update                # 実書き込み
    fi
fi
```

FAT の第1パーティション（sda1）、無ければパーティション無しの sda 全体をマウント。

## 2. コンテナの二段構造

```
USB:/updater.full  --(dione_server -d)-->  /tmp/updater.tar  --(update)-->  各 mtd へ生書き込み
```

- **`updater.full`** … `dione_server -d` が復号する外側コンテナ。ヘッダ
  `Updater_Header{ magic(%c%c%c%c), type, size, version(0x%08x), buildNumber }` ＋本体（§8.5）。
  本体変換（暗号/難読化/素通し）は**未解明＝HOLD（§7 の後述）**。strings 上は署名必須/暗号必須
  ゲート不検出＝軽いラッパの可能性。
- **`updater.tar`** … 復号後の素の tar。中身は下記。**ここは完全に判明**。

## 3. `updater.tar` の中身と書き込み先（update スクリプト）

`update` 本体は `tar xf updater.tar` 後、**ファイル名の有無で書き込み先を決める**:

| tar 内ファイル | 動作 | OpenIRD |
|---|---|---|
| `bslotimg.bin` | `dione_server -w /dev/mtd5:` （カーネル） | **= openird-mkimage pack-kernel 出力（RDHB 梱包済）** |
| `rootfs.squashfs` | ROM 起動時は **mtd8 にステージ**→ root= を mtdblock8 に変更→再起動→次回 `restore_rootfs` が mtd8→mtd6 へ commit | OpenIRD rootfs。§5 の含意に注意 |
| `slot.squashfs` | `dione_server -u`（slot0/1 更新, `-t` で status 調整） | 任意（アプリ層） |
| `flimage.bin` | `dione_server -w /dev/mtd0:` （**libooter/IPL**） | **絶対に含めない**（ブートローダ温存＝復旧性） |
| `target.txt` | 版ゲート。`grep "$(dione_server -v)" target.txt` 不一致で exit 1 | **任意**。tar に入れなければゲート無効 |
| `check.sh` | 追加チェック。非0で中止 | **任意**。入れなければスキップ |
| `keep_nvram.txt` | 在れば NVRAM 温存。無ければ `wlancap_nvram_tool delete` | 任意（Wi-Fi 設定温存なら入れる） |

**版/チェックは任意**（スクリプトは `if [ -e target.txt ]` / `if [ -x check.sh ]` でしか見ない）。
∴ **最小構成 = `bslotimg.bin` と `rootfs.squashfs` を束ねた tar**（target.txt/check.sh 省略で版ゲートも無効）。

## 4. 実書き込みに使う dione_server サブコマンド（usage 由来・Open）

`-w /dev/mtdN:<file>`（消去して生書き込み） / `-d <full>`（復号） / `-u <squashfs>`（slot 更新） /
`-t <s0>:<s1>`（slot status） / `-r`（reboot） / `-0/-1`（LED） / `-s`（reset SW 状態） / `-v`（版）。

## 5. rootfs の mtd8 ステージと「非 vendor rootfs」への含意（重要）

ROM 起動中は mtd6 が `/` としてマウント中で直接上書きできないため、update は
**rootfs.squashfs を一旦 mtd8(slot1) に書き → `root=/dev/mtdblock8` に変更 → 再起動**する。
次回起動で `slot_entry.sh` が `root=/dev/mtdblock8` を見て **`update restore_rootfs`**
（`dione_server -w /dev/mtd6:/dev/mtd8` で mtd8→mtd6 commit、root= を mtd6 に戻す）を実行。

**含意**: この restore は「mtd8 から起動した rootfs が *vendor の slot_entry/update を持つ*」前提。
OpenIRD rootfs をこの経路で入れると、mtd8 起動後に vendor restore が走らない＝**mtd8→mtd6
commit が起きず半端な状態**になる。したがって OpenIRD 側で次のいずれかが必要:
- OpenIRD rootfs の初回起動で mtd8→mtd6 の commit と `root=` 復帰を自前で行う、または
- updater.tar に rootfs.squashfs を入れず、rootfs は別手段で mtd6 に置く設計にする。
（NFS 起動分岐では update は mtd6 に**直書き**するので staging 問題は起きない＝参考）

## 6. 安全・復旧

- **libooter(mtd0) を書かない**限り、ブートローダは常に健在。
- kernel(mtd5) は**単一面・直書き**＝唯一の不可避リスク。rootfs は mtd8 ステージで回復寄り。
- **USB 更新も Linux 駆動**＝bad kernel で起動不能になれば USB 復旧も動かない。その場合の
  最終手段は **SPI-NOR クリップ（外部ライタで mtd5 書き戻し）**。∴ 配布は**起動確認済みイメージ**に限る。

## 7. 未解明（HOLD）

- **`updater.full` の `.full` ラッパ形式**（`dione_server -d` の復号＝Updater_decode）。
  中身(updater.tar)は完全判明だが、外側を作る encode が未特定。
  解決策: (a) 純正 `.full` を入手し BLUR/RDHB と同様に構造解析、(b) 実機で `dione_server -d` が
  非暗号入力を素通しするか test。割れ次第 `openird-mkimage pack-full`（updater.tar→updater.full）
  を実装し、USB factory が完成。

## OpenIRD への落とし込み（まとめ）

- end-user factory = **`updater.full` 入り FAT USB を挿して電源**（ボタン不要）。
- `updater.full` の中身 = `updater.tar`{ `bslotimg.bin`(=pack-kernel 出力), `rootfs.squashfs` }。
  `flimage.bin` は入れない。`target.txt`/`check.sh` 省略で版ゲート無効。
- 残作業: §7 の `.full` 解明 → `pack-full` 実装、§5 の rootfs commit の扱い。

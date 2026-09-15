# overlayfs カーネルパッチの QEMU 機能テスト

機種のベンダカーネル（GPL tarball ＋ `board/<family>/kernel/patches`）を、QEMU で動く汎用ボード
向けにビルドし、overlayfs の動作を約 80 項目で確かめる。実機を焼かずに、**コンパイルでは
見つからない意味の違い**（デッドロック、データ破損、権限）を拾うのが目的。
移植の経緯と見つかったバグは [docs/kernel-overlayfs.md](../../docs/kernel-overlayfs.md)。

## 前提

- Linux か WSL2 の **ext4 上**の clone（`/mnt/c` では展開・ビルドができない。docs/BUILDING.md と同じ）。
- 先に `make kernel BOARD=<device>` を一度通しておく（`dl/` の toolchain と GPL tarball を使う）。
- ホストのパッケージ（Ubuntu 例）: `qemu-system-arm`（機種のアーキに合わせる）、`squashfs-tools`、
  `e2fsprogs`、`python3`、`curl`。QEMU は 10.2 で確認（`-audio none` を使う）。
- テスト用の静的 busybox は初回に busybox.net から `dl/` へ取得し、sha256 を検証する
  （テスト専用。配布物・リポジトリには入らない）。

## 使い方

```
make test-overlayfs BOARD=mb86h61_softbank_sb-tv02-wfpl
make test-overlayfs BOARD=mb86h61_softbank_sb-tv02-wfpl TEST_ARGS="-n 5"   # 5 回繰り返す
tests/overlayfs/run.sh --skip-patch '0013-*' mb86h61_softbank_sb-tv02-wfpl  # パッチを外して切り分け
```

| オプション / 環境変数 | 意味 |
|---|---|
| `-n N` | テストを N 回繰り返す（競合系の取りこぼし確認） |
| `--skip-patch GLOB` | 機種パッチのうち GLOB に一致するものを当てない（複数可） |
| `--debug` | `kconfig/debug-locking.fragment`（lockdep 等）を足す。**mb86h61 のツリーでは起動しない** |
| `--no-build` | ビルドを省き、前回のカーネルでテストだけ流す |
| `STALL_SEC`（既定 60） | コンソール出力がこの秒数止まったら「完走せず」と判定して診断する |
| `RUN_TIMEOUT`（既定 900） | 1 回の上限秒数 |

成果物は `output/test-overlayfs/<BOARD>/`（`kernel/`、`images/`、`run-N.log`、`diag-N.txt`）。
mb86h61 ではビルド込みで 1 回約 40 秒（テスト自体は数秒）。

## 何を確かめるか

lower = squashfs（loop）、upper = jffs2＋xattr（mtdram 16MB・消去単位 64KB＝実機の SPI-NOR 相当）。

| ID | 内容 |
|---|---|
| S | mount |
| B | 読み取り、copy-up（追記・O_TRUNC・1MB チャンクを跨ぐ 2.5MB の md5）、chmod/chown/touch、symlink、FIFO・デバイスノード、statfs |
| W / R | whiteout、不透明ディレクトリ、`rm -rf`、ディレクトリ一覧のマージ、ディレクトリの fsync |
| N | rename（lower ファイル・上書き・マージ済みディレクトリ）、hardlink |
| P | uid 1000 での権限（読めない・書けない・辿れない・実行できない、自分のファイルの copy-up と whiteout）、ro/rw remount |
| X | copy-up（dentry の unhash）と一般ユーザの権限チェックを同時に走らせる |
| Z | jffs2 と overlay の umount→mount 後の永続性、**dcache が空の状態**での readdir / rmdir |
| E | upper=ext2（xattr あり）でも whiteout・不透明ディレクトリが動く |
| T | upper=tmpfs。3.0 未満の tmpfs は `trusted.*` xattr が無いので whiteout が失敗する＝**NOTE（想定内）** |
| K | カーネルログに WARNING / BUG / Oops / ロック検証の警告が無い |

## 結果の読み方

- `SUMMARY: … FAIL=0` で、カーネルの異常ログが無ければ合格。終了コードは 0。
- NOTE 2 件は想定内: T03（tmpfs の whiteout 不可）と kmemleak 無し。
- ゲストの時計は 1970 年から始まる（RTC 無し）。時刻を比べるテストはそれを前提にしている。

## 完走しないとき（自動診断）

コンソール出力が `STALL_SEC` 止まると、run.sh は動いているゲストに QMP で接続して
[diag.sh](diag.sh) を走らせる:

1. **SysRq-w** を送る → ブロック中タスクのスタックがコンソールに出る（デッドロック向け）
2. **PC / LR を 3 回採る** → `System.map` で関数名にする（無限ループ・panic 待機の判別）
3. **`__log_buf` を物理メモリから吸い出す** → コンソール初期化前の panic も読める

0013 を外したときの実際の出力（jffs2 上のデッドロック。抜粋・アドレス省略）:

```
run 1: ✗ 完走せず（30s 出力なし / panic / ハング）。最後のテスト出力:
    PASS: W07 rmdir non-empty lower dir fails [...]
  SysRq-w（ブロック中のタスク）:
    rm            D c0d7ecf8     0   430    429 0x00000000
    (__mutex_lock_slowpath) from (jffs2_do_unlink)
    (jffs2_do_unlink) from (jffs2_unlink) … (vfs_unlink) from (ovl_unlink_whiteout)
    (ovl_unlink_whiteout) from (jffs2_readdir)
    (jffs2_readdir) from (vfs_readdir) … (ovl_remove_whiteouts) from (ovl_rmdir)
```

`--debug` のときは `__log_buf` から `kernel BUG at mm/slab.c:2160!` と `start_kernel` からの
バックトレースが出る（このツリーのデバッグ設定の問題で、overlayfs とは無関係）。

## 別の機種・カーネルで使う

run.sh は機種の `build.env`（`ARCH`・toolchain・tarball）と `patches/` をそのまま使う。
アーキごとの違いは `targets/<ARCH>.env` に閉じている。

| 変数 | 意味（arm の値） |
|---|---|
| `QEMU_SYSTEM` / `QEMU_MACHINE` / `QEMU_MEM` / `QEMU_EXTRA` | `qemu-system-arm` / `versatilepb` / `256` / `-audio none` |
| `KDEFCONFIG` / `KCONFIG_TARGET_FRAGMENT` | ベンダツリー内の QEMU ボードの defconfig / アーキ用の追加設定 |
| `KIMAGE` / `KIMAGE_PATH` | ビルドターゲットと出来上がる場所 |
| `CONSOLE` | `ttyAMA0` |
| `KERNEL_VA_TO_PA` | `__log_buf` の物理アドレス = 仮想アドレス − この値（`0xc0000000`） |
| `QEMU_PC_REGEX` / `QEMU_LR_REGEX` | QEMU の `info registers` から PC / 戻り先を抜く正規表現 |
| `BUSYBOX_URL` / `BUSYBOX_FILE` / `BUSYBOX_SHA256` | テスト用の静的 busybox |

**2 機種目（emma3sep, linux-2.6.27.39, MIPS BE）で `targets/mips.env` を作るときの目安**
（未検証。実際に試して直すこと）:

- QEMU は `qemu-system-mips -M malta`、`KDEFCONFIG=malta_defconfig`（mainline 2.6.27 にはある。
  ベンダツリーに残っているか確認）、`KIMAGE=vmlinux`・`KIMAGE_PATH=vmlinux`、`CONSOLE=ttyS0`。
- `KERNEL_VA_TO_PA=0x80000000`（KSEG0）。`QEMU_PC_REGEX` は MIPS の `info registers` の表記に合わせる。
- busybox は `https://busybox.net/downloads/binaries/1.21.1/busybox-mips`（存在は確認済み。sha256 は取得して記入）。
- `KCONFIG_TARGET_FRAGMENT` で実機の `.config` に合わせる（エンディアン、プリエンプション等）。

**カーネルの版で変わる点**:

- **squashfs が mainline に入ったのは 2.6.29。** 2.6.27 のベンダツリーが squashfs 3.x を独自に
  持っている場合、ホストの `mksquashfs`（4.0 形式）のイメージは読めない。`mkimages.sh` の
  lower 層を 3.x 形式にするか、別の読み取り専用 FS に替える。
- `CONFIG_DETECT_HUNG_TASK` は 2.6.30 から。無いカーネルでは `INFO: task … blocked` は出ないが、
  停止検出と SysRq-w で同じ情報が取れる。
- run.sh は必要な `CONFIG_*` が有効にならないと止まる（名前が変わっていればそこで分かる）。
- `guest/init.sh` の mount 構文は v4 系（`-t overlayfs`、`workdir` 無し）。後の版の overlayfs を
  使うなら setup 節を合わせる。

## 既知の制約

- QEMU の CPU は実機と違う（mb86h61 実機は ARM1176、テストは ARM926）。VFS の検証にはこれで
  足りるが、実機での確認は別に要る。
- mb86h61 の 2.6.31.6-rt19 ツリーは `DEBUG_SPINLOCK` 系・lockdep・`DEBUG_SLAB`・kmemleak で
  起動しない。ロック検証はできていない。
- 電源断（copy-up 途中の中断）は試していない。

# overlayfs のカーネル取り込み — 2.6.31 移植の記録と、別カーネルへ移すときの手順

rootfs を「squashfs（読み取り専用）＋書き込み層」にする OpenWrt 流の構成のために、
古いベンダカーネルへ overlayfs を移植した記録。**2 機種目以降で同じ作業をするときの
出発点**として、何を選び、何が壊れ、何を確かめたかを残す。

## 1. 現状（SB-TV02-WFPL / linux-2.6.31.6-rt19）

| 項目 | 状態 |
|---|---|
| パッチ | `br-ext-chip-fujitsu/board/mb86h61/kernel/patches/0003`〜`0014`（fuzz 0 で適用） |
| 設定 | `linux.config` に `CONFIG_OVERLAYFS_FS=y` |
| mtd5 への影響 | Image 2,364,128B → RDHB 1,193,701B（上限 1,572,864B の 75%、取り込み前比 +約 6KB） |
| QEMU 機能テスト | 82 項目 × 5 回 PASS、カーネル異常ログ 0（[tests/overlayfs](../tests/overlayfs/README.md)） |
| 実機 | **未確認**（起動・動作とも） |
| upper 層に使う FS | 実機の `linux.config` では未有効（区画の切り直しと合わせて jffs2+XATTR か ubifs を入れる） |

## 2. なぜ 2010 年の v4 か

- mainline に overlayfs が入ったのは **3.18**（2014-10 マージ）。2.6.x の対象機には無い。
- 前身は Miklos Szeredi の patch 系列（2010-09 の prototype から 4 年間 out-of-tree）。
  VFS は **2.6.37 で `file_system_type->mount`**、**2.6.38 で RCU path walk
  （`->permission` にフラグ引数）**と大きく変わるので、その直前の最終版
  **v4（linux-fsdevel, 2010-10-26）**が古いカーネルからの移植距離が最短になる。
- mini_fo / unionfs / aufs は mainline に入らなかった別系統の実装で、後の mainline
  overlayfs の修正や構造を参照できないため採らなかった。

**入手元**: marc.info の linux-fsdevel アーカイブ。`https://marc.info/?l=linux-fsdevel&m=<id>&q=mbox`
でヘッダ付きの全文が取れる（`q=raw` は本文だけ）。各パッチ本文に元の URL を残してある。
lore.kernel.org と git.kernel.org はボット検査のため自動取得できず、Miklos の `vfs.git` には
`overlayfs.v11` 以降のブランチしか残っていない（v1〜v10 は削除済み）。mainline の参照は
GitHub の torvalds/linux ミラーの raw（例: `v3.18/fs/overlayfs/readdir.c`）で読める。

## 3. パッチ一覧

| # | 内容 | 由来 | 2.6.31 での扱い |
|---|---|---|---|
| 0003 | vfs: open forwarding（`f_op->open_other`） | v4 1/8 | refresh。`open_other` は `struct file_operations` の**末尾**に置く（v4 は `->open` の直後。ベンダ製 .ko の `->flush` 以降がずれる。[tests/kabi](../tests/kabi/README.md)） |
| 0004 | vfs: `s_op->is_same_inode()` | v4 3/8 | refresh。宣言の hunk だけ `vfs_statfs()` の直後へ移した |
| 0005 | vfs: `do_splice_direct()` を export | v4 4/8 | refresh |
| 0006 | `security_inode_permission` を export | v4 5/8 | refresh |
| 0007 | overlay filesystem prototype | v4 6/8 | refresh |
| 0008 | statfs 対応 | v4 7/8 | refresh |
| 0009 | ドキュメント | v4 8/8 | refresh |
| 0010 | `ovl_is_same_inode` の lower 欠落対応 | 2010-11 修正 1/2 | refresh |
| 0011 | `ovl_copy_up_xattr` の EOPNOTSUPP 対応 | 2010-11 修正 2/2 | refresh |
| — | vfs: `->permission` に dentry を渡す | v4 2/8 | **不採用**（43 ファイル改変・34 hunk reject。0012 で不要化） |
| 0012 | 2.6.31 への移植 | OpenIRD | §4 |
| 0013 | readdir コールバックからの再入によるデッドロック修正 | OpenIRD | §5.1 |
| 0014 | 1MB 超ファイルの copy-up 切り詰め修正 | OpenIRD | §5.2 |

## 4. 移植で変えたこと（0012）

- **inode を dentry 毎に持たせる。** v4 は通常ファイル・symlink・特殊ファイルの inode を
  スーパーブロック単位で 1 個ずつ共有しており、`->permission(inode)` ではどのファイルか
  分からない。それが v4 2/8（VFS 全体の改変）を必要とした理由。mainline 3.18 と同じく
  非ディレクトリも dentry 毎に inode を割り当て、`ovl_permission()` は inode の alias から
  dentry を引く。
- **alias は `i_dentry` を自前で辿る。** copy-up は非ディレクトリの dentry を unhash するが、
  2.6.31（2.6.27 も同じ）の `d_find_alias()` は unhash 済みの非ディレクトリ alias を返さない。
- `->fsync` と `vfs_fsync()` は dentry 引数ありの形（2.6.35 で変更）に合わせる。
- `inode_operations->check_acl` は無い（2.6.32 で追加）。2.6.31 の VFS と同じく
  `generic_permission(inode, mask, NULL)` を呼ぶ。

## 5. 動かして初めて見つかったバグ

どちらもコンパイルは警告なしで通り、QEMU で実際に動かして見つかった。

### 5.1 upper が jffs2 だとデッドロック（0013）

- **症状**: lower にあるディレクトリの `rm -rf` が固まる（`INFO: task rm blocked for more than 120 seconds`）。
  再 mount 直後（dcache が空）に、whiteout を含むマージ済みディレクトリを `ls` しても固まる。
- **原因**: `jffs2_readdir()` はディレクトリの `f->sem` を握ったまま filldir を呼ぶ。v4 は
  filldir の中で `lookup_one_len()`（whiteout 判定）と `vfs_unlink()`（rmdir 時の whiteout
  削除）を呼び、`jffs2_lookup()`/`jffs2_do_unlink()` が同じ `f->sem` を取りに行く。
- **見逃しやすい理由**: ext2 や tmpfs は readdir 中に固有のロックを持たないので出ない。
  jffs2 でも、lookup が dcache にヒットしている間は出ない。
- **修正**: コールバックでは名前を集めるだけにし、判定と削除は `vfs_readdir()` の後に
  upper ディレクトリの `i_mutex` を取り直して行う。mainline 3.18 の
  `ovl_dir_mark_whiteouts()` / `ovl_cleanup_whiteouts()` と同じ構造。
- **テスト**: W08, Z12〜Z14。

### 5.2 1MB を超えるファイルの copy-up で中身が消える（0014）

- **症状**: 2.5MB のファイルに追記すると upper には 1,048,576B しか残らない。エラーは出ない。
- **原因**: **2.6.34 までの `direct_splice_actor()` は `sd->pos` に書き込み、`out->f_pos` を
  進めない**（2.6.35 で `&file->f_pos` に変更）。v4 の `ovl_copy_up_data()` は 2.6.35 以降の
  挙動を前提に、1MB チャンク毎に `new_file->f_pos` から位置を取り直していたため、2.6.31 では
  毎回先頭から読んで先頭に書いていた。
- **修正**: オフセットをループ外で保持する。あわせて致命的シグナルで中断したときに 2 つの
  file を閉じずに return していた upstream のリークも直した。
- **テスト**: B09, Z10。

## 6. rootfs・区画設計への含意

- **upper 層の FS は `trusted.*` xattr が必須。** whiteout は `trusted.overlay.whiteout` 付きの
  symlink、不透明ディレクトリは `trusted.overlay.opaque` で表す。
  - jffs2（`CONFIG_JFFS2_FS_XATTR`）・ext2（`CONFIG_EXT2_FS_XATTR`）: QEMU で動作確認済み。ubifs も xattr を持つ（未テスト）。
  - **tmpfs は 2.6.31 では upper にできない**（`trusted.*` 対応は 3.0 から）。copy-up は通るが、
    lower のファイルを消すと `Operation not supported` になる（実測）。RAM 上に書き込み層を
    置くなら ramdisk＋ext2 などにする。
- **mount 構文**（v4）: `mount -t overlayfs overlayfs -o lowerdir=<RO>,upperdir=<RW> <mnt>`
  （型名は `overlayfs`、`workdir=` は無い）。
- **copy-up はアトミックではない。** `workdir` が無いので、電源断で upper に書きかけの
  ファイルが残りうる。未検証・未対策。
- lower と upper を overlay の外から直接書き換えることは想定されていない（v4 の既知の制約）。

## 7. 検証できていないこと

- 実機（ARM1176 / MB86H61）での起動と動作。QEMU は ARM926（versatilepb）。
- **ロック検証。** この 2.6.31.6-rt19 ツリーは `DEBUG_SPINLOCK`/`DEBUG_MUTEXES`/
  `DEBUG_SPINLOCK_SLEEP`、lockdep、`DEBUG_SLAB`、kmemleak のどれを入れても `start_kernel`
  の中（`kmem_cache_create` の `BUG_ON` など、FS 初期化より前）で落ちる。ロックの正しさは
  mainline と同じ構造にしたことと、競合テスト（X01〜X03）で確かめただけで、証明ではない。
- v4 の `ovl_fill_super()` は mount 成功時にオプション文字列を解放しない（読んで気づいた
  リーク）。kmemleak が使えないため未確認・未修正。
- 電源断時の挙動。

## 8. 別のカーネルへ移すとき（2 機種目 BMMPX1301 = linux-2.6.27.39 / MIPS BE を想定）

### 8.1 手順

1. 機種の `board/<family>/kernel/build.env` と `make kernel BOARD=…` を先に整える
   （テストはそのツリー・パッチ・toolchain を使う）。
2. 0003〜0014 をその機種の `patches/` に写して当てる。**reject を fuzz を上げて押し込まない**。
   今回、`-F3` で当てたら宣言の hunk が文脈と無関係な位置に当たった。reject は文脈を読んで
   手で直す。
3. 下の API 差を直してコンパイルを通す。
4. `tests/overlayfs/targets/<ARCH>.env` を用意して `make test-overlayfs BOARD=…`。
   コンパイルが通っても意味の違いは実行しないと分からない（§5 はどちらもそうだった）。
5. 完走しなければ run.sh の自動診断を読む。どのパッチが原因かは `--skip-patch` で切り分ける。
6. `make test-kabi BOARD=…` で、ベンダ製 .ko から見た構造体の位置が変わっていないことを確かめる
   （その機種でベンダの受信部品を動かす段階 B の前提）。

### 8.2 2.6.27 で問題になる API 差

mainline の各タグのソースで確認した（2026-09-15）。

| API | v4 の前提 | 2.6.27 | 変わった版 | 影響 |
|---|---|---|---|---|
| `struct cred` / `prepare_creds()` / `override_creds()` | 使う | **無い** | 2.6.29 | 権限を一時的に上げる 7 箇所（readdir・lookup・copy-up など）を `current` の fsuid/capability の保存・復元に書き換える |
| `dentry_open(dentry, mnt, flags, cred)` | 4 引数 | 3 引数 | 2.6.29 | `path_open()` |
| `vfs_fsync()` | 使う | **無い** | 2.6.29 | `ovl_dir_fsync()` |
| `kern_path()` | 使う | **無い** | 2.6.28 | `ovl_fill_super()`（`path_lookup()`＋nameidata に） |
| squashfs（mainline） | — | **無い** | 2.6.29 | テストの lower 層。ベンダツリーが squashfs 3.x を独自に持つなら `mkimages.sh` を 3.x 形式に合わせる |
| `->fsync` の引数 | (file, datasync) | dentry あり | 2.6.35 | 0012 で対応済み |
| `i_op->check_acl` | 使う | 無い | 2.6.32 | 0012 で対応済み |
| `direct_splice_actor()` の書き込み位置 | `out->f_pos` | `sd->pos` | 2.6.35 | 0014 で対応済み（2.6.27 も同じ挙動） |
| `d_find_alias()` が unhash 済み非 dir を返さない | — | 同じ | — | 0012 で対応済み |
| `jffs2_readdir()` が `f->sem` を保持 | — | 同じ | — | 0013 で対応済み |
| tmpfs の `trusted.*` xattr | — | 無い | 3.0 | upper 層に使えない |
| `inode_permission()` | 使う | ある | 2.6.27 | そのまま |
| `->get_sb` / `->permission(inode, mask)` | 使う | 同じ | 2.6.37 / 2.6.38 で変更 | そのまま |

2.6.27 には `CONFIG_DETECT_HUNG_TASK`（2.6.30 から）も無いので、ハングは `INFO: task … blocked`
では出ない。run.sh の停止検出と SysRq-w で見る。

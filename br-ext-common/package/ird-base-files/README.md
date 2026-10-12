# ird-base-files
全機種共通の rootfs skeleton・init・既定設定。

## 最初のログイン

書き込んだ直後のイメージは、ネットワークから `ssh root@<機器のアドレス>` でそのまま入れる。
シリアルコンソールを前提にしない（OpenWrt と同じ考え方）。

- root のパスワードは空で、dropbear を `-B`（空のパスワードのログインを受ける）で起動する
  （`files/etc/default/dropbear`。Buildroot の `S50dropbear` がこのファイルを読む）。
- **`passwd` でパスワードを付けると、以後はそれを聞く。** dropbear がこの経路を通すのは
  パスワード欄が空の間だけなので、設定を戻す操作は無い。
- **付けるまでは、機器に届く誰もが root で入れる。** ログインのたびに警告を出す
  （`files/etc/profile.d/openird.sh`）。
- アドレスは DHCP で取る（ホスト名 `openird` を名乗る）。取れるまでは `192.168.1.1/24`
  （`files/etc/init.d/S40network`）。

実機での確認（2026-10-12・POE-BR100-YB1）: この設定を入れた rootfs を書いて電源を入れ直し、
シリアルを使わずにパスワードなしの `ssh root@<機器>` で入れること、ログイン時に警告が出ること、
そのセッションから受信（`ird-rec`）まで動くことを見た。`-B` の無い版（`v26.10-rc2`）は、同じ
操作を `Permission denied` で拒否した。`passwd` のあとに閉じることは、同じ版の dropbear を
ホスト向けにビルドして確かめた（実機ではパスワードを設定していない）。

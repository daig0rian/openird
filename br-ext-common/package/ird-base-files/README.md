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

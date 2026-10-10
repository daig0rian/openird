# ird-addon
アドオン（実行時パッケージ `.ird`）を永続 overlay に導入／削除する道具。形式・索引・
検証・受領記録の決まりは [docs/addon.md](../../../docs/addon.md)。

- `files/usr/sbin/ird-addon` … BusyBox の ash とアプレットだけで動く sh スクリプト。
- `files/etc/openird/addons.conf` … アドオンリポジトリの URL 群。**base 既定は空**。
- 機器の Arch id は board の `/etc/openird/board.conf` の `ADDON_ARCH=`。
- テストは `make test-addon`（`tests/addon/`。偽の機器ツリーに対して走らせる）。

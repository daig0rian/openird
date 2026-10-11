# SPDX-License-Identifier: GPL-2.0-only
# ベンダの squashfs4 rootfs/slot を root 権限なしで展開・再梱包するための共通関数。
# scripts/build-vendor-rig.sh と scripts/build-vendor-base.sh が source する。
#
# 前提: 呼び出し側が WORK（ext4 上の作業ディレクトリ）を定義していること。
#   - ベンダイメージは静的デバイスノードを数百個持つ。非 root の unsquashfs は落とすので、
#     マニフェスト（unsquashfs -lls）から mksquashfs の pseudo-file を作って戻す。
#   - 非 root 展開で umask に削られたモードも、マニフェストから chmod で戻す。
#   - 梱包は 2.6.31 の squashfs4 が読める設定（gzip / 128KB / xattr 無し）に固定。

# lls の 1 行から「perm<TAB>path」を取り出す（パスに空白があっても壊れない）
_lls_fields() {
  sed -nE 's/^([bcdlps-][rwxsStT-]{9}) +[^ ]+ +.*[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2} (.*)$/\1\t\2/p' "$1"
}

_AWK_MODE='
function mode(p,  u,g,o,sp) {
  u=0; g=0; o=0; sp=0
  if (substr(p,2,1)=="r") u+=4; if (substr(p,3,1)=="w") u+=2
  if (substr(p,4,1)=="x"||substr(p,4,1)=="s") u+=1
  if (substr(p,4,1)=="s"||substr(p,4,1)=="S") sp+=4
  if (substr(p,5,1)=="r") g+=4; if (substr(p,6,1)=="w") g+=2
  if (substr(p,7,1)=="x"||substr(p,7,1)=="s") g+=1
  if (substr(p,7,1)=="s"||substr(p,7,1)=="S") sp+=2
  if (substr(p,8,1)=="r") o+=4; if (substr(p,9,1)=="w") o+=2
  if (substr(p,10,1)=="x"||substr(p,10,1)=="t") o+=1
  if (substr(p,10,1)=="t"||substr(p,10,1)=="T") sp+=1
  return sp "" u "" g "" o
}
'

# デバイスノード・FIFO・socket を mksquashfs の pseudo-file として書き出す
mkpseudo() {
  awk "$_AWK_MODE"'
    /^[bcps]/ {
      type=substr($1,1,1); maj=""; min=""
      for (i=3; i<=NF; i++) {
        if ($i ~ /,$/)     { maj=$i; sub(/,$/,"",maj); min=$(i+1); break }
        else if ($i ~ /,/) { split($i, a, ","); maj=a[1]; min=a[2]; break }
      }
      path=$NF; sub(/^squashfs-root/, "", path)
      if (type=="c" || type=="b") {
        if (maj != "" && min != "") printf "%s %s %s 0 0 %d %d\n", path, type, mode($1), maj, min
      } else {
        printf "%s %s %s 0 0\n", path, type, mode($1)
      }
    }
  ' "$1"
}

# 非 root 展開で落ちたモードをマニフェストから戻す
fixmodes() {
  _lls_fields "$1" | awk -F'\t' "$_AWK_MODE"'
    $1 ~ /^[-d]/ { p=$2; sub(/^squashfs-root/, "", p); printf "%s\t%s\n", mode($1), p }
  ' | while IFS=$'\t' read -r m p; do
      [ -e "$2$p" ] && chmod "$m" "$2$p"
    done
  return 0
}

# 比較用に lls を正規化（時刻を落とし、パス基準で並べる）
norm() {
  sed -E 's/[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}//' "$1" \
    | sed -E 's/^squashfs-root//; s#squashfs-root/#/#' \
    | sed -E 's/[[:space:]]+/ /g' | sort
}

# 2.6.31 の squashfs4 が読める設定に固定する
pack() {
  local src="$1" out="$2" pseudo="$3"
  local args=(-b 131072 -comp gzip -no-xattrs -noappend -all-root -no-progress)
  [ -s "$pseudo" ] && args+=(-pf "$pseudo")
  mksquashfs "$src" "$out" "${args[@]}" > "$out.mksq.log" 2>&1
}

# 元イメージを展開してマニフェストとモードを揃える
prepare() {
  local name="$1" img="$2"
  unsquashfs -lls "$img" > "$WORK/$name.lls" 2>/dev/null
  rm -rf "$WORK/$name.src"
  unsquashfs -no-progress -d "$WORK/$name.src" "$img" > "$WORK/$name.unsq.log" 2>&1 || true
  mkpseudo "$WORK/$name.lls" > "$WORK/$name.pseudo"
  fixmodes "$WORK/$name.lls" "$WORK/$name.src"
  echo "  $name: $(wc -l < "$WORK/$name.lls") エントリ / pseudo $(wc -l < "$WORK/$name.pseudo") 行"
}

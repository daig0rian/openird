# OpenIRD login banner (ASCII only - serial consoles are not UTF-8 safe)
[ -r /etc/openird_release ] && . /etc/openird_release

printf '\n OpenIRD %s %s\n' "${OPENIRD_BUILD:-unknown}" "${OPENIRD_BOARD:+($OPENIRD_BOARD)}"

if grep -q ' /overlay tmpfs ' /proc/mounts 2>/dev/null; then
	printf '   overlay  tmpfs - VOLATILE, changes are lost on reboot\n'
elif grep -q ' /overlay ' /proc/mounts 2>/dev/null; then
	printf '   overlay  %s\n' "$(awk '$2=="/overlay"{print $1" ("$3")"}' /proc/mounts)"
else
	printf '   overlay  none - read-only root\n'
fi

if [ -n "$(awk -F: '$1=="root"{print $2}' /etc/shadow 2>/dev/null | tr -d '!*')" ]; then
	printf '   ssh      dropbear\n'
else
	printf '   ssh      dropbear (set a root password with "passwd" to allow login)\n'
fi
printf '\n'

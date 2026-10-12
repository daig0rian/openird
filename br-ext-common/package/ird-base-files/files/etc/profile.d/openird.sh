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

# dropbear runs with -B (/etc/default/dropbear): a blank root password lets
# anyone on the network in, a locked one ("!" or "*") lets nobody in
if [ -r /etc/shadow ]; then
	case "$(awk -F: '$1=="root"{print $2}' /etc/shadow)" in
	'')
		printf '   ssh      dropbear - NO ROOT PASSWORD, anyone on the network can log in\n'
		printf '            set one now with "passwd"\n' ;;
	'!'*|'*'*)
		printf '   ssh      dropbear (root is locked: set a password with "passwd")\n' ;;
	*)
		printf '   ssh      dropbear\n' ;;
	esac
else
	printf '   ssh      dropbear\n'
fi
printf '\n'

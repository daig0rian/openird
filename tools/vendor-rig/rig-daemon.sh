#!/bin/sh
# OpenIRD observation rig: start the vendor application the way the rig wants it.
# Installed into the modified slot0 as /slot/rig-daemon.sh.
#
# Three things this handles, all learned on hardware:
#  1) dione_server builds br0 and enslaves eth0.  If eth0 still carries the
#     address the rig gave it, the LAN dies.  The vendor's own /slot/app/d
#     clears it the same way before starting the daemon.
#  2) Started plainly, the daemon takes the console as its stdin and the factory
#     test shell (prompt '->') occupies it, so no shell commands can be typed.
#     Giving it stdin from a FIFO keeps both channels usable.
#  3) The Dione driver must be loaded first, as the vendor's slot_entry.sh did.
#
# The daemon arms the hardware watchdog: if it exits, the board resets about 20
# seconds later.  There is no stop_watchdog in the production package.
FIFO=/tmp/dbgin

[ -p "$FIFO" ] || mkfifo "$FIFO" || exit 1
# Hold a writer open so the daemon's reader never sees EOF.
sleep 999999 > "$FIFO" &

lsmod | grep -q '^h57fmb' || insmod /slot/drv/h57fmb.ko
/sbin/ifconfig eth0 0.0.0.0

dione_server < "$FIFO" > /dev/console 2>&1 &

echo ""
echo "vendor application started: stdin = $FIFO, output on /dev/console"
echo "inject factory test shell commands like this:"
echo "  echo \"forceinit\"                > $FIFO   # power-cycles Dione, re-enumerates USB"
echo "  echo \"dpower 1 1\"               > $FIFO"
echo "  echo \"settuner 0 557142857 0 0\" > $FIFO   # terrestrial ch27, tunes AND opens TS"
echo "  echo \"gsignal\"                  > $FIFO   # Sync / CN / BER / Exist_A,B,C"
echo "  echo \"chktunerts\"               > $FIFO   # 'there is TS_IN' when TS reaches Dione"
echo "  echo \"list\"                     > $FIFO   # all 84 commands"
echo ""
echo "sfreq tunes the demodulator only and leaves the TS path closed, which is"
echo "what you want when reading the TS yourself through the FAPex TSD."
echo "playstop (no arguments) closes the Dione TS streamer again."
echo ""

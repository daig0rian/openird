
# ==== OpenIRD observation rig ====================================
# Everything above this line is the vendor slot_entry.sh, unchanged.
# Removed from here on: "insmod /slot/drv/h57fmb.ko" and the
# "while : ; do dione_server ; done" loop, i.e. the vendor application.
# rcS then falls through to its own "setsid cttyhack /bin/sh" loop, so the
# serial console becomes a root shell with the vendor environment fully set up
# (FAPex modules loaded, sysctl applied, USB stack up, LD_LIBRARY_PATH and PATH
# pointing into /slot).

# Network, so the rig is reachable before the vendor application runs.
# NOTE: dione_server builds br0 and enslaves eth0, so this address must be
# cleared before starting it.  /slot/rig-daemon.sh does that for you.
/sbin/ifconfig eth0 192.168.11.10 netmask 255.255.255.0 up
/sbin/route add default gw 192.168.11.1 2>/dev/null
# Unauthenticated root shell on tcp/23 for observation convenience.
# Remove this line if the rig is ever on an untrusted network.
/usr/sbin/telnetd -l /bin/sh

echo ""
echo "================ OpenIRD observation rig ================"
echo "Vendor application is NOT started."
echo ""
echo "Start it, keeping this console as a shell:"
echo "  /slot/rig-daemon.sh"
echo ""
echo "Or start it the way the vendor script did (console becomes the"
echo "factory test shell, prompt '->'):"
echo "  insmod /slot/drv/h57fmb.ko"
echo "  dione_server"
echo ""
echo "To start it under the FAPex ioctl tracer:"
echo "  LD_PRELOAD=/slot/fapex_trace.so dione_server"
echo "  (tracer writes /tmp/fapex-trace.log)"
echo ""
echo "Shell: serial console, or telnet 192.168.11.10"
echo "========================================================"
echo ""

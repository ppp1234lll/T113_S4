#!/bin/sh
# ============================================================================
# bsp-survey.sh -- IVSBox BSP capability survey
#
# 跑在 T113 板子上（不是主机）。输出纯 ASCII，便于回传后直接贴进
#   docs/bsp-capability.md
#
# 用法（板子上）:
#   chmod +x bsp-survey.sh
#   ./bsp-survey.sh > /tmp/bsp-survey.txt 2>&1
#   然后从主机把 /tmp/bsp-survey.txt 拷回来:
#   scp root@<board-ip>:/tmp/bsp-survey.txt .
#
# 说明:
#   Tina / OpenWrt 用的是 busybox ash，本脚本只用 POSIX sh 语法。
#   任何一段失败都不会中断整个盘点（没有 set -e）。
#   板子上的中文控制台通常是 GBK，所以本脚本的输出全部是英文。
# ============================================================================

sec() {
	echo
	echo "===== $1 ====="
}

have() {
	command -v "$1" >/dev/null 2>&1
}

# 逐个报告命令是否存在，比单纯 command -v 更好读
which_all() {
	for c in "$@"; do
		printf '%-18s: ' "$c"
		if have "$c"; then
			command -v "$c"
		else
			echo "NO"
		fi
	done
}

# 打印一条命令再执行它（命令不存在时只报 skip）
show() {
	echo "\$ $*"
	if have "$1"; then
		"$@" 2>&1
	else
		echo "[skip] not found: $1"
	fi
}

echo "IVSBox BSP survey"
echo "generated: $(date 2>/dev/null)"

# ---------------------------------------------------------------------------
sec "1. identity / kernel"
show id
show uname -a
echo "\$ cat /proc/version";        cat /proc/version 2>&1
echo "\$ cat /etc/openwrt_release"; cat /etc/openwrt_release 2>&1
echo "\$ cat /etc/os-release";      cat /etc/os-release 2>&1
show cat /proc/uptime

# ---------------------------------------------------------------------------
sec "2. cpu / memory / storage"
echo "\$ cat /proc/cpuinfo"; cat /proc/cpuinfo 2>&1
show free
echo "\$ head -8 /proc/meminfo"; head -8 /proc/meminfo 2>&1
show df -h
echo "\$ cat /proc/partitions"; cat /proc/partitions 2>&1
echo "\$ cat /proc/mtd";        cat /proc/mtd 2>&1

# ---------------------------------------------------------------------------
sec "3. libc flavour (决定交叉工具链与动态库路径)"
echo "\$ ls -l /lib/ld-* /lib/libc* /lib/libuClibc*"
ls -l /lib/ld-* /lib/libc* /lib/libuClibc* 2>&1
echo "\$ ls -l /lib/libpthread* /lib/libdl* /librt*"
ls -l /lib/libpthread* /lib/libdl* /lib/librt* 2>&1

# ---------------------------------------------------------------------------
sec "4. serial ports (板间链路 / GPS / 调试口)"
echo "\$ ls -l /dev/ttyS* /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM*"
ls -l /dev/ttyS* /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM* 2>&1
echo "\$ cat /proc/tty/driver/serial"
cat /proc/tty/driver/serial 2>&1
echo "\$ cat /proc/devices | grep -i tty"
cat /proc/devices 2>/dev/null | grep -i tty

# ---------------------------------------------------------------------------
sec "5. network interfaces and routes"
show ip -o link
show ip -o addr
show ip route
show ifconfig
echo "\$ cat /proc/net/dev"; cat /proc/net/dev 2>&1

# ---------------------------------------------------------------------------
sec "6. usb and 4G modem"
show lsusb
echo "\$ ls /sys/class/net"; ls /sys/class/net 2>&1
echo "\$ ls -l /sys/class/net/*/device 2>/dev/null"; ls -l /sys/class/net/*/device 2>/dev/null
echo "\$ dmesg | tail -50"
dmesg 2>/dev/null | tail -50
echo "\$ dmesg | grep -i -E 'usb|ec200|ml307|qmi|cdc|rndis|option' | tail -40"
dmesg 2>/dev/null | grep -i -E 'usb|ec200|ml307|qmi|cdc|rndis|option' | tail -40

# ---------------------------------------------------------------------------
sec "7. watchdog (看护方案的前提)"
echo "\$ ls -l /dev/watchdog*"; ls -l /dev/watchdog* 2>&1
echo "\$ ls -l /sys/class/watchdog/"; ls -l /sys/class/watchdog/ 2>&1
for w in /sys/class/watchdog/*; do
	[ -e "$w" ] || continue
	echo "-- $w"
	echo "   identity: $(cat "$w/identity" 2>/dev/null)"
	echo "   timeout : $(cat "$w/timeout" 2>/dev/null)"
	echo "   state   : $(cat "$w/state" 2>/dev/null)"
done

# ---------------------------------------------------------------------------
sec "8. media stack (录像/抓图的前提)"
which_all ffmpeg ffprobe gst-launch-1.0
echo "\$ ls -l /usr/bin/ffmpeg /usr/bin/ffprobe /usr/local/bin/ffmpeg"
ls -l /usr/bin/ffmpeg /usr/bin/ffprobe /usr/local/bin/ffmpeg 2>&1
echo "\$ ls -l /usr/lib/libavformat* /usr/lib/libavcodec* /usr/lib/libavutil* /usr/lib/libswscale*"
ls -l /usr/lib/libavformat* /usr/lib/libavcodec* /usr/lib/libavutil* /usr/lib/libswscale* 2>&1
if have ffmpeg; then
	show ffmpeg -hide_banner -version
fi
echo "\$ ls -l /dev/video*"; ls -l /dev/video* 2>&1

# ---------------------------------------------------------------------------
sec "9. audio (音频广播的前提)"
which_all aplay arecord amixer alsactl
echo "\$ ls /dev/snd"; ls /dev/snd 2>&1
echo "\$ cat /proc/asound/cards"; cat /proc/asound/cards 2>&1
echo "\$ ls -l /usr/lib/libasound*"; ls -l /usr/lib/libasound* 2>&1

# ---------------------------------------------------------------------------
sec "10. database / json / crypto"
echo "\$ ls -l /usr/lib/libsqlite3* /usr/lib/libcjson*"
ls -l /usr/lib/libsqlite3* /usr/lib/libcjson* 2>&1
echo "\$ ls -l /usr/lib/libssl* /usr/lib/libcrypto* /usr/lib/libmbed*"
ls -l /usr/lib/libssl* /usr/lib/libcrypto* /usr/lib/libmbed* 2>&1
which_all sqlite3 openssl
if have sqlite3; then
	show sqlite3 --version
fi

# ---------------------------------------------------------------------------
sec "11. cgroup / netfilter (资源隔离与防火墙)"
echo "\$ ls /sys/fs/cgroup"; ls /sys/fs/cgroup 2>&1
echo "\$ cat /sys/fs/cgroup/cgroup.controllers"; cat /sys/fs/cgroup/cgroup.controllers 2>&1
echo "\$ cat /proc/cgroups"; cat /proc/cgroups 2>&1
which_all nft iptables ip6tables
echo "\$ ls -l /usr/lib/libnftables* /usr/lib/libmnl*"
ls -l /usr/lib/libnftables* /usr/lib/libmnl* 2>&1
echo "\$ lsmod | head -40"; lsmod 2>/dev/null | head -40

# ---------------------------------------------------------------------------
sec "12. service management / logging (procd / logd / ubus)"
echo "\$ ls -l /sbin/procd /sbin/logd /sbin/ubusd /bin/ubus"
ls -l /sbin/procd /sbin/logd /sbin/ubusd /bin/ubus 2>&1
echo "\$ ls /etc/init.d"; ls /etc/init.d 2>&1
echo "\$ ls /etc/rc.d";   ls /etc/rc.d 2>&1
echo "\$ ls /etc/config"; ls /etc/config 2>&1
echo "\$ ls /etc/hotplug.d"; ls /etc/hotplug.d 2>&1
which_all logd procd ubus

# ---------------------------------------------------------------------------
sec "13. 4G dial stack"
which_all udhcpc dhcpcd pppd chat uqmi mmcli quectel-CM qmicli

# ---------------------------------------------------------------------------
sec "14. GPS"
which_all gpsd gpsctl

# ---------------------------------------------------------------------------
sec "15. builder / debug tools on target"
which_all ssh scp wget curl tcpdump socat strace gdb gdbserver lsof top ps_mem

# ---------------------------------------------------------------------------
sec "16. kernel config highlights"
if [ -f /proc/config.gz ]; then
	echo "\$ zcat /proc/config.gz | grep -E 'WATCHDOG|NETFILTER|CGROUP|SND|USB_NET|IPV6|PPP|FHANDLE|SQLITE'"
	zcat /proc/config.gz 2>/dev/null | grep -E 'WATCHDOG|NETFILTER|CGROUP|SND|USB_NET|IPV6|PPP|FHANDLE|SQLITE'
else
	echo "[skip] /proc/config.gz not present (kernel config not exposed)"
fi

# ---------------------------------------------------------------------------
sec "17. writable storage (OTA / 配置 / 媒体落盘的前提)"
echo "\$ mount"; mount 2>&1
echo "\$ df -h /data /opt /mnt /tmp /"; df -h /data /opt /mnt /tmp / 2>&1
for d in /data /opt /mnt /tmp; do
	if [ -d "$d" ]; then
		printf 'write test %-6s: ' "$d"
		if touch "$d/.ivs_survey_test" 2>/dev/null; then
			echo "OK"
			rm -f "$d/.ivs_survey_test" 2>/dev/null
		else
			echo "FAIL (read-only or missing)"
		fi
	else
		printf 'write test %-6s: ' "$d"
		echo "NO SUCH DIRECTORY"
	fi
done

# ---------------------------------------------------------------------------
sec "18. done"
echo "survey finished"

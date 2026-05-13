SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

VC=\
"full-screen | \
1920x1080 | \
1600x900 | \
1280x960 | \
1280x720 | \
1024x768 | \
960x640 | \
960x540 | \
800x600 | \
800x480 | \
640x480 | \
640x340"

MODE=\
"graphic | \
nographic | \
customize1 | \
customize2 | \
customize3"

USAGE="usage $0 [$MODE] [$VC]"

if [ $# != 1 ] ; then
	if [ $# != 2 ] ; then
		echo $USAGE
		exit 1
	fi
fi

if [ $# != 2 ] ; then
	DEFAULT_VC="1280x720"
else
	case "$2" in
	full-screen)
		DEFAULT_VC="$(xrandr -q 2>/dev/zero | awk '/*/{print $1}')"
		FULL_SCREEN=-full-screen
		;;
	*)
		DEFAULT_VC=$2
		;;
	esac
fi

WIDTH="$(echo $DEFAULT_VC | sed 's/\(.*\)x\(.*\)/\1/g')"
HEIGHT="$(echo $DEFAULT_VC | sed 's/\(.*\)x\(.*\)/\2/g')"

case "$1" in
graphic)
	GRAPHIC_PARAM="--display gtk,zoom-to-fit=false --serial vc:$DEFAULT_VC --serial vc:$DEFAULT_VC --serial vc:$DEFAULT_VC --monitor vc:$DEFAULT_VC --parallel none"
	ROWS="$(echo $WIDTH / 8 |bc)"
	COLS="$(echo $HEIGHT / 16 |bc)"
	DEFAULT_V=":vn:$COLS""x""$ROWS:"
	;;
nographic)
	DEFAULT_VN="$(stty size | sed '/ \+/s//x/g')" 
    GRAPHIC_PARAM="-nographic --parallel none"
	DEFAULT_V=":vn:$DEFAULT_VN:"
    ;;
customize1)
	GRAPHIC_PARAM="--display gtk,zoom-to-fit=false --serial vc:$DEFAULT_VC --serial vc:$DEFAULT_VC --serial vc:$DEFAULT_VC --monitor stdio --parallel none"
	ROWS="$(echo $WIDTH / 8 |bc)"
	COLS="$(echo $HEIGHT / 16 |bc)"
	DEFAULT_V=":vn:$COLS""x""$ROWS:"
	;;
customize2)
	GRAPHIC_PARAM="--display gtk,zoom-to-fit=false --serial telnet::3441,server,nowait --serial telnet::3442,server,nowait --serial telnet::3443,server,nowait --monitor stdio --parallel none"
	DEFAULT_V=":vn:24x80:"
	;;
customize3)
	GRAPHIC_PARAM="--display gtk,zoom-to-fit=false --serial telnet::3441,server,nowait --serial telnet::3442,server,nowait --serial telnet::3443,server,nowait --monitor none --parallel none"
	DEFAULT_V=":vn:24x80:"
	;;
--help)
	echo $USAGE
	exit 0
	;;
*)
	echo $USAGE
	exit 1	
	;;
esac
qemu-system-riscv64 \
	-machine virt \
	-m 64M \
	-smp 2 \
	-bios $SHELL_FOLDER/output/opensbi/fw_jump.bin \
	-kernel $SHELL_FOLDER/output/os/kernel \
	-drive file=$SHELL_FOLDER/output/os/fs.img,if=none,format=raw,id=x0 \
	-device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
	$GRAPHIC_PARAM
#!/bin/bash

# --- 基础环境配置 ---
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
CROSS_PREFIX=riscv64-unknown-linux-gnu
OUTPUT=$SHELL_FOLDER/output
export ARCH=riscv  
export CROSS_COMPILE=riscv64-unknown-linux-gnu- 
export PATH=$PATH:/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/


# 创建输出根目录
mkdir -p $OUTPUT
mkdir -p $OUTPUT/bin
image=$OUTPUT/bin/kernel.bin
k210=$OUTPUT/bin/k210.bin

build_opensbi() {
    echo -e "\033[1;4;41;32m Building OpenSBI (Generic Platform) \033[0m"
    
    # 1. 编译 DTB
    cd $SHELL_FOLDER/dts
    mkdir -p $OUTPUT/opensbi
    dtc -I dts -O dtb -o $OUTPUT/opensbi/xv6-qemu.dtb xv6-qemu.dts

    # 2. 编译 Generic 固件
    cd $SHELL_FOLDER/opensbi-0.9
    make clean
    make CROSS_COMPILE=$CROSS_PREFIX- \
         PLATFORM=generic \
         FW_FDT_PATH=$OUTPUT/opensbi/xv6-qemu.dtb \
         FW_TEXT_START=0x80000000 \
         FW_JUMP_ADDR=0x80200000

    # 3. 拷贝生成的文件
    cp build/platform/generic/firmware/fw_jump.bin $OUTPUT/opensbi/fw_jump.bin
    cp build/platform/generic/firmware/fw_jump.elf $OUTPUT/opensbi/fw_jump.elf
}

build_trusted() {
    echo -e "\033[1;4;41;32m Building Trusted Domain \033[0m"
    mkdir -p $OUTPUT/trusted_domain
    cd $SHELL_FOLDER/trusted_domain
    make CROSS_COMPILE=$CROSS_PREFIX- clean
    make CROSS_COMPILE=$CROSS_PREFIX- -j$(nproc)
    cp ./build/trusted_fw.* $OUTPUT/trusted_domain/
}

build_opensbi_k210() {
    echo -e "\033[1;4;41;32m Building OpenSBI (K210 Platform) \033[0m"

    # 1. 编译 K210 DTB（参考，传递内核用）
    cd $SHELL_FOLDER/dts
    mkdir -p $OUTPUT/opensbi
    dtc -I dts -O dtb -o $OUTPUT/opensbi/xv6-k210.dtb xv6-k210.dts

    # 2. 编译 Kendryte K210 平台固件
    #    opensbi-0.9/platform/kendryte/k210/ 提供 K210 专用支持：
    #    - PLL 时钟频率计算（从 sysctl 寄存器读取）
    #    - 硬编码的 UART/PLIC/CLINT 基地址
    #    嵌入式 k210.dts 传递给内核（内核忽略 FDT，使用硬编码地址）
    cd $SHELL_FOLDER/opensbi-0.9
    make clean
    make CROSS_COMPILE=$CROSS_PREFIX- \
         PLATFORM=kendryte/k210

    # 3. 拷贝生成的文件
    cp build/platform/kendryte/k210/firmware/fw_jump.bin $OUTPUT/opensbi/fw_jump.bin
    cp build/platform/kendryte/k210/firmware/fw_jump.elf $OUTPUT/opensbi/fw_jump.elf
}

build_xv6() {
    echo -e "\033[1;4;41;32m Building xv6 kernel (QEMU) \033[0m"
    mkdir -p $OUTPUT/os
    cd $SHELL_FOLDER/os
    make clean
    make platform=qemu -j$(nproc)
    make platform=qemu fs
    cp $SHELL_FOLDER/os/kernel/kernel $OUTPUT/os
    cp $SHELL_FOLDER/os/fs.img $OUTPUT/os
}

build_xv6_k210() {
    echo -e "\033[1;4;41;32m Building xv6 kernel (K210) \033[0m"
    mkdir -p $OUTPUT/os
    cd $SHELL_FOLDER/os
    make clean
    make platform=k210
    cp $SHELL_FOLDER/os/kernel/kernel $OUTPUT/os/kernel
}

build_k210() {
    # build firmware
    build_opensbi_k210
    build_xv6_k210
    
    # package firmware
    OBJCOPY=${CROSS_COMPILE}objcopy
    $OBJCOPY $OUTPUT/os/kernel --strip-all -O binary $image
    cp $OUTPUT/opensbi/fw_jump.bin $k210

    dd if=$image of=$k210 bs=128k seek=1 conv=notrunc
}

boot_k210() {
    # download firmware and boot
    k210_serialport=/dev/ttyUSB0
    # sudo chmod 777 $k210_serialport
	python3 $SHELL_FOLDER/tools/kflash.py -p $k210_serialport -b 115200 -t $k210
}

# --- 逻辑控制 ---
usage() {
    echo "Usage: $0 [module]"
    echo "Modules:"
    echo " sbi, trusted, xv6, k210, all"
    exit 1
}

# 如果没有参数，默认编译所有
MODULE=${1:-all}

case "$MODULE" in
    "sbi")      build_opensbi ;;
    "trusted")  build_trusted ;;
    "xv6")      build_xv6 ;;
    "k210")
        build_k210
        ;;
    "download")
        boot_k210
        ;;
    "all")
        build_opensbi
        build_trusted
        build_xv6
        ;;
    *)
        usage
        ;;
esac

echo ">>> Done: $MODULE"
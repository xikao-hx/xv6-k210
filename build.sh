#!/bin/bash

# --- 基础环境配置 ---
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
CROSS_PREFIX=riscv64-unknown-linux-gnu
OUTPUT=$SHELL_FOLDER/output

# 创建输出根目录
mkdir -p $OUTPUT

build_opensbi() {
    echo -e "\033[1;4;41;32m Building OpenSBI (Generic Platform) \033[0m"
    
    # 1. 编译 DTB
    cd $SHELL_FOLDER/dts
    mkdir -p $OUTPUT/opensbi
    dtc -I dts -O dtb -o $OUTPUT/opensbi/xv6-k210.dtb xv6-k210.dts

    # 2. 编译 Generic 固件
    cd $SHELL_FOLDER/opensbi-0.9
    make clean
    make CROSS_COMPILE=$CROSS_PREFIX- \
         PLATFORM=generic \
         FW_FDT_PATH=$OUTPUT/opensbi/xv6-k210.dtb \
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

    # 1. 编译 K210 DTB
    cd $SHELL_FOLDER/dts
    mkdir -p $OUTPUT/opensbi
    dtc -I dts -O dtb -o $OUTPUT/opensbi/k210.dtb k210.dts

    # 2. 编译 Generic 固件（FW_JUMP_ADDR=0x80020000 适配 K210 内存布局）
    cd $SHELL_FOLDER/opensbi-0.9
    make clean
    make CROSS_COMPILE=$CROSS_PREFIX- \
         PLATFORM=generic \
         FW_FDT_PATH=$OUTPUT/opensbi/k210.dtb \
         FW_TEXT_START=0x80000000 \
         FW_JUMP_ADDR=0x80020000

    # 3. 拷贝生成的文件
    cp build/platform/generic/firmware/fw_jump.bin $OUTPUT/opensbi/fw_jump-k210.bin
    cp build/platform/generic/firmware/fw_jump.elf $OUTPUT/opensbi/fw_jump-k210.elf
}

build_xv6() {
    echo -e "\033[1;4;41;32m Building xv6 kernel (QEMU) \033[0m"
    mkdir -p $OUTPUT/os
    cd $SHELL_FOLDER/os
    make clean
    make qemu
    cp $SHELL_FOLDER/os/kernel/kernel $OUTPUT/os
    cp $SHELL_FOLDER/os/fs.img $OUTPUT/os
}

build_xv6_k210() {
    echo -e "\033[1;4;41;32m Building xv6 kernel (K210) \033[0m"
    mkdir -p $OUTPUT/os
    cd $SHELL_FOLDER/os
    make clean
    make platform=k210
    cp $SHELL_FOLDER/os/kernel/kernel $OUTPUT/os/kernel-k210
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
    "-sbi")      build_opensbi ;;
    "-trusted")  build_trusted ;;
    "-xv6")      build_xv6 ;;
    "-k210")
        build_opensbi_k210
        build_xv6_k210
        ;;
    "-all")
        build_opensbi
        build_trusted
        build_xv6
        ;;
    *)
        usage
        ;;
esac

echo ">>> Done: $MODULE"
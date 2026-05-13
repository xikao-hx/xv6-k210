#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// entry.S 直接跳转到 main，不需要 M 模式启动逻辑
// 以下内容全部删除：M 模式寄存器、timerinit、mret 等

// 仅保留栈内存（与 entry.S 对应，避免链接错误）
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// 空实现 start 函数，避免链接报错（实际不会执行）
void start() {
    // S 模式下从 entry.S 直接进入 main，无需任何 M 模式逻辑
}

// 以下 M 模式定时器相关全部删除
// void timerinit() { ... }
// uint64 mscratch0[...];
// extern void timervec();
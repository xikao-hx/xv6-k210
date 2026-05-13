// symtab.h
#ifndef SYMTAB_H
#define SYMTAB_H

// 只包含必须的类型定义头文件
#include "types.h"  

// 符号表项结构
struct symtab_entry {
  uint64 addr;       // 函数起始地址
  char *name;        // 函数名
  int line;          // 行号（简化版）
};

// 全局符号表和长度的外部声明
extern struct symtab_entry symtab[];
extern int symtab_len;

// 查找地址对应的符号
struct symtab_entry* lookup_sym(uint64 addr);

#endif // SYMTAB_H
#!/bin/bash
echo "// Auto-generated symbol table" > kernel/symtab.c
echo "#include \"symtab.h\"" >> kernel/symtab.c
echo "" >> kernel/symtab.c
echo "struct symtab_entry symtab[] = {" >> kernel/symtab.c

# 从已编译的内核中提取符号（如果存在）
if [ -f kernel/kernel ]; then
    riscv64-unknown-elf-nm -n kernel/kernel | grep ' T ' | \
    awk '{
        # 提取地址和符号名
        addr = $1;
        name = $3;
        # 过滤掉局部标签（以.开头）
        if (name !~ /^\./) {
            printf "  {0x%s, \"%s\", 0},\n", addr, name
        }
    }' | head -100 >> kernel/symtab.c
else
    # 后备：基本符号表
    echo "  {0x80000000, \"_entry\", 0}," >> kernel/symtab.c
    echo "  {0x80000086, \"start\", 0}," >> kernel/symtab.c
    echo "  {0x800000b2, \"main\", 0}," >> kernel/symtab.c
    echo "  {0x80000544, \"panic\", 0}," >> kernel/symtab.c
    echo "  {0x80000596, \"printf\", 0}," >> kernel/symtab.c
    echo "  {0x80000860, \"backtrace\", 0}," >> kernel/symtab.c
    echo "  {0x80000c54, \"kfree\", 0}," >> kernel/symtab.c
    echo "  {0x80000e30, \"kalloc\", 0}," >> kernel/symtab.c
    echo "  {0x800017fc, \"uvmunmap\", 0}," >> kernel/symtab.c
    echo "  {0x80002982, \"exit\", 0}," >> kernel/symtab.c
    echo "  {0x800039de, \"sys_exit\", 0}," >> kernel/symtab.c
    echo "  {0x80003932, \"syscall\", 0}," >> kernel/symtab.c
    echo "  {0x80003122, \"usertrap\", 0}," >> kernel/symtab.c
fi

echo "};" >> kernel/symtab.c
echo "" >> kernel/symtab.c
echo "int symtab_len = sizeof(symtab) / sizeof(struct symtab_entry);" >> kernel/symtab.c

echo "Symbol table generated successfully."
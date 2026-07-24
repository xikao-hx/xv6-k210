OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x80020000;

    .text : {
        *(.text .text.*)
        . = ALIGN(0x1000);
        _trampoline = .;
        *(trampsec)
        . = ALIGN(0x1000);
        ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
        _sigtramp = .;
        *(sigtrampsec)
        . = ALIGN(0x1000);
        ASSERT(. - _sigtramp == 0x1000, "error: sigtramp larger than one page");
        PROVIDE(etext = .);
    }

    .rodata : {
        . = ALIGN(16);
        *(.srodata .srodata.*)
        . = ALIGN(16);
        *(.rodata .rodata.*)
    }

    .data : {
        . = ALIGN(16);
        *(.sdata .sdata.*)
        . = ALIGN(16);
        *(.data .data.*)
    }

    .bss : {
        . = ALIGN(16);
        PROVIDE(__bss_start = .);
        *(.bss.stack)
        . = ALIGN(16);
        *(.sbss .sbss.*)
        . = ALIGN(16);
        *(.bss .bss.*)
        . = ALIGN(16);
        PROVIDE(__bss_end = .);
    }

    PROVIDE(end = .);
}

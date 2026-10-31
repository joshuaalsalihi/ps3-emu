/* Linker script for the example programs: a minimal PS3-style memory layout.
   Code starts at 0x10000, like PS3 executables; data starts on the next 64 KB boundary. */

ENTRY(_start_descriptor)

SECTIONS {
    . = 0x10000;
    .text : { *(.text*) }

    . = ALIGN(0x10000);
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .got : { *(.got) *(.toc) }
    .bss : { *(.bss*) *(COMMON) }
}

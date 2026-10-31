# Startup code for the C examples: call main, then exit with its return value.
#
# The C code is compiled for the ELFv2 ABI, which has no function descriptors, because lld does
# not link calls through the older ELFv1 descriptors. Only the entry point uses a PS3-style
# descriptor. The emulator's loader sets r1 (stack) and r2 (TOC) from it before we start.

    .text
_start:
    bl      main
    nop
    li      11, 3                       # sys_process_exit(status in r3)
    sc

    .data
    .p2align 3
    .globl _start_descriptor
_start_descriptor:
    .long   _start, .TOC.

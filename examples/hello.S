# The smallest useful program: print a line with sys_tty_write, then exit.
#
# LV2 syscalls take their number in r11 and arguments in r3-r10, and return a result in r3.

    .text
_start:
    li      3, 0                        # channel 0
    lis     4, message@ha               # buffer address (high half, adjusted for the low half)
    addi    4, 4, message@l             #                (low half)
    li      5, message_end - message    # length
    li      6, 0                        # where to store the byte count: nowhere
    li      11, 403                     # sys_tty_write
    sc

    li      3, 0                        # exit status
    li      11, 3                       # sys_process_exit
    sc

    .data
    .p2align 3
# The ELF entry point is a function descriptor: code address, then TOC pointer (unused here).
    .globl _start_descriptor
_start_descriptor:
    .long   _start, 0

message:
    .ascii  "Hello from the PPU!\n"
message_end:

/* Android 3.10.0 SMP PREEMPT module ABI from the shipped mali.ko:
 * .gnu.linkonce.this_module size 0x170, name +0xc, init +0xbc, exit +0x168.
 * No persistent resources: init always fails after the read, so exit is NULL.
 * The builder verifies this layout against the original module before linking.
 */
.section .gnu.linkonce.this_module,"aw",%progbits
.balign 4
.global __this_module
__this_module:
    .space 12
    .asciz "vop_colour_read"
    .org __this_module + 0xbc
    .word init_module
    .org __this_module + 0x170
.size __this_module, . - __this_module
.section .modinfo,"a",%progbits
    .asciz "license=GPL"
    .asciz "vermagic=3.10.0 SMP preempt mod_unload ARMv7 p2v8 "
    .asciz "description=Temporary read-only VOP colour snapshot; init self-unloads"
.section .note.GNU-stack,"",%progbits

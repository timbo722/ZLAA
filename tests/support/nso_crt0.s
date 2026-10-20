// Minimal Switch-style module start, modelled on libnx's switch_crt0.s.
// The word at offset 4 of .text is the offset to the MOD0 header, which tells
// the loader where the dynamic section, .bss and .eh_frame_hdr are.
    .section .crt0, "ax"
    .global _start
_start:
    b       .                       // entry point (never executed in tests)
    .word   __nx_mod0 - _start

    // linker-script symbols must be hidden so they are not looked up dynamically
    .hidden __bss_start__, __bss_end__, __eh_frame_hdr_start, __eh_frame_hdr_end

    .section .rodata.mod0, "a"
    .align  2
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC             - __nx_mod0
    .word   __bss_start__        - __nx_mod0
    .word   __bss_end__          - __nx_mod0
    .word   __eh_frame_hdr_start - __nx_mod0
    .word   __eh_frame_hdr_end   - __nx_mod0
    .word   0                    // module object offset (unused)

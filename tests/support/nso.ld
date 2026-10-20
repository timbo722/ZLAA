/* Linker script for test NSOs, simplified from libnx's switch.ld.
 * NSO needs three page-aligned segments in this order: text (rx), rodata (r), data (rw). */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

PHDRS {
    text    PT_LOAD FLAGS(5);
    rodata  PT_LOAD FLAGS(4);
    data    PT_LOAD FLAGS(6);
    dyn     PT_DYNAMIC;
}

SECTIONS {
    . = 0;
    .text : {
        KEEP(*(.crt0))
        *(.text .text.*)
        *(.plt .plt.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        KEEP(*(.rodata.mod0))
        *(.rodata .rodata.*)
    } :rodata
    .dynsym   : { *(.dynsym) } :rodata
    .dynstr   : { *(.dynstr) } :rodata
    .hash     : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .rela.dyn : { *(.rela.dyn) *(.rela.*) } :rodata
    .note.gnu.build-id : { *(.note.gnu.build-id) } :rodata
    .eh_frame_hdr : {
        __eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __eh_frame_hdr_end = .;
    } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) } :rodata

    . = ALIGN(0x1000);
    .data       : { *(.data .data.*) } :data
    .init_array : { KEEP(*(.init_array .init_array.*)) } :data
    .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
    .dynamic    : { *(.dynamic) } :data :dyn
    .got        : { *(.got) } :data
    .got.plt    : { *(.got.plt) } :data
    __bss_start__ = .;
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    __bss_end__ = .;

    /DISCARD/ : { *(.comment) *(.note.GNU-stack) }
}

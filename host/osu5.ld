/*
 * Link layout for the eboot. PS5_Vulkan's tooling/native/ps5-pie.ld and
 * tooling/psbc/ps5-pie-unwind.ld (GPL-3.0-or-later, BlackBearReloaded and
 * Mihawk-99) with two additions for the .NET runtime linked in alongside RADV:
 *  - a PT_TLS header, without which lld refuses thread-locals outright (RADV has
 *    none; the runtime is full of them). ps5-native-tool still builds its own
 *    PT_TLS from the section flags, this is only for the intermediate ELF.
 *  - the boundary names compat's dl_iterate_phdr reports for code and unwind tables.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    text PT_LOAD FLAGS (0x5);
    ro PT_LOAD FLAGS (0x4);
    relro PT_LOAD FLAGS (0x6);
    data PT_LOAD FLAGS (0x6);
    dynamic PT_DYNAMIC FLAGS (0x6);
    tls PT_TLS FLAGS (0x4);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text .text.*)
        *(.plt .plt.*)
    } : text

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) {
        KEEP(*(.eh_frame_hdr))
    } : ro
    .eh_frame : { KEEP(*(.eh_frame)) } : ro
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } : ro
    .rodata : { *(.rodata .rodata.*) } : ro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } : relro
    .got : { *(.got) } : relro
    .got.plt : { *(.got.plt) } : relro
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } : relro
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } : relro
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } : relro

    /* Kept only long enough for the converter to read and rewrite them. */
    .dynsym : { *(.dynsym) } : relro
    .gnu.hash : { *(.gnu.hash) } : relro
    .dynstr : { *(.dynstr) } : relro
    .rela.dyn : { *(.rela.dyn) } : relro
    .rela.plt : { *(.rela.plt) } : relro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } : data : dynamic
    .data : { *(.data .data.*) } : data
    /* the runtime's module table: writable, so it can't sit in RELRO where lld
     * would otherwise drop it, and it is only found through __start/__stop */
    __modules : { KEEP(*(__modules)) } : data
    .tdata : { *(.tdata .tdata.*) } : data : tls
    .tbss : { *(.tbss .tbss.*) } : data : tls
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } : data
}

PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));

PROVIDE(__executable_start = ADDR(.text));
PROVIDE(_etext = ADDR(.text) + SIZEOF(.text));
PROVIDE(__GNU_EH_FRAME_HDR = ADDR(.eh_frame_hdr));
PROVIDE(__GNU_EH_FRAME_HDR_END = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));

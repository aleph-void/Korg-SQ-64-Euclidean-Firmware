/* Euclidean firmware memory layout.
 * Application flash is the 64 KiB window installed from VSB payload 0x00000.
 * RAM: 16 KiB for .data/.bss, a 224 KiB update receive buffer, 16 KiB stack.
 */
ENTRY(reset_handler)

MEMORY {
  FLASH (rx)  : ORIGIN = 0x00200000, LENGTH = 64K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_stack_top = 0x20040000;
_update_buffer = 0x20004000;   /* 0x38000 bytes, used only in update mode */

SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  .data : {
    . = ALIGN(4);
    _data_start = .;
    *(.data*)
    . = ALIGN(4);
    _data_end = .;
  } > RAM AT > FLASH
  _data_load = LOADADDR(.data);
  .bss (NOLOAD) : {
    . = ALIGN(4);
    _bss_start = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _bss_end = .;
  } > RAM
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) }
}

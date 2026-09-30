/* META memory map: application flash after the 16 KiB loader, 32 KiB RAM.
 * The flash is twice the META's so instrumented builds (hashes, checks) of
 * projects at the limit still link; PlatformIO checks the real size. */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x4000, LENGTH = 0x7C000
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 0x8000
  SDROM (rw) : ORIGIN = 0x10000000, LENGTH = 0x400000 /* stand-in SD card: ROM, save file at 0x10380000 */
}
ENTRY(Reset_Handler)
SECTIONS
{
  /* Whole ROM of a streaming project (host/*_romfull.c), outside the
   * flash: the firmware would read it from the SD card. */
  .sdrom : { KEEP(*(.rodata.gbl_rom_full)) } > SDROM
  .text : {
    KEEP(*(.isr_vector))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH
  _sidata = LOADADDR(.data);
  .data : {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH
  .bss (NOLOAD) : {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}

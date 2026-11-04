/* STM32F429IG: 1 MiB flash, 64 KiB CCM, 192 KiB SRAM, 32 MiB external SDRAM.
 *
 * CCM is reachable by the CPU only (no DMA), so it holds .data, .bss and the
 * stack: CPU accesses there never compete with the sampling DMA on the bus
 * matrix. Anything a DMA stream touches goes into the SRAM / SDRAM sections:
 *   .sram1  (SRAM1+SRAM2) LCD frame buffer
 *   .sram3  (SRAM3)       on-chip sample ring
 *   .sdram  (FMC bank 1)  large sample ring; usable only after sdram_init()
 * None of these are zeroed at startup.
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
  CCM   (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
  SRAM1 (rw)  : ORIGIN = 0x20000000, LENGTH = 128K  /* SRAM1 112K + SRAM2 16K */
  SRAM3 (rw)  : ORIGIN = 0x20020000, LENGTH = 64K
  SDRAM (rw)  : ORIGIN = 0xC0000000, LENGTH = 32M
}

_estack = ORIGIN(CCM) + LENGTH(CCM);
_Min_Stack_Size = 0x2000;   /* checked at link time, see ._stack_check */

SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*)
    *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array    : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array    : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data) *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >CCM AT> FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >CCM

  /* Make sure the stack still fits after everything else */
  ._stack_check (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Stack_Size;
  } >CCM

  .sram1 (NOLOAD) : { . = ALIGN(4); *(.sram1) *(.sram1*) . = ALIGN(4); } >SRAM1
  .sram3 (NOLOAD) : { . = ALIGN(1024); *(.sram3) *(.sram3*) } >SRAM3
  .sdram (NOLOAD) : { . = ALIGN(1024); *(.sdram) *(.sdram*) } >SDRAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

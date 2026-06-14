/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user stack */
_estack = 0x20050000;    /* End of 320 KB SRAM1 */

/* Define memory regions */
MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 32K   /* Sectors 0 & 1 only! */
  RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 320K  /* SRAM1 */
}

/* Define output sections */
SECTIONS
{
/* Interrupt Vector Table goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Keeps the vector table from being optimized away */
    . = ALIGN(4);
  } >FLASH

  /* .text: The actual executable program code */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* All code */
    *(.text*)          /* All sub-code sections */
    *(.glue_7)         /* Glue code for ARM/Thumb interworking */
    *(.glue_7t)
    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))

    . = ALIGN(4);
    _etext = .;        /* Define a global symbol at the end of code */
  } >FLASH

  /* .rodata: Read-only data (like constant variables and strings) */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* C++ Global Constructors Section */
  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
    KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

/* ====================================================================== */
  /* NEW: .RamFunc Section configuration                                   */
  /* ====================================================================== */
  _siramfunc = LOADADDR(.RamFunc);

  .RamFunc :
  {
    . = ALIGN(4);
    _sramfunc = .;        /* Symbol for startup code: start destination in RAM */
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _eramfunc = .;        /* Symbol for startup code: end destination in RAM */
  } >RAM AT> FLASH

  /* ====================================================================== */
  
  /* Used by the startup assembly code to initialize data from FLASH to RAM */
  _sidata = LOADADDR(.data);

  /* .data: Initialized global/static variables. Goes into RAM, loaded from FLASH */
  .data : 
  {
    . = ALIGN(4);
    _sdata = .;        /* Create a symbol at the start of data */
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;        /* Create a symbol at the end of data */
  } >RAM AT> FLASH

  /* .bss: Uninitialized global/static variables. Allocated in RAM and zeroed on boot */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;         /* Start of BSS symbol */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;         /* End of BSS symbol */
    __bss_end__ = _ebss;
  } >RAM

  /* Dynamic memory allocation (Heap) tracking symbols */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + 0x200;    /* Reserves 512 bytes for a basic bare-metal Heap if needed */
    . = . + 0x400;    /* Reserves 1KB min for Stack safety tracking */
    . = ALIGN(8);
  } >RAM

  /* Remove information from the compiler libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user stack */
_estack = 0x20050000;    /* End of 320 KB SRAM1 */

/* Define memory regions tailored for the Main Application */
MEMORY
{
  FLASH (rx)      : ORIGIN = 0x08008000, LENGTH = 992K  (1024 - 2x16)
  RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 320K   
}

SECTIONS
{
  /* 1. Vector table goes exactly at the origin point (0x08008000) */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) 
    . = ALIGN(4);     
  } >FLASH

  /* 2. C++ Code and Executable Logic */
  .text :
  {
    . = ALIGN(4);
    *(.text)           
    *(.text*)          
    *(.glue_7)         
    *(.glue_7t)
    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
  } >FLASH

  /* 3. Read-only constants and strings */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* C++ Global Constructors Sections */
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

  /* Used by startup assembly to initialize data from FLASH to RAM */
  _sidata = LOADADDR(.data);

  /* 4. Initialized variables relocated to RAM at runtime */
  .data : 
  { 
    . = ALIGN(4);
    _sdata = .;        
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;        
  } >RAM AT> FLASH

  /* 5. Uninitialized variables zeroed out at runtime */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;         
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;         
    __bss_end__ = _ebss;
  } >RAM

  /* 6. Dynamic memory allocation (Heap) and minimal stack spacing rules */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + 0x400;    
    . = . + 0x800;    
    . = ALIGN(8);
  } >RAM

  /* Remove information from standard libraries to keep binary light */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
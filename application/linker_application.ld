/* 
Physical Address    Layout Block

0x08000000		   [ Bootloader code - sectors 0 & 1 ]				2x 16KB
0x08008000         [ .isr_vector (Interrupt Vector Table) ]    		ALIGN(512)-> Force the next section to align to 512 bytes (0x08008200)
0x08008200         [ .fw_header  (application_header struct) ]  	struct FirmwareHeader with 4 bytes of uint32_t = 16 bytes
                    ├── magic        = 0x5A5A5A5A
                    ├── payload_size = [Calculated value of your C++ code]
                    └── expected_crc = 0xFFFFFFFF
0x08008210         [ .text       (Your actual compiled functions start here) ]            
                 
By tracking where the code starts (_s_app_code) and where the final piece of flash data ends (_e_app_code), 
the linker can calculate the total size for us perfectly using:	_app_size = _e_app_code - _s_app_code
*/



/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user stack */
_estack = 0x20050000;    /* End of 320 KB SRAM1 */

/* Define memory regions tailored for the Main Application */
MEMORY
{
  /* Starts exactly at APP_START_ADDR (1KB into Sector 2) */
  FLASH (rx)      : ORIGIN = 0x08008000, LENGTH = 2016K  
  RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 320K   /* SRAM1 */
}

/* Define output sections */
SECTIONS
{
  /* 1. Vector table goes exactly at the origin point (0x08008000) */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) 
    . = ALIGN(512);     /* Force the next section to align to 512 bytes (0x08008200) */
  } >FLASH

  /* 2. THE FIRMWARE HEADER: Fixed location right after vectors (Exactly 16 bytes fixed size) */
  .fw_header :
  {
    . = ALIGN(4);
    KEEP(*(.fw_header))
    . = ALIGN(4);
  } >FLASH
  
  /* 3. Your exact optimization: Size tracking starts directly with the C++ code */
  .text :
  {
    . = ALIGN(4);
    _s_app_code = .;    /* Mark the absolute start of your actual C++ code */
    *(.text)           
    *(.text*)          
    *(.glue_7)         
    *(.glue_7t)
    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))

    . = ALIGN(4);
    _etext = .;        
  } >FLASH

  /* .rodata: Read-only constants and strings */
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

  /* Used by startup assembly to initialize data from FLASH to RAM */
  _sidata = LOADADDR(.data);

 /* .data: Initialized variables relocated to RAM at runtime */
  .data : 
  { 
	. = ALIGN(4);
    _sdata = .;        
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;        
  } >RAM AT> FLASH

  /* Place the calculations here, completely outside the section blocks */
  _e_app_code = _sidata + SIZEOF(.data);
  _app_size   = _e_app_code - _s_app_code;


  /* .bss: Uninitialized variables zeroed out at runtime */
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

  /* Dynamic memory allocation (Heap) and minimal stack spacing rules */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + 0x400;    /* Reserves 1KB for application dynamic memory */
    . = . + 0x800;    /* Reserves 2KB min for application stack tracking */
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
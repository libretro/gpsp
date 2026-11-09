/* The MultiBoot block, at a fixed place in the free end of the built-in
 * BIOS (its code ends at 0x243C). Code and constants only: it runs from
 * ROM. */
ENTRY(swi_MultiBoot)
SECTIONS
{
  . = 0x2440;
  .text : { *(.text.entry) *(.text.entry2) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
ASSERT(SIZEOF(.data) == 0, "the BIOS block cannot have writable data")
ASSERT(. <= 0x4000, "the BIOS block does not fit")

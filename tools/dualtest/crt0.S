@ Minimal GBA ROM start: the header is left blank (gpSP boots it anyway).
  .section .text.start, "ax"
  .global _start
  .arm
_start:
  b real_start
  .fill 0xBC, 1, 0
real_start:
  mov r0, #0x1F          @ System mode
  msr cpsr_c, r0
  ldr sp, =0x03007F00
  bl main
1:
  b 1b

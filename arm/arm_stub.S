
#include "../gpsp_config.h"

#define defsymbl(symbol) \
.type symbol, %function ;\
.global symbol ;         \
.global _##symbol ;      \
symbol:                  \
_##symbol:

.text
.align 2

#define REG_R0            (0 * 4)
#define REG_R1            (1 * 4)
#define REG_R2            (2 * 4)
#define REG_R3            (3 * 4)
#define REG_R4            (4 * 4)
#define REG_R5            (5 * 4)
#define REG_R6            (6 * 4)
#define REG_R7            (7 * 4)
#define REG_R8            (8 * 4)
#define REG_R9            (9 * 4)
#define REG_R10           (10 * 4)
#define REG_R11           (11 * 4)
#define REG_R12           (12 * 4)
#define REG_R13           (13 * 4)
#define REG_R14           (14 * 4)
#define REG_SP            (13 * 4)
#define REG_LR            (14 * 4)
#define REG_PC            (15 * 4)

#define REG_N_FLAG        (16 * 4)
#define REG_Z_FLAG        (17 * 4)
#define REG_C_FLAG        (18 * 4)
#define REG_V_FLAG        (19 * 4)
#define REG_CPSR          (20 * 4)

#define REG_SAVE          (21 * 4)
#define REG_SAVE2         (22 * 4)
#define REG_SAVE3         (23 * 4)

#define CPU_MODE          (29 * 4)
#define CPU_HALT_STATE    (30 * 4)
#define CHANGED_PC_STATUS (31 * 4)
#define COMPLETED_FRAME   (32 * 4)
#define OAM_UPDATED       (33 * 4)

#define reg_a0            r0
#define reg_a1            r1
#define reg_a2            r2

#define reg_s0            r9
#define reg_base          r11
#define reg_flags         r9

#define reg_cycles        r12

#define reg_x0            r3
#define reg_x1            r4
#define reg_x2            r5
#define reg_x3            r6
#define reg_x4            r7
#define reg_x5            r8


#define MODE_SUPERVISOR   3


#ifdef __ARM_ARCH_7A__
  #define extract_u16(rd, rs) \
    uxth rd, rs
#else
  #define extract_u16(rd, rs) \
    bic  rd, rs, #0xff000000 ;\
    bic  rd, rd, #0x00ff0000
#endif

@ Will load the register set from memory into the appropriate cached registers.
@ See arm_emit.h for listing explanation.

#define load_registers_arm()                                                 ;\
  ldr reg_x0, [reg_base, #REG_R0]                                            ;\
  ldr reg_x1, [reg_base, #REG_R1]                                            ;\
  ldr reg_x2, [reg_base, #REG_R6]                                            ;\
  ldr reg_x3, [reg_base, #REG_R9]                                            ;\
  ldr reg_x4, [reg_base, #REG_R12]                                           ;\
  ldr reg_x5, [reg_base, #REG_R14]                                           ;\

#define load_registers_thumb()                                               ;\
  ldr reg_x0, [reg_base, #REG_R0]                                            ;\
  ldr reg_x1, [reg_base, #REG_R1]                                            ;\
  ldr reg_x2, [reg_base, #REG_R2]                                            ;\
  ldr reg_x3, [reg_base, #REG_R3]                                            ;\
  ldr reg_x4, [reg_base, #REG_R4]                                            ;\
  ldr reg_x5, [reg_base, #REG_R5]                                            ;\


@ Will store the register set from cached registers back to memory.

#define store_registers_arm()                                                ;\
  str reg_x0, [reg_base, #REG_R0]                                            ;\
  str reg_x1, [reg_base, #REG_R1]                                            ;\
  str reg_x2, [reg_base, #REG_R6]                                            ;\
  str reg_x3, [reg_base, #REG_R9]                                            ;\
  str reg_x4, [reg_base, #REG_R12]                                           ;\
  str reg_x5, [reg_base, #REG_R14]                                           ;\

#define store_registers_thumb()                                              ;\
  str reg_x0, [reg_base, #REG_R0]                                            ;\
  str reg_x1, [reg_base, #REG_R1]                                            ;\
  str reg_x2, [reg_base, #REG_R2]                                            ;\
  str reg_x3, [reg_base, #REG_R3]                                            ;\
  str reg_x4, [reg_base, #REG_R4]                                            ;\
  str reg_x5, [reg_base, #REG_R5]                                            ;\


@ Returns an updated persistent cpsr with the cached flags register.
@ Uses reg as a temporary register and returns the CPSR here.

#define collapse_flags_no_update(reg)                                        ;\
  ldr reg, [reg_base, #REG_CPSR]          /* reg = cpsr                    */;\
  bic reg, reg, #0xF0000000               /* clear ALU flags in cpsr       */;\
  and reg_flags, reg_flags, #0xF0000000   /* clear non-ALU flags           */;\
  orr reg, reg, reg_flags                 /* update cpsr with ALU flags    */;\

@ Updates cpsr using the above macro.

#define collapse_flags(reg)                                                  ;\
  collapse_flags_no_update(reg)                                              ;\
  str reg, [reg_base, #REG_CPSR]                                             ;\

@ Loads the saved flags register from the persistent cpsr.

#define extract_flags()                                                      ;\
  ldr reg_flags, [reg_base, #REG_CPSR]                                       ;\
  msr cpsr_f, reg_flags                                                      ;\


#define save_flags()                                                         ;\
  mrs reg_flags, cpsr                                                        ;\

#define restore_flags()                                                      ;\
  msr cpsr_f, reg_flags                                                      ;\

@ Align the stack to 64 bits (ABIs that don't require it, still recommend so)
#define call_c_saved_regs r2, r3, r12, lr

@ Calls a C function - reloads the stack pointer and saves all caller save
@ registers which are important to the dynarec.

#define call_c_function(function)                                            ;\
  stmdb sp!, { call_c_saved_regs }                                           ;\
  bl function                                                                ;\
  ldmia sp!, { call_c_saved_regs }                                           ;\

@ Jumps to PC (ARM or Thumb modes)
@ This is really two functions/routines in one
@ r0 contains the PC

.align 2
#define execute_pc_builder(mode, align)                                      ;\
defsymbl(arm_indirect_branch_##mode)                                         ;\
  save_flags()                                                               ;\
execute_pc_##mode:                                                           ;\
  bic r0, r0, #(align)                    /* Align PC                      */;\
  mov r1, r0, lsr #24                     /* Get region                    */;\
  ldr pc, [pc, r1, lsl #2]                                                   ;\
  nop                                                                        ;\
  .long 3f                                /* 0 BIOS (like ROM)             */;\
  .long 3f                                /* 1 Bad region                  */;\
  .long 1f                                /* 2 EWRAM                       */;\
  .long 2f                                /* 3 IWRAM                       */;\
  .long 3f                                /* 4 Not supported               */;\
  .long 3f                                /* 5 Not supported               */;\
  .long 3f                                /* 6 Not supported               */;\
  .long 3f                                /* 7 Not supported               */;\
  .long 3f                                /* 8 ROM                         */;\
  .long 3f                                /* 9 ROM                         */;\
  .long 3f                                /* A ROM                         */;\
  .long 3f                                /* B ROM                         */;\
  .long 3f                                /* C ROM                         */;\
  .long 3f                                /* D ROM                         */;\
  .long 3f                                /* E ROM                         */;\
  .long 3f                                /* F Bad region                  */;\
                                                                             ;\
3:                                                                           ;\
  call_c_function(block_lookup_address_##mode)                               ;\
  restore_flags()                                                            ;\
  bx r0                                                                      ;\
1:                                                                           ;\
  ldr r1, =(ewram+0x40000)                /* Load base addr                */;\
  mov r2, r0, lsl #14                     /* addr &= 0x3ffff               */;\
  mov r2, r2, lsr #14                                                        ;\
  ldrh r2, [r1, r2]                       /* Load half word there          */;\
  ldr r1, =(ram_block_ptrs)                                                  ;\
  ldr r1, [r1, r2, lsl #2]                /* Pointer to the cache          */;\
  cmp r1, #0                              /* NULL means not translated     */;\
  beq 3b                                  /* Need to translate             */;\
  restore_flags()                                                            ;\
  bx r1                                                                      ;\
2:                                                                           ;\
  ldr r1, =(iwram)                        /* Load base addr                */;\
  mov r2, r0, lsl #17                     /* addr &= 0x7fff                */;\
  mov r2, r2, lsr #17                                                        ;\
  ldrh r2, [r1, r2]                       /* Load half word there          */;\
  ldr r1, =(ram_block_ptrs)                                                  ;\
  ldr r1, [r1, r2, lsl #2]                /* Pointer to the cache          */;\
  cmp r1, #0                              /* NULL means not translated     */;\
  beq 3b                                  /* Need to translate             */;\
  restore_flags()                                                            ;\
  bx r1                                                                      ;\
.size arm_indirect_branch_##mode, .-arm_indirect_branch_##mode

execute_pc_builder(arm, 0x3)
execute_pc_builder(thumb, 0x1)

@ Resumes execution from saved PC, in any mode

execute_pc:
  ldr r0, [reg_base, #REG_PC]             @ load new PC
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = flags
  tst r1, #0x20                           @ see if Thumb bit is set
  bne 2f

  load_registers_arm()
  b execute_pc_arm

2:
  load_registers_thumb()
  b execute_pc_thumb

@ Update the GBA hardware (video, sound, input, etc)

@ Input:
@ r0: current PC

#define return_straight()                                                    ;\
  bx lr                                                                      ;\

#define return_add()                                                         ;\
  add pc, lr, #4                                                             ;\

#define load_pc_straight()                                                   ;\
  ldr r0, [lr, #-8]                                                          ;\

#define load_pc_add()                                                        ;\
  ldr r0, [lr]                                                               ;\


#define arm_update_gba_builder(name, mode, return_op)                        ;\
                                                                             ;\
.align 2                                                                     ;\
defsymbl(arm_update_gba_##name)                                              ;\
  load_pc_##return_op()                                                      ;\
  str r0, [reg_base, #REG_PC]             /* write out the PC              */;\
                                                                             ;\
  save_flags()                                                               ;\
  collapse_flags(r0)                      /* update the flags              */;\
                                                                             ;\
  store_registers_##mode()                /* save out registers            */;\
wait_halt_##name:                                                            ;\
  call_c_function(update_gba)             /* update GBA state              */;\
                                                                             ;\
  ldr r1, [reg_base, #COMPLETED_FRAME]    /* return if new frame           */;\
  cmp r1, #0                                                                 ;\
  bne return_to_main                                                         ;\
                                                                             ;\
  ldr r1, [reg_base, #CPU_HALT_STATE]     /* keep iterating if halted      */;\
  cmp r1, #0                                                                 ;\
  bne wait_halt_##name                                                       ;\
                                                                             ;\
  mvn reg_cycles, r0                      /* load new cycle count          */;\
                                                                             ;\
  ldr r0, [reg_base, #CHANGED_PC_STATUS]  /* load PC changed status        */;\
  cmp r0, #0                              /* see if PC has changed         */;\
  bne execute_pc                          /* go jump/translate             */;\
                                                                             ;\
  load_registers_##mode()                 /* reload registers              */;\
  restore_flags()                                                            ;\
  return_##return_op()                    /* continue, no PC change        */;\
.size arm_update_gba_##mode, .-arm_update_gba_##mode

arm_update_gba_builder(arm, arm, straight)
arm_update_gba_builder(thumb, thumb, straight)

arm_update_gba_builder(idle_arm, arm, add)
arm_update_gba_builder(idle_thumb, thumb, add)



@ These are b stubs for performing indirect branches. They are not
@ linked to and don't return, instead they link elsewhere.

@ Input:
@ r0: PC to branch to

.align 2
defsymbl(arm_indirect_branch_dual_arm)
  save_flags()
  tst r0, #0x01                           @ check lower bit
  beq execute_pc_arm                      @ Keep executing ARM code

  bic r0, r0, #0x01                       @ Switch to Thumb mode
  store_registers_arm()                   @ save out ARM registers
  load_registers_thumb()                  @ load in Thumb registers
  ldr r1, [reg_base, #REG_CPSR]           @ load cpsr
  orr r1, r1, #0x20                       @ set Thumb mode
  str r1, [reg_base, #REG_CPSR]           @ store flags
  b execute_pc_thumb                      @ Now execute Thumb
.size arm_indirect_branch_dual_arm, .-arm_indirect_branch_dual_arm

.align 2
defsymbl(arm_indirect_branch_dual_thumb)
  save_flags()
  tst r0, #0x01                           @ check lower bit
  bne execute_pc_thumb                    @ Keep executing Thumb mode

  store_registers_thumb()                 @ save out Thumb registers
  load_registers_arm()                    @ load in ARM registers
  ldr r1, [reg_base, #REG_CPSR]           @ load cpsr
  bic r1, r1, #0x20                       @ clear Thumb mode
  str r1, [reg_base, #REG_CPSR]           @ store flags
  b execute_pc_arm                        @ Now execute ARM
.size arm_indirect_branch_dual_thumb, .-arm_indirect_branch_dual_thumb

@ Update the cpsr.

@ Input:
@ r0: new cpsr value
@ r1: bitmask of which bits in cpsr to update
@ r2: current PC

.align 2
defsymbl(execute_store_cpsr)
  save_flags()
  and reg_flags, r0, r1                   @ reg_flags = new_cpsr & store_mask
  ldr r0, [reg_base, #REG_CPSR]           @ r0 = cpsr
  bic r0, r0, r1                          @ r0 = cpsr & ~store_mask
  orr reg_flags, reg_flags, r0            @ reg_flags = new_cpsr | cpsr

  mov r0, reg_flags                       @ also put new cpsr in r0

  store_registers_arm()                   @ save ARM registers
  ldr r2, [lr]                            @ r2 = pc
  call_c_function(execute_store_cpsr_body)
  load_registers_arm()                    @ restore ARM registers

  cmp r0, #0                              @ check new PC
  beq 1f                                  @ if it's zero, return

  b execute_pc_arm

1:
  restore_flags()
  add pc, lr, #4                          @ return
.size execute_store_cpsr, .-execute_store_cpsr

@ Update the current spsr.

@ Input:
@ r0: new cpsr value
@ r1: bitmask of which bits in spsr to update

.align 2
defsymbl(execute_store_spsr)
  ldr r1, =spsr                           @ r1 = spsr
  ldr r2, [reg_base, #CPU_MODE]           @ r2 = CPU_MODE
  str r0, [r1, r2, lsl #2]                @ spsr[CPU_MODE] = new_spsr
  bx lr
.size execute_store_spsr, .-execute_store_spsr

@ Read the current spsr.

@ Output:
@ r0: spsr

.align 2
defsymbl(execute_read_spsr)
  ldr r0, =spsr                           @ r0 = spsr
  ldr r1, [reg_base, #CPU_MODE]           @ r1 = CPU_MODE
  ldr r0, [r0, r1, lsl #2]                @ r0 = spsr[CPU_MODE]
  bx lr                                   @ return
.size execute_read_spsr, .-execute_read_spsr

@ Restore the cpsr from the mode spsr and mode shift.

@ Input:
@ r0: current pc

.align 2
defsymbl(execute_spsr_restore)
  save_flags()
  ldr r1, =spsr                           @ r1 = spsr
  ldr r2, [reg_base, #CPU_MODE]           @ r2 = cpu_mode
  ldr r1, [r1, r2, lsl #2]                @ r1 = spsr[cpu_mode] (new cpsr)
  str r1, [reg_base, #REG_CPSR]           @ update cpsr
  mov reg_flags, r1                       @ also, update shadow flags

  @ This function call will pass r0 (address) and return it.
  store_registers_arm()                   @ save ARM registers
  call_c_function(execute_spsr_restore_body)

  ldr r1, [reg_base, #REG_CPSR]           @ r1 = cpsr
  tst r1, #0x20                           @ see if Thumb mode is set
  bne 2f                                  @ if so handle it

  load_registers_arm()                    @ restore ARM registers
  b execute_pc_arm

2:
  load_registers_thumb()                  @ load Thumb registers
  b execute_pc_thumb


@ Setup the mode transition work for calling an SWI.

@ Input:
@ r0: current pc

#define execute_swi_builder(mode)                                            ;\
                                                                             ;\
.align 2                                                                     ;\
defsymbl(execute_swi_##mode)                                                 ;\
  save_flags()                                                               ;\
  ldr r1, =reg_mode                       /* r1 = reg_mode                 */;\
  /* reg_mode[MODE_SUPERVISOR][6] = pc                                     */;\
  ldr r0, [lr]                            /* load PC                       */;\
  str r0, [r1, #((MODE_SUPERVISOR * (7 * 4)) + (6 * 4))]                     ;\
  collapse_flags_no_update(r0)            /* r0 = cpsr                     */;\
  ldr r1, =spsr                           /* r1 = spsr                     */;\
  str r0, [r1, #(MODE_SUPERVISOR * 4)]    /* spsr[MODE_SUPERVISOR] = cpsr  */;\
  bic r0, r0, #0x3F                       /* clear mode flag in r0         */;\
  orr r0, r0, #0x13                       /* set to supervisor mode        */;\
  str r0, [reg_base, #REG_CPSR]           /* update cpsr                   */;\
                                                                             ;\
  mov r0, #MODE_SUPERVISOR                                                   ;\
                                                                             ;\
  store_registers_##mode()                /* store regs for mode           */;\
  call_c_function(set_cpu_mode)           /* set the CPU mode to svsr      */;\
  load_registers_arm()                    /* load ARM regs                 */;\
                                                                             ;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\

execute_swi_builder(arm)
execute_swi_builder(thumb)


@ Wrapper for calling SWI functions in C (or can implement some in ASM if
@ desired)

#define execute_swi_function_builder(swi_function, mode)                     ;\
                                                                             ;\
.align 2                                                                     ;\
defsymbl(execute_swi_hle_##swi_function##_##mode)                            ;\
  save_flags()                                                               ;\
  store_registers_##mode()                                                   ;\
  call_c_function(execute_swi_hle_##swi_function##_c)                        ;\
  load_registers_##mode()                                                    ;\
  restore_flags()                                                            ;\
  bx lr                                                                      ;\

execute_swi_function_builder(div, arm)
execute_swi_function_builder(div, thumb)


@ Start program execution. Normally the mode should be Thumb and the
@ PC should be 0x8000000, however if a save state is preloaded this
@ will be different.

@ Input:
@ r0: initial value for cycle counter

@ Uses sp as reg_base; must hold consistently true.

.align 2
defsymbl(execute_arm_translate)

  @ save the registers to be able to return later
  stmdb sp!, { r4, r5, r6, r7, r8, r9, r10, r11, r12, lr }

  ldr reg_base, =reg                      @ init base_reg

  mvn reg_cycles, r0                      @ load cycle counter

  @ Check whether the CPU is sleeping already, we should just wait for IRQs
  ldr r1, [reg_base, #CPU_HALT_STATE]
  cmp r1, #0
  bne alert_loop

  ldr r0, [reg_base, #REG_PC]             @ r0 = current pc
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = flags
  tst r1, #0x20                           @ see if Thumb bit is set

  bne 1f                                  @ if so lookup thumb

  load_registers_arm()                    @ load ARM registers
  call_c_function(block_lookup_address_arm)
  extract_flags()                         @ load flags
  bx r0                                   @ jump to first ARM block

1:
  load_registers_thumb()                  @ load Thumb registers
  call_c_function(block_lookup_address_thumb)
  extract_flags()                         @ load flags
  bx r0                                   @ jump to first Thumb block


@ Epilogue to return to the main thread (whatever called execute_arm_translate)

return_to_main:
  @ restore the saved regs and return
  ldmia sp!, { r4, r5, r6, r7, r8, r9, r10, r11, r12, lr }
  bx lr


#define store_align_8()                                                      ;\
  and r1, r1, #0xff                                                          ;\

#define store_align_16()                                                     ;\
  bic r0, r0, #0x01                                                          ;\
  extract_u16(r1, r1)                                                        ;\

#define store_align_32()                                                     ;\
  bic r0, r0, #0x03                                                          ;\

#define mask_addr_8(nbits)                                                   ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits)            /* high bits are now clear      */;\

#define mask_addr_16(nbits)                                                  ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits + 1)        /* high bits are now clear      */;\
  mov r0, r0, lsl #1                       /* LSB is also zero             */;\

#define mask_addr_32(nbits)                                                  ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits + 2)        /* high bits are now clear      */;\
  mov r0, r0, lsl #2                       /* 2 LSB are also zero          */;\

@ Vram, OAM and palette memories can only be accessed at a 16 bit boundary
#define mask_addr_bus16_32(nbits) mask_addr_32(nbits)
#define mask_addr_bus16_16(nbits) mask_addr_16(nbits)
#define mask_addr_bus16_8(nbits) \
  mask_addr_16(nbits)            \
  extract_u16(r1, r1)


@ Write out to memory.

@ Input:
@ r0: address
@ r1: value
@ r2: current pc
@
@ The instruction at LR is not an inst but a u32 data that contains the PC
@ Used for SMC. That's why return is essentially `pc = lr + 4`

#define execute_store_body(store_type, tblnum)                               ;\
  save_flags()                                                               ;\
  str lr, [reg_base, #REG_SAVE3]          /* save lr                       */;\
                                                                             ;\
  mov lr, r0, lsr #24                     /* lr = region number            */;\
  cmp lr, #15                                                                ;\
  movcs lr, #15                           /* lr = min(lr, 15)              */;\
                                                                             ;\
  add lr, lr, #(16*tblnum + 64)           /* lr += table offset            */;\
  ldr pc, [reg_base, lr, lsl #2]          /* jump to handler               */;\

#define store_fnptr_table(store_type)                                        ;\
ptr_tbl_##store_type:                                                        ;\
  .word ext_store_ignore                  /* 0x00: BIOS, ignore            */;\
  .word ext_store_ignore                  /* 0x01: ignore                  */;\
  .word ext_store_ewram_u##store_type     /* 0x02: ewram                   */;\
  .word ext_store_iwram_u##store_type     /* 0x03: iwram                   */;\
  .word ext_store_u##store_type           /* 0x04: I/O regs                */;\
  .word ext_store_u##store_type           /* 0x05: palette RAM             */;\
  .word ext_store_vram_u##store_type      /* 0x06: vram                    */;\
  .word ext_store_oam_ram_u##store_type   /* 0x07: oam ram                 */;\
  .word ext_store_u##store_type           /* 0x08: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x09: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x0A: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x0B: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x0C: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x0D: EEPROM                  */;\
  .word ext_store_u##store_type           /* 0x0E: backup                  */;\
  .word ext_store_ignore                  /* 0x0F: ignore                  */;\

@ for ignored areas, just return
ext_store_ignore:
  ldr lr, [reg_base, #REG_SAVE3]          @ pop lr off of stack
  restore_flags()
  add pc, lr, #4                          @ return


#define execute_store_builder(store_type, store_op, store_op16, load_op, tn) ;\
                                                                             ;\
.align 2                                                                     ;\
defsymbl(execute_store_u##store_type)                                        ;\
  execute_store_body(store_type, tn)                                         ;\
                                                                             ;\
ext_store_u##store_type:                                                     ;\
  ldr lr, [reg_base, #REG_SAVE3]          /* pop lr off of stack           */;\
  ldr r2, [lr]                            /* load PC                       */;\
  str r2, [reg_base, #REG_PC]             /* write out PC                  */;\
  store_align_##store_type()                                                 ;\
  call_c_function(write_memory##store_type)                                  ;\
  b write_epilogue                        /* handle additional write stuff */;\
                                                                             ;\
ext_store_iwram_u##store_type:                                               ;\
  mask_addr_##store_type(15)              /* Mask to mirror memory (+align)*/;\
  ldr r2, =(iwram+0x8000)                 /* r2 = iwram base               */;\
  store_op r1, [r0, r2]                   /* store data                    */;\
  sub r2, r2, #0x8000                     /* r2 = iwram smc base           */;\
  load_op r1, [r0, r2]                    /* r1 = SMC sentinel             */;\
  cmp r1, #0                              /* see if it's not 0             */;\
  bne 3f                                  /* if so perform smc write       */;\
  ldr lr, [reg_base, #REG_SAVE3]          /* pop lr off of stack           */;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
                                                                             ;\
ext_store_ewram_u##store_type:                                               ;\
  mask_addr_##store_type(18)              /* Mask to mirror memory (+align)*/;\
  ldr r2, =(ewram)                        /* r2 = ewram base               */;\
  store_op r1, [r0, r2]                   /* store data                    */;\
  add r2, r2, #0x40000                    /* r2 = ewram smc base           */;\
  load_op r1, [r0, r2]                    /* r1 = SMC sentinel             */;\
  cmp r1, #0                              /* see if it's not 0             */;\
  bne 3f                                  /* if so perform smc write       */;\
  ldr lr, [reg_base, #REG_SAVE3]          /* pop lr off of stack           */;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
                                                                             ;\
ext_store_vram_u##store_type:                                                ;\
  mask_addr_bus16_##store_type(17)        /* Mask to mirror memory (+align)*/;\
  cmp r0, #0x18000                        /* Check if exceeds 96KB         */;\
  subcs r0, r0, #0x8000                   /* Mirror to the last bank       */;\
  ldr r2, =(vram)                         /* r2 = vram base                */;\
  store_op16 r1, [r0, r2]                 /* store data                    */;\
  ldr lr, [reg_base, #REG_SAVE3]          /* pop lr off of stack           */;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
                                                                             ;\
ext_store_oam_ram_u##store_type:                                             ;\
  mask_addr_bus16_##store_type(10)        /* Mask to mirror memory (+align)*/;\
  sub r2, reg_base, #0x400                /* r2 = oam ram base             */;\
  store_op16 r1, [r0, r2]                 /* store data                    */;\
  str r2, [reg_base, #OAM_UPDATED]        /* write non zero to signal      */;\
  ldr lr, [reg_base, #REG_SAVE3]          /* pop lr off of stack           */;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
                                                                             ;\
3:                                                                           ;\
  ldr lr, [reg_base, #REG_SAVE3]          /* restore lr                    */;\
  ldr r0, [lr]                            /* load PC                       */;\
  str r0, [reg_base, #REG_PC]             /* write out PC                  */;\
  b smc_write                             /* perform smc write             */;\
.size execute_store_u##store_type, .-execute_store_u##store_type

execute_store_builder(8,  strb, strh, ldrb, 0)
execute_store_builder(16, strh, strh, ldrh, 1)
execute_store_builder(32, str,  str,  ldr,  2)

@ This is a store that is executed in a strm case (so no SMC checks in-between)

defsymbl(execute_store_u32_safe)
  execute_store_body(32_safe, 3)
  restore_flags()
  ldr pc, [reg_base, #REG_SAVE3]          @ return

ext_store_u32_safe:
  ldr lr, [reg_base, #REG_SAVE3]          @ Restore lr
  call_c_function(write_memory32)         @ Perform 32bit store
  restore_flags()
  bx lr                                   @ Return

ext_store_iwram_u32_safe:
  mask_addr_8(15)                         @ Mask to mirror memory (no need to align!)
  ldr r2, =(iwram+0x8000)                 @ r2 = iwram base
  str r1, [r0, r2]                        @ store data
  restore_flags()
  ldr pc, [reg_base, #REG_SAVE3]          @ return

ext_store_ewram_u32_safe:
  mask_addr_8(18)                         @ Mask to mirror memory (no need to align!)
  ldr r2, =(ewram)                        @ r2 = ewram base
  str r1, [r0, r2]                        @ store data
  restore_flags()
  ldr pc, [reg_base, #REG_SAVE3]          @ return

ext_store_vram_u32_safe:
  mask_addr_8(17)                         @ Mask to mirror memory (no need to align!)
  ldr r2, =(vram)                         @ r2 = vram base
  cmp r0, #0x18000                        @ Check if exceeds 96KB
  subcs r0, r0, #0x8000                   @ Mirror to the last bank
  str r1, [r0, r2]                        @ store data
  restore_flags()
  ldr pc, [reg_base, #REG_SAVE3]          @ return

ext_store_oam_ram_u32_safe:
  mask_addr_8(10)                         @ Mask to mirror memory (no need to align!)
  sub r2, reg_base, #0x400                @ r2 = oam ram base
  str r1, [r0, r2]                        @ store data
  str r2, [reg_base, #OAM_UPDATED]        @ store anything non zero here
  restore_flags()
  ldr pc, [reg_base, #REG_SAVE3]          @ return
.size execute_store_u32_safe, .-execute_store_u32_safe

write_epilogue:
  cmp r0, #0                              @ check if the write rose an alert
  beq 4f                                  @ if not we can exit

  collapse_flags(r1)                      @ interrupt needs current flags

  cmp r0, #2                              @ see if the alert is due to SMC
  beq smc_write                           @ if so, goto SMC handler

  ldr r1, [reg_base, #REG_CPSR]           @ r1 = cpsr
  tst r1, #0x20                           @ see if Thumb bit is set
  bne 1f                                  @ if so do Thumb update

  store_registers_arm()                   @ save ARM registers
  b alert_loop

1:
  store_registers_thumb()                 @ save Thumb registers

alert_loop:
  call_c_function(update_gba)             @ update GBA until CPU isn't halted

  ldr r1, [reg_base, #COMPLETED_FRAME]    @ Check whether a frame was completed
  cmp r1, #0
  bne return_to_main

  ldr r1, [reg_base, #CPU_HALT_STATE]     @ Check whether the CPU is halted
  cmp r1, #0
  bne alert_loop                          @ Keep looping until it is

  mvn reg_cycles, r0                      @ load new cycle count
  b execute_pc                            @ restart execution at PC

4:
  restore_flags()
  add pc, lr, #4                          @ return


smc_write:
  call_c_function(flush_translation_cache_ram)

lookup_pc:
  ldr r0, [reg_base, #REG_PC]             @ r0 = new pc
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = flags
  tst r1, #0x20                           @ see if Thumb bit is set
  beq execute_pc_arm                      @ if not lookup ARM
  b   execute_pc_thumb


#define sign_extend_u8(reg)
#define sign_extend_u16(reg)
#define sign_extend_u32(reg)

#define sign_extend_s8(reg)                                                  ;\
  mov reg, reg, lsl #24                   /* shift reg into upper 8bits    */;\
  mov reg, reg, asr #24                   /* shift down, sign extending    */;\

#define sign_extend_s16(reg)                                                 ;\
  mov reg, reg, lsl #16                   /* shift reg into upper 16bits   */;\
  mov reg, reg, asr #16                   /* shift down, sign extending    */;\

#define execute_load_op_u8(load_op)                                          ;\
  mov r0, r0, lsl #17                                                        ;\
  load_op r0, [r2, r0, lsr #17]                                              ;\

#define execute_load_op_s8(load_op)                                          ;\
  mov r0, r0, lsl #17                                                        ;\
  mov r0, r0, lsr #17                                                        ;\
  load_op r0, [r2, r0]                                                       ;\

#define execute_load_op_u16(load_op)                                         ;\
  execute_load_op_s8(load_op)                                                ;\

#define execute_load_op_s16(load_op)                                         ;\
  execute_load_op_s8(load_op)                                                ;\

#define execute_load_op_u16(load_op)                                         ;\
  execute_load_op_s8(load_op)                                                ;\

#define execute_load_op_u32(load_op)                                         ;\
  execute_load_op_u8(load_op)                                                ;\


#define execute_load_builder(load_type, load_function, load_op, mask)        ;\
                                                                             ;\
.align 2                                                                     ;\
defsymbl(execute_load_##load_type)                                           ;\
  save_flags()                                                               ;\
  tst r0, mask                            /* make sure address is in range */;\
  bne ext_load_##load_type                /* if not do ext load            */;\
                                                                             ;\
  ldr r2, =memory_map_read                /* r2 = memory_map_read          */;\
  mov r1, r0, lsr #15                     /* r1 = page index of address    */;\
  ldr r2, [r2, r1, lsl #2]                /* r2 = memory page              */;\
                                                                             ;\
  cmp r2, #0                              /* see if map is ext             */;\
  beq ext_load_##load_type                /* if so do ext load             */;\
                                                                             ;\
  execute_load_op_##load_type(load_op)                                       ;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
                                                                             ;\
ext_load_##load_type:                                                        ;\
  ldr r1, [lr]                            /* r1 = PC                       */;\
  str r1, [reg_base, #REG_PC]             /* update PC                     */;\
  call_c_function(read_memory##load_function)                                ;\
  sign_extend_##load_type(r0)             /* sign extend result            */;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\
.size execute_load_##load_type, .-execute_load_##load_type

.pool

execute_load_builder(u8, 8, ldrneb, #0xF0000000)
execute_load_builder(s8, 8, ldrnesb, #0xF0000000)
execute_load_builder(u16, 16, ldrneh, #0xF0000001)
execute_load_builder(s16, 16_signed, ldrnesh, #0xF0000001)
execute_load_builder(u32, 32, ldrne, #0xF0000000)

.data

defsymbl(memory_map_read)
  .space 0x8000
defsymbl(palette_ram)
  .space 0x400
defsymbl(palette_ram_converted)
  .space 0x400
defsymbl(spsr)
  .space 24
defsymbl(reg_mode)
  .space 196

defsymbl(oam_ram)
  .space 0x400
defsymbl(reg)
  .space 0x100, 0
@ Store pointer tables down here
store_fnptr_table(8)
store_fnptr_table(16)
store_fnptr_table(32)
store_fnptr_table(32_safe)

@ Vita and 3DS (and of course mmap) map their own cache sections through some
@ platform-speficic mechanisms.
#if !defined(HAVE_MMAP) && !defined(VITA) && !defined(_3DS)

@ Make this section executable!
.text
#ifdef __ANDROID__
@ Unfortunately Android builds don't like nobits, so we ship a ton of zeros
@ TODO: Revisit this whenever we upgrade to the latest clang NDK
.section .jit,"awx",%progbits
#else
.section .jit,"awx",%nobits
#endif
.align 4
defsymbl(rom_translation_cache)
  .space ROM_TRANSLATION_CACHE_SIZE
.size rom_translation_cache, .-rom_translation_cache
defsymbl(ram_translation_cache)
  .space RAM_TRANSLATION_CACHE_SIZE
.size ram_translation_cache, .-ram_translation_cache

#endif


# gameplaySP
#
# Copyright (C) 2006 Exophase <exophase@gmail.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "../gpsp_config.h"

.set mips32r2
.align 4

.global mips_update_gba
.global mips_indirect_branch_arm
.global mips_indirect_branch_thumb
.global mips_indirect_branch_dual
.global execute_read_cpsr
.global execute_read_spsr
.global execute_swi
.global execute_spsr_restore
.global execute_store_cpsr
.global execute_store_spsr
.global execute_lsl_flags_reg
.global execute_lsr_flags_reg
.global execute_asr_flags_reg
.global execute_ror_flags_reg
.global execute_arm_translate_internal
.global icache_region_sync
.global reg_check
.global palette_ram
.global palette_ram_converted
.global oam_ram
.global init_emitter
.global mips_lookup_pc
.global smc_write
.global mips_cheat_hook

.global write_io_epilogue
.global memory_map_read
.global tmemld
.global tmemst
.global tmemst
.global reg
.global spsr
.global reg_mode
.global oam_update

# MIPS register layout:

# $0 - constant zero
# $1 - temporary
# $2 - temporary / return value
# $3 - ARM r0 (not saved)
# $4 - temporary / function argument 0
# $5 - temporary / function argument 1
# $6 - temporary / function argument 2
# $7 - ARM r1 (not saved)
# $8 - ARM r2 (not saved)
# $9 - ARM r3 (not saved)
# $10 - ARM r4 (not saved)
# $11 - ARM r5 (not saved)
# $12 - ARM r6 (not saved)
# $13 - ARM r7 (not saved)
# $14 - ARM r8 (not saved)
# $15 - ARM r9 (not saved)
# $16 - ARM machine state pointer (saved)
# $17 - cycle counter (saved)
# $18 - ARM r10 (saved)
# $19 - block start address (roughly r15) (saved)
# $20 - ARM negative register (saved)
# $21 - ARM zero register (saved)
# $22 - ARM carry register (saved)
# $23 - ARM overflow register (saved)
# $24 - ARM r11 (not saved)
# $25 - ARM r12 (not saved)
# $26 - kernel temporary 0
# $27 - kernel temporary 1
# $28 - ARM r13 (saved)
# $29 - stack pointer
# $30 - ARM r14 (saved)
# $31 - return address

.equ REG_R0,              (0 * 4)
.equ REG_R1,              (1 * 4)
.equ REG_R2,              (2 * 4)
.equ REG_R3,              (3 * 4)
.equ REG_R4,              (4 * 4)
.equ REG_R5,              (5 * 4)
.equ REG_R6,              (6 * 4)
.equ REG_R7,              (7 * 4)
.equ REG_R8,              (8 * 4)
.equ REG_R9,              (9 * 4)
.equ REG_R10,             (10 * 4)
.equ REG_R11,             (11 * 4)
.equ REG_R12,             (12 * 4)
.equ REG_R13,             (13 * 4)
.equ REG_R14,             (14 * 4)
.equ REG_PC,              (15 * 4)
.equ REG_N_FLAG,          (16 * 4)
.equ REG_Z_FLAG,          (17 * 4)
.equ REG_C_FLAG,          (18 * 4)
.equ REG_V_FLAG,          (19 * 4)
.equ REG_CPSR,            (20 * 4)
.equ REG_SAVE,            (21 * 4)
.equ REG_SAVE2,           (22 * 4)
.equ REG_SAVE3,           (23 * 4)
.equ CPU_MODE,            (29 * 4)
.equ CPU_HALT_STATE,      (30 * 4)
.equ CHANGED_PC_STATUS,   (31 * 4)
.equ COMPLETED_FRAME,     (32 * 4)
.equ OAM_UPDATED,         (33 * 4)
.equ GP_SAVE,             (34 * 4)

.equ SPSR_BASE,           (0x100 + 0x400 * 3)
.equ REGMODE_BASE,        (SPSR_BASE + 24)
.equ SUPERVISOR_SPSR,     (3 * 4 + SPSR_BASE)
.equ SUPERVISOR_LR,       ((3 * (7 * 4)) + (6 * 4) + REGMODE_BASE)
.equ FNPTRS_MEMOPS,       (REGMODE_BASE + 196)
.equ FNPTRS_BASE,         (FNPTRS_MEMOPS + 960)

.set noat
.set noreorder

# make sure $16 has the register base for these macros

.macro collapse_flag flag_reg, shift
  ins $2, $\flag_reg, \shift, 1    # insert flag into CPSR
.endm

.macro collapse_flags
  lw $2, REG_CPSR($16)            # load CPSR
  andi $2, $2, 0xFF               # isolate lower 8bits
  collapse_flag 20, 31            # store flags
  collapse_flag 21, 30
  collapse_flag 22, 29
  collapse_flag 23, 28
  sw $2, REG_CPSR($16)            # store CPSR
.endm

.macro extract_flag shift, flag_reg
  ext $\flag_reg, $1, \shift, 1   # extract flag from CPSR
.endm

.macro extract_flags_body         # extract flags from $1
  extract_flag 31, 20             # load flags
  extract_flag 30, 21
  extract_flag 29, 22
  extract_flag 28, 23
.endm

.macro extract_flags
  lw $1, REG_CPSR($16)            # load CPSR
  extract_flags_body
.endm

.macro save_registers
  sw $3, REG_R0($16)
  sw $7, REG_R1($16)
  sw $8, REG_R2($16)
  sw $9, REG_R3($16)
  sw $10, REG_R4($16)
  sw $11, REG_R5($16)
  sw $12, REG_R6($16)
  sw $13, REG_R7($16)
  sw $14, REG_R8($16)
  sw $15, REG_R9($16)
  sw $24, REG_R11($16)
  sw $25, REG_R12($16)

  sw $18, REG_R10($16)
  sw $28, REG_R13($16)
  sw $30, REG_R14($16)

  lw $28, GP_SAVE($16)
.endm

.macro restore_registers
  lw $3, REG_R0($16)
  lw $7, REG_R1($16)
  lw $8, REG_R2($16)
  lw $9, REG_R3($16)
  lw $10, REG_R4($16)
  lw $11, REG_R5($16)
  lw $12, REG_R6($16)
  lw $13, REG_R7($16)
  lw $14, REG_R8($16)
  lw $15, REG_R9($16)
  lw $24, REG_R11($16)
  lw $25, REG_R12($16)

  lw $18, REG_R10($16)
  lw $28, REG_R13($16)
  lw $30, REG_R14($16)
.endm

# PIC ABI mandates to jump to target via $t9

#ifdef PIC
.macro cfncall target, targetid
  lw $t9, (FNPTRS_BASE + \targetid * 4)($16)
  jalr $t9
  nop
.endm
#else
.macro cfncall target, targetid
  jal \target
  nop
.endm
#endif


# Process a hardware event. Since an interrupt might be
# raised we have to check if the PC has changed.

# $4: next address
# $16: register base
# $17: cycle counter

.balign 64

# This gets called every time the cycle counter runs out
# (checked at every branch/jump)
mips_update_gba:
  sw $4, REG_PC($16)              # current PC = $4

  sw $ra, REG_SAVE2($16)          # save return addr
  collapse_flags                  # update cpsr
  save_registers                  # save registers
  sw $0, CHANGED_PC_STATUS($16)
  cfncall update_gba, 0           # process the next event

  lw $1, COMPLETED_FRAME($16)     # Check whether we completed a frame
  bne $1, $0, return_to_main      # Return to main thread now

  addu $17, $2, $0                # $17 = new cycle count (ret value)

  lw $ra, REG_SAVE2($16)          # restore return address

  lw $1, CHANGED_PC_STATUS($16)
  bne $1, $0, lookup_pc
  nop

  restore_registers

  jr $ra                          # if not, go back to caller
  nop


# Processes cheats whenever we hit the master PC
mips_cheat_hook:
  sw $ra, REG_SAVE2($16)
  save_registers
  cfncall process_cheats, 8
  lw $ra, REG_SAVE2($16)
  restore_registers
  jr $ra
  nop


# Loads the main context and returns to it.
# ARM regs must be saved before branching here
return_to_main:
  lw $28, GP_SAVE($16)            # Restore previous state
  lw $s0,  0($sp)
  lw $s1,  4($sp)
  lw $s2,  8($sp)
  lw $s3, 12($sp)
  lw $s4, 16($sp)
  lw $s5, 20($sp)
  lw $s6, 24($sp)
  lw $s7, 28($sp)
  lw $fp, 32($sp)
  lw $ra, 36($sp)
  jr $ra                          # Return to main
  add $sp, $sp, 48                # Restore stack pointer (delay slot)


# Perform an indirect branch.

# $4: GBA address to branch to

mips_indirect_branch_arm:
  save_registers
  cfncall block_lookup_address_arm, 1
  restore_registers
  jr $2                           # $2 = value returned
  nop

mips_indirect_branch_thumb:
  save_registers
  cfncall block_lookup_address_thumb, 2
  restore_registers
  jr $2                           # $2 = value returned
  nop

mips_indirect_branch_dual:
  save_registers
  cfncall block_lookup_address_dual, 3
  nop
  restore_registers
  jr $2                           # $2 = value returned
  nop


write_io_epilogue:
  beq $2, $0, no_alert            # 0 means nothing happened
  addiu $4, $2, -2                # see if return value is 2 (delay slot)
  beq $4, $0, smc_dma             # is it an SMC alert? (return value = 2)
  nop
  addiu $4, $2, -3                # see if return value is 3
  beq $4, $0, irq_alert           # is it an IRQ alert? (return value = 3)
  nop
  collapse_flags                  # make sure flags are good for update_gba



alert_loop:
  cfncall update_gba, 0           # process the next event

  lw $1, COMPLETED_FRAME($16)     # Check whether we completed a frame
  bne $1, $0, return_to_main      # Return to main thread now

  lw $1, CPU_HALT_STATE($16)      # check if CPU is sleeping
  bne $1, $0, alert_loop          # see if it hasn't changed
  nop

  addu $17, $2, $0                # $17 = new cycle counter
  lw $4, REG_PC($16)              # $4 = new PC

  j lookup_pc
  nop

irq_alert:
  restore_registers
  j lookup_pc                     # PC has changed, get a new one
  nop

no_alert:
  restore_registers
  lw $ra, REG_SAVE3($16)          # restore return
  jr $ra                          # we can return
  nop

smc_dma:
  cfncall flush_translation_cache_ram, 4
  j lookup_pc
  nop

smc_write:
  save_registers
  sw $6, REG_PC($16)              # save PC
  cfncall flush_translation_cache_ram, 4

mips_lookup_pc:
lookup_pc:
  lw $2, REG_CPSR($16)            # $2 = cpsr
  andi $2, $2, 0x20               # isolate mode bit
  beq $2, $0, lookup_pc_arm       # if T bit is zero use arm handler
  nop

lookup_pc_thumb:
  lw $4, REG_PC($16)                     # load PC as arg 0
  cfncall block_lookup_address_thumb, 2  # get Thumb address
  restore_registers
  jr $2                                  # jump to result
  nop

lookup_pc_arm:
  lw $4, REG_PC($16)                   # load PC as arg 0
  cfncall block_lookup_address_arm, 1  # get ARM address
  restore_registers
  jr $2                                # jump to result
  nop

# Return the current cpsr

execute_read_cpsr:
  collapse_flags                  # fold flags into cpsr, put cpsr into $2
  jr $ra                          # return
  nop

# Return the current spsr

execute_read_spsr:
  lw $1, CPU_MODE($16)            # $1 = cpu_mode
  sll $1, $1, 2                   # adjust to word offset size
  addu $2, $1, $16
  jr $ra                          # return
  lw $2, SPSR_BASE($2)            # $2 = spsr[cpu_mode] (delay slot)

# Switch into SWI, has to collapse flags
# $4: Current pc

execute_swi:
  sw $ra, REG_SAVE3($16)
  sw $4, SUPERVISOR_LR($16)       # store next PC in the supervisor's LR
  collapse_flags                  # get cpsr in $2
  sw $2, SUPERVISOR_SPSR($16)     # save cpsr in SUPERVISOR_CPSR
  ins $2, $0, 0, 6                # zero out bottom 6 bits of CPSR
  ori $2, 0x13                    # set mode to supervisor
  sw $2, REG_CPSR($16)            # write back CPSR
  save_registers
  li $4, 3                        # 3 is supervisor mode
  cfncall set_cpu_mode, 5         # set the CPU mode to supervisor
  lw $ra, REG_SAVE3($16)
  restore_registers
  jr $ra                          # return
  nop

# $4: pc to restore to
# returns in $4

execute_spsr_restore:
  lw $1, CPU_MODE($16)            # $1 = cpu_mode

  beq $1, $0, no_spsr_restore     # only restore if the cpu isn't usermode
  sll $2, $1, 2                   # adjust to word offset size (delay)

  addu $2, $2, $16
  lw $1, SPSR_BASE($2)            # $1 = spsr[cpu_mode]
  sw $1, REG_CPSR($16)            # cpsr = spsr[cpu_mode]
  extract_flags_body              # extract flags from $1
  sw $ra, REG_SAVE3($16)
  save_registers
  cfncall execute_spsr_restore_body, 6  # do the dirty work in this C function
  restore_registers
  lw $ra, REG_SAVE3($16)
  jr $ra
  addu $4, $2, $0                 # move return value to $4

no_spsr_restore:
  jr $ra
  nop

# $4: new cpsr
# $5: store mask
# $6: current PC

execute_store_cpsr:
  and $1, $4, $5                  # $1 = new_cpsr & store_mask
  lw $2, REG_CPSR($16)            # $2 = current cpsr
  nor $4, $5, $0                  # $4 = ~store_mask
  and $2, $2, $4                  # $2 = (cpsr & (~store_mask))
  or $1, $1, $2                   # $1 = new cpsr combined with old
  extract_flags_body              # extract flags from $1
  sw $ra, REG_SAVE3($16)
  save_registers
  addu $4, $1, $0                 # load the new CPSR
  cfncall execute_store_cpsr_body, 7   # do the dirty work in this C function

  bne $2, $0, changed_pc_cpsr     # this could have changed the pc
  nop

  restore_registers

  lw $ra, REG_SAVE3($16)
  jr $ra
  nop

changed_pc_cpsr:
  addu $4, $2, $0                      # load new address in $4
  cfncall block_lookup_address_arm, 1  # GBA address is in $4
  restore_registers                    # restore registers
  jr $2                                # jump to the new address
  nop


# $4: new spsr
# $5: store mask

execute_store_spsr:
  lw $1, CPU_MODE($16)            # $1 = cpu_mode
  sll $1, $1, 2                   # adjust to word offset size
  addu $1, $1, $16
  lw $2, SPSR_BASE($1)            # $2 = spsr[cpu_mode]
  and $4, $4, $5                  # $4 = new_spsr & store_mask
  nor $5, $5, $0                  # $5 = ~store_mask
  and $2, $2, $5                  # $2 = (spsr & (~store_mask))
  or $4, $4, $2                   # $4 = new spsr combined with old
  jr $ra                          # return
  sw $4, SPSR_BASE($1)            # spsr[cpu_mode] = $4 (delay slot)

# $4: value
# $5: shift

execute_lsl_flags_reg:
  beq $5, $0, lsl_shift_zero      # is the shift zero?
  sltiu $1, $5, 32                # $1 = (shift < 32) (delay)
  beq $1, $0, lsl_shift_high      # is the shift >= 32?
  li $2, 32

  subu $2, $2, $5                 # $2 = (32 - shift)
  srlv $2, $4, $2                 # $2 = (value >> (32 - shift))
  andi $22, $2, 1                 # c flag = (value >> (32 - shift)) & 0x01

lsl_shift_zero:
  jr $ra                          # return
  sllv $4, $4, $5                 # return (value << shift) (delay)

lsl_shift_high:
  sltiu $1, $5, 33                # $1 = (shift < 33) (delay)
  bne $1, $0, lsl_shift_done      # jump if shift == 32
  andi $22, $4, 1                 # c flag = value & 0x01 (delay)

  add $22, $0, $0                 # c flag = 0 otherwise

lsl_shift_done:
  jr $ra                          # return
  add $4, $0, $0                  # value = 0 no matter what


execute_lsr_flags_reg:
  beq $5, $0, lsr_shift_zero      # is the shift zero?
  sltiu $1, $5, 32                # $1 = (shift < 32) (delay)
  beq $1, $0, lsr_shift_high      # is the shift >= 32?
  addiu $2, $5, -1                # $2 = shift - 1 (delay)

  srlv $2, $4, $2                 # $2 = (value >> (shift - 1))
  andi $22, $2, 1                 # c flag = (value >> (32 - shift)) & 0x01

lsr_shift_zero:
  jr $ra                          # return
  srlv $4, $4, $5                 # return (value >> shift) (delay)

lsr_shift_high:
  sltiu $1, $5, 33                # $1 = (shift < 33) (delay)
  bne $1, $0, lsr_shift_done      # jump if shift == 32
  srl $22, $4, 31                 # c flag = value >> 31 (delay)

  add $22, $0, $0                 # c flag = 0 otherwise

lsr_shift_done:
  jr $ra                          # return
  add $4, $0, $0                  # value = 0 no matter what


execute_asr_flags_reg:
  beq $5, $0, asr_shift_zero      # is the shift zero?
  sltiu $1, $5, 32                # $1 = (shift < 32) (delay)
  beq $1, $0, asr_shift_high      # is the shift >= 32?
  addiu $2, $5, -1                # $2 = shift - 1 (delay)

  srlv $2, $4, $2                 # $2 = (value >> (shift - 1))
  andi $22, $2, 1                 # c flag = (value >> (32 - shift)) & 0x01

asr_shift_zero:
  jr $ra                          # return
  srav $4, $4, $5                 # return (value >> shift) (delay)

asr_shift_high:
  sra $4, $4, 31                  # value >>= 31
  jr $ra                          # return
  andi $22, $4, 1                 # c flag = value & 0x01


execute_ror_flags_reg:
  beq $5, $0, ror_zero_shift      # is the shift zero?
  addiu $1, $5, -1                # $1 = (shift - 1) (delay)

  srav $1, $4, $1                 # $1 = (value >> (shift - 1))
  andi $22, $1, 1                 # c flag = $1 & 1

ror_zero_shift:
  jr $ra                          # return
  rotrv $4, $4, $5                # return (value ror shift) delay

# $4: cycle counter argument
# $5: pointer to reg

execute_arm_translate_internal:
  add $sp, $sp, -48               # Store the main thread context
  sw $s0,  0($sp)
  sw $s1,  4($sp)
  sw $s2,  8($sp)
  sw $s3, 12($sp)
  sw $s4, 16($sp)
  sw $s5, 20($sp)
  sw $s6, 24($sp)
  sw $s7, 28($sp)
  sw $fp, 32($sp)
  sw $ra, 36($sp)

  move $16, $5
  sw $28, GP_SAVE($16)

  addu $17, $4, $0                # load cycle counter register

  extract_flags                   # load flag variables

  # CPU might be sleeping, do not wake ip up!
  lw $1, CPU_HALT_STATE($16)      # check if CPU is sleeping
  bne $1, $0, alert_loop          # see if it hasn't changed

  lw $1, REG_CPSR($16)
  and $1, $1, 0x20                # see if Thumb bit is set in flags

  bne $1, $0, 1f
  lw $4, REG_PC($16)              # load PC into $4 (delay)

  cfncall block_lookup_address_arm, 1
  restore_registers               # load initial register values
  jr $2                           # jump to return
  nop

1:
  cfncall block_lookup_address_thumb, 2
  restore_registers               # load initial register values
  jr $2                           # jump to return
  nop


.data
.align 6

memory_map_read:
  .space 0x8000

# memory_map_read is immediately before arm_reg on purpose (offset used
# to access it, via lw op). We do not use write though.
reg:
  .space 0x100

# Placed here for easy access
palette_ram:
  .space 0x400
palette_ram_converted:
  .space 0x400
oam_ram:
  .space 0x400
spsr:
  .space 24     # u32[6]
reg_mode:
  .space 196    # u32[7][7];

# Here we store:
#  void *tmemld[11][16];  # 10 types of loads
#  void *tmemst[ 4][16];  #  3 types of stores
# Essentially a list of pointers to the different mem load handlers
# Keep them close for a fast patcher.
tmemld:
  .space 704
tmemst:
  .space 256
fnptrs:
  .long update_gba                     # 0
  .long block_lookup_address_arm       # 1
  .long block_lookup_address_thumb     # 2
  .long block_lookup_address_dual      # 3
  .long flush_translation_cache_ram    # 4
  .long set_cpu_mode                   # 5
  .long execute_spsr_restore_body      # 6
  .long execute_store_cpsr_body        # 7
  .long process_cheats                 # 8

#if !defined(HAVE_MMAP)

# Make this section executable!
.text
.section .jit,"awx",%nobits
.align 2
.global stub_arena
.global rom_translation_cache
.global ram_translation_cache

stub_arena:
  .space STUB_ARENA_SIZE
rom_translation_cache:
  .space ROM_TRANSLATION_CACHE_SIZE
ram_translation_cache:
  .space RAM_TRANSLATION_CACHE_SIZE

#endif


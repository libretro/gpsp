/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"
#include <ctype.h>

timer_type timer[4];

u32 global_cycles_per_instruction = 1;

u32 cpu_ticks = 0;

u32 execute_cycles = 960;
s32 video_count = 960;
u32 ticks;

u32 arm_frame = 0;
u32 thumb_frame = 0;
u32 last_frame = 0;

u32 cycle_memory_access = 0;
u32 cycle_pc_relative_access = 0;
u32 cycle_sp_relative_access = 0;
u32 cycle_block_memory_access = 0;
u32 cycle_block_memory_sp_access = 0;
u32 cycle_block_memory_words = 0;
u32 cycle_dma16_words = 0;
u32 cycle_dma32_words = 0;
u32 flush_ram_count = 0;
u32 gbc_update_count = 0;
u32 oam_update_count = 0;

char main_path[512];
char save_path[512];

void trigger_ext_event(void);

static void update_timers(irq_type *irq_raised)
{
   unsigned i;
   for (i = 0; i < 4; i++)
   {
      if(timer[i].status == TIMER_INACTIVE)
         continue;

      if(timer[i].status != TIMER_CASCADE)
      {
         timer[i].count -= execute_cycles;
         /* io_registers accessors range: REG_TM0D, REG_TM1D, REG_TM2D, REG_TM3D */
         io_registers[128 + (i * 2)] = -(timer[i].count > timer[i].prescale);
      }

      if(timer[i].count > 0)
         continue;

      /* irq_raised value range: IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3 */
      if(timer[i].irq == TIMER_TRIGGER_IRQ)
         *irq_raised |= (8 << i);

      if((i != 3) && (timer[i + 1].status == TIMER_CASCADE))
      {
         timer[i + 1].count--;
         io_registers[REG_TM0D + (i + 1) * 2] = -(timer[i + 1].count);
      }

      if(i < 2)
      {
         if(timer[i].direct_sound_channels & 0x01)
            sound_timer(timer[i].frequency_step, 0);

         if(timer[i].direct_sound_channels & 0x02)
            sound_timer(timer[i].frequency_step, 1);
      }

      timer[i].count += (timer[i].reload << timer[i].prescale);
   }
}

void init_main(void)
{
  u32 i;

  for(i = 0; i < 4; i++)
  {
    dma[i].start_type = DMA_INACTIVE;
    dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
    timer[i].status = TIMER_INACTIVE;
    timer[i].reload = 0x10000;
    timer[i].stop_cpu_ticks = 0;
  }

  timer[0].direct_sound_channels = TIMER_DS_CHANNEL_BOTH;
  timer[1].direct_sound_channels = TIMER_DS_CHANNEL_NONE;

  cpu_ticks = 0;

  execute_cycles = 960;
  video_count = 960;

#ifdef HAVE_DYNAREC
  init_caches();
  init_emitter();
#endif
}

u32 no_alpha = 0;

u32 update_gba(void)
{
  irq_type irq_raised = IRQ_NONE;

  do
  {
    unsigned i;
    cpu_ticks += execute_cycles;

    reg[CHANGED_PC_STATUS] = 0;
    reg[COMPLETED_FRAME] = 0;

    if(gbc_sound_update)
    {
      gbc_update_count++;
      update_gbc_sound(cpu_ticks);
      gbc_sound_update = 0;
    }

    update_timers(&irq_raised);

    video_count -= execute_cycles;

    if(video_count <= 0)
    {
      u32 vcount = io_registers[REG_VCOUNT];
      u32 dispstat = io_registers[REG_DISPSTAT];

      if((dispstat & 0x02) == 0)
      {
        // Transition from hrefresh to hblank
        video_count += (272);
        dispstat |= 0x02;

        if((dispstat & 0x01) == 0)
        {
          u32 i;
          if(reg[OAM_UPDATED])
            oam_update_count++;

          if(no_alpha)
            io_registers[REG_BLDCNT] = 0;
          update_scanline();

          // If in visible area also fire HDMA
          for(i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_HBLANK)
              dma_transfer(dma + i);
          }
        }

        if(dispstat & 0x10)
          irq_raised |= IRQ_HBLANK;
      }
      else
      {
        // Transition from hblank to next line
        video_count += 960;
        dispstat &= ~0x02;

        vcount++;

        if(vcount == 160)
        {
          // Transition from vrefresh to vblank
          u32 i;

          dispstat |= 0x01;
          if(dispstat & 0x8)
          {
            irq_raised |= IRQ_VBLANK;
          }

          affine_reference_x[0] =
           (s32)(address32(io_registers, 0x28) << 4) >> 4;
          affine_reference_y[0] =
           (s32)(address32(io_registers, 0x2C) << 4) >> 4;
          affine_reference_x[1] =
           (s32)(address32(io_registers, 0x38) << 4) >> 4;
          affine_reference_y[1] =
           (s32)(address32(io_registers, 0x3C) << 4) >> 4;

          for(i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_VBLANK)
              dma_transfer(dma + i);
          }
        }
        else

        if(vcount == 228)
        {
          // Transition from vblank to next screen
          dispstat &= ~0x01;

/*        printf("frame update (%x), %d instructions total, %d RAM flushes\n",
           reg[REG_PC], instruction_count - last_frame, flush_ram_count);
          last_frame = instruction_count;
*/
/*          printf("%d gbc audio updates\n", gbc_update_count);
          printf("%d oam updates\n", oam_update_count); */
          gbc_update_count = 0;
          oam_update_count = 0;
          flush_ram_count = 0;

          update_gbc_sound(cpu_ticks);
          gbc_sound_update = 0;

          /* If there's no cheat hook, run on vblank! */
          if (cheat_master_hook == ~0U)
             process_cheats();

          vcount = 0;
          // We completed a frame, tell the dynarec to exit to the main thread
          reg[COMPLETED_FRAME] = 1;
        }

        if(vcount == (dispstat >> 8))
        {
          // vcount trigger
          dispstat |= 0x04;
          if(dispstat & 0x20)
          {
            irq_raised |= IRQ_VCOUNT;
          }
        }
        else
          dispstat &= ~0x04;

        io_registers[REG_VCOUNT] = vcount;
      }
      io_registers[REG_DISPSTAT] = dispstat;
    }

    if(irq_raised)
      raise_interrupt(irq_raised);

    execute_cycles = video_count;

    for (i = 0; i < 4; i++)
    {
       if(timer[i].status != TIMER_PRESCALE)
          continue;

       if(timer[i].count < execute_cycles)
          execute_cycles = timer[i].count;
    }
  } while(reg[CPU_HALT_STATE] != CPU_ACTIVE && !reg[COMPLETED_FRAME]);

  return execute_cycles;
}

void reset_gba(void)
{
  init_main();
  init_memory();
  init_cpu();
  reset_sound();
}

u32 file_length(FILE *fp)
{
  u32 length;

  fseek(fp, 0, SEEK_END);
  length = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  return length;
}

void change_ext(const char *src, char *buffer, const char *extension)
{
  char *dot_position;
  strcpy(buffer, src);
  dot_position = strrchr(buffer, '.');

  if(dot_position)
    strcpy(dot_position, extension);
}

#define main_savestate_builder(type)            \
void main_##type##_savestate(void)              \
{                                               \
  state_mem_##type##_variable(cpu_ticks);       \
  state_mem_##type##_variable(execute_cycles);  \
  state_mem_##type##_variable(video_count);     \
  state_mem_##type##_array(timer);              \
}

main_savestate_builder(read)
main_savestate_builder(write)


void printout(void *str, u32 val)
{
  printf(str, val);
}

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

/* Sound */
#define gbc_sound_tone_control_low(channel, address)                          \
{                                                                             \
  u32 initial_volume = (value >> 12) & 0x0F;                                  \
  u32 envelope_ticks = ((value >> 8) & 0x07) * 4;                             \
  gbc_sound_channel[channel].length_ticks = 64 - (value & 0x3F);              \
  gbc_sound_channel[channel].sample_data =                                    \
   square_pattern_duty[(value >> 6) & 0x03];                                  \
  gbc_sound_channel[channel].envelope_direction = (value >> 11) & 0x01;       \
  gbc_sound_channel[channel].envelope_initial_volume = initial_volume;        \
  gbc_sound_channel[channel].envelope_volume = initial_volume;                \
  gbc_sound_channel[channel].envelope_initial_ticks = envelope_ticks;         \
  gbc_sound_channel[channel].envelope_ticks = envelope_ticks;                 \
  gbc_sound_channel[channel].envelope_status = (envelope_ticks != 0);         \
  gbc_sound_channel[channel].envelope_volume = initial_volume;                \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, address) = value;                                   \
}                                                                             \

#define gbc_sound_tone_control_high(channel, address)                         \
{                                                                             \
  u32 rate = value & 0x7FF;                                                   \
  gbc_sound_channel[channel].rate = rate;                                     \
  gbc_sound_channel[channel].frequency_step =                                 \
   float_to_fp16_16(((131072.0 / (2048 - rate)) * 8.0) / sound_frequency);    \
  gbc_sound_channel[channel].length_status = (value >> 14) & 0x01;            \
  if(value & 0x8000)                                                          \
  {                                                                           \
    gbc_sound_channel[channel].active_flag = 1;                               \
    gbc_sound_channel[channel].sample_index -= float_to_fp16_16(1.0 / 12.0);  \
    gbc_sound_channel[channel].envelope_ticks =                               \
     gbc_sound_channel[channel].envelope_initial_ticks;                       \
    gbc_sound_channel[channel].envelope_volume =                              \
     gbc_sound_channel[channel].envelope_initial_volume;                      \
  }                                                                           \
                                                                              \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, address) = value;                                   \
}                                                                             \

#define gbc_sound_tone_control_sweep()                                        \
{                                                                             \
  u32 sweep_ticks = ((value >> 4) & 0x07) * 2;                                \
  gbc_sound_channel[0].sweep_shift = value & 0x07;                            \
  gbc_sound_channel[0].sweep_direction = (value >> 3) & 0x01;                 \
  gbc_sound_channel[0].sweep_status = (value != 8);                           \
  gbc_sound_channel[0].sweep_ticks = sweep_ticks;                             \
  gbc_sound_channel[0].sweep_initial_ticks = sweep_ticks;                     \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, 0x60) = value;                                      \
}                                                                             \

#define gbc_sound_wave_control()                                              \
{                                                                             \
  gbc_sound_channel[2].wave_type = (value >> 5) & 0x01;                       \
  gbc_sound_channel[2].wave_bank = (value >> 6) & 0x01;                       \
  gbc_sound_channel[2].master_enable = 0;                                     \
  if(value & 0x80)                                                            \
    gbc_sound_channel[2].master_enable = 1;                                   \
                                                                              \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, 0x70) = value;                                      \
}                                                                             \

static u32 gbc_sound_wave_volume[4] = { 0, 16384, 8192, 4096 };

#define gbc_sound_tone_control_low_wave()                                     \
{                                                                             \
  gbc_sound_channel[2].length_ticks = 256 - (value & 0xFF);                   \
  if((value >> 15) & 0x01)                                                    \
    gbc_sound_channel[2].wave_volume = 12288;                                 \
  else                                                                        \
    gbc_sound_channel[2].wave_volume =                                        \
     gbc_sound_wave_volume[(value >> 13) & 0x03];                             \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, 0x72) = value;                                      \
}                                                                             \

#define gbc_sound_tone_control_high_wave()                                    \
{                                                                             \
  u32 rate = value & 0x7FF;                                                   \
  gbc_sound_channel[2].rate = rate;                                           \
  gbc_sound_channel[2].frequency_step =                                       \
   float_to_fp16_16((2097152.0 / (2048 - rate)) / sound_frequency);           \
  gbc_sound_channel[2].length_status = (value >> 14) & 0x01;                  \
  if(value & 0x8000)                                                          \
  {                                                                           \
    gbc_sound_channel[2].sample_index = 0;                                    \
    gbc_sound_channel[2].active_flag = 1;                                     \
  }                                                                           \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, 0x74) = value;                                      \
}                                                                             \

#define gbc_sound_noise_control()                                             \
{                                                                             \
  u32 dividing_ratio = value & 0x07;                                          \
  u32 frequency_shift = (value >> 4) & 0x0F;                                  \
  if(dividing_ratio == 0)                                                     \
  {                                                                           \
    gbc_sound_channel[3].frequency_step =                                     \
     float_to_fp16_16(1048576.0 / (1 << (frequency_shift + 1)) /              \
     sound_frequency);                                                        \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    gbc_sound_channel[3].frequency_step =                                     \
     float_to_fp16_16(524288.0 / (dividing_ratio *                            \
     (1 << (frequency_shift + 1))) / sound_frequency);                        \
  }                                                                           \
  gbc_sound_channel[3].noise_type = (value >> 3) & 0x01;                      \
  gbc_sound_channel[3].length_status = (value >> 14) & 0x01;                  \
  if(value & 0x8000)                                                          \
  {                                                                           \
    gbc_sound_channel[3].sample_index = 0;                                    \
    gbc_sound_channel[3].active_flag = 1;                                     \
    gbc_sound_channel[3].envelope_ticks =                                     \
     gbc_sound_channel[3].envelope_initial_ticks;                             \
    gbc_sound_channel[3].envelope_volume =                                    \
     gbc_sound_channel[3].envelope_initial_volume;                            \
  }                                                                           \
  gbc_sound_update = 1;                                                       \
  address16(io_registers, 0x7C) = value;                                      \
}                                                                             \

static void gbc_trigger_sound(u32 value)
{
   u32 channel;

   /* Trigger all 4 GBC sound channels */
   for (channel = 0; channel < 4; channel++)
   {
      gbc_sound_master_volume_right = value & 0x07;
      gbc_sound_master_volume_left = (value >> 4) & 0x07;
      gbc_sound_channel[channel].status = 
         ((value >> (channel + 8)) & 0x01) | ((value >> (channel + 11)) & 0x03);
   }
   address16(io_registers, 0x80) = value;
}

#define trigger_sound()                                                       \
{                                                                             \
  timer[0].direct_sound_channels = (((value >> 10) & 0x01) == 0) |            \
   ((((value >> 14) & 0x01) == 0) << 1);                                      \
  timer[1].direct_sound_channels = (((value >> 10) & 0x01) == 1) |            \
   ((((value >> 14) & 0x01) == 1) << 1);                                      \
  direct_sound_channel[0].volume = (value >> 2) & 0x01;                       \
  direct_sound_channel[0].status = (value >> 8) & 0x03;                       \
  direct_sound_channel[1].volume = (value >> 3) & 0x01;                       \
  direct_sound_channel[1].status = (value >> 12) & 0x03;                      \
  gbc_sound_master_volume = value & 0x03;                                     \
                                                                              \
  if((value >> 11) & 0x01)                                                    \
    sound_reset_fifo(0);                                                      \
  if((value >> 15) & 0x01)                                                    \
    sound_reset_fifo(1);                                                      \
  address16(io_registers, 0x82) = value;                                      \
}                                                                             \

static void sound_control_x(u32 value)
{
   if (value & 0x80)
   {
      if (sound_on != 1)
         sound_on = 1;
   }
   else
   {
      u32 i;
      for (i = 0; i < 4; i++)
         gbc_sound_channel[i].active_flag = 0;
      sound_on = 0;
   }

   address16(io_registers, 0x84) = 
      (address16(io_registers, 0x84) & 0x000F) | (value & 0xFFF0);
}

#define sound_update_frequency_step(timer_number)                             \
  timer[timer_number].frequency_step =                                        \
   float_to_fp8_24((GBC_BASE_RATE / sound_frequency) / (timer_reload))        \

/* Main */
extern timer_type timer[4];
static u32 prescale_table[] = { 0, 6, 8, 10 };

#define count_timer(timer_number)                                             \
  timer[timer_number].reload = 0x10000 - value;                               \
  if(timer_number < 2)                                                        \
  {                                                                           \
    u32 timer_reload =                                                        \
     timer[timer_number].reload << timer[timer_number].prescale;              \
    sound_update_frequency_step(timer_number);                                \
  }                                                                           \

#define adjust_sound_buffer(timer_number, channel)                            \
  if(timer[timer_number].direct_sound_channels & (0x01 << channel))           \
  {                                                                           \
    direct_sound_channel[channel].buffer_index =                              \
     (gbc_sound_buffer_index + buffer_adjust) % BUFFER_SIZE;                  \
  }                                                                           \

static void trigger_timer(u32 timer_number, u32 value)
{
   if (value & 0x80)
   {
      if(timer[timer_number].status == TIMER_INACTIVE)
      {
         u32 prescale = prescale_table[value & 0x03];
         u32 timer_reload = timer[timer_number].reload;

         if((value >> 2) & 0x01)
            timer[timer_number].status = TIMER_CASCADE;
         else
            timer[timer_number].status = TIMER_PRESCALE;

         timer[timer_number].prescale = prescale;
         timer[timer_number].irq = (value >> 6) & 0x01;

         address16(io_registers, 0x100 + (timer_number * 4)) = -timer_reload;

         timer_reload <<= prescale;
         timer[timer_number].count = timer_reload;

         if(timer_reload < execute_cycles)
            execute_cycles = timer_reload;

         if(timer_number < 2)
         {
            u32 buffer_adjust =
               (u32)(((float)(cpu_ticks - gbc_sound_last_cpu_ticks) *
                        sound_frequency) / GBC_BASE_RATE) * 2;

            sound_update_frequency_step(timer_number);
            adjust_sound_buffer(timer_number, 0);
            adjust_sound_buffer(timer_number, 1);
         }
      }
   }
   else
   {
      if(timer[timer_number].status != TIMER_INACTIVE)
      {
         timer[timer_number].status = TIMER_INACTIVE;
         timer[timer_number].stop_cpu_ticks = cpu_ticks;
      }
   }
   address16(io_registers, 0x102 + (timer_number * 4)) = value;
}

// This table is configured for sequential access on system defaults

u32 waitstate_cycles_sequential[16][3] =
{
  { 1, 1, 1 }, // BIOS
  { 1, 1, 1 }, // Invalid
  { 3, 3, 6 }, // EWRAM (default settings)
  { 1, 1, 1 }, // IWRAM
  { 1, 1, 1 }, // IO Registers
  { 1, 1, 2 }, // Palette RAM
  { 1, 1, 2 }, // VRAM
  { 1, 1, 2 }, // OAM
  { 3, 3, 6 }, // Gamepak (wait 0)
  { 3, 3, 6 }, // Gamepak (wait 0)
  { 5, 5, 9 }, // Gamepak (wait 1)
  { 5, 5, 9 }, // Gamepak (wait 1)
  { 9, 9, 17 }, // Gamepak (wait 2)
  { 9, 9, 17 }, // Gamepak (wait 2)
};

// Different settings for gamepak ws0-2 sequential (2nd) access

u32 gamepak_waitstate_sequential[2][3][3] =
{
  {
    { 3, 3, 6 },
    { 5, 5, 9 },
    { 9, 9, 17 }
  },
  {
    { 2, 2, 3 },
    { 2, 2, 3 },
    { 2, 2, 3 }
  }
};

u16 io_registers[1024 * 16];
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 vram[1024 * 96];
u8 bios_rom[1024 * 16];
u32 bios_read_protect;

// Up to 128kb, store SRAM, flash ROM, or EEPROM here.
u8 gamepak_backup[1024 * 128];

// Keeps us knowing how much we have left.
u8 *gamepak_rom;
u32 gamepak_size;

dma_transfer_type dma[4];

u8 *memory_regions[16];
u32 memory_limits[16];

typedef struct
{
  u32 page_timestamp;
  u32 physical_index;
} gamepak_swap_entry_type;

u32 gamepak_ram_buffer_size;
u32 gamepak_ram_pages;

// Enough to map the gamepak RAM space.
gamepak_swap_entry_type *gamepak_memory_map;

// This is global so that it can be kept open for large ROMs to swap
// pages from, so there's no slowdown with opening and closing the file
// a lot.
FILE *gamepak_file_large = NULL;

// Writes to these respective locations should trigger an update
// so the related subsystem may react to it.

// If GBC audio is written to:
u32 gbc_sound_update = 0;

// If the GBC audio waveform is modified:
u32 gbc_sound_wave_update = 0;

// Keep it 32KB until the upper 64KB is accessed, then make it 64KB.

backup_type_type backup_type = BACKUP_NONE;
sram_size_type sram_size = SRAM_SIZE_32KB;

flash_mode_type flash_mode = FLASH_BASE_MODE;
u32 flash_command_position = 0;
u8 *flash_bank_ptr = gamepak_backup;

flash_device_id_type flash_device_id = FLASH_DEVICE_MACRONIX_64KB;
flash_manufacturer_id_type flash_manufacturer_id =
 FLASH_MANUFACTURER_MACRONIX;
flash_size_type flash_size = FLASH_SIZE_64KB;

u8 read_backup(u32 address)
{
  u8 value = 0;

  if(backup_type == BACKUP_NONE)
    backup_type = BACKUP_SRAM;

  if(backup_type == BACKUP_SRAM)
    value = gamepak_backup[address];
  else if(flash_mode == FLASH_ID_MODE)
  {
    if (flash_size == FLASH_SIZE_128KB)
    {
      /* ID manufacturer type */
      if(address == 0x0000)
        value = FLASH_MANUFACTURER_MACRONIX;
      /* ID device type */
      else if(address == 0x0001)
        value = FLASH_DEVICE_MACRONIX_128KB;
    }
    else
    {
      /* ID manufacturer type */
      if(address == 0x0000)
        value = FLASH_MANUFACTURER_PANASONIC;
      /* ID device type */
      else if(address == 0x0001)
        value = FLASH_DEVICE_PANASONIC_64KB;
    }
  }
  else
    value = flash_bank_ptr[address];

  return value;
}

#define read_backup8()                                                        \
  value = read_backup(address & 0xFFFF)                                       \

#define read_backup16()                                                       \
  value = 0                                                                   \

#define read_backup32()                                                       \
  value = 0                                                                   \


// EEPROM is 512 bytes by default; it is autodetecte as 8KB if
// 14bit address DMAs are made (this is done in the DMA handler).

eeprom_size_type eeprom_size = EEPROM_512_BYTE;
eeprom_mode_type eeprom_mode = EEPROM_BASE_MODE;
u32 eeprom_address_length;
u32 eeprom_address = 0;
s32 eeprom_counter = 0;
u8 eeprom_buffer[8];

void function_cc write_eeprom(u32 unused_address, u32 value)
{
  switch(eeprom_mode)
  {
    case EEPROM_BASE_MODE:
      backup_type = BACKUP_EEPROM;
      eeprom_buffer[0] |= (value & 0x01) << (1 - eeprom_counter);
      eeprom_counter++;
      if(eeprom_counter == 2)
      {
        if(eeprom_size == EEPROM_512_BYTE)
          eeprom_address_length = 6;
        else
          eeprom_address_length = 14;

        eeprom_counter = 0;

        switch(eeprom_buffer[0] & 0x03)
        {
          case 0x02:
            eeprom_mode = EEPROM_WRITE_ADDRESS_MODE;
            break;

          case 0x03:
            eeprom_mode = EEPROM_ADDRESS_MODE;
            break;
        }
        address16(eeprom_buffer, 0) = 0;
      }
      break;

    case EEPROM_ADDRESS_MODE:
    case EEPROM_WRITE_ADDRESS_MODE:
      eeprom_buffer[eeprom_counter / 8]
       |= (value & 0x01) << (7 - (eeprom_counter % 8));
      eeprom_counter++;
      if(eeprom_counter == eeprom_address_length)
      {
        if(eeprom_size == EEPROM_512_BYTE)
        {
          eeprom_address =
           (address16(eeprom_buffer, 0) >> 2) * 8;
        }
        else
        {
          eeprom_address = (((u32)eeprom_buffer[1] >> 2) |
           ((u32)eeprom_buffer[0] << 6)) * 8;
        }

        address16(eeprom_buffer, 0) = 0;
        eeprom_counter = 0;

        if(eeprom_mode == EEPROM_ADDRESS_MODE)
          eeprom_mode = EEPROM_ADDRESS_FOOTER_MODE;
        else
        {
          eeprom_mode = EEPROM_WRITE_MODE;
          memset(gamepak_backup + eeprom_address, 0, 8);
        }
      }
      break;

    case EEPROM_WRITE_MODE:
      gamepak_backup[eeprom_address + (eeprom_counter / 8)] |=
       (value & 0x01) << (7 - (eeprom_counter % 8));
      eeprom_counter++;
      if(eeprom_counter == 64)
      {
        eeprom_counter = 0;
        eeprom_mode = EEPROM_WRITE_FOOTER_MODE;
      }
      break;

    case EEPROM_ADDRESS_FOOTER_MODE:
    case EEPROM_WRITE_FOOTER_MODE:
      eeprom_counter = 0;
      if(eeprom_mode == EEPROM_ADDRESS_FOOTER_MODE)
        eeprom_mode = EEPROM_READ_HEADER_MODE;
      else
        eeprom_mode = EEPROM_BASE_MODE;
      break;

    default:
      break;
  }
}

#define read_memory_gamepak(type)                                             \
  u32 gamepak_index = address >> 15;                                          \
  u8 *map = memory_map_read[gamepak_index];                                   \
                                                                              \
  if(!map)                                                             \
    map = load_gamepak_page(gamepak_index & 0x3FF);                           \
                                                                              \
  value = address##type(map, address & 0x7FFF)                                \

#define read_open8()                                                          \
  if(!(reg[REG_CPSR] & 0x20))                                                 \
    value = read_memory8(reg[REG_PC] + 4 + (address & 0x03));                 \
  else                                                                        \
    value = read_memory8(reg[REG_PC] + 2 + (address & 0x01))                  \

#define read_open16()                                                         \
  if(!(reg[REG_CPSR] & 0x20))                                                 \
    value = read_memory16(reg[REG_PC] + 4 + (address & 0x02));                \
  else                                                                        \
    value = read_memory16(reg[REG_PC] + 2)                                    \

#define read_open32()                                                         \
  if(!(reg[REG_CPSR] & 0x20))                                                 \
    value = read_memory32(reg[REG_PC] + 4);                                   \
  else                                                                        \
  {                                                                           \
    u32 current_instruction = read_memory16(reg[REG_PC] + 2);                 \
    value = current_instruction | (current_instruction << 16);                \
  }                                                                           \

u32 function_cc read_eeprom(void)
{
  u32 value;

  switch(eeprom_mode)
  {
    case EEPROM_BASE_MODE:
      value = 1;
      break;

    case EEPROM_READ_MODE:
      value = (gamepak_backup[eeprom_address + (eeprom_counter / 8)] >>
       (7 - (eeprom_counter % 8))) & 0x01;
      eeprom_counter++;
      if(eeprom_counter == 64)
      {
        eeprom_counter = 0;
        eeprom_mode = EEPROM_BASE_MODE;
      }
      break;

    case EEPROM_READ_HEADER_MODE:
      value = 0;
      eeprom_counter++;
      if(eeprom_counter == 4)
      {
        eeprom_mode = EEPROM_READ_MODE;
        eeprom_counter = 0;
      }
      break;

    default:
      value = 0;
      break;
  }

  return value;
}


#define read_memory(type)                                                     \
  switch(address >> 24)                                                       \
  {                                                                           \
    case 0x00:                                                                \
      /* BIOS */                                                              \
      if(reg[REG_PC] >= 0x4000)                                               \
        value = address##type(&bios_read_protect, address & 0x03);            \
      else                                                                    \
        value = address##type(bios_rom, address & 0x3FFF);                    \
      break;                                                                  \
                                                                              \
    case 0x02:                                                                \
      /* external work RAM */                                                 \
      value = address##type(ewram, (address & 0x3FFFF));                      \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      /* internal work RAM */                                                 \
      value = address##type(iwram, (address & 0x7FFF) + 0x8000);              \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      /* I/O registers */                                                     \
      value = address##type(io_registers, address & 0x3FF);                   \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
      /* palette RAM */                                                       \
      value = address##type(palette_ram, address & 0x3FF);                    \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      /* VRAM */                                                              \
      address &= 0x1FFFF;                                                     \
      if(address > 0x18000)                                                   \
        address -= 0x8000;                                                    \
                                                                              \
      value = address##type(vram, address);                                   \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      /* OAM RAM */                                                           \
      value = address##type(oam_ram, address & 0x3FF);                        \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
    case 0x09:                                                                \
    case 0x0A:                                                                \
    case 0x0B:                                                                \
    case 0x0C:                                                                \
      /* gamepak ROM */                                                       \
      if((address & 0x1FFFFFF) >= gamepak_size)                               \
        value = 0;                                                            \
      else                                                                    \
      {                                                                       \
        read_memory_gamepak(type);                                            \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      if((address & 0x1FFFFFF) < gamepak_size)                                \
      {                                                                       \
        read_memory_gamepak(type);                                            \
      }                                                                       \
      else                                                                    \
        value = read_eeprom();                                                \
                                                                              \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
    case 0x0F:                                                                \
      read_backup##type();                                                    \
      break;                                                                  \
                                                                              \
    default:                                                                  \
      read_open##type();                                                      \
      break;                                                                  \
  }                                                                           \

static cpu_alert_type trigger_dma(u32 dma_number, u32 value)
{
  if(value & 0x8000)
  {
    if(dma[dma_number].start_type == DMA_INACTIVE)
    {
      u32 start_type = (value >> 12) & 0x03;
      u32 dest_address = address32(io_registers, (dma_number * 12) + 0xB4) &
       0xFFFFFFF;

      dma[dma_number].dma_channel = dma_number;
      dma[dma_number].source_address =
       address32(io_registers, (dma_number * 12) + 0xB0) & 0xFFFFFFF;
      dma[dma_number].dest_address = dest_address;
      dma[dma_number].source_direction = (value >>  7) & 0x03;
      dma[dma_number].repeat_type = (value >> 9) & 0x01;
      dma[dma_number].start_type = start_type;
      dma[dma_number].irq = (value >> 14) & 0x01;

      /* If it is sound FIFO DMA make sure the settings are a certain way */
      if((dma_number >= 1) && (dma_number <= 2) &&
       (start_type == DMA_START_SPECIAL))
      {
        dma[dma_number].length_type = DMA_32BIT;
        dma[dma_number].length = 4;
        dma[dma_number].dest_direction = DMA_FIXED;
        if(dest_address == 0x40000A4)
          dma[dma_number].direct_sound_channel = DMA_DIRECT_SOUND_B;
        else
          dma[dma_number].direct_sound_channel = DMA_DIRECT_SOUND_A;
      }
      else
      {
        u32 length = address16(io_registers, (dma_number * 12) + 0xB8);

        if((dma_number == 3) && ((dest_address >> 24) == 0x0D) &&
         ((length & 0x1F) == 17))
          eeprom_size = EEPROM_8_KBYTE;

        if(dma_number < 3)
          length &= 0x3FFF;

        if(length == 0)
        {
          if(dma_number == 3)
            length = 0x10000;
          else
            length = 0x04000;
        }

        dma[dma_number].length = length;
        dma[dma_number].length_type = (value >> 10) & 0x01;
        dma[dma_number].dest_direction = (value >> 5) & 0x03;
      }

      address16(io_registers, (dma_number * 12) + 0xBA) = value;
      if(start_type == DMA_START_IMMEDIATELY)
        return dma_transfer(dma + dma_number);
    }
  }
  else
  {
    dma[dma_number].start_type = DMA_INACTIVE;
    dma[dma_number].direct_sound_channel = DMA_NO_DIRECT_SOUND;
    address16(io_registers, (dma_number * 12) + 0xBA) = value;
  }

  return CPU_ALERT_NONE;
}


#define access_register8_high(address)                                        \
  value = (value << 8) | (address8(io_registers, address))                    \

#define access_register8_low(address)                                         \
  value = ((address8(io_registers, address + 1)) << 8) | value                \

#define access_register16_high(address)                                       \
  value = (value << 16) | (address16(io_registers, address))                  \

#define access_register16_low(address)                                        \
  value = ((address16(io_registers, address + 2)) << 16) | value              \

cpu_alert_type function_cc write_io_register8(u32 address, u32 value)
{
  value &= 0xff;
  switch(address)
  {
    case 0x00:
    {
      u32 dispcnt = io_registers[REG_DISPCNT];

      if((value & 0x07) != (dispcnt & 0x07))
        reg[OAM_UPDATED] = 1;

      address8(io_registers, 0x00) = value;
      break;
    }

    // DISPSTAT (lower byte)
    case 0x04:
      address8(io_registers, 0x04) =
       (address8(io_registers, 0x04) & 0x07) | (value & ~0x07);
      break;

    // VCOUNT
    case 0x06:
    case 0x07:
      break;

    // BG2 reference X
    case 0x28:
      access_register8_low(0x28);
      access_register16_low(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    case 0x29:
      access_register8_high(0x28);
      access_register16_low(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    case 0x2A:
      access_register8_low(0x2A);
      access_register16_high(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    case 0x2B:
      access_register8_high(0x2A);
      access_register16_high(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    // BG2 reference Y
    case 0x2C:
      access_register8_low(0x2C);
      access_register16_low(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    case 0x2D:
      access_register8_high(0x2C);
      access_register16_low(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    case 0x2E:
      access_register8_low(0x2E);
      access_register16_high(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    case 0x2F:
      access_register8_high(0x2E);
      access_register16_high(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    // BG3 reference X
    case 0x38:
      access_register8_low(0x38);
      access_register16_low(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    case 0x39:
      access_register8_high(0x38);
      access_register16_low(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    case 0x3A:
      access_register8_low(0x3A);
      access_register16_high(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    case 0x3B:
      access_register8_high(0x3A);
      access_register16_high(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    // BG3 reference Y
    case 0x3C:
      access_register8_low(0x3C);
      access_register16_low(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    case 0x3D:
      access_register8_high(0x3C);
      access_register16_low(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    case 0x3E:
      access_register8_low(0x3E);
      access_register16_high(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    case 0x3F:
      access_register8_high(0x3E);
      access_register16_high(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    // Sound 1 control sweep
    case 0x60:
      access_register8_low(0x60);
      gbc_sound_tone_control_sweep();
      break;

    case 0x61:
      access_register8_low(0x60);
      gbc_sound_tone_control_sweep();
      break;

    // Sound 1 control duty/length/envelope
    case 0x62:
      access_register8_low(0x62);
      gbc_sound_tone_control_low(0, 0x62);
      break;

    case 0x63:
      access_register8_high(0x62);
      gbc_sound_tone_control_low(0, 0x62);
      break;

    // Sound 1 control frequency
    case 0x64:
      access_register8_low(0x64);
      gbc_sound_tone_control_high(0, 0x64);
      break;

    case 0x65:
      access_register8_high(0x64);
      gbc_sound_tone_control_high(0, 0x64);
      break;

    // Sound 2 control duty/length/envelope
    case 0x68:
      access_register8_low(0x68);
      gbc_sound_tone_control_low(1, 0x68);
      break;

    case 0x69:
      access_register8_high(0x68);
      gbc_sound_tone_control_low(1, 0x68);
      break;

    // Sound 2 control frequency
    case 0x6C:
      access_register8_low(0x6C);
      gbc_sound_tone_control_high(1, 0x6C);
      break;

    case 0x6D:
      access_register8_high(0x6C);
      gbc_sound_tone_control_high(1, 0x6C);
      break;

    // Sound 3 control wave
    case 0x70:
      access_register8_low(0x70);
      gbc_sound_wave_control();
      break;

    case 0x71:
      access_register8_high(0x70);
      gbc_sound_wave_control();
      break;

    // Sound 3 control length/volume
    case 0x72:
      access_register8_low(0x72);
      gbc_sound_tone_control_low_wave();
      break;

    case 0x73:
      access_register8_high(0x72);
      gbc_sound_tone_control_low_wave();
      break;

    // Sound 3 control frequency
    case 0x74:
      access_register8_low(0x74);
      gbc_sound_tone_control_high_wave();
      break;

    case 0x75:
      access_register8_high(0x74);
      gbc_sound_tone_control_high_wave();
      break;

    // Sound 4 control length/envelope
    case 0x78:
      access_register8_low(0x78);
      gbc_sound_tone_control_low(3, 0x78);
      break;

    case 0x79:
      access_register8_high(0x78);
      gbc_sound_tone_control_low(3, 0x78);
      break;

    // Sound 4 control frequency
    case 0x7C:
      access_register8_low(0x7C);
      gbc_sound_noise_control();
      break;

    case 0x7D:
      access_register8_high(0x7C);
      gbc_sound_noise_control();
      break;

    // Sound control L
    case 0x80:
      access_register8_low(0x80);
      gbc_trigger_sound(value);
      break;

    case 0x81:
      access_register8_high(0x80);
      gbc_trigger_sound(value);
      break;

    // Sound control H
    case 0x82:
      access_register8_low(0x82);
      trigger_sound();
      break;

    case 0x83:
      access_register8_high(0x82);
      trigger_sound();
      break;

    // Sound control X
    case 0x84:
      sound_control_x(value);
      break;

    // Sound wave RAM
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0x9D:
    case 0x9E:
    case 0x9F:
      gbc_sound_wave_update = 1;
      address8(io_registers, address) = value;
      break;

    // Sound FIFO A
    case 0xA0:
      sound_timer_queue8(0, value);
      break;

    // Sound FIFO B
    case 0xA4:
      sound_timer_queue8(1, value);
      break;

    // DMA control (trigger byte)
    case 0xBB:
      access_register8_low(0xBA);
      return trigger_dma(0, value);

    case 0xC7:
      access_register8_low(0xC6);
      return trigger_dma(1, value);

    case 0xD3:
      access_register8_low(0xD2);
      return trigger_dma(2, value);

    case 0xDF:
      access_register8_low(0xDE);
      return trigger_dma(3, value);

    // Timer counts
    case 0x100:
      access_register8_low(0x100);
      count_timer(0);
      break;

    case 0x101:
      access_register8_high(0x100);
      count_timer(0);
      break;

    case 0x104:
      access_register8_low(0x104);
      count_timer(1);
      break;

    case 0x105:
      access_register8_high(0x104);
      count_timer(1);
      break;

    case 0x108:
      access_register8_low(0x108);
      count_timer(2);
      break;

    case 0x109:
      access_register8_high(0x108);
      count_timer(2);
      break;

    case 0x10C:
      access_register8_low(0x10C);
      count_timer(3);
      break;

    case 0x10D:
      access_register8_high(0x10C);
      count_timer(3);
      break;

    // Timer control (trigger byte)
    case 0x103:
      access_register8_low(0x102);
      trigger_timer(0, value);
      break;

    case 0x107:
      access_register8_low(0x106);
      trigger_timer(1, value);
      break;

    case 0x10B:
      access_register8_low(0x10A);
      trigger_timer(2, value);
      break;

    case 0x10F:
      access_register8_low(0x10E);
      trigger_timer(3, value);
      break;

    // IF
    case 0x202:
      address8(io_registers, 0x202) &= ~value;
      break;

    case 0x203:
      address8(io_registers, 0x203) &= ~value;
      break;

    // Halt
    case 0x301:
      if((value & 0x01) == 0)
        reg[CPU_HALT_STATE] = CPU_HALT;
      else
        reg[CPU_HALT_STATE] = CPU_STOP;

      return CPU_ALERT_HALT;
      break;

    default:
      address8(io_registers, address) = value;
      break;
  }

  return CPU_ALERT_NONE;
}

cpu_alert_type function_cc write_io_register16(u32 address, u32 value)
{
  value &= 0xffff;
  switch(address)
  {
    case 0x00:
    {
      u32 dispcnt = io_registers[REG_DISPCNT];
      if((value & 0x07) != (dispcnt & 0x07))
        reg[OAM_UPDATED] = 1;

      address16(io_registers, 0x00) = value;
      break;
    }

    // DISPSTAT
    case 0x04:
      address16(io_registers, 0x04) =
       (address16(io_registers, 0x04) & 0x07) | (value & ~0x07);
      break;

    // VCOUNT
    case 0x06:
      break;

    // BG2 reference X
    case 0x28:
      access_register16_low(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    case 0x2A:
      access_register16_high(0x28);
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    // BG2 reference Y
    case 0x2C:
      access_register16_low(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    case 0x2E:
      access_register16_high(0x2C);
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    // BG3 reference X

    case 0x38:
      access_register16_low(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    case 0x3A:
      access_register16_high(0x38);
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    // BG3 reference Y
    case 0x3C:
      access_register16_low(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    case 0x3E:
      access_register16_high(0x3C);
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    // Sound 1 control sweep
    case 0x60:
      gbc_sound_tone_control_sweep();
      break;

    // Sound 1 control duty/length/envelope
    case 0x62:
      gbc_sound_tone_control_low(0, 0x62);
      break;

    // Sound 1 control frequency
    case 0x64:
      gbc_sound_tone_control_high(0, 0x64);
      break;

    // Sound 2 control duty/length/envelope
    case 0x68:
      gbc_sound_tone_control_low(1, 0x68);
      break;

    // Sound 2 control frequency
    case 0x6C:
      gbc_sound_tone_control_high(1, 0x6C);
      break;

    // Sound 3 control wave
    case 0x70:
      gbc_sound_wave_control();
      break;

    // Sound 3 control length/volume
    case 0x72:
      gbc_sound_tone_control_low_wave();
      break;

    // Sound 3 control frequency
    case 0x74:
      gbc_sound_tone_control_high_wave();
      break;

    // Sound 4 control length/envelope
    case 0x78:
      gbc_sound_tone_control_low(3, 0x78);
      break;

    // Sound 4 control frequency
    case 0x7C:
      gbc_sound_noise_control();
      break;

    // Sound control L
    case 0x80:
      gbc_trigger_sound(value);
      break;

    // Sound control H
    case 0x82:
      trigger_sound();
      break;

    // Sound control X
    case 0x84:
      sound_control_x(value);
      break;

    // Sound wave RAM
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0x9D:
    case 0x9E:
      gbc_sound_wave_update = 1;
      address16(io_registers, address) = value;
      break;

    // Sound FIFO A
    case 0xA0:
      sound_timer_queue16(0, value);
      break;

    // Sound FIFO B
    case 0xA4:
      sound_timer_queue16(1, value);
      break;

    // DMA control
    case 0xBA:
      return trigger_dma(0, value);

    case 0xC6:
      return trigger_dma(1, value);

    case 0xD2:
      return trigger_dma(2, value);

    case 0xDE:
      return trigger_dma(3, value);

    // Timer counts
    case 0x100:
      count_timer(0);
      break;

    case 0x104:
      count_timer(1);
      break;

    case 0x108:
      count_timer(2);
      break;

    case 0x10C:
      count_timer(3);
      break;

    /* Timer control 0 */
    case 0x102:
      trigger_timer(0, value);
      break;

    /* Timer control 1 */
    case 0x106:
      trigger_timer(1, value);
      break;

    /* Timer control 2 */
    case 0x10A:
      trigger_timer(2, value);
      break;

    /* Timer control 3 */
    case 0x10E:
      trigger_timer(3, value);
      break;

    // P1
    case 0x130:
      break;

    // Interrupt flag
    case 0x202:
      address16(io_registers, 0x202) &= ~value;
      break;

    // WAITCNT
    case 0x204:
      break;

    // Halt
    case 0x300:
      if(((value >> 8) & 0x01) == 0)
        reg[CPU_HALT_STATE] = CPU_HALT;
      else
        reg[CPU_HALT_STATE] = CPU_STOP;

      return CPU_ALERT_HALT;

    default:
      address16(io_registers, address) = value;
      break;
  }

  return CPU_ALERT_NONE;
}


cpu_alert_type function_cc write_io_register32(u32 address, u32 value)
{
  switch(address)
  {
    // BG2 reference X
    case 0x28:
      affine_reference_x[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x28) = value;
      break;

    // BG2 reference Y
    case 0x2C:
      affine_reference_y[0] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x2C) = value;
      break;

    // BG3 reference X
    case 0x38:
      affine_reference_x[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x38) = value;
      break;

    // BG3 reference Y
    case 0x3C:
      affine_reference_y[1] = (s32)(value << 4) >> 4;
      address32(io_registers, 0x3C) = value;
      break;

    // Sound FIFO A
    case 0xA0:
      sound_timer_queue32(0, value);
      break;

    // Sound FIFO B
    case 0xA4:
      sound_timer_queue32(1, value);
      break;

    default:
    {
      cpu_alert_type alert_low =
        write_io_register16(address, value & 0xFFFF);

      cpu_alert_type alert_high =
        write_io_register16(address + 2, value >> 16);

      if(alert_high)
        return alert_high;

      return alert_low;
    }
  }

  return CPU_ALERT_NONE;
}

#define write_palette8(address, value)                                        \

#define write_palette16(address, value)                                       \
{                                                                             \
  u32 palette_address = address;                                              \
  address16(palette_ram, palette_address) = value;                            \
  convert_palette(value);                                                     \
  address16(palette_ram_converted, palette_address) = value;                  \
}                                                                             \

#define write_palette32(address, value)                                       \
{                                                                             \
  u32 palette_address = address;                                              \
  u32 value_high = value >> 16;                                               \
  u32 value_low = value & 0xFFFF;                                             \
  address32(palette_ram, palette_address) = value;                            \
  convert_palette(value_high);                                                \
  convert_palette(value_low);                                                 \
  value = (value_high << 16) | value_low;                                     \
  address32(palette_ram_converted, palette_address) = value;                  \
}                                                                             \


void function_cc write_backup(u32 address, u32 value)
{
  value &= 0xFF;

  if(backup_type == BACKUP_NONE)
    backup_type = BACKUP_SRAM;


  // gamepak SRAM or Flash ROM
  if((address == 0x5555) && (flash_mode != FLASH_WRITE_MODE))
  {
    if((flash_command_position == 0) && (value == 0xAA))
    {
      backup_type = BACKUP_FLASH;
      flash_command_position = 1;
    }

    if(flash_command_position == 2)
    {
      switch(value)
      {
        case 0x90:
          // Enter ID mode, this also tells the emulator that we're using
          // flash, not SRAM

          if(flash_mode == FLASH_BASE_MODE)
            flash_mode = FLASH_ID_MODE;

          break;

        case 0x80:
          // Enter erase mode
          if(flash_mode == FLASH_BASE_MODE)
            flash_mode = FLASH_ERASE_MODE;
          break;

        case 0xF0:
          // Terminate ID mode
          if(flash_mode == FLASH_ID_MODE)
            flash_mode = FLASH_BASE_MODE;
          break;

        case 0xA0:
          // Write mode
          if(flash_mode == FLASH_BASE_MODE)
            flash_mode = FLASH_WRITE_MODE;
          break;

        case 0xB0:
          // Bank switch
          // Here the chip is now officially 128KB.
          flash_size = FLASH_SIZE_128KB;
          if(flash_mode == FLASH_BASE_MODE)
            flash_mode = FLASH_BANKSWITCH_MODE;
          break;

        case 0x10:
          // Erase chip
          if(flash_mode == FLASH_ERASE_MODE)
          {
            if(flash_size == FLASH_SIZE_64KB)
              memset(gamepak_backup, 0xFF, 1024 * 64);
            else
              memset(gamepak_backup, 0xFF, 1024 * 128);
            flash_mode = FLASH_BASE_MODE;
          }
          break;

        default:
          break;
      }
      flash_command_position = 0;
    }
    if(backup_type == BACKUP_SRAM)
      gamepak_backup[0x5555] = value;
  }
  else

  if((address == 0x2AAA) && (value == 0x55) &&
   (flash_command_position == 1))
    flash_command_position = 2;
  else
  {
    if((flash_command_position == 2) &&
     (flash_mode == FLASH_ERASE_MODE) && (value == 0x30))
    {
      // Erase sector
      memset(flash_bank_ptr + (address & 0xF000), 0xFF, 1024 * 4);
      flash_mode = FLASH_BASE_MODE;
      flash_command_position = 0;
    }
    else

    if((flash_command_position == 0) &&
     (flash_mode == FLASH_BANKSWITCH_MODE) && (address == 0x0000) &&
     (flash_size == FLASH_SIZE_128KB))
    {
      flash_bank_ptr = gamepak_backup + ((value & 0x01) * (1024 * 64));
      flash_mode = FLASH_BASE_MODE;
    }
    else

    if((flash_command_position == 0) && (flash_mode == FLASH_WRITE_MODE))
    {
      // Write value to flash ROM
      flash_bank_ptr[address] = value;
      flash_mode = FLASH_BASE_MODE;
    }
    else

    if(backup_type == BACKUP_SRAM)
    {
      // Write value to SRAM
      // Hit 64KB territory?
      if(address >= 0x8000)
        sram_size = SRAM_SIZE_64KB;
      gamepak_backup[address] = value;
    }
  }
}

#define write_backup8()                                                       \
  write_backup(address & 0xFFFF, value)                                       \

#define write_backup16()                                                      \

#define write_backup32()                                                      \

#define write_vram8()                                                         \
  address &= ~0x01;                                                           \
  address16(vram, address) = ((value << 8) | value)                           \

#define write_vram16()                                                        \
  address16(vram, address) = value                                            \

#define write_vram32()                                                        \
  address32(vram, address) = value                                            \

// RTC code derived from VBA's (due to lack of any real publically available
// documentation...)

typedef enum
{
  RTC_DISABLED,
  RTC_IDLE,
  RTC_COMMAND,
  RTC_OUTPUT_DATA,
  RTC_INPUT_DATA
} rtc_state_type;

typedef enum
{
  RTC_COMMAND_RESET            = 0x60,
  RTC_COMMAND_WRITE_STATUS     = 0x62,
  RTC_COMMAND_READ_STATUS      = 0x63,
  RTC_COMMAND_OUTPUT_TIME_FULL = 0x65,
  RTC_COMMAND_OUTPUT_TIME      = 0x67
} rtc_command_type;

typedef enum
{
  RTC_WRITE_TIME,
  RTC_WRITE_TIME_FULL,
  RTC_WRITE_STATUS
} rtc_write_mode_type;

rtc_state_type rtc_state = RTC_DISABLED;
rtc_write_mode_type rtc_write_mode;
u8 rtc_registers[3];
u32 rtc_command;
u32 rtc_data[12];
u32 rtc_status = 0x40;
u32 rtc_data_bytes;
s32 rtc_bit_count;

static u32 encode_bcd(u8 value)
{
  int l = 0;
  int h = 0;

  value = value % 100;
  l = value % 10;
  h = value / 10;

  return h * 16 + l;
}

#define write_rtc_register(index, _value)                                     \
  update_address = 0x80000C4 + (index * 2);                                   \
  rtc_registers[index] = _value;                                              \
  rtc_page_index = update_address >> 15;                                      \
  map = memory_map_read[rtc_page_index];                                      \
                                                                              \
  if(!map)                                                                    \
    map = load_gamepak_page(rtc_page_index & 0x3FF);                          \
                                                                              \
  address16(map, update_address & 0x7FFF) = _value                            \

void function_cc write_rtc(u32 address, u32 value)
{
  u32 rtc_page_index;
  u32 update_address;
  u8 *map;

  value &= 0xFFFF;

  switch(address)
  {
    // RTC command
    // Bit 0: SCHK, perform action
    // Bit 1: IO, input/output command data
    // Bit 2: CS, select input/output? If high make I/O write only
    case 0xC4:
      if(rtc_state == RTC_DISABLED)
        rtc_state = RTC_IDLE;
      if(!(rtc_registers[0] & 0x04))
        value = (rtc_registers[0] & 0x02) | (value & ~0x02);
      if(rtc_registers[2] & 0x01)
      {
        // To begin writing a command 1, 5 must be written to the command
        // registers.
        if((rtc_state == RTC_IDLE) && (rtc_registers[0] == 0x01) &&
         (value == 0x05))
        {
          // We're now ready to begin receiving a command.
          write_rtc_register(0, value);
          rtc_state = RTC_COMMAND;
          rtc_command = 0;
          rtc_bit_count = 7;
        }
        else
        {
          write_rtc_register(0, value);
          switch(rtc_state)
          {
            // Accumulate RTC command by receiving the next bit, and if we
            // have accumulated enough bits to form a complete command
            // execute it.
            case RTC_COMMAND:
              if(rtc_registers[0] & 0x01)
              {
                rtc_command |= ((value & 0x02) >> 1) << rtc_bit_count;
                rtc_bit_count--;
              }

              // Have we received a full RTC command? If so execute it.
              if(rtc_bit_count < 0)
              {
                switch(rtc_command)
                {
                  // Resets RTC
                  case RTC_COMMAND_RESET:
                    rtc_state = RTC_IDLE;
                    memset(rtc_registers, 0, sizeof(rtc_registers));
                    break;

                  // Sets status of RTC
                  case RTC_COMMAND_WRITE_STATUS:
                    rtc_state = RTC_INPUT_DATA;
                    rtc_data_bytes = 1;
                    rtc_write_mode = RTC_WRITE_STATUS;
                    break;

                  // Outputs current status of RTC
                  case RTC_COMMAND_READ_STATUS:
                    rtc_state = RTC_OUTPUT_DATA;
                    rtc_data_bytes = 1;
                    rtc_data[0] = rtc_status;
                    break;

                  // Actually outputs the time, all of it
                  case RTC_COMMAND_OUTPUT_TIME_FULL:
                  {
                    struct tm *current_time;
                    time_t current_time_flat;

                    time(&current_time_flat);
                    current_time = localtime(&current_time_flat);

                    rtc_state = RTC_OUTPUT_DATA;
                    rtc_data_bytes = 7;
                    rtc_data[0] = encode_bcd(current_time->tm_year);
                    rtc_data[1] = encode_bcd(current_time->tm_mon + 1);
                    rtc_data[2] = encode_bcd(current_time->tm_mday);
                    rtc_data[3] = encode_bcd(current_time->tm_wday);
                    rtc_data[4] = encode_bcd(current_time->tm_hour);
                    rtc_data[5] = encode_bcd(current_time->tm_min);
                    rtc_data[6] = encode_bcd(current_time->tm_sec);

                    break;
                  }

                  // Only outputs the current time of day.
                  case RTC_COMMAND_OUTPUT_TIME:
                  {
                    struct tm *current_time;
                    time_t current_time_flat;

                    time(&current_time_flat);
                    current_time = localtime(&current_time_flat);

                    rtc_state = RTC_OUTPUT_DATA;
                    rtc_data_bytes = 3;
                    rtc_data[0] = encode_bcd(current_time->tm_hour);
                    rtc_data[1] = encode_bcd(current_time->tm_min);
                    rtc_data[2] = encode_bcd(current_time->tm_sec);
                    break;
                  }
                }
                rtc_bit_count = 0;
              }
              break;

            // Receive parameters from the game as input to the RTC
            // for a given command. Read one bit at a time.
            case RTC_INPUT_DATA:
              // Bit 1 of parameter A must be high for input
              if(rtc_registers[1] & 0x02)
              {
                // Read next bit for input
                if(!(value & 0x01))
                {
                  rtc_data[rtc_bit_count >> 3] |=
                   ((value & 0x01) << (7 - (rtc_bit_count & 0x07)));
                }
                else
                {
                  rtc_bit_count++;

                  if(rtc_bit_count == (rtc_data_bytes * 8))
                  {
                    rtc_state = RTC_IDLE;
                    switch(rtc_write_mode)
                    {
                      case RTC_WRITE_STATUS:
                        rtc_status = rtc_data[0];
                        break;

                      default:
                        break;
                    }
                  }
                }
              }
              break;

            case RTC_OUTPUT_DATA:
              // Bit 1 of parameter A must be low for output
              if(!(rtc_registers[1] & 0x02))
              {
                // Write next bit to output, on bit 1 of parameter B
                if(!(value & 0x01))
                {
                  u8 current_output_byte = rtc_registers[2];

                  current_output_byte =
                   (current_output_byte & ~0x02) |
                   (((rtc_data[rtc_bit_count >> 3] >>
                   (rtc_bit_count & 0x07)) & 0x01) << 1);

                  write_rtc_register(0, current_output_byte);

                }
                else
                {
                  rtc_bit_count++;

                  if(rtc_bit_count == (rtc_data_bytes * 8))
                  {
                    rtc_state = RTC_IDLE;
                    memset(rtc_registers, 0, sizeof(rtc_registers));
                  }
                }
              }
              break;

            default:
              break;
          }
        }
      }
      else
      {
        write_rtc_register(2, value);
      }
      break;

    // Write parameter A
    case 0xC6:
      write_rtc_register(1, value);
      break;

    // Write parameter B
    case 0xC8:
      write_rtc_register(2, value);
      break;
  }
}

#define write_rtc8()                                                          \

#define write_rtc16()                                                         \
  write_rtc(address & 0xFF, value)                                            \

#define write_rtc32()                                                         \

#define write_memory(type)                                                    \
  switch(address >> 24)                                                       \
  {                                                                           \
    case 0x02:                                                                \
      /* external work RAM */                                                 \
      address##type(ewram, (address & 0x3FFFF)) = value;                      \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      /* internal work RAM */                                                 \
      address##type(iwram, (address & 0x7FFF) + 0x8000) = value;              \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      /* I/O registers */                                                     \
      return write_io_register##type(address & 0x3FF, value);                 \
                                                                              \
    case 0x05:                                                                \
      /* palette RAM */                                                       \
      write_palette##type(address & 0x3FF, value);                            \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      /* VRAM */                                                              \
      address &= 0x1FFFF;                                                     \
      if(address >= 0x18000)                                                  \
        address -= 0x8000;                                                    \
                                                                              \
      write_vram##type();                                                     \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      /* OAM RAM */                                                           \
      reg[OAM_UPDATED] = 1;                                                   \
      address##type(oam_ram, address & 0x3FF) = value;                        \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
      /* gamepak ROM or RTC */                                                \
      write_rtc##type();                                                      \
      break;                                                                  \
                                                                              \
    case 0x09:                                                                \
    case 0x0A:                                                                \
    case 0x0B:                                                                \
    case 0x0C:                                                                \
      /* gamepak ROM space */                                                 \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      write_eeprom(address, value);                                           \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
      write_backup##type();                                                   \
      break;                                                                  \
  }                                                                           \

u8 function_cc read_memory8(u32 address)
{
  u8 value;
  read_memory(8);
  return value;
}

u32 read_memory8s(u32 address) {
  return (u32)((s8)read_memory8(address));
}

u16 function_cc read_memory16_signed(u32 address)
{
  u16 value;

  if(address & 0x01)
    return (s8)read_memory8(address);

  read_memory(16);

  return value;
}

u32 read_memory16s(u32 address) {
  return (u32)((s16)read_memory16_signed(address));
}

// unaligned reads are actually 32bit

u32 function_cc read_memory16(u32 address)
{
  u32 value;
  bool unaligned = (address & 0x01);
  address &= ~0x01;
  read_memory(16);
  if (unaligned) {
    ror(value, value, 8);
  }

  return value;
}


u32 function_cc read_memory32(u32 address)
{
  u32 value;
  u32 rotate = (address & 0x03) * 8;
  address &= ~0x03;
  read_memory(32);
  ror(value, value, rotate);
  return value;
}

cpu_alert_type function_cc write_memory8(u32 address, u8 value)
{
  write_memory(8);
  return CPU_ALERT_NONE;
}

cpu_alert_type function_cc write_memory16(u32 address, u16 value)
{
  write_memory(16);
  return CPU_ALERT_NONE;
}

cpu_alert_type function_cc write_memory32(u32 address, u32 value)
{
  write_memory(32);
  return CPU_ALERT_NONE;
}

char backup_filename[512];

u32 load_backup(char *name)
{
  FILE *fd = fopen(name, "rb");

  if(fd)
  {
    u32 backup_size = file_length(fd);

    fread(gamepak_backup, 1, backup_size, fd);
    fclose(fd);

    // The size might give away what kind of backup it is.
    switch(backup_size)
    {
      case 0x200:
        backup_type = BACKUP_EEPROM;
        eeprom_size = EEPROM_512_BYTE;
        break;

      case 0x2000:
        backup_type = BACKUP_EEPROM;
        eeprom_size = EEPROM_8_KBYTE;
        break;

      case 0x8000:
        backup_type = BACKUP_SRAM;
        sram_size = SRAM_SIZE_32KB;
        break;

      // Could be either flash or SRAM, go with flash
      case 0x10000:
        backup_type = BACKUP_FLASH;
        sram_size = (sram_size_type)FLASH_SIZE_64KB;
        break;

      case 0x20000:
        backup_type = BACKUP_FLASH;
        flash_size = FLASH_SIZE_128KB;
        break;
    }
    return 1;
  }
  else
  {
    backup_type = BACKUP_NONE;
    memset(gamepak_backup, 0xFF, 1024 * 128);
  }

  return 0;
}

u32 save_backup(char *name)
{
  if(backup_type != BACKUP_NONE)
  {
    FILE *fd = fopen(name, "wb");

    if(fd)
    {
      u32 backup_size = 0;

      switch(backup_type)
      {
        case BACKUP_SRAM:
          if(sram_size == SRAM_SIZE_32KB)
            backup_size = 0x8000;
          else
            backup_size = 0x10000;
          break;

        case BACKUP_FLASH:
          if(flash_size == FLASH_SIZE_64KB)
            backup_size = 0x10000;
          else
            backup_size = 0x20000;
          break;

        case BACKUP_EEPROM:
          if(eeprom_size == EEPROM_512_BYTE)
            backup_size = 0x200;
          else
            backup_size = 0x2000;
          break;

        default:
          break;
      }

      fwrite(gamepak_backup, 1, backup_size, fd);
      fclose(fd);
      return 1;
    }
  }

  return 0;
}

void update_backup(void)
{
  if (!use_libretro_save_method)
    save_backup(backup_filename);
}

#define CONFIG_FILENAME "game_config.txt"

static char *skip_spaces(char *line_ptr)
{
  while(*line_ptr == ' ')
    line_ptr++;

  return line_ptr;
}

static s32 parse_config_line(char *current_line, char *current_variable, char *current_value)
{
  char *line_ptr = current_line;
  char *line_ptr_new;

  if((current_line[0] == 0) || (current_line[0] == '#'))
    return -1;

  line_ptr_new = strchr(line_ptr, ' ');
  if(!line_ptr_new)
    return -1;

  *line_ptr_new = 0;
  strcpy(current_variable, line_ptr);
  line_ptr_new = skip_spaces(line_ptr_new + 1);

  if(*line_ptr_new != '=')
    return -1;

  line_ptr_new = skip_spaces(line_ptr_new + 1);
  strcpy(current_value, line_ptr_new);
  line_ptr_new = current_value + strlen(current_value) - 1;
  if(*line_ptr_new == '\n')
  {
    line_ptr_new--;
    *line_ptr_new = 0;
  }

  if(*line_ptr_new == '\r')
    *line_ptr_new = 0;

  return 0;
}

typedef struct
{
   char romtitle[256];
   char gamepak_title[256];
   char gamepak_code[256];
   char gamepak_maker[256];
   int flash_size;
   flash_device_id_type flash_device_id;
   int save_type;
   int rtc_enabled;
   int mirroring_enabled;
   int use_bios;
   u32 idle_loop_target_pc;
   u32 iwram_stack_optimize;
   u32 translation_gate_target_1;
   u32 translation_gate_target_2;
   u32 translation_gate_target_3;
} ini_t;

#include "gba_over.h"

static s32 load_game_config_over(char *gamepak_title, char *gamepak_code, char *gamepak_maker)
{
  unsigned i = 0;

  for (i = 0; i < 256; i++)
  {
     if (gbaover[i].romtitle[0] == '\0')
        return -1;

     if (strcmp(gbaover[i].gamepak_code, gamepak_code))
        continue;

     if (strcmp(gbaover[i].gamepak_title, gamepak_title))
        continue;
     
     printf("romtitle     : %s\n", gbaover[i].romtitle);
     printf("gamepak title: %s\n", gbaover[i].gamepak_title);
     printf("gamepak code : %s\n", gbaover[i].gamepak_code);
     printf("gamepak maker: %s\n", gbaover[i].gamepak_maker);

     printf("INPUT gamepak title: %s\n", gamepak_title);
     printf("INPUT gamepak code : %s\n", gamepak_code);
     printf("INPUT gamepak maker: %s\n", gamepak_maker);

     if (gbaover[i].idle_loop_target_pc != 0)
        idle_loop_target_pc = gbaover[i].idle_loop_target_pc;

     iwram_stack_optimize = gbaover[i].iwram_stack_optimize;
     
     flash_device_id      = gbaover[i].flash_device_id;
     if (flash_device_id == FLASH_DEVICE_MACRONIX_128KB)
      flash_size = FLASH_SIZE_128KB;

     if (gbaover[i].translation_gate_target_1 != 0)
     {
        translation_gate_target_pc[translation_gate_targets] = gbaover[i].translation_gate_target_1;
        translation_gate_targets++;
     }

     if (gbaover[i].translation_gate_target_2 != 0)
     {
        translation_gate_target_pc[translation_gate_targets] = gbaover[i].translation_gate_target_2;
        translation_gate_targets++;
     }

     if (gbaover[i].translation_gate_target_3 != 0)
     {
        translation_gate_target_pc[translation_gate_targets] = gbaover[i].translation_gate_target_3;
        translation_gate_targets++;
     }

     printf("found entry in over ini file.\n");

     return 0;
  }

  return -1;
}

static s32 load_game_config(char *gamepak_title, char *gamepak_code, char *gamepak_maker)
{
  char current_line[256];
  char current_variable[256];
  char current_value[256];
  char config_path[512];
  FILE *config_file;

  sprintf(config_path, "%s" PATH_SEPARATOR "%s", main_path, CONFIG_FILENAME);

  printf("config_path is : %s\n", config_path);

  config_file = fopen(config_path, "rb");

  if(config_file)
  {
    while(fgets(current_line, 256, config_file))
    {
      if(parse_config_line(current_line, current_variable, current_value)
       != -1)
      {
        if(strcmp(current_variable, "game_name") ||
         strcmp(current_value, gamepak_title))
          continue;

        if(!fgets(current_line, 256, config_file) ||
         (parse_config_line(current_line, current_variable,
           current_value) == -1) ||
         strcmp(current_variable, "game_code") ||
         strcmp(current_value, gamepak_code))
          continue;

        if(!fgets(current_line, 256, config_file) ||
         (parse_config_line(current_line, current_variable,
           current_value) == -1) ||
         strcmp(current_variable, "vender_code") ||
          strcmp(current_value, gamepak_maker))
          continue;

        while(fgets(current_line, 256, config_file))
        {
          if(parse_config_line(current_line, current_variable, current_value)
           != -1)
          {
            if(!strcmp(current_variable, "game_name"))
            {
              fclose(config_file);
              return 0;
            }

            if(!strcmp(current_variable, "idle_loop_eliminate_target"))
               idle_loop_target_pc = strtol(current_value, NULL, 16);

            if(!strcmp(current_variable, "translation_gate_target"))
            {
               if(translation_gate_targets < MAX_TRANSLATION_GATES)
               {
                  translation_gate_target_pc[translation_gate_targets] =
                     strtol(current_value, NULL, 16);
                  translation_gate_targets++;
               }
            }

            if(!strcmp(current_variable, "iwram_stack_optimize") &&
                  !strcmp(current_value, "no\0")) /* \0 for broken toolchain workaround */
               iwram_stack_optimize = 0;

            if(!strcmp(current_variable, "flash_rom_type") &&
              !strcmp(current_value, "128KB"))
              flash_device_id = FLASH_DEVICE_MACRONIX_128KB;
          }
        }

        fclose(config_file);
        return 0;
      }
    }

    fclose(config_file);
  }

  printf("game config missing\n");
  return -1;
}

static s32 load_gamepak_raw(const char *name)
{
  FILE *fd = fopen(name, "rb");

  if(fd)
  {
    u32 file_size = file_length(fd);

    // First, close the last one if it was open, we won't
    // be needing it anymore.
    if(gamepak_file_large)
      fclose(gamepak_file_large);

    // If it's a big file size keep it, don't close it, we'll
    // probably want to load from it more later.
    if(file_size <= gamepak_ram_buffer_size)
    {
      fread(gamepak_rom, 1, file_size, fd);
      fclose(fd);

      gamepak_file_large = NULL;
    }
    else
    {
      // Read in just enough for the header
      fread(gamepak_rom, 1, 0x100, fd);
      gamepak_file_large = fd;
    }

    return file_size;
  }

  return -1;
}

char gamepak_title[13];
char gamepak_code[5];
char gamepak_maker[3];
char gamepak_filename[512];

u32 load_gamepak(const struct retro_game_info* info, const char *name)
{
   char *p;

   s32 file_size = load_gamepak_raw(name);

   if(file_size == -1)
      return -1;

   gamepak_size = (file_size + 0x7FFF) & ~0x7FFF;

   strncpy(gamepak_filename, name, sizeof(gamepak_filename));
   gamepak_filename[sizeof(gamepak_filename) - 1] = 0;

   p = strrchr(gamepak_filename, PATH_SEPARATOR_CHAR);
   if (p)
      p++;
   else
      p = gamepak_filename;

   snprintf(backup_filename, sizeof(backup_filename), "%s%c%s", save_path, PATH_SEPARATOR_CHAR, p);
   p = strrchr(backup_filename, '.');
   if (p)
      strcpy(p, ".sav");

   if (!use_libretro_save_method)
     load_backup(backup_filename);

   memcpy(gamepak_title, gamepak_rom + 0xA0, 12);
   memcpy(gamepak_code, gamepak_rom + 0xAC, 4);
   memcpy(gamepak_maker, gamepak_rom + 0xB0, 2);
   gamepak_title[12] = 0;
   gamepak_code[4] = 0;
   gamepak_maker[2] = 0;

   idle_loop_target_pc = 0xFFFFFFFF;
   iwram_stack_optimize = 1;
   translation_gate_targets = 0;
   flash_device_id = FLASH_DEVICE_MACRONIX_64KB;
   flash_size = FLASH_SIZE_64KB;

   if ((load_game_config_over(gamepak_title, gamepak_code, gamepak_maker)) == -1)
      load_game_config(gamepak_title, gamepak_code, gamepak_maker);

   return 0;
}

s32 load_bios(char *name)
{
  FILE *fd = fopen(name, "rb");

  if(!fd)
    return -1;

  fread(bios_rom, 1, 0x4000, fd);

  // This is a hack to get Zelda working, because emulating
  // the proper memory read behavior here is much too expensive.
  fclose(fd);
  return 0;
}

// DMA memory regions can be one of the following:
// IWRAM - 32kb offset from the contiguous iwram region.
// EWRAM - also contiguous but with self modifying code check mirror.
// VRAM - 96kb offset from the contiguous vram region, should take care
// Palette RAM - Converts palette entries when written to.
// OAM RAM - Sets OAM modified flag to true.
// I/O registers - Uses the I/O register function.
// of mirroring properly.
// Segmented RAM/ROM - a region >= 32kb, the translated address has to
//  be reloaded if it wraps around the limit (cartride ROM)
// Ext - should be handled by the memory read/write function.

// The following map determines the region of each (assumes DMA access
// is not done out of bounds)

typedef enum
{
  DMA_REGION_IWRAM,
  DMA_REGION_EWRAM,
  DMA_REGION_VRAM,
  DMA_REGION_PALETTE_RAM,
  DMA_REGION_OAM_RAM,
  DMA_REGION_IO,
  DMA_REGION_GAMEPAK,
  DMA_REGION_EXT,
  DMA_REGION_BIOS,
  DMA_REGION_NULL
} dma_region_type;

dma_region_type dma_region_map[16] =
{
  DMA_REGION_BIOS,          // 0x00 - BIOS
  DMA_REGION_NULL,          // 0x01 - Nothing
  DMA_REGION_EWRAM,         // 0x02 - EWRAM
  DMA_REGION_IWRAM,         // 0x03 - IWRAM
  DMA_REGION_IO,            // 0x04 - I/O registers
  DMA_REGION_PALETTE_RAM,   // 0x05 - palette RAM
  DMA_REGION_VRAM,          // 0x06 - VRAM
  DMA_REGION_OAM_RAM,       // 0x07 - OAM RAM
  DMA_REGION_GAMEPAK,       // 0x08 - gamepak ROM
  DMA_REGION_GAMEPAK,       // 0x09 - gamepak ROM
  DMA_REGION_GAMEPAK,       // 0x0A - gamepak ROM
  DMA_REGION_GAMEPAK,       // 0x0B - gamepak ROM
  DMA_REGION_GAMEPAK,       // 0x0C - gamepak ROM
  DMA_REGION_EXT,           // 0x0D - EEPROM
  DMA_REGION_EXT,           // 0x0E - gamepak SRAM/flash ROM
  DMA_REGION_EXT            // 0x0F - gamepak SRAM/flash ROM
};

#define dma_adjust_ptr_inc(ptr, size)                                         \
  ptr += (size / 8)                                                           \

#define dma_adjust_ptr_dec(ptr, size)                                         \
  ptr -= (size / 8)                                                           \

#define dma_adjust_ptr_fix(ptr, size)                                         \

#define dma_adjust_ptr_writeback()                                            \
  dma->dest_address = dest_ptr                                                \

#define dma_adjust_ptr_reload()                                               \

#define dma_print(src_op, dest_op, transfer_size, wb)                         \
  printf("dma from %x (%s) to %x (%s) for %x (%s) (%s) (%d) (pc %x)\n",       \
   src_ptr, #src_op, dest_ptr, #dest_op, length, #transfer_size, #wb,         \
   dma->irq, reg[15]);                                                        \

#define dma_smc_vars_src()                                                    \

#define dma_smc_vars_dest()                                                   \
  u32 smc_trigger = 0                                                         \

#define dma_vars_iwram(type)                                                  \
  dma_smc_vars_##type()                                                       \

#define dma_vars_ewram(type)                                                  \
  dma_smc_vars_##type()

#define dma_oam_ram_dest()                                                    \
  reg[OAM_UPDATED] = 1                                                        \

#define dma_vars_oam_ram(type)                                                \
  dma_oam_ram_##type()                                                        \

#define dma_vars_io(type)
#define dma_vars_vram(type)
#define dma_vars_palette_ram(type)
#define dma_vars_bios(type)
#define dma_vars_ext(type)

#define dma_oam_ram_src()

#define dma_segmented_load_src()                                              \
  memory_map_read[src_current_region]                                         \

#define dma_vars_gamepak(type)                                                \
  u32 type##_new_region;                                                      \
  u32 type##_current_region = type##_ptr >> 15;                               \
  u8 *type##_address_block = dma_segmented_load_##type();                     \
  if(type##_address_block == NULL)                                            \
  {                                                                           \
    if((type##_ptr & 0x1FFFFFF) >= gamepak_size)                              \
      break;                                                                  \
    type##_address_block = load_gamepak_page(type##_current_region & 0x3FF);  \
  }                                                                           \

#define dma_gamepak_check_region(type)                                        \
  type##_new_region = (type##_ptr >> 15);                                     \
  if(type##_new_region != type##_current_region)                              \
  {                                                                           \
    type##_current_region = type##_new_region;                                \
    type##_address_block = dma_segmented_load_##type();                       \
    if(type##_address_block == NULL)                                          \
    {                                                                         \
      type##_address_block =                                                  \
       load_gamepak_page(type##_current_region & 0x3FF);                      \
    }                                                                         \
  }                                                                           \

#define dma_read_iwram(type, transfer_size)                                   \
  read_value = address##transfer_size(iwram + 0x8000, type##_ptr & 0x7FFF)    \

#define dma_read_vram(type, transfer_size)                                    \
  read_value = address##transfer_size(vram, type##_ptr & 0x1FFFF)             \

#define dma_read_io(type, transfer_size)                                      \
  read_value = address##transfer_size(io_registers, type##_ptr & 0x7FFF)      \

#define dma_read_oam_ram(type, transfer_size)                                 \
  read_value = address##transfer_size(oam_ram, type##_ptr & 0x3FF)            \

#define dma_read_palette_ram(type, transfer_size)                             \
  read_value = address##transfer_size(palette_ram, type##_ptr & 0x3FF)        \

#define dma_read_ewram(type, transfer_size)                                   \
  read_value = address##transfer_size(ewram, type##_ptr & 0x3FFFF)            \

#define dma_read_gamepak(type, transfer_size)                                 \
  dma_gamepak_check_region(type);                                             \
  read_value = address##transfer_size(type##_address_block,                   \
   type##_ptr & 0x7FFF)                                                       \

// DMAing from the BIOS is funny, just returns 0..

#define dma_read_bios(type, transfer_size)                                    \
  read_value = 0                                                              \

#define dma_read_ext(type, transfer_size)                                     \
  read_value = read_memory##transfer_size(type##_ptr)                         \

#define dma_write_iwram(type, transfer_size)                                  \
  address##transfer_size(iwram + 0x8000, type##_ptr & 0x7FFF) = read_value;   \
  smc_trigger |= address##transfer_size(iwram, type##_ptr & 0x7FFF)           \

#define dma_write_vram(type, transfer_size)                                   \
  address##transfer_size(vram, type##_ptr & 0x1FFFF) = read_value             \

#define dma_write_io(type, transfer_size)                                     \
  write_io_register##transfer_size(type##_ptr & 0x3FF, read_value)            \

#define dma_write_oam_ram(type, transfer_size)                                \
  address##transfer_size(oam_ram, type##_ptr & 0x3FF) = read_value            \

#define dma_write_palette_ram(type, transfer_size)                            \
  write_palette##transfer_size(type##_ptr & 0x3FF, read_value)                \

#define dma_write_ext(type, transfer_size)                                    \
  write_memory##transfer_size(type##_ptr, read_value)                         \

#define dma_write_ewram(type, transfer_size)                                  \
  address##transfer_size(ewram, type##_ptr & 0x3FFFF) = read_value;           \
  smc_trigger |= address##transfer_size(ewram,                                \
   (type##_ptr & 0x3FFFF) + 0x40000)                                          \

#define dma_epilogue_iwram()                                                  \
  if(smc_trigger)                                                             \
  {                                                                           \
    /* Special return code indicating to retranslate to the CPU code */       \
    return_value = CPU_ALERT_SMC;                                             \
  }                                                                           \

#define dma_epilogue_ewram()                                                  \
  if(smc_trigger)                                                             \
  {                                                                           \
    /* Special return code indicating to retranslate to the CPU code */       \
    return_value = CPU_ALERT_SMC;                                             \
  }                                                                           \

#define dma_epilogue_vram()                                                   \

#define dma_epilogue_io()                                                     \

#define dma_epilogue_oam_ram()                                                \

#define dma_epilogue_palette_ram()                                            \

#define dma_epilogue_GAMEPAK()                                                \

#define dma_epilogue_ext()                                                    \

#define print_line()                                                          \
  dma_print(src_op, dest_op, transfer_size, wb);                              \

#define dma_transfer_loop_region(src_region_type, dest_region_type, src_op,   \
 dest_op, transfer_size, wb)                                                  \
{                                                                             \
  dma_vars_##src_region_type(src);                                            \
  dma_vars_##dest_region_type(dest);                                          \
                                                                              \
  for(i = 0; i < length; i++)                                                 \
  {                                                                           \
    dma_read_##src_region_type(src, transfer_size);                           \
    dma_write_##dest_region_type(dest, transfer_size);                        \
    dma_adjust_ptr_##src_op(src_ptr, transfer_size);                          \
    dma_adjust_ptr_##dest_op(dest_ptr, transfer_size);                        \
  }                                                                           \
  dma->source_address = src_ptr;                                              \
  dma_adjust_ptr_##wb();                                                      \
  dma_epilogue_##dest_region_type();                                          \
  break;                                                                      \
}                                                                             \

#define dma_transfer_loop(src_op, dest_op, transfer_size, wb);                \
{                                                                             \
  u32 src_region = src_ptr >> 24;                                             \
  u32 dest_region = dest_ptr >> 24;                                           \
  dma_region_type src_region_type = dma_region_map[src_region];               \
  dma_region_type dest_region_type = dma_region_map[dest_region];             \
                                                                              \
  switch(src_region_type | (dest_region_type << 4))                           \
  {                                                                           \
    case (DMA_REGION_BIOS | (DMA_REGION_IWRAM << 4)):                         \
      dma_transfer_loop_region(bios, iwram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_IWRAM << 4)):                        \
      dma_transfer_loop_region(iwram, iwram, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_IWRAM << 4)):                        \
      dma_transfer_loop_region(ewram, iwram, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_IWRAM << 4)):                         \
      dma_transfer_loop_region(vram, iwram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_IWRAM << 4)):                  \
      dma_transfer_loop_region(palette_ram, iwram, src_op, dest_op,           \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_IWRAM << 4)):                      \
      dma_transfer_loop_region(oam_ram, iwram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_IWRAM << 4)):                           \
      dma_transfer_loop_region(io, iwram, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_IWRAM << 4)):                      \
      dma_transfer_loop_region(gamepak, iwram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_IWRAM << 4)):                          \
      dma_transfer_loop_region(ext, iwram, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_EWRAM << 4)):                         \
      dma_transfer_loop_region(bios, ewram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_EWRAM << 4)):                        \
      dma_transfer_loop_region(iwram, ewram, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_EWRAM << 4)):                        \
      dma_transfer_loop_region(ewram, ewram, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_EWRAM << 4)):                         \
      dma_transfer_loop_region(vram, ewram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_EWRAM << 4)):                  \
      dma_transfer_loop_region(palette_ram, ewram, src_op, dest_op,           \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_EWRAM << 4)):                      \
      dma_transfer_loop_region(oam_ram, ewram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_EWRAM << 4)):                           \
      dma_transfer_loop_region(io, ewram, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_EWRAM << 4)):                      \
      dma_transfer_loop_region(gamepak, ewram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_EWRAM << 4)):                          \
      dma_transfer_loop_region(ext, ewram, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_VRAM << 4)):                          \
      dma_transfer_loop_region(bios, vram, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_VRAM << 4)):                         \
      dma_transfer_loop_region(iwram, vram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_VRAM << 4)):                         \
      dma_transfer_loop_region(ewram, vram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_VRAM << 4)):                          \
      dma_transfer_loop_region(vram, vram, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_VRAM << 4)):                   \
      dma_transfer_loop_region(palette_ram, vram, src_op, dest_op,            \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_VRAM << 4)):                       \
      dma_transfer_loop_region(oam_ram, vram, src_op, dest_op,                \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_VRAM << 4)):                            \
      dma_transfer_loop_region(io, vram, src_op, dest_op,                     \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_VRAM << 4)):                       \
      dma_transfer_loop_region(gamepak, vram, src_op, dest_op,                \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_VRAM << 4)):                           \
      dma_transfer_loop_region(ext, vram, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_PALETTE_RAM << 4)):                   \
      dma_transfer_loop_region(bios, palette_ram, src_op, dest_op,            \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_PALETTE_RAM << 4)):                  \
      dma_transfer_loop_region(iwram, palette_ram, src_op, dest_op,           \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_PALETTE_RAM << 4)):                  \
      dma_transfer_loop_region(ewram, palette_ram, src_op, dest_op,           \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_PALETTE_RAM << 4)):                   \
      dma_transfer_loop_region(vram, palette_ram, src_op, dest_op,            \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_PALETTE_RAM << 4)):            \
      dma_transfer_loop_region(palette_ram, palette_ram, src_op, dest_op,     \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_PALETTE_RAM << 4)):                \
      dma_transfer_loop_region(oam_ram, palette_ram, src_op, dest_op,         \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_PALETTE_RAM << 4)):                     \
      dma_transfer_loop_region(io, palette_ram, src_op, dest_op,              \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_PALETTE_RAM << 4)):                \
      dma_transfer_loop_region(gamepak, palette_ram, src_op, dest_op,         \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_PALETTE_RAM << 4)):                    \
      dma_transfer_loop_region(ext, palette_ram, src_op, dest_op,             \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_OAM_RAM << 4)):                       \
      dma_transfer_loop_region(bios, oam_ram, src_op, dest_op,                \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_OAM_RAM << 4)):                      \
      dma_transfer_loop_region(iwram, oam_ram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_OAM_RAM << 4)):                      \
      dma_transfer_loop_region(ewram, oam_ram, src_op, dest_op,               \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_OAM_RAM << 4)):                       \
      dma_transfer_loop_region(vram, oam_ram, src_op, dest_op,                \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_OAM_RAM << 4)):                \
      dma_transfer_loop_region(palette_ram, oam_ram, src_op, dest_op,         \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_OAM_RAM << 4)):                    \
      dma_transfer_loop_region(oam_ram, oam_ram, src_op, dest_op,             \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_OAM_RAM << 4)):                         \
      dma_transfer_loop_region(io, oam_ram, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_OAM_RAM << 4)):                    \
      dma_transfer_loop_region(gamepak, oam_ram, src_op, dest_op,             \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_OAM_RAM << 4)):                        \
      dma_transfer_loop_region(ext, oam_ram, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_IO << 4)):                            \
      dma_transfer_loop_region(bios, io, src_op, dest_op,                     \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_IO << 4)):                           \
      dma_transfer_loop_region(iwram, io, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_IO << 4)):                           \
      dma_transfer_loop_region(ewram, io, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_IO << 4)):                            \
      dma_transfer_loop_region(vram, io, src_op, dest_op,                     \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_IO << 4)):                     \
      dma_transfer_loop_region(palette_ram, io, src_op, dest_op,              \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_IO << 4)):                         \
      dma_transfer_loop_region(oam_ram, io, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_IO << 4)):                              \
      dma_transfer_loop_region(io, io, src_op, dest_op,                       \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_IO << 4)):                         \
      dma_transfer_loop_region(gamepak, io, src_op, dest_op,                  \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_IO << 4)):                             \
      dma_transfer_loop_region(ext, io, src_op, dest_op,                      \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_BIOS | (DMA_REGION_EXT << 4)):                           \
      dma_transfer_loop_region(bios, ext, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IWRAM | (DMA_REGION_EXT << 4)):                          \
      dma_transfer_loop_region(iwram, ext, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EWRAM | (DMA_REGION_EXT << 4)):                          \
      dma_transfer_loop_region(ewram, ext, src_op, dest_op,                   \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_VRAM | (DMA_REGION_EXT << 4)):                           \
      dma_transfer_loop_region(vram, ext, src_op, dest_op,                    \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_PALETTE_RAM | (DMA_REGION_EXT << 4)):                    \
      dma_transfer_loop_region(palette_ram, ext, src_op, dest_op,             \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_OAM_RAM | (DMA_REGION_EXT << 4)):                        \
      dma_transfer_loop_region(oam_ram, ext, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_IO | (DMA_REGION_EXT << 4)):                             \
      dma_transfer_loop_region(io, ext, src_op, dest_op,                      \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_GAMEPAK | (DMA_REGION_EXT << 4)):                        \
      dma_transfer_loop_region(gamepak, ext, src_op, dest_op,                 \
       transfer_size, wb);                                                    \
                                                                              \
    case (DMA_REGION_EXT | (DMA_REGION_EXT << 3)):                            \
      dma_transfer_loop_region(ext, ext, src_op, dest_op,                     \
       transfer_size, wb);                                                    \
  }                                                                           \
  break;                                                                      \
}                                                                             \

cpu_alert_type dma_transfer(dma_transfer_type *dma)
{
  u32 i;
  u32 length = dma->length;
  u32 read_value;
  u32 src_ptr = dma->source_address;
  uintptr_t dest_ptr = dma->dest_address;
  cpu_alert_type return_value = CPU_ALERT_NONE;

  // Technically this should be done for source and destination, but
  // chances are this is only ever used (probably mistakingly!) for dest.
  // The only game I know of that requires this is Lucky Luke.
  if((dest_ptr >> 24) != ((dest_ptr + length - 1) >> 24))
  {
    u32 first_length = ((dest_ptr & 0xFF000000) + 0x1000000) - dest_ptr;
    u32 second_length = length - first_length;
    dma->length = first_length;

    dma_transfer(dma);

    dma->length = length;

    length = second_length;
    dest_ptr += first_length;
    src_ptr += first_length;
  }

  if(dma->length_type == DMA_16BIT)
  {
    src_ptr &= ~0x01;
    dest_ptr &= ~0x01;
    cycle_dma16_words += length;

    switch((dma->dest_direction << 2) | dma->source_direction)
    {
       case 0x00:
          dma_transfer_loop(inc, inc, 16, writeback);
       case 0x01:
          dma_transfer_loop(dec, inc, 16, writeback);
       case 0x02:
          dma_transfer_loop(fix, inc, 16, writeback);
       case 0x03:
          break;
       case 0x04:
          dma_transfer_loop(inc, dec, 16, writeback);
       case 0x05:
          dma_transfer_loop(dec, dec, 16, writeback);
       case 0x06:
          dma_transfer_loop(fix, dec, 16, writeback);
       case 0x07:
          break;
       case 0x08:
          dma_transfer_loop(inc, fix, 16, writeback);
       case 0x09:
          dma_transfer_loop(dec, fix, 16, writeback);
       case 0x0A:
          dma_transfer_loop(fix, fix, 16, writeback);
       case 0x0B:
          break;
       case 0x0C:
          dma_transfer_loop(inc, inc, 16, reload);
       case 0x0D:
          dma_transfer_loop(dec, inc, 16, reload);
       case 0x0E:
          dma_transfer_loop(fix, inc, 16, reload);
       case 0x0F:
          break;
    }
  }
  else
  {
    src_ptr &= ~0x03;
    dest_ptr &= ~0x03;
    cycle_dma32_words += length;

    switch((dma->dest_direction << 2) | dma->source_direction)
    {
       case 0x00:
          dma_transfer_loop(inc, inc, 32, writeback);
       case 0x01:
          dma_transfer_loop(dec, inc, 32, writeback);
       case 0x02:
          dma_transfer_loop(fix, inc, 32, writeback);
       case 0x03:
          break;
       case 0x04:
          dma_transfer_loop(inc, dec, 32, writeback);
       case 0x05:
          dma_transfer_loop(dec, dec, 32, writeback);
       case 0x06:
          dma_transfer_loop(fix, dec, 32, writeback);
       case 0x07:
          break;
       case 0x08:
          dma_transfer_loop(inc, fix, 32, writeback);
       case 0x09:
          dma_transfer_loop(dec, fix, 32, writeback);
       case 0x0A:
          dma_transfer_loop(fix, fix, 32, writeback);
       case 0x0B:
          break;
       case 0x0C:
          dma_transfer_loop(inc, inc, 32, reload);
       case 0x0D:
          dma_transfer_loop(dec, inc, 32, reload);
       case 0x0E:
          dma_transfer_loop(fix, inc, 32, reload);
       case 0x0F:
          break;
    }
  }

  if((dma->repeat_type == DMA_NO_REPEAT) ||
   (dma->start_type == DMA_START_IMMEDIATELY))
  {
    dma->start_type = DMA_INACTIVE;
    address16(io_registers, (dma->dma_channel * 12) + 0xBA) &=
     (~0x8000);
  }

  if(dma->irq)
  {
    raise_interrupt(IRQ_DMA0 << dma->dma_channel);
    return_value = CPU_ALERT_IRQ;
  }

  return return_value;
}

// Be sure to do this after loading ROMs.

#define map_region(type, start, end, mirror_blocks, region)                   \
  for(map_offset = (start) / 0x8000; map_offset <                             \
   ((end) / 0x8000); map_offset++)                                            \
  {                                                                           \
    memory_map_##type[map_offset] =                                           \
     ((u8 *)region) + ((map_offset % mirror_blocks) * 0x8000);                \
  }                                                                           \

#define map_null(type, start, end)                                            \
  for(map_offset = start / 0x8000; map_offset < (end / 0x8000);               \
   map_offset++)                                                              \
    memory_map_##type[map_offset] = NULL;                                     \

#define map_vram(type)                                                        \
  for(map_offset = 0x6000000 / 0x8000; map_offset < (0x7000000 / 0x8000);     \
   map_offset += 4)                                                           \
  {                                                                           \
    memory_map_##type[map_offset] = vram;                                     \
    memory_map_##type[map_offset + 1] = vram + 0x8000;                        \
    memory_map_##type[map_offset + 2] = vram + (0x8000 * 2);                  \
    memory_map_##type[map_offset + 3] = vram + (0x8000 * 2);                  \
  }                                                                           \


// Picks a page to evict
u32 page_time = 0;

static u32 evict_gamepak_page(void)
{
  // Find the one with the smallest frame timestamp
  u32 page_index = 0;
  u32 physical_index;
  u32 smallest = gamepak_memory_map[0].page_timestamp;
  u32 i;

  for(i = 1; i < gamepak_ram_pages; i++)
  {
    if(gamepak_memory_map[i].page_timestamp <= smallest)
    {
      smallest = gamepak_memory_map[i].page_timestamp;
      page_index = i;
    }
  }

  physical_index = gamepak_memory_map[page_index].physical_index;

  memory_map_read[(0x8000000 / (32 * 1024)) + physical_index] = NULL;
  memory_map_read[(0xA000000 / (32 * 1024)) + physical_index] = NULL;
  memory_map_read[(0xC000000 / (32 * 1024)) + physical_index] = NULL;

  return page_index;
}

u8 *load_gamepak_page(u32 physical_index)
{
  if(physical_index >= (gamepak_size >> 15))
    return gamepak_rom;

  u32 page_index = evict_gamepak_page();
  u32 page_offset = page_index * (32 * 1024);
  u8 *swap_location = gamepak_rom + page_offset;

  gamepak_memory_map[page_index].page_timestamp = page_time;
  gamepak_memory_map[page_index].physical_index = physical_index;
  page_time++;

  fseek(gamepak_file_large, physical_index * (32 * 1024), SEEK_SET);
  fread(swap_location, 1, (32 * 1024), gamepak_file_large);
  memory_map_read[(0x8000000 / (32 * 1024)) + physical_index] = swap_location;
  memory_map_read[(0xA000000 / (32 * 1024)) + physical_index] = swap_location;
  memory_map_read[(0xC000000 / (32 * 1024)) + physical_index] = swap_location;

  // If RTC is active page the RTC register bytes so they can be read
  if((rtc_state != RTC_DISABLED) && (physical_index == 0))
    memcpy(swap_location + 0xC4, rtc_registers, sizeof(rtc_registers));

  return swap_location;
}

static void init_memory_gamepak(void)
{
  u32 map_offset = 0;

  if(gamepak_size > gamepak_ram_buffer_size)
  {
    // Large ROMs get special treatment because they
    // can't fit into the 16MB ROM buffer.
    u32 i;
    for(i = 0; i < gamepak_ram_pages; i++)
    {
      gamepak_memory_map[i].page_timestamp = 0;
      gamepak_memory_map[i].physical_index = 0;
    }

    map_null(read, 0x8000000, 0xD000000);
  }
  else
  {
    map_region(read, 0x8000000, 0x8000000 + gamepak_size, 1024, gamepak_rom);
    map_null(read, 0x8000000 + gamepak_size, 0xA000000);
    map_region(read, 0xA000000, 0xA000000 + gamepak_size, 1024, gamepak_rom);
    map_null(read, 0xA000000 + gamepak_size, 0xC000000);
    map_region(read, 0xC000000, 0xC000000 + gamepak_size, 1024, gamepak_rom);
    map_null(read, 0xC000000 + gamepak_size, 0xE000000);
  }
}

void init_gamepak_buffer(void)
{
  // Try to initialize 32MB (this is mainly for non-PSP platforms)
  gamepak_rom = NULL;

  gamepak_ram_buffer_size = 32 * 1024 * 1024;
  gamepak_rom = malloc(gamepak_ram_buffer_size);

  if(!gamepak_rom)
  {
    // Try 16MB, for PSP, then lower in 2MB increments
    gamepak_ram_buffer_size = 16 * 1024 * 1024;
    gamepak_rom = malloc(gamepak_ram_buffer_size);

    while(!gamepak_rom)
    {
      gamepak_ram_buffer_size -= (2 * 1024 * 1024);
      gamepak_rom = malloc(gamepak_ram_buffer_size);
    }
  }

  // Here's assuming we'll have enough memory left over for this,
  // and that the above succeeded (if not we're in trouble all around)
  gamepak_ram_pages = gamepak_ram_buffer_size / (32 * 1024);
  gamepak_memory_map = malloc(sizeof(gamepak_swap_entry_type) *
   gamepak_ram_pages);
}

void init_memory(void)
{
  u32 map_offset = 0;

  memory_regions[0x00] = (u8 *)bios_rom;
  memory_regions[0x01] = (u8 *)bios_rom;
  memory_regions[0x02] = (u8 *)ewram;
  memory_regions[0x03] = (u8 *)iwram + 0x8000;
  memory_regions[0x04] = (u8 *)io_registers;
  memory_regions[0x05] = (u8 *)palette_ram;
  memory_regions[0x06] = (u8 *)vram;
  memory_regions[0x07] = (u8 *)oam_ram;
  memory_regions[0x08] = (u8 *)gamepak_rom;
  memory_regions[0x09] = (u8 *)(gamepak_rom + 0xFFFFFF);
  memory_regions[0x0A] = (u8 *)gamepak_rom;
  memory_regions[0x0B] = (u8 *)(gamepak_rom + 0xFFFFFF);
  memory_regions[0x0C] = (u8 *)gamepak_rom;
  memory_regions[0x0D] = (u8 *)(gamepak_rom + 0xFFFFFF);
  memory_regions[0x0E] = (u8 *)gamepak_backup;

  memory_limits[0x00] = 0x3FFF;
  memory_limits[0x01] = 0x3FFF;
  memory_limits[0x02] = 0x3FFFF;
  memory_limits[0x03] = 0x7FFF;
  memory_limits[0x04] = 0x7FFF;
  memory_limits[0x05] = 0x3FF;
  memory_limits[0x06] = 0x17FFF;
  memory_limits[0x07] = 0x3FF;
  memory_limits[0x08] = 0x1FFFFFF;
  memory_limits[0x09] = 0x1FFFFFF;
  memory_limits[0x0A] = 0x1FFFFFF;
  memory_limits[0x0B] = 0x1FFFFFF;
  memory_limits[0x0C] = 0x1FFFFFF;
  memory_limits[0x0D] = 0x1FFFFFF;
  memory_limits[0x0E] = 0xFFFF;

  // Fill memory map regions, areas marked as NULL must be checked directly
  map_region(read, 0x0000000, 0x1000000, 1, bios_rom);
  map_null(read, 0x1000000, 0x2000000);
  map_region(read, 0x2000000, 0x3000000, 8, ewram);
  map_region(read, 0x3000000, 0x4000000, 1, &iwram[0x8000]);
  map_region(read, 0x4000000, 0x5000000, 1, io_registers);
  map_null(read, 0x5000000, 0x6000000);
  map_null(read, 0x6000000, 0x7000000);
  map_vram(read);
  map_null(read, 0x7000000, 0x8000000);
  init_memory_gamepak();
  map_null(read, 0xE000000, 0x10000000);

  memset(io_registers, 0, sizeof(io_registers));
  memset(oam_ram, 0, sizeof(oam_ram));
  memset(palette_ram, 0, sizeof(palette_ram));
  memset(iwram, 0, sizeof(iwram));
  memset(ewram, 0, sizeof(ewram));
  memset(vram, 0, sizeof(vram));

  io_registers[REG_DISPCNT] = 0x80;
  io_registers[REG_P1] = 0x3FF;
  io_registers[REG_BG2PA] = 0x100;
  io_registers[REG_BG2PD] = 0x100;
  io_registers[REG_BG3PA] = 0x100;
  io_registers[REG_BG3PD] = 0x100;
  io_registers[REG_RCNT] = 0x8000;

  backup_type = BACKUP_NONE;

  sram_size = SRAM_SIZE_32KB;
  //flash_size = FLASH_SIZE_64KB;

  flash_bank_ptr = gamepak_backup;
  flash_command_position = 0;
  eeprom_size = EEPROM_512_BYTE;
  eeprom_mode = EEPROM_BASE_MODE;
  eeprom_address = 0;
  eeprom_counter = 0;

  flash_mode = FLASH_BASE_MODE;

  rtc_state = RTC_DISABLED;
  memset(rtc_registers, 0, sizeof(rtc_registers));
  bios_read_protect = 0xe129f000;
}

void memory_term(void)
{
  if (gamepak_file_large)
  {
    fclose(gamepak_file_large);
    gamepak_file_large = NULL;
  }

  if (gamepak_memory_map)
  {
    free(gamepak_memory_map);
    gamepak_memory_map = NULL;
  }

  if (gamepak_rom)
  {
    free(gamepak_rom);
    gamepak_rom = NULL;
  }
}

#define savestate_block(type)   \
  cpu_##type##_savestate();     \
  input_##type##_savestate();   \
  main_##type##_savestate();    \
  memory_##type##_savestate();  \
  sound_##type##_savestate();   \
  video_##type##_savestate()


const u8 *state_mem_read_ptr;
u8 *state_mem_write_ptr;

void gba_load_state(const void* src)
{
   u32 i;
   u32 current_color;

   state_mem_read_ptr = src;
   savestate_block(read);

#ifdef HAVE_DYNAREC
   if (dynarec_enable)
      init_caches();
#endif

   reg[OAM_UPDATED] = 1;
   gbc_sound_update = 1;

   for(i = 0; i < 512; i++)
   {
      current_color = palette_ram[i];
      palette_ram_converted[i] =
       convert_palette(current_color);
   }

   // Oops, these contain raw pointers
   for(i = 0; i < 4; i++)
      gbc_sound_channel[i].sample_data = square_pattern_duty[2];

   instruction_count = 0;

   reg[CHANGED_PC_STATUS] = 1;
}

void gba_save_state(void* dst)
{
  state_mem_write_ptr = dst;
  savestate_block(write);
}


#define memory_savestate_builder(type)                         \
void memory_##type##_savestate(void)                           \
{                                                              \
  state_mem_##type##_variable(backup_type);                    \
  state_mem_##type##_variable(sram_size);                      \
  state_mem_##type##_variable(flash_mode);                     \
  state_mem_##type##_variable(flash_command_position);         \
  state_mem_##type##_variable(flash_bank_ptr);                 \
  state_mem_##type##_variable(flash_device_id);                \
  state_mem_##type##_variable(flash_manufacturer_id);          \
  state_mem_##type##_variable(flash_size);                     \
  state_mem_##type##_variable(eeprom_size);                    \
  state_mem_##type##_variable(eeprom_mode);                    \
  state_mem_##type##_variable(eeprom_address_length);          \
  state_mem_##type##_variable(eeprom_address);                 \
  state_mem_##type##_variable(eeprom_counter);                 \
  state_mem_##type##_variable(rtc_state);                      \
  state_mem_##type##_variable(rtc_write_mode);                 \
  state_mem_##type##_array(rtc_registers);                     \
  state_mem_##type##_variable(rtc_command);                    \
  state_mem_##type##_array(rtc_data);                          \
  state_mem_##type##_variable(rtc_status);                     \
  state_mem_##type##_variable(rtc_data_bytes);                 \
  state_mem_##type##_variable(rtc_bit_count);                  \
  state_mem_##type##_array(eeprom_buffer);                     \
  state_mem_##type##_array(dma);                               \
                                                               \
  state_mem_##type(iwram + 0x8000, 0x8000);                    \
  state_mem_##type(ewram, 0x40000);                            \
  state_mem_##type(vram, 0x18000);                             \
  state_mem_##type(oam_ram, 0x400);                            \
  state_mem_##type(palette_ram, 0x400);                        \
  state_mem_##type(io_registers, 0x8000);                      \
                                                               \
  /* This is a hack, for now. */                               \
  if((flash_bank_ptr < gamepak_backup) ||                      \
   (flash_bank_ptr > (gamepak_backup + (1024 * 64))))          \
    flash_bank_ptr = gamepak_backup;                           \
}

memory_savestate_builder(read)
memory_savestate_builder(write)


/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
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

#define SERIAL_MODE_DISABLED      0
#define SERIAL_MODE_GBP           1  // Connected to the GB Player
#define SERIAL_MODE_RFU           2  // Wifi Adapter
#define SERIAL_MODE_SERIAL_POKE   3  // Connected to serial cable (Pokemon emulation mode)
#define SERIAL_MODE_SERIAL_AW1    4  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_SERIAL_AW2    5  // Connected to serial cable (AdvWars emulation mode)
#define SERIAL_MODE_AUTO          6  // Choose best fit automatically
#define SERIAL_MODE_SERIAL_CABLE  7  // Connected to serial cable (real time, protocol agnostic)
#define SERIAL_MODE_DUAL_CABLE    8  // Cable to the second GBA of this core (Dual Cable Link)

extern int serial_mode;

// Register writes
cpu_alert_type write_siocnt(u16 value);
cpu_alert_type write_rcnt(u16 value);

// Serial IRQ interface
u32 serial_next_event();
bool update_serial(unsigned cycles);

// Savestate accessors
u32 serial_get_irq_cycles(void);
void serial_set_irq_cycles(u32 v);

// RFU interface
void rfu_reset(void);
bool rfu_update(unsigned cycles);
u32 rfu_transfer(u32 value);
void rfu_frame_update(void);
void rfu_net_receive(const void* buf, size_t len, uint16_t client_id);
void rfu_net_disconnect(int client_id);

// Serial implementations
void serialproto_reset(void);

void serialpoke_frame_update(void);
void serialpoke_master_send(void);
bool serialpoke_update(unsigned cycles);
void serialpoke_net_receive(const void* buf, size_t len, uint16_t client_id);

void serialaw_frame_update(void);
void serialaw_master_send(void);
bool serialaw_update(unsigned cycles);
void serialaw_net_receive(const void* buf, size_t len, uint16_t client_id);

// Real time link cable
void serialcable_reset(void);
void serialcable_session_reset(void);
void serialcable_replug(void);
void serialcable_peer_connected(uint16_t client_id);
void serialcable_peer_disconnected(uint16_t client_id);
void serialcable_mode_changed(void);
u16 serialcable_mul_siocnt(void);
u16 serialcable_normal_write(u16 oldval, u16 newval);
u16 serialcable_multi_write(u16 oldval, u16 newval);
void serialcable_send_written(u16 oldval);
void serialcable_frame_start(void);
bool serialcable_frame_end(void);
u32 serialcable_next_event(void);
bool serialcable_update(unsigned cycles);
void serialcable_net_receive(const void* buf, size_t len, uint16_t client_id);

// Dual Cable Link (serial_dual.c, dual.c)
cpu_alert_type dualport_write_siocnt(u16 value);
cpu_alert_type dualport_write_rcnt(u16 value);

// GBP interface
u32 gbp_transfer(u32 value);
void gbp_reset(void);
u32 gbp_get_state(void);
void gbp_set_state(u32 v);


/* gameplaySP
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

// One end of the in-memory link cable (Dual Cable Link, see dual.h).
// This file is part of both core instances: each GBA has its own port.
//
// The cable is an AGB-005 Game Link Cable (GBATEK), for 2 to 4 GBAs:
//  - The parent's SI is grounded, each child's SI is the SO of the one
//    before it (slot - 1). SC and SD are common wires.
//  - Multiplayer: only the parent starts. SIOMULTI0-3 read FFFF during the
//    transfer and the word of each GBA at the end (FFFF where there is
//    none), on every end.
//  - Normal mode is for two: between the parent and the first child. The
//    child gets the parent's bits, the parent always gets zero (its SI is
//    grounded).
//  - General purpose: the pins as wired above.
//
// TIME. Each port counts session cycles; both GBAs start the session at the
// same time (0). Whatever a port does is stamped with its time, and the other
// one applies it when its own clock gets there. Writes made by the game
// happen somewhere inside the block of code that runs until the next
// scheduler call, so they are stamped with the time of that call: it is
// already decided, and the other port cannot have got there yet.
//
// Ports only ever stop (wait for the other one) when the scheduler is called
// (update_gba), and only after the time for that call is decided. Waiting
// changes nothing for the game, so the machine is the same whatever the two
// threads do.

#include "common.h"
#include "dual.h"

#define DMODE_NORMAL   0
#define DMODE_MULTI    1
#define DMODE_UART     2
#define DMODE_GPIO     3
#define DMODE_JOYBUS   4

// Wire state word (DEV_WIRES.v0)
#define DW_SO          0x01   // SO level this port drives
#define DW_SC_DRIVE    0x02   // GPIO: drives SC
#define DW_SC          0x04
#define DW_SD_DRIVE    0x08   // GPIO: drives SD
#define DW_SD          0x10
#define DW_MODE_SH     8      // bits 8..10: DMODE_*

// Transfer in progress on this port
#define DX_NONE        0
#define DX_MP_PARENT   1
#define DX_MP_CHILD    2
#define DX_N_MASTER    3
#define DX_N_SLAVE     4

// Transfer times, as serial.c (multiplayer ones are per player)
#define DCYC_256K_8      524
#define DCYC_256K_32    2097
#define DCYC_2M_8         67
#define DCYC_2M_32       268
static const u32 dual_mp_cycles[4] = { 31457, 7864, 5242, 2621 };

static struct {
  u32 active;
  u32 slot;
  u32 n;                // GBAs on the cable
  u64 clock;            // Session cycles
  u64 promised;         // Nothing we do is stamped earlier than this
  u32 keys;             // Buttons for the current frame

  u32 wires_sent;       // Last wire word posted (~0: none)
  u32 so_out;           // Normal mode SO level (latched while busy)
  u32 p_wires[4];       // The other ports, as last heard
  u32 p_known;          // (one bit per port)

  u32 xfer;             // DX_*
  u64 t_done;
  u32 seq;
  u32 bits;             // Normal mode: 8 or 32
  u32 word_rx;          // Normal mode: what this port receives
  u16 word_parent;      // Multiplayer: the parent's word...
  u16 word_own;         // ...and ours (child)
} dp;

#define DP_KNOWN(s)      ((dp.p_known >> (s)) & 1)

// The other end of a normal mode cable: the parent and the first child.
static int dual_partner(void) {
  return (dp.slot < 2 && dp.n >= 2) ? (int)(dp.slot ^ 1) : -1;
}

static u32 dual_mode(void) {
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  const u16 rcnt = read_ioreg(REG_RCNT);
  if (rcnt & 0x8000)
    return (rcnt & 0x4000) ? DMODE_JOYBUS : DMODE_GPIO;
  if (siocnt & 0x2000)
    return (siocnt & 0x1000) ? DMODE_UART : DMODE_MULTI;
  return DMODE_NORMAL;
}

// Time stamp for something the game does now: the time of the next
// scheduler call (always later than our current, already published, time).
static u64 dual_io_time(void) {
  return MAX(dp.clock + MAX(execute_cycles, 1), dp.promised + 1);
}

// What this port drives on the cable.
static u32 dual_wires(void) {
  const u16 rcnt = read_ioreg(REG_RCNT);
  const u32 mode = dual_mode();
  u32 w = mode << DW_MODE_SH;
  switch (mode) {
  case DMODE_NORMAL:
    w |= dp.so_out ? DW_SO : 0;
    break;
  case DMODE_GPIO:
    // An input pin is not driven: the other side sees the pull-up.
    w |= (rcnt & 0x80) ? ((rcnt & 0x08) ? DW_SO : 0) : DW_SO;
    if (rcnt & 0x10)
      w |= DW_SC_DRIVE | ((rcnt & 0x01) ? DW_SC : 0);
    if (rcnt & 0x20)
      w |= DW_SD_DRIVE | ((rcnt & 0x02) ? DW_SD : 0);
    break;
  default:
    w |= DW_SO;     // Idle high
    break;
  };
  return w;
}

static void dual_post_all(const dual_event *ev) {
  u32 s;
  for (s = 0; s < dp.n; s++)
    if (s != dp.slot)
      dualbus_post(dp.slot, s, ev);
}

static void dual_post_wires(u64 t) {
  const u32 w = dual_wires();
  if (w != dp.wires_sent) {
    dual_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = DEV_WIRES;
    ev.t = t;
    ev.v0 = w;
    dual_post_all(&ev);
    dp.wires_sent = w;
  }
}

// Our SI: grounded on the parent end, the SO of the one before on a child.
static u32 dual_si(void) {
  if (!dp.slot)
    return 0;
  return DP_KNOWN(dp.slot - 1) ? ((dp.p_wires[dp.slot - 1] & DW_SO) != 0) : 1;
}

// SD in multiplayer: high once every other GBA is in multiplayer mode.
static bool dual_all_multi(void) {
  u32 s;
  for (s = 0; s < dp.n; s++)
    if (s != dp.slot && (!DP_KNOWN(s) ||
                         ((dp.p_wires[s] >> DW_MODE_SH) & 7) != DMODE_MULTI))
      return false;
  return dp.n > 1;
}

// A common wire (SC or SD) in general purpose mode: any GBA driving it low
// pulls it low, otherwise the pull-up keeps it high.
static u32 dual_common_wire(u32 drive, u32 level) {
  u32 s, v = 1;
  for (s = 0; s < dp.n; s++)
    if (s != dp.slot && DP_KNOWN(s) && (dp.p_wires[s] & drive) &&
        !(dp.p_wires[s] & level))
      v = 0;
  return v;
}

// Refresh the bits driven by the other end (SIOCNT 2/3, RCNT 0-3). Returns
// true if a general purpose SI falling edge asks for an IRQ.
static bool dual_refresh_inputs(void) {
  u16 siocnt = read_ioreg(REG_SIOCNT);
  u16 rcnt = read_ioreg(REG_RCNT);
  const u32 si = dual_si();
  bool irq = false;

  switch (dual_mode()) {
  case DMODE_NORMAL:
    siocnt = (siocnt & ~0x0004) | (si ? 0x0004 : 0);
    write_ioreg(REG_SIOCNT, siocnt);
    break;
  case DMODE_MULTI:
    {
      const bool sd = dual_all_multi();
      const bool busy = (siocnt & 0x0080) != 0;
      siocnt = (siocnt & ~0x000C) | (si ? 0x0004 : 0) | (sd ? 0x0008 : 0);
      write_ioreg(REG_SIOCNT, siocnt);
      // RCNT 0-3 read the pins (games test SC, low while busy)
      rcnt = (rcnt & ~0x000F) | (busy ? 0 : 0x1) | (sd ? 0x2 : 0) |
             (si ? 0x4 : 0) | 0x8;
      write_ioreg(REG_RCNT, rcnt);
    }
    break;
  case DMODE_GPIO:
    {
      const u32 old_si = (rcnt >> 2) & 1;
      u32 v = rcnt & 0x0F;
      if (!(rcnt & 0x10))   // SC input
        v = (v & ~1u) | dual_common_wire(DW_SC_DRIVE, DW_SC);
      if (!(rcnt & 0x20))   // SD input
        v = (v & ~2u) | (dual_common_wire(DW_SD_DRIVE, DW_SD) << 1);
      if (!(rcnt & 0x40))   // SI input
        v = (v & ~4u) | (si ? 4 : 0);
      if (!(rcnt & 0x80))   // SO input: nothing drives it on this cable
        v = (v & ~8u) | 8;
      if (old_si && !(v & 4) && (rcnt & 0x100))
        irq = true;
      write_ioreg(REG_RCNT, (rcnt & ~0x000F) | v);
    }
    break;
  default:
    break;
  };
  return irq;
}

static void dual_mp_start_parent(u64 t) {
  dual_event ev;
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  dp.xfer = DX_MP_PARENT;
  dp.t_done = t + dual_mp_cycles[siocnt & 3] * dp.n;
  dp.seq++;
  dp.word_parent = read_ioreg(REG_SIOMLT_SEND);
  write_ioreg(REG_SIOMULTI0, 0xFFFF);
  write_ioreg(REG_SIOMULTI1, 0xFFFF);
  write_ioreg(REG_SIOMULTI2, 0xFFFF);
  write_ioreg(REG_SIOMULTI3, 0xFFFF);

  memset(&ev, 0, sizeof(ev));
  ev.kind = DEV_MP_START;
  ev.t = t;
  ev.t_done = dp.t_done;
  ev.v0 = dp.word_parent;
  ev.seq = dp.seq;
  dual_post_all(&ev);
}

static void dual_n_start_master(u64 t) {
  dual_event ev;
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  const bool is32 = (siocnt & 0x1000) != 0;
  dp.xfer = DX_N_MASTER;
  dp.bits = is32 ? 32 : 8;
  dp.t_done = t + ((siocnt & 0x2) ? (is32 ? DCYC_2M_32 : DCYC_2M_8)
                                  : (is32 ? DCYC_256K_32 : DCYC_256K_8));

  memset(&ev, 0, sizeof(ev));
  ev.kind = DEV_N_START;
  ev.t = t;
  ev.t_done = dp.t_done;
  ev.v0 = is32 ? read_ioreg32(REG_SIODATA32_L) : (read_ioreg(REG_SIODATA8) & 0xFF);
  ev.a = dp.bits;
  if (dual_partner() >= 0)
    dualbus_post(dp.slot, dual_partner(), &ev);
}

// Something from the other end is due now.
static bool dual_apply(const dual_event *ev) {
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  const u32 mode = dual_mode();

  switch (ev->kind) {
  case DEV_WIRES:
    dp.p_wires[ev->from] = ev->v0;
    dp.p_known |= 1u << ev->from;
    return dual_refresh_inputs();

  case DEV_MP_START:
    // Only the children get these. One takes part if it is in multiplayer
    // mode and not busy; either way everybody gets an answer.
    if (dp.slot && mode == DMODE_MULTI && dp.xfer == DX_NONE) {
      dp.xfer = DX_MP_CHILD;
      dp.seq = ev->seq;
      dp.t_done = ev->t_done;
      dp.word_parent = ev->v0;
      dp.word_own = read_ioreg(REG_SIOMLT_SEND);
      write_ioreg(REG_SIOMULTI0, 0xFFFF);
      write_ioreg(REG_SIOMULTI1, 0xFFFF);
      write_ioreg(REG_SIOMULTI2, 0xFFFF);
      write_ioreg(REG_SIOMULTI3, 0xFFFF);
      write_ioreg(REG_SIOCNT, siocnt | 0x0080);     // Busy
      dualbus_reply(dp.slot, ev->seq, dp.word_own, 1);
      dual_refresh_inputs();
    }
    else
      dualbus_reply(dp.slot, ev->seq, 0xFFFF, 0);
    return false;

  case DEV_N_START:
    // The other end clocks a transfer: we get its bits if we are waiting for
    // an external clock with the same word size. The parent always gets zero.
    if (mode == DMODE_NORMAL && (siocnt & 0x0081) == 0x0080 &&
        dp.xfer == DX_NONE && ((siocnt & 0x1000) ? 32 : 8) == ev->a) {
      dp.xfer = DX_N_SLAVE;
      dp.bits = ev->a;
      dp.word_rx = dp.slot ? ev->v0 : 0;
      // Our view of it is late by the lookahead when it comes from a child.
      dp.t_done = MAX(ev->t_done + (ev->from ? DUAL_LOOKAHEAD : 0), dp.clock);
    }
    return false;
  };
  return false;
}

// The transfer time is over.
static bool dual_complete(void) {
  u16 siocnt = read_ioreg(REG_SIOCNT);
  const bool irq = (siocnt & 0x4000) != 0;

  switch (dp.xfer) {
  case DX_MP_PARENT:
  case DX_MP_CHILD:
    {
      // Each child answered this transfer when it got to its start (none is
      // ever that far behind): wait for the answers, whenever they get here
      // they are the same ones.
      u32 s;
      write_ioreg(REG_SIOMULTI0, dp.word_parent);
      for (s = 1; s < 4; s++) {
        u32 w = 0xFFFF;
        if (s == dp.slot)
          w = dp.word_own;
        else if (s < dp.n && !dualbus_wait_reply(dp.slot, s, dp.seq, &w))
          w = 0xFFFF;
        write_ioreg(REG_SIOMULTI0 + s, w & 0xFFFF);
      }
      siocnt &= ~(0x0080 | 0x0030 | 0x0040);
      siocnt |= (dp.slot & 3) << 4;                 // ID
      write_ioreg(REG_SIOCNT, siocnt);
    }
    break;

  case DX_N_MASTER:
  case DX_N_SLAVE:
    {
      u32 rx;
      if (dp.xfer == DX_N_SLAVE)
        rx = dp.word_rx;
      else if (!dp.slot)
        rx = 0;                                     // SI grounded
      else                                          // The idle SO before us
        rx = dual_si() ? 0xFFFFFFFF : 0;
      if (dp.bits == 32) {
        write_ioreg(REG_SIODATA32_L, rx & 0xFFFF);
        write_ioreg(REG_SIODATA32_H, rx >> 16);
      }
      else
        write_ioreg(REG_SIODATA8, (read_ioreg(REG_SIODATA8) & 0xFF00) | (rx & 0xFF));
      siocnt &= ~0x0080;
      write_ioreg(REG_SIOCNT, siocnt);
      dp.so_out = (siocnt & 0x0008) ? 1 : 0;        // SO back to bit 3
    }
    break;

  default:
    return false;
  };

  dp.xfer = DX_NONE;
  dual_refresh_inputs();
  // (Our current time is already published: what we do now is later.)
  dual_post_wires(MAX(dp.clock, dp.promised) + 1);
  return irq;
}

cpu_alert_type dualport_write_siocnt(u16 value) {
  const u16 oldval = read_ioreg(REG_SIOCNT);
  const u64 t = dual_io_time();
  u16 newval;

  write_ioreg(REG_SIOCNT, value);   // So that the mode reads right
  switch (dual_mode()) {
  case DMODE_MULTI:
    // Baud rate, start (parent only), mode and IRQ are writable.
    newval = (value & 0x7F03) | (oldval & 0x007C);
    if (dp.slot || dp.xfer != DX_NONE)
      newval = (newval & ~0x0080) | (oldval & 0x0080);
    else
      newval |= value & 0x0080;
    write_ioreg(REG_SIOCNT, newval);
    if ((newval & 0x0080) && !(oldval & 0x0080) && !dp.slot &&
        dp.xfer == DX_NONE)
      dual_mp_start_parent(t);
    break;

  case DMODE_NORMAL:
    newval = (value & 0x7F8B) | (oldval & 0x0004);
    if (dp.xfer != DX_NONE)
      newval |= 0x0080;               // Start stays set until done
    write_ioreg(REG_SIOCNT, newval);
    if (!(newval & 0x0080))
      dp.so_out = (newval & 0x0008) ? 1 : 0;
    // A transfer runs whenever start is set with the internal clock: games
    // arm the port first and switch the clock source with start still set.
    if ((newval & 0x0080) && (newval & 0x0001) && dp.xfer == DX_NONE &&
        (!(oldval & 0x0080) || !(oldval & 0x0001)))
      dual_n_start_master(t);
    break;

  default:
    break;      // UART / GPIO / JOY BUS: just stored
  };

  dual_refresh_inputs();
  dual_post_wires(t);
  return CPU_ALERT_NONE;
}

cpu_alert_type dualport_write_rcnt(u16 value) {
  const u64 t = dual_io_time();
  bool irq;
  write_ioreg(REG_RCNT, value);
  irq = dual_refresh_inputs();
  dual_post_wires(t);
  if (irq) {
    flag_interrupt(IRQ_SERIAL);
    return CPU_ALERT_IRQ;
  }
  return CPU_ALERT_NONE;
}

// Called at the top of every scheduler iteration, with the cycles that were
// run since the last one: our clock moves on, and we wait (if need be) until
// the other end is where the rules say it must be.
void dualport_sync(unsigned cycles) {
  dp.clock += cycles;
  dualbus_sync(dp.slot, MAX(dp.clock, dp.promised), dp.clock);
}

// Our frame is over: we stop until the next one, and the other GBA may need
// to go a few cycles further than us to finish its own (both overshoot the
// end of the frame a little, never by the same amount). We promise that
// nothing we do from now on is stamped before a scanline later, so that it
// can get there without waiting for our next frame.
void dualport_frame_end(void) {
  dp.promised = MAX(dp.promised, dp.clock + DUAL_FRAME_SLACK);
  dualbus_sync(dp.slot, dp.promised, 0);
}

// Called from update_serial, right after dualport_sync.
int dualport_update(void) {
  dual_event ev;
  bool irq = false;

  while (dualbus_pop(dp.slot, dp.clock, &ev))
    irq |= dual_apply(&ev);

  if (dp.xfer != DX_NONE && dp.clock >= dp.t_done)
    irq |= dual_complete();

  return irq;
}

// Our own transfer end is an event; what comes from the other end is not
// (it is applied at our next event): when it shows up depends on the other
// thread, an event for it would make our machine depend on that too.
u32 dualport_next_event(void) {
  if (dp.xfer != DX_NONE)
    return (dp.t_done > dp.clock) ? (u32)MIN(dp.t_done - dp.clock, 0x7FFFFFFF) : 1;
  return ~0U;
}

void dualport_begin(unsigned slot) {
  memset(&dp, 0, sizeof(dp));
  dp.active = 1;
  dp.slot = slot;
  dp.n = dualbus_players();
  dp.wires_sent = ~0U;
  dp.so_out = (read_ioreg(REG_SIOCNT) & 0x0008) ? 1 : 0;
  // Anything that was going on is lost, the cable was just plugged in.
  write_ioreg(REG_SIOCNT, read_ioreg(REG_SIOCNT) & ~0x0080);
  dual_refresh_inputs();
  dual_post_wires(0);
}

void dualport_end(void) {
  dp.active = 0;
}

void dualport_set_keys(uint32_t keys) { dp.keys = keys; }
uint32_t dualport_get_keys(void) { return dp.keys; }
uint64_t dualport_clock(void) { return dp.clock; }

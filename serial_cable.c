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

// Serial-Cable: a real (protocol agnostic) link cable between GBAs.
//
// Unlike the Pokemon/Advance Wars modes, nothing is faked or batched here:
// every single serial transfer is exchanged with the other consoles while it
// happens, exactly as the wire would do it.
//
// The netplay host is the parent (player 1), clients are children. For each
// multiplayer transfer:
//
//   parent                                child(ren)
//     XFER(seq, parent word)  -------->   start the transfer, latch its word
//                             <--------   REPLY(seq, child word)
//     [transfer time elapses, waits for the replies if they are late]
//     DONE(seq, all 4 words)  -------->   fill SIOMULTIx, raise serial IRQ
//     raise serial IRQ
//
// With only two consoles the child knows both words as soon as XFER gets
// there, so it finishes the transfer on its own and no DONE is sent.
//
// A child word is latched once, when the transfer gets to the child, and it
// is never changed afterwards: both ends always agree on what was exchanged.
//
// The above costs a network round trip per transfer, and games that check
// what the other console answers to each word (ie. Pokemon) break as soon
// as a single one is off: words cannot be delayed or made up. There is one
// thing that can be done though. Most games load their next word right
// after a transfer ends (serial IRQ) and leave it alone until the next one
// is over. A child whose game has been doing exactly that for a good while
// sends that word as soon as it is loaded ("early"), without waiting for
// the transfer to start: the parent usually has it by the time it needs it,
// and two transfers fit in a round trip. Every early word is still checked
// against what the game has when the transfer does start: if it ever
// differs (that one word was wrong, both consoles saw it though) the child
// goes back to waiting for the transfers, for the rest of the session.
// Games that load their words in any other way never get to use this.
//
// Normal (8/32 bit) mode goes one way only on a GBA cable: the SI pin of the
// parent end is grounded, so the parent always receives zero, and only the
// second console gets the parent's bits. The parent never needs an answer:
// it sends XFER and finishes the transfer on time, no round trip.
//
// Both consoles must also advance at the same pace. Emulators run a frame
// in a burst and then sleep in the frontend, where they cannot answer. A
// child that runs ahead goes to sleep right when the parent needs it, and
// every transfer of the frame is then stuck behind that nap. To prevent it
// the parent reports its emulated clock (PKT_TICK and every transfer) and
// the children never run past it: they wait for the parent inside the core,
// where they still answer at once. Children also line up the end of their
// frames with the parent's, so that everybody goes to sleep at the same time
// and nobody is missed.
//
// This has a catch: a child that is held does not run its game, and a
// transfer must not get to a child before its game had the time to load the
// word (it would send the previous one again). So a child first gives its
// game the very same time the parent's one had between the two transfers,
// and only then latches it.
//
// That time is paid by the parent, waiting, on top of the round trip. To
// save it a child does not sit still after a transfer: it goes on for as
// long as the parent took to start the last one, expecting the next to be
// alike (games send their words at a steady pace). When it is, the child is
// already there and answers at once. The word is still the one its game had
// at the very moment the transfer started for the parent: if it was loaded
// after that, the previous one is sent.
//
// Sleeping at the same time is not enough, the consoles also have to wake
// up together, and that is up to the frontends: each one runs its frames on
// its own schedule (display, audio), with whatever phase between them. A
// child that is done with a frame and leaves is not back until its own next
// frame is due, even if the parent starts the next one right away: the
// parent then loses that time on every frame. For this reason the parent
// says, at the end of each frame, when it expects to run the next one. A
// child that would not be back by then does not leave: it runs the next
// frame too in the same go (its video skips a frame), which gets it ahead of
// its own schedule: from then on it comes back early and waits for the
// parent inside the core, as it should.
//
// All waits are bounded: if a peer stops answering (paused, menu, gone) we
// stop waiting for it and it simply reads as a console that is not
// transmitting.

#include "common.h"
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
  #include <sched.h>
#endif

// Debug print logic:
//#define SERIALCABLE_DEBUG 1
#ifdef SERIALCABLE_DEBUG
  #define CBL_DEBUG_LOG(...) printf(__VA_ARGS__)
#else
  #define CBL_DEBUG_LOG(...)
#endif

void netpacket_send(uint16_t client_id, const void *buf, size_t len);
void netpacket_poll_receive();
bool netpacket_active(void);
u64 netpacket_time_usec(void);

// While the Dual Cable Link is getting ready (dual.c: hello, waiting for the
// players, setup), this cable stays unplugged, so that the games do not wait
// on the network meanwhile. If the dual cable is not used after all, it is
// plugged then (serialcable_replug).
#ifndef GPSP_INSTANCE_B
bool dual_cable_pending(void);
#define cable_plugged() (netpacket_active() && !dual_cable_pending())
#else
#define cable_plugged() netpacket_active()
#endif

#define NET_CABLE_HEADER       0x4d434236   // MCB6 (Multiplayer CaBle, v6)

#define PKT_STATUS             1    // a: status flags
#define PKT_XFER               2    // a: data, b: transfer config, c: parent clock
#define PKT_REPLY              3    // a: data, b: transfer mode (multiplayer only)
#define PKT_DONE               4    // a: words 0/1, b: words 2/3, c: parent clock
#define PKT_TICK               5    // a: tick flags, b: time to next frame, c: parent clock

// Status flags
#define ST_MULTI               0x1  // Console is in multiplayer mode
#define ST_SO                  0x2  // SO line level (normal mode)
#define ST_ALLREADY            0x4  // Parent only: every console is in multiplayer mode
#define ST_UNKNOWN             0xFF

// Tick flags
#define TICK_ACTIVE            0x1  // The link is in use, keep up with the parent
#define TICK_FRAME_END         0x2  // Parent just finished a frame, and it expects to
                                    // start the next one in b-1 microseconds (if b)

// XFER config. Bits 0..7: transfer mode.
//   Multiplayer: bits 8..9 baud rate, 20..23 consoles
//   Normal:      bit 16 set for 32 bit transfers
#define XMODE_MULTI            0
#define XMODE_NORMAL           1

#define CBL_QUEUE_SIZE         4096      // A normal mode transfer burst (multiboot) can be frames ahead
#define CBL_POLL_CYCLES        4096      // Children poll the network ~68 times per frame
#define CBL_TICK_CYCLES        35112     // Parent reports its clock 8 times per frame
#define CBL_ACTIVE_CYCLES      (60 * CBL_FRAME_CYCLES)  // The link is in use for ~1s after a transfer
#define CBL_SLACK_CYCLES       2048      // How far past the parent clock a child can run
#define CBL_FRAME_CYCLES        280896
#define CBL_ALIGN_CYCLES       (CBL_FRAME_CYCLES / 16)   // Frame alignment step
#define CBL_AHEAD_MAX_CYCLES   (CBL_FRAME_CYCLES / 4)    // Longest wait for a transfer we get ahead of
#define CBL_AHEAD_MARGIN       256       // ...and how much further we go, just in case
#define CBL_EARLY_STREAK       512       // Well behaved transfers it takes to send words early
#define CBL_MAX_LAG_CYCLES     CBL_FRAME_CYCLES   // A child that fell behind does not owe more than this

// Host-time limits for the blocking waits (microseconds).
// Giving up on a word means the game gets a console that is not there, which
// can be the end of the link: it must not happen just because the network
// stalled for a moment (WiFi does, for as long as a beacon interval, ~100ms).
#define CBL_PARENT_TIMEOUT_US  500000    // Waiting for the children words
#define CBL_CHILD_TIMEOUT_US   250000    // Waiting for the parent to catch up
#define CBL_NAP_AFTER_US       8000      // Peer is stuck: stop burning CPU while we wait
#define CBL_NAP_US             500
#define CBL_NOCLOCK_POLLS      20000     // Clock did not move in this many polls: there is none
#define CBL_NOCLOCK_MAX_POLLS  100000    // Limit for the waits in that case
#define CBL_FRAME_US           16743     // A frame (59.73 fps)
#define CBL_STAY_MIN_US        2000      // Parent would wait this long: do not leave
#define CBL_STAY_MAX_US        40000     // ...unless it is not coming back soon
#define CBL_STAY_MARGIN_US     1000

// Transfer timings, see serial.c
static const u16 multi_cycles[4] = { 31457, 7864, 5242, 2621 };
#define NORMAL_CYC_256KHZ_8BIT   524
#define NORMAL_CYC_2MHZ_8BIT      67

#define ME   (netplay_client_id & 3)

typedef struct {
  u8 type;
  u32 seq, a, b, c;
} cable_event;

static struct {
  struct {
    u8 present;                  // Console is plugged to the cable
    u8 flags;                    // Last reported status
    u8 responsive;               // Answered in time last time we waited for it
    // The words it sent, for the ongoing transfer and (early) for the next
    // one, indexed by the lowest bit of the sequence number.
    struct {
      u8 valid;
      u8 mode;                   // Kind of transfer it was meant for
      u32 seq;
      u32 data;
    } reply[2];
  } peer[4];

  u8 status_sent;                // Last status we reported
  u8 so_out;                     // Normal mode SO level (latched during transfers)
  u8 link_dirty;                 // Line levels might have changed
  u32 clock;                     // Emulated time
  u32 poll_cycles;

  // Host time of our frames (microseconds)
  u64 f_entry;                   // Current frame started
  u8 f_stayed;                   // Extra frames run since we got in

  // Parent
  u8 m_busy;                     // Transfer in progress
  u8 m_mode;
  u8 m_mask;                     // Consoles taking part in the transfer
  u8 m_bits32;
  u32 m_seq;
  u32 m_cycles;                  // Remaining transfer time
  u32 m_send;
  u32 m_active;                  // Time left until the link is considered idle
  u32 m_tick;                    // Time since we last reported our clock

  // Child
  cable_event queue[CBL_QUEUE_SIZE];
  u16 q_head, q_count;
  u8 s_inxfer;                   // Transfer in progress
  u8 s_local;                    // ...that we finish ourselves (no DONE needed)
  u8 s_live;                     // Parent is using the link and reporting its clock
  u8 s_resync;                   // Parent is back, adopt its clock
  u8 s_fin_valid;
  u32 s_fin_pclock;              // Parent clock when the last transfer ended
  u32 s_since_fin;               // Time since it ended for us
  u8 s_replied;                  // Our word for the ongoing transfer was sent
  u8 s_pframe_valid;
  u16 s_pword;                   // Parent word of the ongoing transfer
  u16 s_word;                    // ...and ours, once sent
  u8 s_latched;                  // The word to send is not what the game has now...
  u16 s_lword;                   // ...but this one
  u16 s_streak;                  // Transfers in a row whose word was loaded as expected
  u8 s_early_off;                // An early word was wrong: no more of them
  u8 s_epending;                 // The game just loaded a word
  u8 s_esent;                    // A word was sent early...
  u32 s_eseq;                    // ...for this transfer
  u16 s_eword;
  u8 s_ahead;                    // Getting ahead of the next transfer
  u32 s_ahead_until;             // ...up to this point of the parent clock
  u32 s_spacing;                 // Time the parent took to start the last transfer
  u8 s_wcount;                   // Words the game loaded since the last transfer
  u16 s_wprev;                   // The one before the last
  u32 s_wtime;                   // When the last one was loaded (as s_since_fin)
  u32 s_seq;                     // Last transfer seen
  u32 s_gap;                     // Time until our next word can be sampled
  u32 s_xferleft;                // Remaining time of the ongoing transfer
  u32 s_wordcyc;                 // Duration of one word of the ongoing transfer
  u32 s_time;                    // Our position on the parent clock
  u32 s_pclock;                  // Parent clock, as last reported
  u32 s_pframe;                  // Parent clock at the end of its last frame
  u8 s_pnext_valid;
  u64 s_pnext;                   // Host time the parent expects to run its next frame
} cbl;

static void cable_send(u16 client_id, u32 type, u32 seq, u32 a, u32 b, u32 c) {
  u32 pkt[6] = {
    netorder32(NET_CABLE_HEADER), netorder32(type), netorder32(seq),
    netorder32(a), netorder32(b), netorder32(c)
  };
  netpacket_send(client_id, pkt, sizeof(pkt));
}

static bool in_multi_mode(void) {
  return !(read_ioreg(REG_RCNT) & 0x8000) &&
         (read_ioreg(REG_SIOCNT) & 0x3000) == 0x2000;
}

static bool in_normal_mode(void) {
  return !(read_ioreg(REG_RCNT) & 0x8000) && !(read_ioreg(REG_SIOCNT) & 0x2000);
}

// Whether every console on the cable is in multiplayer mode (SD line).
static bool parent_all_ready(void) {
  u32 i;
  for (i = 1; i <= 3; i++)
    if (cable_plugged() && cbl.peer[i].present && !(cbl.peer[i].flags & ST_MULTI))
      return false;
  return true;
}

static u8 own_status(void) {
  u8 flags = 0;
  if (in_multi_mode()) {
    flags |= ST_MULTI;
    if (!ME && parent_all_ready())
      flags |= ST_ALLREADY;
  }
  else if (in_normal_mode() && cbl.so_out)
    flags |= ST_SO;
  return flags;
}

// Tell the other consoles about our mode/lines whenever they change.
static void cable_sync_status(void) {
  u8 flags = own_status();
  if (flags != cbl.status_sent && cable_plugged()) {
    cbl.status_sent = flags;
    cable_send(RETRO_NETPACKET_BROADCAST, PKT_STATUS, 0, flags, 0, 0);
  }
}

// SI/SD/ID fields of SIOCNT in multiplayer mode.
u16 serialcable_mul_siocnt(void) {
  bool ready;
  if (!cable_plugged())
    return 0x8;        // Nothing plugged: lone parent.

  if (!ME)
    ready = parent_all_ready();
  else
    ready = cbl.peer[0].present && (cbl.peer[0].flags & ST_ALLREADY);

  return (ME << 4) | (ME ? 0x4 : 0) | (ready ? 0x8 : 0);
}

// In multiplayer mode RCNT bits 0..3 read the SC/SD/SI/SO pins, and games
// look at them (SC is low while a transfer is going on).
static void cable_update_pins(void) {
  if (in_multi_mode()) {
    const u16 siocnt = read_ioreg(REG_SIOCNT);
    u16 rcnt = read_ioreg(REG_RCNT) & ~0x000F;
    rcnt |= (siocnt & 0x0080) ? 0 : 0x1;     // SC
    rcnt |= (siocnt & 0x0008) ? 0x2 : 0;     // SD: everybody is ready
    rcnt |= (siocnt & 0x0004) ? 0x4 : 0;     // SI
    rcnt |= 0x8;                             // SO: idle high
    write_ioreg(REG_RCNT, rcnt);
  }
}

// Whether the game can be using the cable right now. If not, the network is
// only read between frames (the frontend does it), not in the middle of them.
static bool child_link_in_use(void) {
  return cbl.s_live || cbl.s_inxfer || cbl.q_count || in_multi_mode() ||
         (in_normal_mode() && (read_ioreg(REG_SIOCNT) & 0x0081) == 0x0080);
}

// Refresh the SIOCNT bits that are driven by the other consoles.
static void cable_refresh_lines(void) {
  u16 siocnt = read_ioreg(REG_SIOCNT);
  if (in_multi_mode()) {
    if (ME)   // For children the start bit is just the busy flag.
      siocnt = (siocnt & ~0x0080) | (cbl.s_inxfer ? 0x0080 : 0);
    write_ioreg(REG_SIOCNT, (siocnt & 0xFF83) | serialcable_mul_siocnt());
    cable_update_pins();
  }
  else if (in_normal_mode()) {
    // SO of the parent is wired to the SI of the second console.
    bool si = (ME == 1) && cbl.peer[0].present && (cbl.peer[0].flags & ST_SO);
    write_ioreg(REG_SIOCNT, (siocnt & ~0x0004) | (si ? 0x0004 : 0));
  }
}

// Console reset: any ongoing transfer is lost, the cable stays plugged.
void serialcable_reset(void) {
  cbl.m_busy = 0;
  cbl.q_head = cbl.q_count = 0;
  cbl.s_inxfer = 0;
  cbl.s_gap = 0;
  cbl.s_fin_valid = 0;
  cbl.s_ahead = 0;
  cbl.s_wcount = 0;
  cbl.s_streak = 0;
  cbl.s_epending = cbl.s_esent = 0;
  cbl.s_replied = 0;
  cbl.so_out = 0;
  cbl.status_sent = ST_UNKNOWN;
  cbl.link_dirty = 1;
}

// The Dual Cable Link was not used after all: the cable gets plugged now, so
// everybody hears where we are again.
void serialcable_replug(void) {
  cbl.status_sent = ST_UNKNOWN;
  cbl.link_dirty = 1;
}

// Netplay session started/finished: (un)plug the cable.
void serialcable_session_reset(void) {
  memset(&cbl, 0, sizeof(cbl));
  serialcable_reset();
}

void serialcable_peer_disconnected(uint16_t client_id) {
  if (client_id <= 3) {
    memset(&cbl.peer[client_id], 0, sizeof(cbl.peer[client_id]));
    cbl.link_dirty = 1;
    if (!client_id)
      cbl.s_live = cbl.s_pframe_valid = 0;
  }
}

// Called after SIOCNT/RCNT are written, with the registers already updated.
void serialcable_mode_changed(void) {
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  // SO follows SIOCNT bit 3 only while no transfer is going on.
  if (!(siocnt & 0x0080))
    cbl.so_out = (siocnt & 0x0008) ? 1 : 0;
  cable_update_pins();
  cable_sync_status();
}

// The game is loading a word to send in multiplayer mode (SIOMLT_SEND),
// "oldval" is the one that was there.
void serialcable_send_written(u16 oldval) {
  if (ME) {
    if (cbl.s_wcount < 2)
      cbl.s_wcount++;
    cbl.s_wprev = oldval;
    cbl.s_wtime = cbl.s_since_fin;
    cbl.s_epending = !cbl.s_inxfer;
  }
}

// Waiting on the network: tells when to give up. The host clock is not
// always there (it reads zero forever), so the polls are counted too.
typedef struct {
  u64 t0;
  u32 polls;
} cable_wait_t;

static bool cable_wait_step(cable_wait_t *w, u32 timeout_us) {
  u64 elapsed;
  if (!w->polls++) {
    w->t0 = netpacket_time_usec();
    return true;
  }
  elapsed = netpacket_time_usec() - w->t0;
  if (elapsed > timeout_us)
    return false;
  if (!elapsed && w->polls > CBL_NOCLOCK_POLLS)
    return w->polls < CBL_NOCLOCK_MAX_POLLS;
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
  // Answers take a couple of ms at most. Past that the other console is
  // stuck somewhere (menu, pause), and there is no point in spinning.
  if (elapsed > CBL_NAP_AFTER_US)
    usleep(CBL_NAP_US);
  else
    // The answer is on its way: let whatever brings it in (the network
    // driver's threads) run now, instead of competing with us for the CPU.
    sched_yield();
#endif
  return true;
}

// A transfer starts: children are told our clock along with it.
static void parent_xfer_started(void) {
  cbl.m_active = CBL_ACTIVE_CYCLES;
  cbl.m_tick = 0;
}

// The game set the start bit in normal mode. Returns the new SIOCNT value.
u16 serialcable_normal_write(u16 oldval, u16 newval) {
  // SI is not writable, it follows the other console.
  newval = (newval & ~0x0004) | (oldval & 0x0004);

  // Only the console with the internal clock can start a transfer.
  if ((newval & 0x0080) && (newval & 0x0001) && !cbl.m_busy && !ME) {
    cbl.m_busy = 1;
    cbl.m_mode = XMODE_NORMAL;
    cbl.m_bits32 = (newval & 0x1000) ? 1 : 0;
    cbl.m_mask = 0;      // Our SI is grounded: nothing to wait for.
    cbl.m_cycles = (newval & 0x2) ? NORMAL_CYC_2MHZ_8BIT : NORMAL_CYC_256KHZ_8BIT;
    if (cbl.m_bits32)
      cbl.m_cycles <<= 2;
    cbl.m_send = cbl.m_bits32 ? read_ioreg32(REG_SIODATA32_L) :
                                (read_ioreg(REG_SIODATA8) & 0xFF);
    cbl.m_seq++;

    if (cable_plugged() && cbl.peer[1].present) {
      cable_send(1, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_NORMAL | (cbl.m_bits32 << 16), cbl.clock);
      parent_xfer_started();
    }
  }
  return newval;
}

// The game wrote SIOCNT in multiplayer mode. Returns the new SIOCNT value.
u16 serialcable_multi_write(u16 oldval, u16 newval) {
  newval = (newval & 0xFF83) | serialcable_mul_siocnt();

  if (ME) {
    // For children the start bit is just the busy flag.
    newval = (newval & ~0x0080) | (cbl.s_inxfer ? 0x0080 : 0);
  }
  else if ((newval & 0x0080) && !cbl.m_busy) {
    u32 i, count = 0;
    cbl.m_mask = 0;
    for (i = 1; i <= 3; i++) {
      if (cable_plugged() && cbl.peer[i].present) {
        cbl.m_mask |= 1 << i;
        count++;
      }
    }

    cbl.m_busy = 1;
    cbl.m_mode = XMODE_MULTI;
    cbl.m_cycles = multi_cycles[newval & 0x3] * (count + 1);
    cbl.m_send = read_ioreg(REG_SIOMLT_SEND);
    cbl.m_seq++;

    // The received words read FFFF until the transfer is over.
    for (i = 0; i <= 3; i++)
      write_ioreg(REG_SIOMULTI0 + i, 0xFFFF);

    if (cbl.m_mask) {
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_XFER, cbl.m_seq, cbl.m_send,
                 XMODE_MULTI | ((newval & 0x3) << 8) | (cbl.m_mask << 20),
                 cbl.clock);
      parent_xfer_started();
    }
  }
  return newval;
}

// Whether a console takes part in the ongoing transfer.
static bool parent_expects(u32 i) {
  return (cbl.m_mask & (1 << i)) && cbl.peer[i].present;
}

// Whether its word got here.
static bool parent_has_reply(u32 i) {
  return cbl.peer[i].reply[cbl.m_seq & 1].valid &&
         cbl.peer[i].reply[cbl.m_seq & 1].seq == cbl.m_seq &&
         cbl.peer[i].reply[cbl.m_seq & 1].mode == cbl.m_mode;
}

// Consoles we are still expecting a word from.
static u32 parent_missing(void) {
  u32 i, missing = 0;
  for (i = 1; i <= 3; i++)
    if (parent_expects(i) && cbl.peer[i].responsive && !parent_has_reply(i))
      missing |= 1 << i;
  return missing;
}

// The transfer time is over, collect the words and finish it.
static bool parent_complete(void) {
  u32 i, missing;
  u16 siocnt;
  cable_wait_t wait = { 0, 0 };

  // The answers are usually here already. If not, hold the console until
  // they arrive: that is what keeps the transfer consistent on both ends.
  // (Receiving packets re-enters serialcable_net_receive)
  while ((missing = parent_missing())) {
    if (!cable_wait_step(&wait, CBL_PARENT_TIMEOUT_US)) {
      // Do not stall again until they show signs of life.
      for (i = 1; i <= 3; i++)
        if (missing & (1 << i))
          cbl.peer[i].responsive = 0;
      CBL_DEBUG_LOG("Transfer %u timed out (missing mask %x)\n", cbl.m_seq, missing);
      break;
    }
    netpacket_poll_receive();
  }

  cbl.m_busy = 0;
  siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;

  if (cbl.m_mode == XMODE_MULTI) {
    u32 w[4];
    w[0] = cbl.m_send & 0xFFFF;
    for (i = 1; i <= 3; i++)
      w[i] = (parent_expects(i) && parent_has_reply(i)) ?
             (cbl.peer[i].reply[cbl.m_seq & 1].data & 0xFFFF) : 0xFFFF;
    for (i = 0; i <= 3; i++)
      write_ioreg(REG_SIOMULTI0 + i, w[i]);
    write_ioreg(REG_SIOCNT, siocnt);
    cable_update_pins();

    // A lone child already knows both words.
    if (cbl.m_mask & (cbl.m_mask - 1)) {
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_DONE, cbl.m_seq,
                 w[0] | (w[1] << 16), w[2] | (w[3] << 16), cbl.clock);
      cbl.m_tick = 0;
    }
  }
  else {
    // With the cable plugged our SI is grounded, unplugged it is pulled up.
    const u32 data = cable_plugged() ? 0 : 0xFFFFFFFF;
    if (cbl.m_bits32) {
      write_ioreg(REG_SIODATA32_L, data & 0xFFFF);
      write_ioreg(REG_SIODATA32_H, data >> 16);
    }
    else
      write_ioreg(REG_SIODATA8, (read_ioreg(REG_SIODATA8) & 0xFF00) | (data & 0xFF));
    write_ioreg(REG_SIOCNT, siocnt);
    // The transfer is over: SO goes back to SIOCNT bit 3.
    if (cbl.so_out != ((siocnt & 0x0008) ? 1 : 0)) {
      cbl.so_out = !cbl.so_out;
      cbl.link_dirty = 1;
    }
  }

  return (siocnt & 0x4000) != 0;
}

// Keep the children posted about our clock while the link is in use.
static void parent_tick(unsigned cycles) {
  if (!cbl.m_active)
    return;

  cbl.m_tick += cycles;
  if (cbl.m_active <= cycles) {
    // Nobody is using the link anymore: let the children run on their own.
    cbl.m_active = 0;
    cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0, 0, 0, cbl.clock);
  }
  else {
    cbl.m_active -= cycles;
    if (cbl.m_tick >= CBL_TICK_CYCLES) {
      cbl.m_tick = 0;
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0, TICK_ACTIVE, 0, cbl.clock);
    }
  }
}

// Block (for a short while) until the parent sends something.
static bool child_wait(u32 timeout_us) {
  const u32 pclock = cbl.s_pclock;
  cable_wait_t wait = { 0, 0 };
  while (!cbl.q_count && cbl.s_live && cbl.s_pclock == pclock) {
    if (!cable_wait_step(&wait, timeout_us))
      return false;
    netpacket_poll_receive();
  }
  return true;
}

// Latch the word the game wants to send and hand it to the parent. It is
// final: whatever the game loads from now on goes in the next transfer.
static void child_send_word(void) {
  const u16 word = cbl.s_latched ? cbl.s_lword :
                   in_multi_mode() ? read_ioreg(REG_SIOMLT_SEND) : 0xFFFF;

  // Words can be sent early as long as the game loads one, and only one,
  // after each transfer and before the next one starts.
  if (cbl.s_wcount == 1 && !cbl.s_latched)
    cbl.s_streak += (cbl.s_streak < CBL_EARLY_STREAK);
  else if (cbl.s_wcount)
    cbl.s_streak = 0;

  if (cbl.s_esent && cbl.s_eseq == cbl.s_seq) {
    // The parent has our word already. It is too late if it was not right.
    if (word != cbl.s_eword) {
      CBL_DEBUG_LOG("Early word for %u was wrong, no more of them\n", cbl.s_seq);
      cbl.s_early_off = 1;
    }
    cbl.s_word = cbl.s_eword;
  }
  else {
    cbl.s_word = word;
    cable_send(0, PKT_REPLY, cbl.s_seq, cbl.s_word, XMODE_MULTI, 0);
  }
  cbl.s_esent = 0;
}

// The game loaded a word and no transfer is going on: if it has been doing
// that every time it is the one for the next transfer, the parent can have
// it right now.
static void child_send_early(void) {
  cbl.s_epending = 0;
  if (!cbl.s_inxfer && !cbl.q_count && cbl.s_wcount == 1 && cbl.s_live &&
      cbl.s_streak >= CBL_EARLY_STREAK && !cbl.s_early_off && in_multi_mode()) {
    cbl.s_esent = 1;
    cbl.s_eseq = cbl.s_seq + 1;
    cbl.s_eword = read_ioreg(REG_SIOMLT_SEND);
    cable_send(0, PKT_REPLY, cbl.s_eseq, cbl.s_eword, XMODE_MULTI, 0);
  }
}

// A multiplayer transfer is over: here are all the words.
static bool child_finish(u16 w0, u16 w1, u16 w2, u16 w3) {
  u16 siocnt = read_ioreg(REG_SIOCNT) & ~0x0080;
  cbl.s_inxfer = 0;

  // On the wire our word is not sampled until one word time into the next
  // transfer: give the game that same time to load its next word.
  cbl.s_gap = cbl.s_wordcyc;
  cbl.s_since_fin = 0;
  cbl.s_wcount = 0;

  if (!in_multi_mode())
    return false;

  write_ioreg(REG_SIOMULTI0, w0);
  write_ioreg(REG_SIOMULTI1, w1);
  write_ioreg(REG_SIOMULTI2, w2);
  write_ioreg(REG_SIOMULTI3, w3);
  write_ioreg(REG_SIOCNT, siocnt);
  cable_update_pins();

  return (siocnt & 0x4000) != 0;
}

// We are the only child: the words on the wire are the parent's and the one
// we sent, which is what the parent is using too.
static bool child_finish_local(void) {
  u16 w[4] = { cbl.s_pword, 0xFFFF, 0xFFFF, 0xFFFF };

  if (cbl.s_replied)
    w[ME] = cbl.s_word;

  return child_finish(w[0], w[1], w[2], w[3]);
}

// The oldest normal mode word that is waiting for its time (see
// child_update): our clock must never get past it, or it would be late.
static bool child_normal_due(u32 *at) {
  if (cbl.q_count) {
    const cable_event *ev = &cbl.queue[cbl.q_head];
    if (ev->type == PKT_XFER && (ev->b & 0xFF) != XMODE_MULTI) {
      *at = ev->c;
      return true;
    }
  }
  return false;
}

// Whether the game is set to take a normal mode word right now (external
// clock, same word size).
static bool child_normal_ready(const cable_event *ev) {
  const u16 siocnt = read_ioreg(REG_SIOCNT);
  const bool bits32 = (ev->b >> 16) & 1;
  return in_normal_mode() && (siocnt & 0x0081) == 0x0080 &&
         ((siocnt & 0x1000) != 0) == bits32;
}

// A normal mode word whose time has come but the game is not ready for yet:
// it has been handed the previous word only just now (several were due at
// once) and is still in its serial interrupt, about to arm the next one.
// It is kept for a little while, as the wire would be if we were on time.
static bool child_normal_held(void) {
  u32 at;
  if (!child_normal_due(&at) || (s32)(cbl.s_time - at) < 0)
    return false;
  return !child_normal_ready(&cbl.queue[cbl.q_head]) &&
         (s32)(cbl.s_time - at) < CBL_FRAME_CYCLES;
}

static void child_clamp_time(void) {
  u32 at;
  if (child_normal_due(&at) && (s32)(cbl.s_time - at) > 0)
    cbl.s_time = at;
}

// The parent clock is (again) known: take our place on it.
static void child_sync(void) {
  if (cbl.s_live && cbl.s_resync) {
    cbl.s_resync = 0;
    cbl.s_time = cbl.s_pclock;
    child_clamp_time();
  }
}

// The parent started a transfer.
static bool child_xfer(const cable_event *ev) {
  const u32 mode = ev->b & 0xFF;
  u16 siocnt = read_ioreg(REG_SIOCNT);

  if (mode == XMODE_MULTI) {
    const u32 mask = (ev->b >> 20) & 0xE;
    const u32 children = ((mask >> 1) & 1) + ((mask >> 2) & 1) + ((mask >> 3) & 1);
    u32 wait = 0;

    if (!(mask & (1 << ME)))
      return false;       // The parent does not know about us yet.

    // The parent started this transfer some time after the previous one
    // ended. Our game gets that same time, it might need it to load the
    // word.
    cbl.s_latched = 0;
    cbl.s_spacing = ~0U;
    if (cbl.s_fin_valid) {
      const u32 spacing = ev->c - cbl.s_fin_pclock;
      if (spacing <= CBL_MAX_LAG_CYCLES) {
        cbl.s_spacing = spacing;
        if (spacing > cbl.s_since_fin)
          wait = spacing - cbl.s_since_fin;
        else if (cbl.s_wcount && in_multi_mode() &&
                 cbl.s_wtime > MAX(spacing, cbl.s_wordcyc)) {
          // We are past the point where our word was taken, and the game
          // loaded another one since: it is for the next transfer. (If it
          // did so twice we do not know better)
          cbl.s_latched = 1;
          cbl.s_lword = (cbl.s_wcount == 1) ? cbl.s_wprev : read_ioreg(REG_SIOMLT_SEND);
        }
      }
    }
    // A transfer is where both games are known to be at the same point:
    // ours will be at the parent clock of its start once that wait is over.
    // Whatever our clock says otherwise is what the frame alignment wants
    // us to make up for (transfers are what drives us when there are many
    // of them, the hold has no say then): run some more if we are behind,
    // or a bit less if we are ahead (never by much, the game needs its time).
    if (cbl.s_live && !cbl.s_resync) {
      s32 behind = (s32)(ev->c - wait - cbl.s_time);
      if (behind > CBL_SLACK_CYCLES)          // (Clocks are not that accurate)
        wait += MIN((u32)behind, CBL_ALIGN_CYCLES);
      else if (behind < -CBL_SLACK_CYCLES) {
        const u32 less = MIN((u32)-behind, wait / 2);
        wait -= less;
        behind += less;
        if (behind < -CBL_ALIGN_CYCLES * 2)   // That is not us: the clock is wrong.
          cbl.s_time += behind;
      }
    }

    cbl.s_seq = ev->seq;
    cbl.s_wordcyc = multi_cycles[(ev->b >> 8) & 0x3];
    cbl.s_xferleft = wait + cbl.s_wordcyc * (children + 1);
    cbl.s_gap = MAX(cbl.s_gap, wait);
    // When the parent is done with it (used if we are the only child).
    cbl.s_fin_pclock = ev->c + cbl.s_wordcyc * (children + 1);
    cbl.s_fin_valid = cbl.s_local = (children == 1);
    cbl.s_inxfer = 1;
    cbl.s_replied = 0;
    cbl.s_ahead = 0;
    cbl.s_pword = ev->a;

    // Our word follows once the game had its time to load it, see
    // child_update.
    if (in_multi_mode()) {
      write_ioreg(REG_SIOMULTI0, 0xFFFF);
      write_ioreg(REG_SIOMULTI1, 0xFFFF);
      write_ioreg(REG_SIOMULTI2, 0xFFFF);
      write_ioreg(REG_SIOMULTI3, 0xFFFF);
      write_ioreg(REG_SIOCNT, siocnt | 0x0080);     // Busy
      cable_update_pins();
    }
  }
  else if (ME == 1) {
    const bool bits32 = (ev->b >> 16) & 1;
    // We must be waiting for an external clock, with the same word size.
    const bool ready = in_normal_mode() && (siocnt & 0x0081) == 0x0080 &&
                       ((siocnt & 0x1000) != 0) == bits32;

    // The parent does not wait for us (its SI is grounded): no reply.
    if (ready) {
      if (bits32) {
        write_ioreg(REG_SIODATA32_L, ev->a & 0xFFFF);
        write_ioreg(REG_SIODATA32_H, ev->a >> 16);
      } else
        write_ioreg(REG_SIODATA8, (read_ioreg(REG_SIODATA8) & 0xFF00) | (ev->a & 0xFF));
      siocnt &= ~0x0080;
      write_ioreg(REG_SIOCNT, siocnt);
      cbl.so_out = (siocnt & 0x0008) ? 1 : 0;
    }

    return ready && (siocnt & 0x4000);
  }
  return false;
}

// Whether the game has something to do before it can be held.
static bool child_busy(void) {
  if (cbl.q_count)
    return true;
  if (cbl.s_inxfer) {
    if (!cbl.s_replied)
      return true;      // The parent might be waiting for our word.
    // About to end, and the next word comes right after.
    return cbl.s_local && cbl.s_xferleft <= cbl.s_wordcyc * 2;
  }
  if (cbl.s_gap)
    return true;
  // Getting ahead of the next transfer.
  if (cbl.s_ahead && (s32)(cbl.s_ahead_until - cbl.s_time) > 0)
    return true;
  cbl.s_ahead = 0;
  return false;
}

// The parent needs our word for the ongoing transfer.
static void child_reply(bool force) {
  if (cbl.s_inxfer && !cbl.s_replied && (force || !cbl.s_gap)) {
    child_send_word();
    cbl.s_replied = 1;
  }
}

static bool child_update(unsigned cycles) {
  bool irq = false;

  cbl.s_time += cycles;
  cbl.s_gap -= MIN(cbl.s_gap, cycles);
  if (cbl.s_since_fin < 0x40000000)
    cbl.s_since_fin += cycles;

  cbl.poll_cycles += cycles;
  if (cbl.poll_cycles >= CBL_POLL_CYCLES) {
    cbl.poll_cycles = 0;
    if (child_link_in_use())
      netpacket_poll_receive();
  }

  child_sync();
  child_reply(false);

  // With a single child both words are known, the transfer ends on time.
  if (cbl.s_inxfer && cbl.s_local) {
    if (cbl.s_xferleft > cycles)
      cbl.s_xferleft -= cycles;
    else {
      child_reply(true);
      irq = child_finish_local();
      // It ended a bit ago, in fact (or we would slowly get ahead).
      cbl.s_since_fin = cycles - cbl.s_xferleft;

      // The next one should come as this one did: be there by then. But
      // not past the end of the parent frame, ours ends right after it and
      // we must not leave before the parent does.
      if (cbl.s_spacing <= CBL_AHEAD_MAX_CYCLES && cbl.s_live && cbl.s_pframe_valid) {
        const u32 frame_end = cbl.s_pframe + CBL_FRAME_CYCLES;
        cbl.s_ahead_until = cbl.s_fin_pclock + cbl.s_spacing + CBL_AHEAD_MARGIN;
        if ((s32)(frame_end - cbl.s_time) > 0 && (s32)(frame_end - cbl.s_time) < CBL_FRAME_CYCLES) {
          if ((s32)(cbl.s_ahead_until - frame_end) > 0)
            cbl.s_ahead_until = frame_end;
          cbl.s_ahead = 1;
        }
      }
    }
  }

  if (cbl.s_epending)
    child_send_early();

  // Do not run ahead of the parent: wait here, where we can still answer.
  while (cbl.s_live && !irq && !child_busy()) {
    child_sync();
    if ((s32)(cbl.s_pclock - cbl.s_time) > CBL_MAX_LAG_CYCLES) {
      cbl.s_time = cbl.s_pclock - CBL_MAX_LAG_CYCLES;
      child_clamp_time();
    }
    if ((s32)(cbl.s_time - cbl.s_pclock) <= CBL_SLACK_CYCLES)
      break;

    if (!child_wait(CBL_CHILD_TIMEOUT_US)) {
      // Parent is not running (paused?), carry on until it shows up again.
      CBL_DEBUG_LOG("Parent clock timed out\n");
      cbl.s_live = cbl.s_pframe_valid = cbl.s_pnext_valid = 0;
    }
  }

  while (cbl.q_count && !irq) {
    const cable_event ev = cbl.queue[cbl.q_head];
    if (ev.type == PKT_XFER && cbl.s_inxfer && cbl.s_local)
      break;      // Previous transfer is not over yet, keep it for later.
    // A normal mode word gets here as soon as the parent sends it, but on
    // the wire it arrives when the parent clock says so: the game must have
    // had that time to get ready for it (multiboot loaders re-arm the
    // transfer after each word), or it is lost.
    if (ev.type == PKT_XFER && (ev.b & 0xFF) != XMODE_MULTI && cbl.s_live &&
        (s32)(ev.c - cbl.s_time) > 0)
      break;
    if (ev.type == PKT_XFER && (ev.b & 0xFF) != XMODE_MULTI && ME == 1 &&
        child_normal_held())
      break;

    cbl.q_head = (cbl.q_head + 1) % CBL_QUEUE_SIZE;
    cbl.q_count--;

    if (ev.type == PKT_XFER) {
      irq = child_xfer(&ev);
      child_reply(false);
    }
    else if (cbl.s_inxfer && !cbl.s_local && ev.seq == cbl.s_seq) {
      irq = child_finish(ev.a & 0xFFFF, ev.a >> 16, ev.b & 0xFFFF, ev.b >> 16);
      cbl.s_fin_pclock = ev.c;
      cbl.s_fin_valid = 1;
    }
  }

  return irq;
}

// The frontend wants a frame.
void serialcable_frame_start(void) {
  cbl.f_entry = netpacket_time_usec();
  cbl.f_stayed = 0;
}

// When we expect to be running the next frame: frontends call us once per
// frame time.
static u64 frame_next_start(u64 now) {
  u64 next = cbl.f_entry + CBL_FRAME_US;
  while (next < now)
    next += CBL_FRAME_US;
  return next;
}

// A frame was emulated, we are about to return to the frontend. Returns true
// if another frame should be emulated right now instead.
bool serialcable_frame_end(void) {
  const u64 now = netpacket_time_usec();
  s32 ahead;

  if (!cable_plugged() || !cbl.f_entry)
    return false;

  if (!ME) {
    // Children must know we will not be around for a while.
    if (cbl.m_active) {
      // We are late if the frame took too long: no sleep for us then.
      const u64 next = (now - cbl.f_entry < CBL_FRAME_US) ? frame_next_start(now) : now;
      cbl.m_tick = 0;
      cable_send(RETRO_NETPACKET_BROADCAST, PKT_TICK, 0,
                 TICK_ACTIVE | TICK_FRAME_END, (u32)(next - now) + 1, cbl.clock);
    }
    return false;
  }

  if (!cbl.s_live || cbl.s_resync || !cbl.s_pframe_valid)
    return false;

  // Make our frames end right after the parent's ones: we both go to sleep
  // at the same time, instead of each one being stuck on the other's nap.
  // "ahead" is how late into the parent frame ours ended. To get it earlier
  // our game has to run that much more than the parent's one, which is done
  // little by little: we take a step back on the parent clock, and make up
  // for it (see the hold in child_update and the wait in child_xfer). If it
  // ended just a bit too early, it is the other way around.
  // (We can only get as far as the slack past the end of the parent frame
  // while it sleeps: the target is half way there)
  ahead = (s32)(cbl.s_time - cbl.s_pframe - CBL_SLACK_CYCLES / 2) % CBL_FRAME_CYCLES;
  if (ahead < 0)
    ahead += CBL_FRAME_CYCLES;
  if (ahead >= CBL_FRAME_CYCLES - CBL_ALIGN_CYCLES * 2) {
    if (ahead < CBL_FRAME_CYCLES - CBL_SLACK_CYCLES / 4)
      cbl.s_time += CBL_FRAME_CYCLES - ahead;
  }
  else if (ahead > CBL_SLACK_CYCLES / 4)
    cbl.s_time -= MIN(ahead, CBL_ALIGN_CYCLES);
  child_clamp_time();

  // The parent is about to need us and we would not be back in time: stay.
  if (cbl.s_pnext_valid && !cbl.f_stayed) {
    const u64 back = frame_next_start(now);
    // We usually get here right before the parent is done with its frame:
    // what we know then is when it was going to start the one it is in,
    // the next one is a frame later.
    const bool stale = (s32)(cbl.s_pclock - cbl.s_pframe) > CBL_FRAME_CYCLES / 2;
    const u64 pnext = cbl.s_pnext + (stale ? CBL_FRAME_US : 0);
    // Only if it puts us ahead for good: the frame that is coming must be
    // over before the frontend wants the one after it, or we would be late
    // again. It should take about as long as this one did.
    if (back > pnext + CBL_STAY_MIN_US && pnext < now + CBL_STAY_MAX_US &&
        pnext + CBL_FRAME_US > now &&
        back > pnext + (now - cbl.f_entry) + CBL_STAY_MARGIN_US) {
      cbl.f_stayed = 1;
      return true;
    }
  }
  return false;
}

// How many cycles until the next cable event happens.
u32 serialcable_next_event(void) {
  u32 next = ~0U;
  if (!ME)
    return cbl.m_busy ? cbl.m_cycles : ~0U;

  if (cbl.s_inxfer && cbl.s_local)
    next = cbl.s_xferleft;
  if (cbl.s_gap && cbl.s_inxfer && !cbl.s_replied)
    next = MIN(next, cbl.s_gap);
  if (cbl.s_ahead && (s32)(cbl.s_ahead_until - cbl.s_time) > 0)
    next = MIN(next, cbl.s_ahead_until - cbl.s_time);
  {
    u32 at;
    if (cbl.s_live && child_normal_due(&at) && (s32)(at - cbl.s_time) > 0)
      next = MIN(next, at - cbl.s_time);
    else if (child_normal_held())
      next = MIN(next, 64);     // Look again once the game had a moment.
  }
  return next;
}

// Account for consumed cycles and return if a serial IRQ should be raised.
bool serialcable_update(unsigned cycles) {
  // update_gba can hand us a negative count: a timer started in the middle
  // of a block shortens the cycles to run after the CPU already took them
  // into account. Time does not go back on the cable (it would read as
  // ages gone by: the link would be idle, children would lose the parent).
  if ((s32)cycles < 0)
    cycles = 0;
  cbl.clock += cycles;

  if (cbl.link_dirty) {
    cbl.link_dirty = 0;
    cable_sync_status();
    cable_refresh_lines();
  }

  if (!cable_plugged())
    cbl.m_active = 0;

  if (ME)
    return cable_plugged() ? child_update(cycles) : false;

  if (cbl.m_busy) {
    if (cbl.m_cycles > cycles)
      cbl.m_cycles -= cycles;
    else {
      const bool irq = parent_complete();
      parent_tick(cycles);
      return irq;
    }
  }
  parent_tick(cycles);
  return false;
}

// Note: this can be called from within our own network polls. It must only
// record what happened, registers are updated from serialcable_update.
void serialcable_net_receive(const void* buf, size_t len, uint16_t client_id) {
  const u32 *pkt = (u32*)buf;
  u32 type, seq, a, b, c;

  if (len != 24 || client_id > 3 || client_id == ME ||
      netorder32(pkt[0]) != NET_CABLE_HEADER)
    return;

  type = netorder32(pkt[1]);
  seq  = netorder32(pkt[2]);
  a    = netorder32(pkt[3]);
  b    = netorder32(pkt[4]);
  c    = netorder32(pkt[5]);

  switch (type) {
  case PKT_STATUS:
    if (!cbl.peer[client_id].present) {
      // New console on the cable, make sure it learns about us too.
      cbl.peer[client_id].present = 1;
      cbl.peer[client_id].responsive = 1;
      cbl.status_sent = ST_UNKNOWN;
    }
    cbl.peer[client_id].flags = a;
    cbl.link_dirty = 1;
    break;

  case PKT_REPLY:
    if (ME)
      break;
    cbl.peer[client_id].responsive = 1;
    // The word for the ongoing transfer or (early) for the next one.
    if (seq == cbl.m_seq || seq == cbl.m_seq + 1) {
      cbl.peer[client_id].reply[seq & 1].data = a;
      cbl.peer[client_id].reply[seq & 1].seq = seq;
      cbl.peer[client_id].reply[seq & 1].mode = b;
      cbl.peer[client_id].reply[seq & 1].valid = 1;
    }
    break;

  case PKT_TICK:
  case PKT_XFER:
  case PKT_DONE:
    if (!ME || client_id != 0)
      break;

    if (type == PKT_TICK && !(a & TICK_ACTIVE)) {
      cbl.s_live = 0;     // Link is idle, nothing to keep up with.
      cbl.s_pframe_valid = 0;
      break;
    }
    if (!cbl.s_live) {
      cbl.s_live = 1;
      cbl.s_resync = 1;
      cbl.s_pclock = c;
    }
    else if ((s32)(c - cbl.s_pclock) > 0)
      cbl.s_pclock = c;

    if (type == PKT_TICK) {
      if (a & TICK_FRAME_END) {
        cbl.s_pframe = c;
        cbl.s_pframe_valid = 1;
        cbl.s_pnext_valid = (b != 0);
        cbl.s_pnext = netpacket_time_usec() + (b - 1);
      }
      break;
    }

    if (cbl.q_count >= CBL_QUEUE_SIZE) {
      // We fell way behind (ie. we were paused): whatever is queued is
      // stale by now, drop it and resync with the next transfer.
      CBL_DEBUG_LOG("Cable queue overflow, dropping stale transfers\n");
      cbl.q_head = cbl.q_count = 0;
      cbl.s_inxfer = 0;
      cbl.s_fin_valid = 0;
      cbl.link_dirty = 1;
      if (type == PKT_DONE)
        break;
    }
    {
      cable_event *ev = &cbl.queue[(cbl.q_head + cbl.q_count) % CBL_QUEUE_SIZE];
      ev->type = type;
      ev->seq = seq;
      ev->a = a;
      ev->b = b;
      ev->c = c;
      cbl.q_count++;
    }
    break;
  };
}

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

// Dual Cable Link left out (HAVE_DUAL=0, see the Makefile): for platforms
// without threads, with a single JIT block, or without the memory for the
// hidden GBAs. The Cable Link always uses the real time cable.
//
// With a console that does have the dual cable, this one answers as a core
// whose settings differ, so that both go for the real time cable at once:
// a client sends BYE to the host, a host sends GIVEUP to a client's HELLO.

#include "common.h"
#include "dual.h"

// Must match dual.c.
#define DUAL_HEADER  0x4d434431   // MCD1
#define DPK_HELLO    1
#define DPK_BYE      8
#define DPK_GIVEUP   12
#define DUAL_NO_SLOT 0xFF

void netpacket_send(uint16_t client_id, const void *buf, size_t len);

static bool in_session;
static bool is_host;
static bool boot_reset;

static void stub_send(unsigned to, u32 type, u32 word, bool with_word) {
  u32 pkt[3];
  pkt[0] = netorder32(DUAL_HEADER);
  pkt[1] = netorder32(type | (DUAL_NO_SLOT << 8));
  pkt[2] = netorder32(word);
  netpacket_send(to, pkt, with_word ? 12 : 8);
}

/* ---- the bus: never used without the hidden GBAs ------------------------ */

unsigned dualbus_players(void) { return 1; }
void dualbus_sync(unsigned slot, uint64_t publish, uint64_t clock) {}
void dualbus_post(unsigned from_slot, unsigned to_slot, const dual_event *ev) {}
int dualbus_pop(unsigned slot, uint64_t now, dual_event *ev) { return 0; }
void dualbus_reply(unsigned from_slot, uint32_t seq, uint32_t word, int active) {}
int dualbus_wait_reply(unsigned slot, unsigned from_slot, uint32_t seq,
                       uint32_t *word) { return 0; }

/* ---- the session: always the real time cable ---------------------------- */

void dual_set_frontend(retro_environment_t cb,
                       void (*message)(const char *text, unsigned ms)) {}
void dual_game_loaded(const char *path, uint32_t cfg) {}
void dual_set_players(unsigned players) {}
void dual_set_other_games(bool on) {}
void dual_set_cfg(uint32_t cfg) {}

void dual_session_start(unsigned client_id) {
  in_session = true;
  is_host = (client_id == 0);
  // A client restarts with the cable plugged, as games only look for it at
  // boot (what dual.c does when it gives up).
  boot_reset = !is_host;
  if (!is_host)
    stub_send(0, DPK_BYE, 0, false);
}

bool dual_peer_connected(unsigned client_id) { return true; }
void dual_peer_disconnected(unsigned client_id) {}

void dual_session_stop(void) {
  in_session = false;
}

bool dual_net_receive(const void *buf, size_t len, uint16_t client_id) {
  const u32 *pkt = (const u32*)buf;
  if (len < 8 || netorder32(pkt[0]) != DUAL_HEADER)
    return false;
  if (in_session && is_host && client_id &&
      (netorder32(pkt[1]) & 0xFF) == DPK_HELLO)
    stub_send(client_id, DPK_GIVEUP, 0, true);
  return true;
}

bool dual_setting_up(void) { return false; }
bool dual_running(void) { return false; }
bool dual_cable_pending(void) { return false; }

bool dual_take_boot_reset(void) {
  const bool r = boot_reset;
  boot_reset = false;
  return r;
}

void dual_request_reset(void) {}
int dual_frame_begin(uint32_t local_keys) { return DUAL_FRAME_RUN; }
void dual_b_frame_start(void) {}
void dual_b_frame_wait(void) {}
void dual_frame_end(void) {}
void dual_shutdown(void) {}

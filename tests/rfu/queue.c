/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include "rfu.c"

static unsigned ack_count, log_count, logged[6];

void netpacket_send(uint16_t id, const void *data, size_t len) {
  (void)id;
  if (len >= 12 && upack32((const u8 *)data + 4) == NET_RFU_CLIENT_ACK)
    ack_count++;
}

void netpacket_poll_receive(void) {}

void rfu_diagnostic_log(unsigned hd, unsigned cd, unsigned nr,
                        unsigned to, unsigned hp, unsigned cp) {
  log_count++;
  logged[0] = hd;
  logged[1] = cd;
  logged[2] = nr;
  logged[3] = to;
  logged[4] = hp;
  logged[5] = cp;
}

static void fresh(int host) {
  rfu_reset();
  memset(&rfu_client, 0, sizeof(rfu_client));
  ack_count = log_count = 0;
  memset(logged, 0, sizeof(logged));
  rfu_diag_host_drop = rfu_diag_client_drop = 0;
  rfu_diag_host_peak = rfu_diag_client_peak = 0;
  rfu_diag_host_noresp = rfu_diag_timeout = rfu_diag_frames = 0;
  rfu_state = host ? RFU_STATE_HOST : RFU_STATE_CLIENT;
  rfu_host.clients[0].devid = 0x1234;
  rfu_host.clients[0].client_id = 1;
  rfu_client.devid = 0x1234;
  rfu_client.clnum = 0;
  rfu_client.host_id = 0;
}

static void inject(int host, unsigned value) {
  u32 packet[4] = {
    netorder32(NET_RFU_HEADER),
    netorder32(host ? NET_RFU_CLIENT_SEND : NET_RFU_HOST_SEND),
    netorder32(host ? ((4u << 24) | 0x1234) : 4),
    value
  };
  rfu_net_receive(packet, sizeof(packet), host ? 1 : 0);
}

static unsigned drain(void) {
  unsigned count = 0;
  while (rfu_data_avail()) {
    rfu_cmd = RFU_CMD_RECV_DATA;
    assert(rfu_process_command() == 2);
    assert(rfu_buf[1] == count + 1);
    count++;
  }
  return count;
}

int main(void) {
  unsigned i;
  int host;
  for (host = 0; host < 2; host++) {
    unsigned delivered, dropped;
    fresh(host);
    for (i = 1; i <= 8; i++)
      inject(host, i);
    delivered = drain();
    dropped = host ? rfu_diag_host_drop : rfu_diag_client_drop;
    assert(delivered == MIN(8, RFU_RX_QUEUE_SIZE));
    assert(dropped == 8 - delivered);
    assert((host ? rfu_diag_host_peak : rfu_diag_client_peak) == delivered);
    /* The existing liveness ACK acknowledges even a dropped host packet. */
    if (!host)
      assert(ack_count == 8);
    printf("burst8 direction=%s capacity=%d delivered=%u dropped=%u\n",
           host ? "client-to-host" : "host-to-client",
           RFU_RX_QUEUE_SIZE, delivered, dropped);

    fresh(host);
    for (i = 1; i <= RFU_RX_QUEUE_SIZE + 1; i++)
      inject(host, i);
    assert(drain() == RFU_RX_QUEUE_SIZE);
    assert((host ? rfu_diag_host_drop : rfu_diag_client_drop) == 1);
    /* A drained queue accepts new data without stale entries. */
    inject(host, 1);
    assert(drain() == 1);

    for (i = 0; i < 299; i++)
      rfu_frame_update();
    assert(log_count == 0);
    rfu_frame_update();
    assert(log_count == 1);
    assert(logged[host ? 0 : 1] == 1);
    assert(logged[host ? 4 : 5] == RFU_RX_QUEUE_SIZE);
  }

  fresh(1);
  rfu_comstate = RFU_COMSTATE_WAITEVENT;
  rfu_timeout_cycles = 1000;
  rfu_resp_timeout = 1;
  rfu_update(1);
  assert(rfu_comstate == RFU_COMSTATE_WAITRESP);
  assert(rfu_buf[1] == 0xF0F);
  assert(rfu_diag_host_noresp == 1);

  fresh(0);
  rfu_comstate = RFU_COMSTATE_WAITEVENT;
  rfu_timeout_cycles = rfu_resp_timeout = 1;
  rfu_update(1);
  assert(rfu_comstate == RFU_COMSTATE_WAITRESP);
  assert((rfu_buf[0] & 255) == RFU_CMD_RESP_TIMEO);
  assert(rfu_diag_timeout == 1);

  fresh(0);
  for (i = 0; i < 600; i++)
    rfu_frame_update();
  assert(log_count == 0);
  puts("FIFO, overflow, reuse, diagnostic cadence and timeout checks passed");
  return 0;
}

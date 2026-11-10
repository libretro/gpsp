/* dualtest: 2 to 4 consoles with RetroArch, in one process, for the Dual
 * Cable Link of gpSP.
 *
 * Loads a copy of the core per console (each copy is a console: dlopen'ed
 * files, so each has its own globals), plays the frontend for all of them
 * and joins their netpacket interfaces with a network in memory that adds a
 * fixed latency. As in RetroArch, the clients only talk to the host, which
 * passes on what a client sends to the others when it reads the network.
 * Messages from the core are printed with the console they come from.
 *
 *   dualtest CORE.so ROM FRAMES [latency_ms] [solo_frames]
 *
 * Environment: PLAYERS=n (2-4) consoles, and the "Cable Link players"
 * option of all of them (PLAYERS1=n gives console 1 another value).
 * JOIN_GAP=n: frames between one client joining and the next.
 * CORE1=file: client 1 runs that build of the core instead (two versions
 * of the link that meet).
 * NET=listen:PORT or NET=IP:PORT (needs PACE=1): only one console here, the
 * host or the client, and the other one in another process (another device)
 * over TCP, to measure the cable on a real network.
 * PACE=1 runs each console at 59.73 frames a second, as on the real ones
 * (SKEW_PPM=n: the clients' clocks are n parts per million faster, client k
 * k times), BW_KBPS=n gives the network that bandwidth each way, JITTER_MS=n
 * delays each packet up to n ms more (in order, as TCP); SPIKE_MS=n with
 * SPIKE_EVERY=k delays one packet in k (at random) n ms more, and the ones
 * behind it wait (a WiFi having a bad moment). RESET_AT=n: the host player
 * presses reset at frame n of the session.
 *
 * KEYS0..KEYS3 = "from-to:key+key;...": what each player presses at those
 * frames of the session (keys: a b select start up down left right l r),
 * instead of random buttons. SHOTS=dir saves the screens (dir/cN_FRAME.ppm)
 * at the frames in SHOT_AT="f,f,..." or every SHOT_EVERY frames. SAVn=file
 * loads console n's save memory, SAVEOUT=dir writes them (dir/cN.sav) at
 * the end. BIOSDIR=dir: official BIOS (dir/gba_bios.bin) instead of the
 * built-in one. FRAMELOG=dir writes dir/cN.log: per frame, the buttons and
 * hashes of the screen, of its left strip and of its middle (to time when
 * a button shows on screen); MEMLOG=off,off,... adds those EWRAM bytes to it
 * ("iOFF": IWRAM, from a core built to give it as RETRO_MEMORY_VIDEO_RAM).
 * RAMDUMP=dir saves the EWRAM (dir/cN_FRAME.ram, and .iwram the same way)
 * at the SHOT_AT frames.
 * With PACE
 * the harness prints how long the setup took and how many frames apart the
 * screens are.
 *
 * Exit status 0 if the session ran FRAMES frames on every console without
 * the core reporting a problem. With the link test ROM (linktest.gba) it
 * also checks that every GBA logged the very same words, with a word from
 * each player.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "libretro.h"

#define MAXCON 4

typedef struct {
  void *h;
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample)(retro_audio_sample_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*init)(void);
  void (*deinit)(void);
  bool (*load_game)(const struct retro_game_info *);
  void (*unload_game)(void);
  void (*run)(void);
  void (*reset)(void);
  void *(*get_memory_data)(unsigned);

  struct retro_netpacket_callback np;
  bool np_set;
  bool started;           // Its netplay session started
  bool playing;           // The host was told it joined (RetroArch only
                          // sends to a client from then on)
  int id;                 // 0 = host, 1-3 = clients
  unsigned frames_shown;  // Frames that were run (not repeated)
  unsigned frames_dupe;
  unsigned frames_held;   // Repeated on purpose (a frame of sound came)
  bool sound;             // Sound since the last frame
  uint32_t keys;
  bool bad;
  FILE *framelog;         // FRAMELOG
} console;

static console con[MAXCON];
static int ncon = 2;
static int latency_us = 0;
static int64_t bw_bps = 0, jitter_us = 0, spike_us = 0, spike_every = 0;
static int64_t link_free[MAXCON], last_at[MAXCON];

typedef struct pkt {
  struct pkt *next;
  int64_t at;
  int to;                 // Console it gets to
  uint16_t from_id;       // Who sent it (as the receiver is told)
  uint16_t dest;          // To the host from a client: who it is for
  size_t len;
  unsigned char data[];
} pkt;

static pkt *q_head, *q_tail;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;

/* NET: the other console is in another process, over TCP. */
static int net_fd = -1, local = -1;
#define LOCAL(i) (net_fd < 0 || (i) == local)

static bool read_all(int fd, void *buf, size_t len) {
  unsigned char *b = buf;
  while (len) {
    ssize_t n = recv(fd, b, len, 0);
    if (n <= 0) return false;
    b += n; len -= (size_t)n;
  }
  return true;
}

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* One hop, from console `link` (whose uplink it takes) to console `to`. */
static void net_queue(int link, int to, uint16_t from_id, uint16_t dest,
                      const void *buf, size_t len) {
  pkt *p = malloc(sizeof(pkt) + len);
  int64_t t;
  pthread_mutex_lock(&q_mtx);
  p->next = NULL;
  t = now_us();
  if (bw_bps) {
    link_free[link] = (t > link_free[link] ? t : link_free[link]) +
                      (int64_t)len * 8 * 1000000 / bw_bps;
    t = link_free[link];
  }
  t += latency_us + (jitter_us ? rand() % (jitter_us + 1) : 0);
  if (spike_every && rand() % spike_every == 0)
    t += spike_us;
  if (t < last_at[link]) t = last_at[link];
  p->at = last_at[link] = t;
  p->to = to;
  p->from_id = from_id;
  p->dest = dest;
  p->len = len;
  memcpy(p->data, buf, len);
  if (q_tail) q_tail->next = p; else q_head = p;
  q_tail = p;
  pthread_mutex_unlock(&q_mtx);
}

static void net_send(int from, int flags, const void *buf, size_t len, uint16_t client_id) {
  int k;
  (void)flags;
  if (net_fd >= 0) {
    /* Frame: length, who sent it, who it is for, then the packet. */
    unsigned char hdr[8];
    const uint32_t l = (uint32_t)len;
    const uint16_t f = (uint16_t)from;
    if (!from && !con[1].playing) return;
    memcpy(hdr, &l, 4); memcpy(hdr + 4, &f, 2); memcpy(hdr + 6, &client_id, 2);
    pthread_mutex_lock(&q_mtx);
    if (send(net_fd, hdr, 8, MSG_NOSIGNAL) != 8 ||
        send(net_fd, buf, len, MSG_NOSIGNAL) != (ssize_t)len)
      con[local].bad = true;
    pthread_mutex_unlock(&q_mtx);
    return;
  }
  if (from) {
    /* A client always sends to the host, which passes it on. NETPLAY8=1:
     * as RetroArch netplay 8, nothing until it is a player. */
    if (getenv("NETPLAY8") && !con[from].playing)
      return;
    net_queue(from, 0, (uint16_t)from, client_id, buf, len);
    return;
  }
  for (k = 1; k < ncon; k++)
    if (con[k].playing &&
        (client_id == RETRO_NETPACKET_BROADCAST || client_id == k))
      net_queue(0, k, 0, 0, buf, len);
}

static void net_poll(int who) {
  pkt **pp, *prev = NULL, *got = NULL, **got_tail = &got;
  const int64_t t = now_us();
  pthread_mutex_lock(&q_mtx);
  pp = &q_head;
  while (*pp) {
    pkt *p = *pp;
    if (p->to == who && p->at <= t) {
      *pp = p->next;
      if (q_tail == p) q_tail = prev;
      p->next = NULL;
      *got_tail = p;
      got_tail = &p->next;
      continue;
    }
    prev = p;
    pp = &p->next;
  }
  pthread_mutex_unlock(&q_mtx);
  /* (Delivered unlocked: the core answers some of them right away.) */
  while (got) {
    pkt *p = got;
    got = p->next;
    if (who == 0 && p->from_id) {
      /* The host: for itself, and/or passed on to the other clients. */
      const uint16_t dest = p->dest;
      int k;
      /* NETPLAY8=1: a packet from a client that is not a player yet (an
       * older client sends from start()) is dropped, as netplay 8 does. */
      if (getenv("NETPLAY8") && !con[p->from_id].playing) {
        free(p);
        continue;
      }
      if (dest == 0 || dest == RETRO_NETPACKET_BROADCAST)
        if (con[0].np_set && con[0].np.receive)
          con[0].np.receive(p->data, p->len, p->from_id);
      for (k = 1; k < ncon; k++)
        if (k != p->from_id && con[k].playing &&
            (dest == RETRO_NETPACKET_BROADCAST || dest == k))
          net_queue(0, k, p->from_id, 0, p->data, p->len);
    }
    else if (con[who].np_set && con[who].np.receive)
      con[who].np.receive(p->data, p->len, p->from_id);
    free(p);
  }
}

/* NET: what comes from the other device goes in the queue, delivered at
 * once, to be taken by net_poll like the packets of the network in memory. */
static void *net_reader(void *arg) {
  (void)arg;
  for (;;) {
    unsigned char hdr[8];
    uint32_t l;
    uint16_t f, d;
    pkt *p;
    if (!read_all(net_fd, hdr, 8)) break;
    memcpy(&l, hdr, 4); memcpy(&f, hdr + 4, 2); memcpy(&d, hdr + 6, 2);
    if (f == 0xFFFF) continue;     /* KEEPALIVE */
    p = malloc(sizeof(pkt) + l);
    if (!read_all(net_fd, p->data, l)) { free(p); break; }
    p->next = NULL; p->at = 0; p->to = local;
    p->from_id = f; p->dest = d; p->len = l;
    pthread_mutex_lock(&q_mtx);
    if (q_tail) q_tail->next = p; else q_head = p;
    q_tail = p;
    pthread_mutex_unlock(&q_mtx);
  }
  printf("NET: the other device left\n");
  return NULL;
}

/* KEEPALIVE_US=n: an empty frame every n us (keeps the WiFi awake?) */
static void *net_keepalive(void *arg) {
  const unsigned us = (unsigned)atoi(getenv("KEEPALIVE_US"));
  unsigned char hdr[8] = { 0, 0, 0, 0, 0xFF, 0xFF, 0, 0 };
  (void)arg;
  for (;;) {
    usleep(us);
    pthread_mutex_lock(&q_mtx);
    if (send(net_fd, hdr, 8, MSG_NOSIGNAL) != 8) { pthread_mutex_unlock(&q_mtx); break; }
    pthread_mutex_unlock(&q_mtx);
  }
  return NULL;
}

static bool net_open(const char *spec) {
  struct sockaddr_in a;
  int one = 1, fd;
  pthread_t th;
  const char *colon = strrchr(spec, ':');
  if (!colon) return false;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)atoi(colon + 1));
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (!strncmp(spec, "listen:", 7)) {
    int l = fd;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1)) return false;
    printf("NET: waiting for the client on port %d\n", atoi(colon + 1));
    fd = accept(l, NULL, NULL);
    close(l);
  }
  else {
    char host[64];
    int tries;
    snprintf(host, sizeof(host), "%.*s", (int)(colon - spec), spec);
    inet_pton(AF_INET, host, &a.sin_addr);
    for (tries = 0; connect(fd, (struct sockaddr *)&a, sizeof(a)) && tries < 600; tries++) {
      close(fd); usleep(100000); fd = socket(AF_INET, SOCK_STREAM, 0);
    }
  }
  if (fd < 0) return false;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  net_fd = fd;
  printf("NET: connected\n");
  if (getenv("KEEPALIVE_US")) pthread_create(&th, NULL, net_keepalive, NULL);
  return pthread_create(&th, NULL, net_reader, NULL) == 0;
}

/* ---- frontend callbacks, one set per console ---- */

/* HOST_PLAYERS_BEFORE=n: the players option the host had before the netplay
 * session (it is changed to PLAYERS in the menu right before connecting). */
static bool opt_netplay;
/* CHANGE_AT=n: at frame n of each session console 0 switches its sound rate
 * (32768 Hz) in the menu, or its dynarec off with CHANGE_DRC=1, and the core
 * is told an option changed. */
static bool opt_changed, opt_change_pending;

static const char *opt(int who, const char *key) {
  if (!strcmp(key, "gpsp_serial")) return getenv("SERIAL") ? getenv("SERIAL") : "mul_cable";
  /* BIOSDIR=dir: the official BIOS, dir/gba_bios.bin */
  /* BUILTIN0=1 (BUILTIN1...): that console uses the built-in BIOS anyway */
  if (!strcmp(key, "gpsp_bios")) {
    char var[16];
    snprintf(var, sizeof(var), "BUILTIN%d", who);
    return getenv("BIOSDIR") && !getenv(var) ? "official" : "builtin";
  }
  /* BOOT=bios: boot through the BIOS (Start+Select there: multiboot) */
  if (!strcmp(key, "gpsp_boot_mode")) return getenv("BOOT") ? getenv("BOOT") : "game";
  /* (CHANGE_DRC=1: what CHANGE_AT changes is the dynarec, turned off) */
  if (!strcmp(key, "gpsp_drc"))
    return who == 0 && opt_changed && getenv("CHANGE_DRC") ? "disabled" : "enabled";
  if (!strcmp(key, "gpsp_frameskip")) return "disabled";
  /* SOUND1=32768: console 1 has another sound rate (a setting that differs) */
  if (!strcmp(key, "gpsp_sound_rate") && who == 1 && getenv("SOUND1")) return getenv("SOUND1");
  if (!strcmp(key, "gpsp_sound_rate") && who == 0 && opt_changed && !getenv("CHANGE_DRC")) return "32768";
  /* SOUND=32768: the sound rate of every console */
  if (!strcmp(key, "gpsp_sound_rate") && getenv("SOUND")) return getenv("SOUND");
  /* OTHER=dual: different games go on the dual cable (they are looked for) */
  if (!strcmp(key, "gpsp_cable_different_games")) return getenv("OTHER") ? "auto" : "realtime";
  if (!strcmp(key, "gpsp_cable_players")) {
    if (who == 0 && !opt_netplay && getenv("HOST_PLAYERS_BEFORE")) return getenv("HOST_PLAYERS_BEFORE");
    if (who == 1 && getenv("PLAYERS1")) return getenv("PLAYERS1");
    return getenv("PLAYERS") ? getenv("PLAYERS") : "2";
  }
  return NULL;
}

/* The core's log, with the console it comes from (corelog0..corelog3, below). */
static void core_log(int who, enum retro_log_level level, const char *fmt, va_list ap) {
  (void)level;
  printf("[console %d] ", who);
  vprintf(fmt, ap);
}
static const retro_log_printf_t core_logs[MAXCON];

static bool env(int who, unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback *)data)->log = core_logs[who];
    return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = opt(who, v->key);
    return v->value != NULL;
  }
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    *(const char **)data = getenv("BIOSDIR") ? getenv("BIOSDIR") : ".";
    return true;
  case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    *(const char **)data = ".";
    return true;
  case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
  case RETRO_ENVIRONMENT_GET_CAN_DUPE:
    if (data) *(bool *)data = true;
    return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return true;
  case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
    if (data) {
      con[who].np = *(const struct retro_netpacket_callback *)data;
      con[who].np_set = true;
    } else
      con[who].np_set = false;
    return true;
  case RETRO_ENVIRONMENT_SET_MESSAGE: {
    const struct retro_message *m = data;
    printf("[console %d] message: %s\n", who, m->msg);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
    const struct retro_message_ext *m = data;
    printf("[console %d] message: %s\n", who, m->msg);
    /* Counted from the first frame of the dual cable (frame 0 everywhere). */
    if (strstr(m->msg, "GBAs connected") && getenv("PACE"))
      con[who].frames_shown = 0;
    if (strstr(m->msg, "out of sync") || strstr(m->msg, "could not") ||
        strstr(m->msg, "not enough") || strstr(m->msg, "did not answer"))
      con[who].bad = true;
    return true;
  }
  case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
    *(unsigned *)data = 1;
    return true;
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool *)data = who == 0 && opt_change_pending;
    if (who == 0) opt_change_pending = false;
    return true;
  default:
    return false;
  }
}

static bool shot_wanted(unsigned f) {
  const char *at = getenv("SHOT_AT"), *every = getenv("SHOT_EVERY");
  if (every && atoi(every) > 0 && f % (unsigned)atoi(every) == 0)
    return true;
  while (at && *at) {
    if ((unsigned)atoi(at) == f)
      return true;
    at = strchr(at, ',');
    if (at) at++;
  }
  return false;
}

static void video(int who, const void *data, unsigned w, unsigned h, size_t pitch) {
  if (data) con[who].frames_shown++; else con[who].frames_dupe++;
  /* A repeated frame with sound: held back by the time sync, not a wait. */
  if (!data && con[who].sound) con[who].frames_held++;
  con[who].sound = false;
  /* FRAMELOG=dir: dir/cN.log, a line per frame: frame, buttons, a hash of
   * the screen (0 if repeated) and the time in us. */
  if (getenv("FRAMELOG")) {
    if (!con[who].framelog) {
      char path[512];
      snprintf(path, sizeof(path), "%s/c%d.log", getenv("FRAMELOG"), who);
      con[who].framelog = fopen(path, "w");
    }
    if (con[who].framelog) {
      /* The whole screen, a strip on its left (the scenery: it only
       * changes when the camera moves, that is when the player walks) and
       * the middle (where the player is: it changes when they turn too). */
      uint32_t hash = 0, strip = 0, middle = 0;
      unsigned x, y;
      if (data) {
        hash = strip = middle = 2166136261u;
        for (y = 0; y < h; y++)
          for (x = 0; x < w; x++) {
            const uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
            hash = (hash ^ p) * 16777619u;
            if (x < 40)
              strip = (strip ^ p) * 16777619u;
            if (x >= w / 2 - 8 && x < w / 2 + 8 && y >= h / 2 - 16 && y < h / 2 + 16)
              middle = (middle ^ p) * 16777619u;
          }
      }
      fprintf(con[who].framelog, "%u %u %08x %08x %lld %08x", con[who].frames_shown + con[who].frames_dupe,
              con[who].keys, hash, strip, (long long)now_us(), middle);
      if (getenv("MEMLOG")) {
        /* "iOFF": IWRAM, for a core that gives it as RETRO_MEMORY_VIDEO_RAM */
        const uint8_t *ewram = con[who].get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        const uint8_t *iwram = con[who].get_memory_data(RETRO_MEMORY_VIDEO_RAM);
        const char *m = getenv("MEMLOG");
        while (*m) {
          char *e;
          const bool iw = *m == 'i';
          const unsigned long off = strtoul(m + iw, &e, 16);
          const uint8_t *ram = iw ? iwram : ewram;
          if (e == m + iw) break;
          if (ram && off < (iw ? 32 : 256) * 1024u) fprintf(con[who].framelog, " %u", ram[off]);
          m = *e ? e + 1 : e;
        }
      }
      fputc('\n', con[who].framelog);
    }
  }
  /* RAMDUMP=dir: the GBA's work RAM (EWRAM) at the SHOT_AT frames. */
  if (data && getenv("RAMDUMP") && shot_wanted(con[who].frames_shown)) {
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/c%d_%05u.ram", getenv("RAMDUMP"), who, con[who].frames_shown);
    if ((f = fopen(path, "wb"))) {
      fwrite(con[who].get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 1, 256 * 1024, f);
      fclose(f);
    }
    snprintf(path, sizeof(path), "%s/c%d_%05u.iwram", getenv("RAMDUMP"), who, con[who].frames_shown);
    if (con[who].get_memory_data(RETRO_MEMORY_VIDEO_RAM) && (f = fopen(path, "wb"))) {
      fwrite(con[who].get_memory_data(RETRO_MEMORY_VIDEO_RAM), 1, 32 * 1024, f);
      fclose(f);
    }
  }
  if (data && getenv("SHOTS") && shot_wanted(con[who].frames_shown)) {
    char path[512];
    FILE *f;
    unsigned x, y;
    snprintf(path, sizeof(path), "%s/c%d_%05u.ppm", getenv("SHOTS"), who, con[who].frames_shown);
    if ((f = fopen(path, "wb"))) {
      fprintf(f, "P6 %u %u 255\n", w, h);
      for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
          const uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
          const unsigned char rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
          fwrite(rgb, 1, 3, f);
        }
      fclose(f);
    }
  }
}

static int16_t input_state(int who, unsigned port, unsigned dev, unsigned idx, unsigned id) {
  if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)con[who].keys;
  return (con[who].keys >> id) & 1;
}

#define CBSET(n) \
  static bool env##n(unsigned c, void *d) { return env(n, c, d); } \
  static void video##n(const void *d, unsigned w, unsigned h, size_t p) { video(n, d, w, h, p); } \
  static int16_t input##n(unsigned p, unsigned d, unsigned i, unsigned id) { return input_state(n, p, d, i, id); } \
  static void np_send##n(int f, const void *b, size_t l, uint16_t c) { net_send(n, f, b, l, c); } \
  static void np_poll##n(void) { net_poll(n); } \
  static size_t audio##n(const int16_t *d, size_t f) { con[n].sound = true; return f; } \
  static void corelog##n(enum retro_log_level l, const char *fmt, ...) \
    { va_list ap; va_start(ap, fmt); core_log(n, l, fmt, ap); va_end(ap); }
CBSET(0)
CBSET(1)
CBSET(2)
CBSET(3)
static const retro_log_printf_t core_logs[MAXCON] = { corelog0, corelog1, corelog2, corelog3 };

static const retro_environment_t envs[MAXCON] = { env0, env1, env2, env3 };
static const retro_video_refresh_t videos[MAXCON] = { video0, video1, video2, video3 };
static const retro_input_state_t inputs[MAXCON] = { input0, input1, input2, input3 };
static const retro_audio_sample_batch_t audios[MAXCON] = { audio0, audio1, audio2, audio3 };
static void (*const np_sends[MAXCON])(int, const void *, size_t, uint16_t) =
  { np_send0, np_send1, np_send2, np_send3 };
static void (*const np_polls[MAXCON])(void) = { np_poll0, np_poll1, np_poll2, np_poll3 };

static void audio(int16_t l, int16_t r) {}
static void input_poll(void) {}

static void load_core(console *c, const char *path, int who) {
#define SYM(f) c->f = dlsym(c->h, "retro_" #f); if (!c->f) { fprintf(stderr, "missing retro_%s\n", #f); exit(2); }
  c->h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!c->h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); exit(2); }
  SYM(set_environment) SYM(set_video_refresh) SYM(set_audio_sample)
  SYM(set_audio_sample_batch) SYM(set_input_poll) SYM(set_input_state)
  SYM(init) SYM(deinit) SYM(load_game) SYM(unload_game) SYM(run)
  SYM(get_memory_data) SYM(reset)
  c->id = who;
  c->set_environment(envs[who]);
  c->set_video_refresh(videos[who]);
  c->set_audio_sample(audio);
  c->set_audio_sample_batch(audios[who]);
  c->set_input_poll(input_poll);
  c->set_input_state(inputs[who]);
  printf("  init\n");
  c->init();
  printf("  init done\n");
}

static bool copy_file(const char *from, const char *to) {
  FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
  char buf[65536];
  size_t n;
  bool ok = a && b;
  while (ok && (n = fread(buf, 1, sizeof(buf), a)) > 0)
    ok = fwrite(buf, 1, n, b) == n;
  if (a) fclose(a);
  if (b) fclose(b);
  return ok;
}

/* KEYS0..KEYS3: "from-to:key+key;..." */
static bool scripted_keys(int who, unsigned frame, uint32_t *keys) {
  static const struct { const char *n; int id; } names[] = {
    { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
    { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
    { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { "l", RETRO_DEVICE_ID_JOYPAD_L }, { "r", RETRO_DEVICE_ID_JOYPAD_R },
  };
  char var[8];
  const char *s;
  snprintf(var, sizeof(var), "KEYS%d", who);
  s = getenv(var);
  if (!s)
    return false;
  *keys = 0;
  while (*s) {
    char *e;
    const unsigned from = (unsigned)strtoul(s, &e, 10);
    unsigned to = from;
    if (*e == '-') to = (unsigned)strtoul(e + 1, &e, 10);
    if (*e == ':') e++;
    while (*e && *e != ';') {
      size_t n = strcspn(e, "+;");
      unsigned k;
      for (k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (strlen(names[k].n) == n && !strncmp(e, names[k].n, n) && frame >= from && frame <= to)
          *keys |= 1u << names[k].id;
      e += n;
      if (*e == '+') e++;
    }
    s = *e ? e + 1 : e;
  }
  return true;
}

/* Buttons that change now and then, different for each player. */
static uint32_t script_keys(int who, unsigned frame) {
  uint32_t x = (frame / 9) * 2654435761u + who * 40503u;
  uint32_t k;
  if (scripted_keys(who, frame, &k))
    return k;
  x ^= x >> 13;
  return x & 0x0FF;   /* A B Select Start and the pad, no L/R/X/Y */
}

static bool all_done(unsigned frames) {
  int i;
  for (i = 0; i < ncon; i++)
    if (LOCAL(i) && con[i].frames_shown < frames)
      return false;
  return true;
}

static bool any_bad(void) {
  int i;
  for (i = 0; i < ncon; i++)
    if (con[i].bad)
      return true;
  return false;
}

/* One frame of every console that has started its session (or of all, if
 * `all`), one after the other. */
static void run_round(bool all) {
  int i;
  for (i = 0; i < ncon; i++)
    if (all || con[i].started) {
      con[i].keys = script_keys(i, con[i].frames_shown);
      net_poll(i);
      con[i].run();
    }
}

/* PACE: one thread per console. The host's thread also keeps the stats:
 * how many frames apart the screens are, sampled at each host frame. */
static struct {
  int64_t t0, t_start[MAXCON];
  unsigned frames;
  double ppm;
  volatile int stop, half;
  unsigned dupe_half[MAXCON], held_half[MAXCON];
  volatile int64_t t_last[MAXCON];
  double period[MAXCON];
  double diff_sum[MAXCON], diff_min[MAXCON], diff_max[MAXCON];
  long long diff_n;
} pace;

static void *pace_main(void *arg) {
  const int i = (int)(intptr_t)arg;
  const double period = pace.period[i];
  const int64_t limit = pace.t0 + 60 * 1000000LL + (int64_t)pace.frames * 40000;
  double next = (double)pace.t0;
  int k;
  while (!pace.stop && now_us() < limit && !all_done(pace.frames)) {
    int64_t t = now_us();
    if (next > t) usleep((useconds_t)(next - t));
    con[i].keys = script_keys(i, con[i].frames_shown);
    net_poll(i);
    /* LINKLOG_RESET=n: the link test ROM's log starts over at frame n of
     * each console (the host's fills up before the clients are there). */
    if (getenv("LINKLOG_RESET") &&
        con[i].frames_shown == (unsigned)atoi(getenv("LINKLOG_RESET"))) {
      uint32_t *m = con[i].get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
      if (m) m[0] = 0;
    }
    con[i].run();
    t = now_us();
    pace.t_last[i] = t;
    next += period;
    if (next < t - period) next = t;      /* missed a frame */
    if (!pace.t_start[i] && con[i].frames_shown)
      pace.t_start[i] = t;
    if (any_bad()) pace.stop = 1;
    if (i) continue;
    for (k = 0; k < ncon && pace.t_start[k]; k++);
    if (k < ncon) continue;
    if (!pace.half) {
      for (k = 0; k < ncon && con[k].frames_shown >= pace.frames / 2; k++);
      if (k == ncon) {
        pace.half = 1;
        for (k = 0; k < ncon; k++) {
          pace.dupe_half[k] = con[k].frames_dupe;
          pace.held_half[k] = con[k].frames_held;
        }
      }
    }
    if (pace.half && !all_done(pace.frames)) {
      for (k = 1; k < ncon; k++) {
        /* Where client k is: its last frame, plus the time since. */
        const double d = (double)con[0].frames_shown - (double)con[k].frames_shown -
                         (double)(t - pace.t_last[k]) / pace.period[k];
        pace.diff_sum[k] += d;
        if (d < pace.diff_min[k]) pace.diff_min[k] = d;
        if (d > pace.diff_max[k]) pace.diff_max[k] = d;
      }
      pace.diff_n++;
    }
  }
  return NULL;
}

static void print_session(int64_t t0) {
  int i;
  printf("session:");
  for (i = 0; i < ncon; i++)
    printf(" %s%d %u frames (%u repeated)%s", i ? "client " : "host", i ? i : 0,
           con[i].frames_shown, con[i].frames_dupe, i + 1 < ncon ? "," : "");
  printf(", %.1f s\n", (now_us() - t0) / 1e6);
}

int main(int argc, char **argv) {
  const char *core, *rom;
  unsigned frames, solo, f;
  struct retro_game_info info;
  char copy[MAXCON][256];
  int i, ok = 1;

  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 4) {
    fprintf(stderr, "usage: %s CORE.so ROM FRAMES [latency_ms] [solo_frames]\n", argv[0]);
    return 2;
  }
  core = argv[1];
  rom = argv[2];
  frames = (unsigned)atoi(argv[3]);
  latency_us = argc > 4 ? atoi(argv[4]) * 1000 : 0;
  bw_bps = getenv("BW_KBPS") ? atoll(getenv("BW_KBPS")) * 1000 : 0;
  jitter_us = getenv("JITTER_MS") ? atoll(getenv("JITTER_MS")) * 1000 : 0;
  spike_us = getenv("SPIKE_MS") ? atoll(getenv("SPIKE_MS")) * 1000 : 0;
  spike_every = getenv("SPIKE_EVERY") ? atoll(getenv("SPIKE_EVERY")) : 0;
  solo = argc > 5 ? (unsigned)atoi(argv[5]) : 30;
  if (getenv("PLAYERS")) ncon = atoi(getenv("PLAYERS"));
  if (ncon < 2 || ncon > MAXCON) { fprintf(stderr, "PLAYERS: 2 to 4\n"); return 2; }
  if (getenv("NET")) {
    if (!getenv("PACE")) { fprintf(stderr, "NET needs PACE=1\n"); return 2; }
    ncon = 2;
    local = strncmp(getenv("NET"), "listen:", 7) ? 1 : 0;
  }

  /* One file each: dlopen gives each its own globals. */
  for (i = 0; i < ncon; i++) {
    copy[i][0] = 0;
    if (local >= 0 && i != local) continue;
    snprintf(copy[i], sizeof(copy[i]), "%s/dualtest_core%d_%d.so",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", i, (int)getpid());
    /* CORE1 in the environment: client 1 runs another build of the core. */
    if (!copy_file(i == 1 && getenv("CORE1") ? getenv("CORE1") : core, copy[i])) {
      fprintf(stderr, "cannot copy the core\n"); return 2;
    }
    printf("console %d: loading core\n", i);
    load_core(&con[i], copy[i], i);
    printf("console %d: loading game\n", i);
    memset(&info, 0, sizeof(info));
    /* ROM2 in the environment: client 1 has another game. */
    info.path = (i == 1 && getenv("ROM2") && local < 0) ? getenv("ROM2") : rom;
    if (!con[i].load_game(&info)) { fprintf(stderr, "load_game failed\n"); return 2; }
    if (!con[i].np_set) { fprintf(stderr, "no netpacket interface\n"); return 2; }
    /* SAVn=file: its save memory, loaded as RetroArch does. */
    {
      char var[8];
      const char *sav;
      snprintf(var, sizeof(var), "SAV%d", i);
      if ((sav = getenv(var))) {
        FILE *fp = fopen(sav, "rb");
        unsigned char *m = con[i].get_memory_data(RETRO_MEMORY_SAVE_RAM);
        size_t got = 0;
        if (fp && m) { got = fread(m, 1, 1024 * 128, fp); fclose(fp); }
        printf("console %d: save %s, %u bytes\n", i, sav, (unsigned)got);
      }
    }
    printf("console %d: ready\n", i);
  }

  /* Each player plays alone for a while, so that the GBAs differ. */
  for (f = 0; f < solo; f++)
    for (i = 0; i < ncon; i++) if (LOCAL(i)) {
      con[i].keys = script_keys(i, f + 1000 * i);
      con[i].run();
    }
  printf("solo frames done (%u)\n", solo);

  /* SESSIONS=n: n netplay sessions one after the other (everybody leaves,
   * plays alone a while, and connects again). */
  {
  const unsigned sessions = getenv("SESSIONS") ? (unsigned)atoi(getenv("SESSIONS")) : 1;
  unsigned sess;
  for (sess = 0; sess < sessions; sess++) {
  if (sess) {
    printf("=== session %u ===\n", sess + 1);
    for (f = 0; f < solo; f++)
      for (i = 0; i < ncon; i++) {
        con[i].keys = script_keys(i, f + 1000 * i);
        con[i].run();
      }
  }
  /* Netplay: the host starts, the clients join one after the other. */
  {
    const unsigned gap = getenv("JOIN_GAP") ? (unsigned)atoi(getenv("JOIN_GAP")) : 0;
    const unsigned late = getenv("LATE_CONNECT") ? (unsigned)atoi(getenv("LATE_CONNECT")) : 0;
    int64_t ts = now_us();
    opt_netplay = true;   /* (no frame runs before the session starts) */
    if (local >= 0) {
      if (!net_open(getenv("NET"))) { fprintf(stderr, "NET: cannot connect\n"); return 2; }
      con[1].playing = true;
      con[local].np.start((unsigned)local, np_sends[local], np_polls[local]);
      con[local].started = true;
      if (!local && con[0].np.connected && !con[0].np.connected(1)) {
        fprintf(stderr, "host refused the client\n");
        return 2;
      }
      printf("session start took %.0f ms\n", (now_us() - ts) / 1e3);
    }
    else {
    con[0].np.start(0, np_sends[0], np_polls[0]);
    con[0].started = true;
    printf("host session start took %.0f ms\n", (now_us() - ts) / 1e3);
    for (i = 1; i < ncon; i++) {
      if (i > 1)
        for (f = 0; f < gap; f++)
          run_round(false);
      ts = now_us();
      con[i].np.start((unsigned)i, np_sends[i], np_polls[i]);
      con[i].started = true;
      printf("client %d session start took %.0f ms\n", i, (now_us() - ts) / 1e3);
      /* LATE_CONNECT=N: the host hears about the client N frames after the
       * client started (as RetroArch can do), so the client's first packets
       * get to a host that is not listening yet. */
      for (f = 0; f < late; f++)
        run_round(false);
      if (late)
        printf("host told about client %d %u frames late\n", i, late);
      /* (RetroArch marks it as playing before it asks the core) */
      con[i].playing = true;
      if (con[0].np.connected && !con[0].np.connected((uint16_t)i)) {
        fprintf(stderr, "host refused client %d\n", i);
        return 2;
      }
    }
    }
  }

  for (i = 0; i < ncon; i++)
    con[i].frames_shown = con[i].frames_dupe = 0;

  if (getenv("PACE")) {
    /* Each console on its own clock and its own thread, as real ones. */
    pthread_t th[MAXCON];
    pace.t0 = now_us();
    pace.frames = frames;
    pace.ppm = getenv("SKEW_PPM") ? atof(getenv("SKEW_PPM")) : 0;
    for (i = 0; i < ncon; i++) {
      pace.period[i] = 1e6 / 59.7275 * (1 - i * pace.ppm / 1e6);
      pace.diff_min[i] = 1e9;
      pace.diff_max[i] = -1e9;
    }
    for (i = 0; i < ncon; i++)
      if (LOCAL(i)) pthread_create(&th[i], NULL, pace_main, (void *)(intptr_t)i);
    for (i = 0; i < ncon; i++)
      if (LOCAL(i)) pthread_join(th[i], NULL);
    print_session(pace.t0);
    printf("setup: plays after");
    for (i = 0; i < ncon; i++)
      printf(" %.0f ms%s", pace.t_start[i] ? (pace.t_start[i] - pace.t0) / 1e3 : -1.0,
             i + 1 < ncon ? "," : "\n");
    if (pace.diff_n) {
      for (i = 1; i < ncon; i++)
        printf("second half: host ahead of client %d by (frames): avg %.2f, min %.2f, max %.2f\n",
               i, pace.diff_sum[i] / pace.diff_n, pace.diff_min[i], pace.diff_max[i]);
      printf("second half, frames repeated (held by the time sync + waits):");
      for (i = 0; i < ncon; i++) {
        const unsigned held = con[i].frames_held - pace.held_half[i];
        printf(" %s %u + %u%s", i ? "client" : "host", held,
               con[i].frames_dupe - pace.dupe_half[i] - held, i + 1 < ncon ? "," : "\n");
      }
    }
  }
  else {
    const int64_t t0 = now_us();
    unsigned rounds = 0;
    const int64_t limit = t0 + 120 * 1000000LL + (int64_t)frames * 100000 * ncon;
    const unsigned reset_at = getenv("RESET_AT") ? (unsigned)atoi(getenv("RESET_AT")) : 0;
    const unsigned change_at = getenv("CHANGE_AT") ? (unsigned)atoi(getenv("CHANGE_AT")) : 0;
    bool changed = false;
    while (!all_done(frames) && now_us() < limit) {
      if (change_at && !changed && con[0].frames_shown >= change_at) {
        printf("console 0 changes its sound rate at frame %u\n", con[0].frames_shown);
        opt_changed = !opt_changed;
        opt_change_pending = changed = true;
      }
      if (reset_at && con[0].frames_shown == reset_at) {
        printf("host presses reset at frame %u\n", reset_at);
        con[0].reset();
        con[0].frames_shown++;    /* (only once) */
      }
      run_round(true);
      rounds++;
      if (rounds % 50 == 0) {
        printf("  round %u:", rounds);
        for (i = 0; i < ncon; i++)
          printf(" %u (+%u rep)", con[i].frames_shown, con[i].frames_dupe);
        printf("\n");
      }
      if (any_bad()) break;
    }
    print_session(t0);
  }

  if (any_bad() || !all_done(frames))
    ok = 0;

  /* LINKLOG=dir: the link test ROM's log of each console (dir/cN.link: the
   * count, then the words), to compare them however they line up. */
  if (getenv("LINKLOG"))
    for (i = 0; i < ncon; i++) if (LOCAL(i)) {
      const uint32_t *m = con[i].get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
      char path[512];
      FILE *fp;
      snprintf(path, sizeof(path), "%s/c%d.link", getenv("LINKLOG"), i);
      if (m && m[0] <= 0xFFF0 && (fp = fopen(path, "wb"))) {
        fwrite(m, 4, 4 + (m[0] < 8192 ? m[0] : 8192), fp);
        fclose(fp);
      }
    }

  /* Link test ROM: every GBA must have logged the same words, two log
   * entries per transfer: the parent's and first child's words, then the
   * other two. Each player's word must be there (FFFF where nobody is). */
  {
    const uint32_t *e[MAXCON];
    bool lt = true;
    for (i = 0; i < ncon; i++) {
      e[i] = LOCAL(i) ? con[i].get_memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
      if (!e[i] || e[i][2] < 1 || e[i][2] > 4)
        lt = false;
    }
    if (lt) {
      unsigned n = e[0][0], k, diff = 0, absent = 0, phantom = 0;
      for (i = 1; i < ncon; i++)
        if (e[i][0] < n) n = e[i][0];
      if (n > 8192) n = 8192;
      n &= ~1u;
      for (k = 0; k < n; k++)
        for (i = 1; i < ncon; i++)
          if (e[0][4 + k] != e[i][4 + k]) diff++;
      for (k = 0; k < n; k += 2) {
        const uint16_t w[4] = { e[0][4 + k] & 0xFFFF, e[0][4 + k] >> 16,
                                e[0][5 + k] & 0xFFFF, e[0][5 + k] >> 16 };
        int s;
        for (s = 0; s < 4; s++) {
          if (s < ncon && w[s] == 0xFFFF) absent++;
          if (s >= ncon && w[s] != 0xFFFF) phantom++;
        }
      }
      printf("link test: roles");
      for (i = 0; i < ncon; i++)
        printf(" %u", e[i][2]);
      printf(", transfers %u, first %u words compared, %u differ, "
             "%u player words missing, %u words from nobody\n",
             e[0][0] / 2, n, diff, absent, phantom);
      if (n > 0)
        printf("  first words: %08x %08x / %08x %08x\n", e[0][4], e[0][5],
               e[ncon - 1][4], e[ncon - 1][5]);
      for (i = 0; i < ncon; i++)
        if (e[i][2] != (unsigned)i + 1)
          ok = 0;
      if (n < 20 || diff || phantom || absent > n / 20)
        ok = 0;
    }
  }

  /* Everybody leaves (the clients first, as the host hears about each). */
  if (sess + 1 < sessions) {
    pkt *p;
    for (i = 1; i < ncon; i++) {
      if (con[i].np_set && con[i].np.stop) con[i].np.stop();
      con[i].started = con[i].playing = false;
      if (con[0].np.disconnected) con[0].np.disconnected((uint16_t)i);
    }
    if (con[0].np.stop) con[0].np.stop();
    con[0].started = false;
    pthread_mutex_lock(&q_mtx);
    while ((p = q_head)) { q_head = p->next; free(p); }
    q_tail = NULL;
    pthread_mutex_unlock(&q_mtx);
    opt_netplay = false;
    printf("session %u: %s\n", sess + 1, ok ? "ok" : "FAILED");
    if (!ok) break;
  }
  }
  }

  for (i = 0; i < ncon; i++)
    if (con[i].framelog)
      fclose(con[i].framelog);

  /* SAVEOUT=dir: each console's save memory, as dir/cN.sav. */
  if (getenv("SAVEOUT"))
    for (i = 0; i < ncon; i++) if (LOCAL(i)) {
      char path[512];
      const unsigned char *m = con[i].get_memory_data(RETRO_MEMORY_SAVE_RAM);
      FILE *fp;
      snprintf(path, sizeof(path), "%s/c%d.sav", getenv("SAVEOUT"), i);
      if (m && (fp = fopen(path, "wb"))) { fwrite(m, 1, 1024 * 128, fp); fclose(fp); }
    }
  for (i = 0; i < ncon; i++) if (LOCAL(i)) {
    if (con[i].np_set && con[i].np.stop) con[i].np.stop();
    con[i].unload_game();
    con[i].deinit();
    unlink(copy[i]);
  }
  printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

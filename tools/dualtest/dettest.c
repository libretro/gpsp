/* dettest: runs one gpSP core alone with scripted buttons and prints a hash
 * of the machine every frame, to compare builds (ARM32 / ARM64 / x86
 * dynarec, interpreter): a GBA copied to another console must give the very
 * same frames there.
 *
 *   dettest CORE.so ROM FRAMES > hashes.txt
 *
 * Environment: DRC=0 runs the interpreter, KEYS = "from-to:key+key;..."
 * as in dualtest (random buttons if not set), DUMP=n,dir: writes the
 * savestate after frame n to dir/state.bin. BIOSDIR=dir: official BIOS
 * (dir/gba_bios.bin). SHOTS=dir with SHOT_EVERY=n: screens. SAV=file loads
 * the save memory, SAVEOUT=file writes it at the end.
 * Each line: frame, hash of EWRAM, hash of the whole savestate.
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static uint32_t cur_keys;

static const char *opt(const char *key) {
  if (!strcmp(key, "gpsp_serial")) return getenv("SERIAL") ? getenv("SERIAL") : "disabled";
  if (!strcmp(key, "gpsp_bios")) return getenv("BIOSDIR") ? "official" : "builtin";
  if (!strcmp(key, "gpsp_boot_mode")) return getenv("BOOT") ? getenv("BOOT") : "game";
  if (!strcmp(key, "gpsp_drc")) return getenv("DRC") && !strcmp(getenv("DRC"), "0") ? "disabled" : "enabled";
  if (!strcmp(key, "gpsp_frameskip")) return "disabled";
  return NULL;
}

static bool env(unsigned cmd, void *data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable *v = data;
    v->value = opt(v->key);
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
  case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
    const struct retro_message_ext *m = data;
    fprintf(stderr, "message: %s\n", m->msg);
    return true;
  }
  default:
    return false;
  }
}

static unsigned cur_frame;
/* SHOTS=dir, SHOT_EVERY=n: the screen every n frames, as dir/FRAME.ppm */
static void video(const void *d, unsigned w, unsigned h, size_t p) {
  const char *dir = getenv("SHOTS"), *ev = getenv("SHOT_EVERY");
  unsigned x, y, every = ev ? (unsigned)atoi(ev) : 0;
  char path[512];
  FILE *fp;
  if (!d || !dir || !every || cur_frame % every) return;
  snprintf(path, sizeof(path), "%s/%05u.ppm", dir, cur_frame);
  if (!(fp = fopen(path, "wb"))) return;
  fprintf(fp, "P6 %u %u 255\n", w, h);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
      const uint16_t c = ((const uint16_t *)((const uint8_t *)d + y * p))[x];
      const unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3),
        (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
      fwrite(rgb, 1, 3, fp);
    }
  fclose(fp);
}
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)idx;
  if (port || dev != RETRO_DEVICE_JOYPAD) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)cur_keys;
  return (cur_keys >> id) & 1;
}

static bool scripted(unsigned frame, uint32_t *keys) {
  static const struct { const char *n; int id; } names[] = {
    { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
    { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
    { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
    { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { "l", RETRO_DEVICE_ID_JOYPAD_L }, { "r", RETRO_DEVICE_ID_JOYPAD_R },
  };
  const char *s = getenv("KEYS");
  if (!s) return false;
  *keys = 0;
  while (*s) {
    char *e;
    const unsigned from = (unsigned)strtoul(s, &e, 10);
    unsigned to = from;
    if (*e == '-') to = (unsigned)strtoul(e + 1, &e, 10);
    if (*e == ':') e++;
    while (*e && *e != ';') {
      size_t n = strcspn(e, "+;"), k;
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

static uint32_t fnv(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

#define SYM(name) do { *(void **)&name = dlsym(h, #name); if (!name) { fprintf(stderr, "no %s\n", #name); return 2; } } while (0)

int main(int argc, char **argv) {
  void *h;
  void (*retro_set_environment)(retro_environment_t);
  void (*retro_set_video_refresh)(retro_video_refresh_t);
  void (*retro_set_audio_sample)(retro_audio_sample_t);
  void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*retro_set_input_poll)(retro_input_poll_t);
  void (*retro_set_input_state)(retro_input_state_t);
  void (*retro_init)(void);
  bool (*retro_load_game)(const struct retro_game_info *);
  void (*retro_run)(void);
  void *(*retro_get_memory_data)(unsigned);
  size_t (*retro_get_memory_size)(unsigned);
  size_t (*retro_serialize_size)(void);
  bool (*retro_serialize)(void *, size_t);
  struct retro_game_info info;
  unsigned frames, f, dump_at = 0;
  const char *dump_dir = NULL;
  size_t ssize;
  uint8_t *st;

  if (argc < 4) { fprintf(stderr, "dettest CORE ROM FRAMES\n"); return 2; }
  frames = (unsigned)atoi(argv[3]);
  if (getenv("DUMP")) {
    dump_at = (unsigned)atoi(getenv("DUMP"));
    dump_dir = strchr(getenv("DUMP"), ',');
    if (dump_dir) dump_dir++;
  }
  h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
  SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
  SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_get_memory_data);
  SYM(retro_get_memory_size); SYM(retro_serialize_size); SYM(retro_serialize);

  retro_set_environment(env);
  retro_init();
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio);
  retro_set_audio_sample_batch(audio_batch);
  retro_set_input_poll(poll);
  retro_set_input_state(input);
  memset(&info, 0, sizeof(info));
  info.path = argv[2];
  if (!retro_load_game(&info)) { fprintf(stderr, "load_game failed\n"); return 2; }

  /* SAV=file: save memory to start with; SAVEOUT=file: written at the end */
  if (getenv("SAV")) {
    FILE *fp = fopen(getenv("SAV"), "rb");
    uint8_t *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (fp && m) {
      fread(m, 1, retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), fp);
      fclose(fp);
    }
  }
  ssize = retro_serialize_size();
  st = malloc(ssize);
  for (f = 1; f <= frames; f++) {
    const uint8_t *ew = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    uint32_t x = (f / 9) * 2654435761u;
    if (!scripted(f, &cur_keys)) { x ^= x >> 13; cur_keys = x & 0xFF; }
    cur_frame = f;
    retro_run();
    memset(st, 0, ssize);
    retro_serialize(st, ssize);
    {
      /* The clock the RTC started from is the time the game was loaded:
       * not part of the comparison (the dual link shares one). */
      static const char key[] = "rtc-base-time";
      size_t i;
      for (i = 0; i + sizeof(key) + 32 < ssize; i++)
        if (!memcmp(&st[i], key, sizeof(key))) {
          memset(&st[i + sizeof(key)], 0, 32);
          break;
        }
    }
    printf("%u %08x %08x\n", f, fnv(ew, retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)),
           fnv(st, ssize));
    if (dump_dir && f == dump_at) {
      char path[512];
      FILE *fp;
      snprintf(path, sizeof(path), "%s/state.bin", dump_dir);
      if ((fp = fopen(path, "wb"))) { fwrite(st, 1, ssize, fp); fclose(fp); }
    }
  }
  if (getenv("SAVEOUT")) {
    FILE *fp = fopen(getenv("SAVEOUT"), "wb");
    const uint8_t *m = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (fp && m) {
      fwrite(m, 1, retro_get_memory_size(RETRO_MEMORY_SAVE_RAM), fp);
      fclose(fp);
    }
  }
  return 0;
}

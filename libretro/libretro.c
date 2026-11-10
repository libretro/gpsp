

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <streams/file_stream.h>
#include <compat/strl.h>
#include <libretro.h>

#include "../common.h"
#include "../memmap.h"
#include "libretro_core_options.h"

#include "../gba_memory.h"
#include "../gba_cc_lut.h"

#if defined(VITA) && defined(HAVE_DYNAREC)
#include <psp2/kernel/sysmem.h>
static int translation_caches_inited = 0;
static inline int align(int x, int n) {
  return (((x >> n) + 1) << n );
}

#define FOUR_KB_ALIGN(x) align(x, 12)
#define MB_ALIGN(x) align(x, 20)

int _newlib_vm_size_user = ROM_TRANSLATION_CACHE_SIZE +
                           RAM_TRANSLATION_CACHE_SIZE; 

int getVMBlock();

#endif

#if defined(_3DS)
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#if defined(HAVE_DYNAREC)
#include <malloc.h>
#include "3ds/3ds_utils.h"
#define MEMOP_PROT   6
#define MEMOP_MAP 4
#define MEMOP_UNMAP 5
int32_t svcDuplicateHandle(uint32_t* out, uint32_t original);
int32_t svcCloseHandle(uint32_t handle);
int32_t svcControlProcessMemory(uint32_t process, void* addr0, void* addr1, uint32_t size, uint32_t type, uint32_t perm);
static int translation_caches_inited = 0;
#endif
#endif

#ifndef MAX_PATH
#define MAX_PATH (512)
#endif

// Usually 59.72750057 Hz, unless GBC_RATE is overclocked (for 60FPS)
#define GBA_FPS ((float) GBC_BASE_RATE) / (308 * 228 * 4)

static s16 *audio_sample_buffer        = NULL;
static float audio_samples_per_frame   = 0.0f;
static float audio_samples_accumulator = 0.0f;

/* Apply the gpsp_sound_rate core option. reinit_env is non-NULL only for
 * mid-session changes (retro_run variable check), where issuing
 * RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO is legal; at load time the new rate
 * is picked up by the normal av_info and buffer init paths. */
static void gpsp_apply_sound_rate(const char *value, retro_environment_t reinit_env)
{
   u32 new_bits = (value && !strcmp(value, "32768")) ? 15 : 16;

   if (new_bits == sound_freq_bits)
      return;

   sound_freq_bits = new_bits;
   sound_frequency = 1u << new_bits;
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;

   if (reinit_env)
   {
      struct retro_system_av_info av_info;
      render_gbc_sound();          /* drain pending ticks at the old rate */
      sound_frequency_changed();   /* recompute all derived steps */
      sound_flush_ring();
      retro_get_system_av_info(&av_info);
      reinit_env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
   }
}

/* Workaround for a RetroArch audio driver
 * limitation: a maximum of 1024 frames
 * can be written per call of audio_batch_cb() */
#define AUDIO_BATCH_FRAMES_MAX 1024

/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 30

u32 skip_next_frame                          = 0;
static frameskip_type current_frameskip_type = no_frameskip;
static u32 frameskip_threshold               = 0;
static u32 frameskip_interval                = 0;
static u32 frameskip_counter                 = 0;
static bool audio_buff_active                = false;
static unsigned audio_buff_occupancy         = 0;
static bool audio_buff_underrun              = false;
static unsigned audio_latency                = 0;
static bool update_audio_latency             = false;
static bios_type selected_bios               = auto_detect;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_environment_t environ_cb;
static retro_set_rumble_state_t rumble_cb;

struct retro_perf_callback perf_cb;

int dynarec_enable;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;

static int rtc_mode = FEAT_AUTODETECT;
static int rumble_mode = FEAT_AUTODETECT;
static int serial_setting = SERIAL_MODE_AUTO;
static bool cable_boot_pending = false;    /* Reset the client when the cable gets plugged */
static unsigned cable_players = 2;         /* GBAs on the Cable Link (2-4) */
static bool cable_dual_pending = false;    /* Real time cable unplugged meanwhile */
static bool cable_bios_boot = false;       /* Linked: boot through the BIOS */
static boot_mode cable_saved_boot;

/* Dual Cable Link only: when the session starts, the linked consoles
 * restart through their BIOS (gba_bios.bin, or the built-in one, as the
 * BIOS option says), whatever the boot mode option of each console says:
 * every GBA of the link then starts the same way. It stays until the
 * session ends (a reset while linked restarts every GBA the same way);
 * everywhere else the boot mode option is followed. */
void cable_force_bios_boot(void)
{
   if (!cable_bios_boot)
      cable_saved_boot = selected_boot_mode;
   selected_boot_mode = boot_bios;
   cable_bios_boot    = true;
}

/* The session is over: the boot mode option counts again. */
void cable_end_bios_boot(void)
{
   if (!cable_bios_boot)
      return;
   cable_bios_boot    = false;
   selected_boot_mode = cable_saved_boot;
}

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static u16 *gba_screen_pixels_prev = NULL;
static u16 *gba_processed_pixels   = NULL;

static void (*video_post_process)(void) = NULL;
static bool post_process_cc  = false;
static bool post_process_mix = false;

static void error_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "[gpSP]: %s\n", text);
}

static void info_msg(const char* text)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[gpSP]: %s\n", text);
}

static void show_warning_message(const char* text, unsigned durationms) {
  unsigned ifversion = 0;
  if (!environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &ifversion) || ifversion >= 1) {
    /* Use the new API to display messages */
    struct retro_message_ext msg = {
      .msg = text, .duration = durationms,
      .priority = 2, .level = RETRO_LOG_WARN,
      .target = RETRO_MESSAGE_TARGET_ALL,
      .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
      .progress = -1,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
  }
  else {
    struct retro_message msg = {.msg = text, .frames = durationms / 17};
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
  }
}

/* Frameskip START */

static void audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   audio_buff_active    = active;
   audio_buff_occupancy = occupancy;
   audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (current_frameskip_type == no_frameskip)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }
   else
   {
      bool calculate_audio_latency = true;

      if (current_frameskip_type == fixed_interval_frameskip)
         environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      else
      {
         struct retro_audio_buffer_status_callback buff_status_cb;
         buff_status_cb.callback = audio_buff_status_cb;

         if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &buff_status_cb))
         {
            error_msg("Frameskip disabled - frontend does not support audio buffer status monitoring");

            audio_buff_active       = false;
            audio_buff_occupancy    = 0;
            audio_buff_underrun     = false;
            audio_latency           = 0;
            calculate_audio_latency = false;
         }
      }

      if (calculate_audio_latency)
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / ((float) GBA_FPS);

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }

   }

   update_audio_latency = true;
   frameskip_counter    = 0;
}

/* Frameskip END */

/* Video post processing START */

/* Note: This code is intentionally W.E.T.
 * (Write Everything Twice). These functions
 * are performance critical, and we cannot
 * afford to do unnecessary comparisons/switches
 * inside the inner for loops */

static void video_post_process_cc(void)
{
   const uint16_t *src = gba_screen_pixels;
   uint16_t       *dst = gba_processed_pixels;
   const size_t    npx = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t src_color = src[i];

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((src_color & 0xFFC0) >> 1) | (src_color & 0x1F)];
   }
}

static void video_post_process_mix(void)
{
   /* Flat loop: pitch equals width so the row-stride bookkeeping in
    * the original two-level form was bookkeeping for no actual stride
    * mismatch.  Compilers can vectorize the flat loop more aggressively;
    * on ARM Cortex-A it makes a measurable difference at -O2.  After the
    * mix we used to memcpy(prev, curr, 75 KB) so the next frame's mix
    * could read the previous frame; that copy is now elided by swapping
    * the two buffer pointers - after the swap, gba_screen_pixels points
    * to what was the prev buffer (about to be overwritten by the next
    * scanline-render pass), and gba_screen_pixels_prev points at the
    * just-rendered frame.  Saves ~4.5 MB/s of memory bandwidth at 60 fps
    * with mix enabled. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      /* Mix colours:
       *   http://blargg.8bitalley.com/info/rgb_mixing.html */
      dst[i] = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;
   }

   /* Swap curr/prev: the just-rendered frame becomes prev for next time,
    * and the previous prev (now stale and about to be overwritten by the
    * next render pass) becomes the new render target.  Equivalent to the
    * old 'memcpy(prev, curr, GBA_SCREEN_BUFFER_SIZE)' but without the
    * copy.  Safe because the scanline renderer fully overwrites every
    * pixel of gba_screen_pixels before any read (fill_line_background
    * runs first per scanline) - no read-before-write hazard on the
    * post-swap buffer. */
   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void video_post_process_cc_mix(void)
{
   /* See video_post_process_mix for the loop-flatten + buffer-swap
    * rationale.  Same shape, plus the gba_cc_lut colour-correction
    * lookup on the mixed value. */
   const uint16_t *src_curr = gba_screen_pixels;
   const uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t       *dst      = gba_processed_pixels;
   const size_t    npx      = GBA_SCREEN_HEIGHT * GBA_SCREEN_PITCH;
   size_t i;

   for (i = 0; i < npx; i++)
   {
      uint16_t rgb_curr = src_curr[i];
      uint16_t rgb_prev = src_prev[i];

      uint16_t rgb_mix  = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;

      /* Convert colour to RGB555 and perform lookup */
      dst[i] = gba_cc_lut[((rgb_mix & 0xFFC0) >> 1) | (rgb_mix & 0x1F)];
   }

   {
      u16 *t = gba_screen_pixels;
      gba_screen_pixels = gba_screen_pixels_prev;
      gba_screen_pixels_prev = t;
   }
}

static void init_post_processing(void)
{
   video_post_process = NULL;

   /* If post processing is disabled, return
    * immediately */
   if (!post_process_cc && !post_process_mix)
      return;

   /* Initialise output buffer, if required */
   if (!gba_processed_pixels &&
       (post_process_cc || post_process_mix))
   {
#ifdef _3DS
      gba_processed_pixels = (u16*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_processed_pixels = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

      if (!gba_processed_pixels)
         return;

      memset(gba_processed_pixels, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Initialise 'history' buffer, if required.  If this alloc fails we
    * must NOT fall through to the mix-variant assignment below: those
    * functions dereference gba_screen_pixels_prev unconditionally.
    * Leave the user setting (post_process_mix) intact - the failure
    * may be transient - but skip the function-pointer assignment for
    * the mix path so video_run keeps working. */
   if (!gba_screen_pixels_prev &&
       post_process_mix)
   {
      gba_screen_pixels_prev = (u16*)malloc(GBA_SCREEN_BUFFER_SIZE);

      if (!gba_screen_pixels_prev && log_cb)
         log_cb(RETRO_LOG_WARN,
                "Failed to allocate frame history buffer; disabling mix component of post-processing this run\n");
      else if (gba_screen_pixels_prev)
         memset(gba_screen_pixels_prev, 0xFFFF, GBA_SCREEN_BUFFER_SIZE);
   }

   /* Assign post processing function.  Treat post_process_mix as
    * effectively false if the history buffer is missing. */
   {
      bool mix_active = post_process_mix && (gba_screen_pixels_prev != NULL);

      if (post_process_cc && mix_active)
         video_post_process = video_post_process_cc_mix;
      else if (post_process_cc)
         video_post_process = video_post_process_cc;
      else if (mix_active)
         video_post_process = video_post_process_mix;
   }
}

/* Video post processing END */

/* Fast forward override */
void set_fastforward_override(bool fastforward)
{
   struct retro_fastforwarding_override ff_override;

   if (!libretro_supports_ff_override)
      return;

   ff_override.ratio        = -1.0f;
   ff_override.notification = true;

   if (fastforward)
   {
      ff_override.fastforward    = true;
      ff_override.inhibit_toggle = true;
   }
   else
   {
      ff_override.fastforward    = false;
      ff_override.inhibit_toggle = false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
}

static void audio_run(void)
{
   s16 *audio_buffer_ptr;
   u32 samples_to_read;
   u32 samples_produced;

   /* If retro_init's malloc failed we still get called every frame;
    * skip the work in that case rather than write through NULL. */
   if (!audio_sample_buffer)
      return;

#ifdef GPSP_INSTANCE_B
   /* Dual Cable Link: nobody hears this GBA (it makes no samples). */
   return;
#endif

   /* audio_samples_per_frame is decimal;
    * get integer component */
   samples_to_read = (u32)audio_samples_per_frame;

   /* Account for fractional component */
   audio_samples_accumulator += audio_samples_per_frame -
         (float)samples_to_read;

   if (audio_samples_accumulator >= 1.0f)
   {
      samples_to_read++;
      audio_samples_accumulator -= 1.0f;
   }

   samples_produced = sound_read_samples(audio_sample_buffer, samples_to_read);

   /* Workaround for a RetroArch audio driver
    * limitation: a maximum of 1024 frames
    * can be written per call of audio_batch_cb(),
    * so we have to send samples in chunks */
   audio_buffer_ptr = audio_sample_buffer;
   while (samples_produced > 0)
   {
      u32 samples_to_write = (samples_produced > AUDIO_BATCH_FRAMES_MAX) ?
            AUDIO_BATCH_FRAMES_MAX : samples_produced;

      audio_batch_cb(audio_buffer_ptr, samples_to_write);

      samples_produced -= samples_to_write;
      audio_buffer_ptr += samples_to_write << 1;
   }
}

#ifndef GPSP_INSTANCE_B
/* A frame of silence: a frame held back by the Dual Cable Link takes as long
 * as any other one (with audio sync, a frame with no sound takes no time). */
static void audio_silence_run(void)
{
   static const s16 zeros[AUDIO_BATCH_FRAMES_MAX * 2];
   u32 samples = (u32)audio_samples_per_frame;

   while (samples > 0)
   {
      u32 samples_to_write = MIN(samples, AUDIO_BATCH_FRAMES_MAX);
      audio_batch_cb(zeros, samples_to_write);
      samples -= samples_to_write;
   }
}
#endif

static void video_run(void)
{
   u16 *gba_screen_pixels_buf = gba_screen_pixels;

   if (skip_next_frame)
   {
      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
      return;
   }

   if (video_post_process)
   {
      video_post_process();
      gba_screen_pixels_buf = gba_processed_pixels;
   }

   video_cb(gba_screen_pixels_buf, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
}

// Netplay (Netpacket) interface

u32 netplay_num_clients = 0, netplay_client_id = 0;
static retro_netpacket_send_t netpacket_send_fn_ptr = NULL;
static retro_netpacket_poll_receive_t netpacket_pollrcv_fn_ptr = NULL;

void netpacket_poll_receive() {
  if (netpacket_pollrcv_fn_ptr)
    netpacket_pollrcv_fn_ptr();
}

bool netpacket_active(void) {
  return netpacket_send_fn_ptr != NULL;
}

// Host (wall clock) time, used to bound the waits on the network.
// Frontends are not required to have a clock (the perf interface), so the
// system one is used wherever there is one.
u64 netpacket_time_usec(void) {
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
  struct timespec ts;
  if (!clock_gettime(CLOCK_MONOTONIC, &ts))
    return (u64)ts.tv_sec * 1000000ULL + (u64)(ts.tv_nsec / 1000);
#endif
  return (u64)perf_cb.get_time_usec();
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len) {
  // Force all packets to be flushed ASAP, to minimize latency.
  if (netpacket_send_fn_ptr)
    netpacket_send_fn_ptr(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, buf, len, client_id);
}

// Something the player must know about the link (shown for a few seconds).
void netpacket_message(const char *text) {
  show_warning_message(text, 4000);
}

/* The Cable Link players option. Read again when a netplay session starts:
 * it may have been changed in the menu just before, with no frame run since
 * (the frame is what reads the changed options). */
static void read_cable_players(void) {
   struct retro_variable var;
   var.key   = "gpsp_cable_players";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      const int players = atoi(var.value);
      cable_players = (players >= 2 && players <= 4) ? (unsigned)players : 2;
   }
#ifndef GPSP_INSTANCE_B
   dual_set_players(cable_players);
   /* Automatic unless the user asked for the real time cable: most people
    * never open the core options. */
   var.key   = "gpsp_cable_different_games";
   var.value = NULL;
   dual_set_other_games(!(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) &&
                          var.value && !strcmp(var.value, "realtime")));
#endif
}

/* Everything that makes this GBA a different machine: each copy of it on
 * the other consoles must be the same. (Worked out again for each netplay
 * session: some of it can be changed in the menu. The hidden GBAs have it
 * too: dual.c checks that a copy came out as its owner says.) */
u32 dual_machine_cfg(void)
{
   u32 i, cfg = 2166136261u;
   /* (Not the boot mode: the link restarts every GBA through the BIOS, see
    * cable_force_bios_boot. The BIOS itself is compared apart.) */
   const u32 v[] = { (u32)dynarec_enable, (u32)rtc_mode, (u32)rumble_mode,
                     idle_loop_target_pc,
                     translation_gate_targets,
                     /* (the sound channels' length and envelope counters,
                      * which the game reads, step at this rate) */
                     sound_freq_bits };
   for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
      cfg = (cfg ^ v[i]) * 16777619u;
   for (i = 0; i < translation_gate_targets; i++)
      cfg = (cfg ^ translation_gate_target_pc[i]) * 16777619u;
   return cfg;
}

/* The options behind it (dual.h, DUAL_OPT_*): a copy of this GBA on another
 * console is loaded with these, whatever that console has. */
u32 dual_machine_opts(void)
{
   return ((u32)dynarec_enable & 1) |
          ((u32)(rtc_mode + 1) & 3) << 1 |
          ((u32)(rumble_mode + 1) & 3) << 3 |
          (sound_freq_bits & 31) << 5;
}

/* The BIOS for a link session: these 16 KB, or the built-in one (NULL).
 * Only between frames, with the game about to restart (or loaded but not
 * run yet, for a hidden GBA). */
static void load_builtin_bios(void);
void dual_set_bios(const u8 *src)
{
   if (src)
      memcpy(bios_rom, src, sizeof(bios_rom));
   else
      load_builtin_bios();
#ifdef HAVE_DYNAREC
   /* Not just a flush: the SWI entry point (BIOS 0x8) is translated once,
    * below the watermark a flush keeps, when the emitter starts (as
    * reset_gba does it). A hidden GBA is not reset before its state is
    * loaded: it would go on into the other BIOS through the old entry. */
#if defined(MMAP_JIT_CACHE)
   if (rom_translation_cache)
#endif
   {
      init_dynarec_caches();
      init_emitter(gamepak_must_swap());
   }
#endif
}

static void netpacket_start(uint16_t client_id, retro_netpacket_send_t send_fn, retro_netpacket_poll_receive_t poll_receive_fn) {
  read_cable_players();
  netpacket_send_fn_ptr = send_fn;
  netpacket_pollrcv_fn_ptr = poll_receive_fn;
  netplay_num_clients = 0;
  netplay_client_id = client_id;

  if (serial_mode == SERIAL_MODE_SERIAL_CABLE) {
    serialcable_session_reset();
#ifndef GPSP_INSTANCE_B
    // Try the dual cable first (every GBA on each console): it restarts
    // every game once all the players are there. If the real time cable is
    // used instead, the client restarts then (dual_take_boot_reset).
    dual_set_cfg(dual_machine_cfg());
    dual_session_start(client_id);
#else
    // The client console is power cycled with the cable already plugged, so
    // that games probing the link at boot see it.
    if (client_id)
      cable_boot_pending = true;
#endif
  }
}

// Netplay session ends.
static void netpacket_stop() {
  netpacket_send_fn_ptr = NULL;
  netpacket_pollrcv_fn_ptr = NULL;

  if (serial_mode == SERIAL_MODE_RFU)
    rfu_net_disconnect(-1);   // Everybody is gone: tell the game.

#ifndef GPSP_INSTANCE_B
  // Back to a single GBA (and to the real time cable mode).
  dual_session_stop();
#endif

  if (serial_mode == SERIAL_MODE_SERIAL_CABLE) {
    // Cable unplugged: we are a lone console again.
    netplay_num_clients = 0;
    netplay_client_id = 0;
    cable_boot_pending = false;
    serialcable_session_reset();
  }
}

static void netpacket_receive(const void* buf, size_t len, uint16_t client_id) {
  switch (serial_mode) {
  case SERIAL_MODE_RFU:
    rfu_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_POKE:
    serialpoke_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_AW1:
  case SERIAL_MODE_SERIAL_AW2:
    serialaw_net_receive(buf, len, client_id);
    break;
  case SERIAL_MODE_SERIAL_CABLE:
  case SERIAL_MODE_DUAL_CABLE:
#ifndef GPSP_INSTANCE_B
    if (dual_net_receive(buf, len, client_id))
      break;
#endif
    serialcable_net_receive(buf, len, client_id);
    break;
  };
}

// Ensure we do not have too many clients for the type of connection used.
static bool netpacket_connected(uint16_t client_id) {
  const uint8_t maxpl[] = {
     0,                        // SERIAL_MODE_DISABLED
     0,                        // SERIAL_MODE_GBP
     MAX_RFU_NETPLAYERS,       // SERIAL_MODE_RFU
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_POKE
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW1
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_AW2
     0,                        // SERIAL_MODE_AUTO
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_SERIAL_CABLE
     MAX_SERMULT_NETPLAYERS,   // SERIAL_MODE_DUAL_CABLE
  };
  u32 max_clients = maxpl[serial_mode] - 1U;

  // The Cable Link takes as many consoles as the players option says.
  if (serial_mode == SERIAL_MODE_SERIAL_CABLE ||
      serial_mode == SERIAL_MODE_DUAL_CABLE)
    max_clients = MIN(max_clients, cable_players - 1U);

  if (netplay_num_clients >= max_clients)
    return false;

  // The cable has 4 ends: the frontend numbers its connections, and one that
  // is busy (ie. a spectator) can push a player past 3, which none of the
  // cable modes has room for (the real time cable would even take it for
  // the host).
  if (maxpl[serial_mode] == MAX_SERMULT_NETPLAYERS &&
      client_id >= MAX_SERMULT_NETPLAYERS)
    return false;

#ifndef GPSP_INSTANCE_B
  if (serial_mode == SERIAL_MODE_SERIAL_CABLE ||
      serial_mode == SERIAL_MODE_DUAL_CABLE) {
    if (!dual_peer_connected(client_id))
      return false;
  }
#endif

  netplay_num_clients++;
  if (serial_mode == SERIAL_MODE_SERIAL_CABLE)
    serialcable_peer_connected(client_id);
  return true;
}

static void netpacket_disconnected(uint16_t client_id) {
  // (Never below zero: nobody could join again for the rest of the session.)
  if (netplay_num_clients)
    netplay_num_clients--;
  if (serial_mode == SERIAL_MODE_RFU)
    rfu_net_disconnect(client_id);
#ifndef GPSP_INSTANCE_B
  if (serial_mode == SERIAL_MODE_SERIAL_CABLE ||
      serial_mode == SERIAL_MODE_DUAL_CABLE)
    dual_peer_disconnected(client_id);
#endif
  if (serial_mode == SERIAL_MODE_SERIAL_CABLE)
    serialcable_peer_disconnected(client_id);
}

const struct retro_netpacket_callback netpacket_iface = {
  netpacket_start,          /* start */
  netpacket_receive,        /* receive */
  netpacket_stop,           /* stop */
  NULL,                     /* poll */
  netpacket_connected,      /* connected */
  netpacket_disconnected,   /* disconnected */
  GPSP_NETPACKET_VERSION,   /* core version char* */
};


#ifdef PERF_TEST

extern struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(X) \
   static struct retro_perf_counter X = {#X}; \
   do { \
      if (!(X).registered) \
         perf_cb.perf_register(&(X)); \
   } while(0)

#define RETRO_PERFORMANCE_START(X) perf_cb.perf_start(&(X))
#define RETRO_PERFORMANCE_STOP(X) perf_cb.perf_stop(&(X))
#else
#define RETRO_PERFORMANCE_INIT(X)
#define RETRO_PERFORMANCE_START(X)
#define RETRO_PERFORMANCE_STOP(X)

#endif

void retro_get_system_info(struct retro_system_info* info)
{
   info->library_name = GPSP_NAME;
 #ifdef GIT_VERSION
   info->library_version = GPSP_VERSION "-" GIT_VERSION;
 #else
   info->library_version = GPSP_VERSION;
 #endif
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gba|bin|agb|gbz|u1" ;
}


void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = GBA_SCREEN_WIDTH;
   info->geometry.base_height = GBA_SCREEN_HEIGHT;
   info->geometry.max_width = GBA_SCREEN_WIDTH;
   info->geometry.max_height = GBA_SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 3.0f / 2.0f;
   info->timing.fps = ((float) GBA_FPS);
   info->timing.sample_rate = sound_frequency;
}

void retro_init(void)
{
   u32 audio_sample_buffer_size;
   audio_samples_per_frame   = (float)sound_frequency / (float)(GBA_FPS);
   audio_samples_accumulator = 0.0f;
   audio_sample_buffer_size  = (((u32)((float)(GBA_SOUND_FREQUENCY) / (float)(GBA_FPS))) + 1) * 2;
   audio_sample_buffer       = (s16*)malloc(audio_sample_buffer_size * sizeof(s16));
   if (!audio_sample_buffer)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR,
                "Failed to allocate audio sample buffer (%u bytes)\n",
                (unsigned)(audio_sample_buffer_size * sizeof(s16)));
      /* Continue: audio_run() now guards against this NULL and the
       * frontend just runs silent.  Better than failing retro_init
       * outright on memory-constrained targets where a small audio
       * allocation may fail but the rest of the core still works. */
   }

#if defined(HAVE_DYNAREC)
  #if defined(MMAP_JIT_CACHE)
   rom_translation_cache = map_jit_block(ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
   if (rom_translation_cache)
      ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
   else if (log_cb)
      /* No usable block within branch range of the text segment (or
       * RWX denied, e.g. hardened runtimes without a JIT entitlement).
       * check_variables pins dynarec_enable to 0 in this case and the
       * core runs on the interpreter. */
      log_cb(RETRO_LOG_ERROR,
             "Failed to map the JIT translation cache; "
             "falling back to the interpreter\n");
  #elif defined(_3DS)
   if (__ctr_svchax && !translation_caches_inited)
   {
      uint32_t currentHandle;
      check_rosalina();
      
      rom_translation_cache_ptr  = memalign(0x1000, ROM_TRANSLATION_CACHE_SIZE);
      ram_translation_cache_ptr  = memalign(0x1000, RAM_TRANSLATION_CACHE_SIZE);

      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      svcControlProcessMemory(currentHandle,
                              rom_translation_cache, rom_translation_cache_ptr,
                              ROM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
      svcControlProcessMemory(currentHandle,
                              ram_translation_cache, ram_translation_cache_ptr,
                              RAM_TRANSLATION_CACHE_SIZE, MEMOP_MAP, 0b111);
      svcCloseHandle(currentHandle);
      rom_translation_ptr = rom_translation_cache;
      ram_translation_ptr = ram_translation_cache;
      ctr_flush_invalidate_cache();
      translation_caches_inited = 1;
   }
  #elif defined(VITA)
   if(!translation_caches_inited){
      void* currentHandle;

      sceBlock = getVMBlock();
      
      if (sceBlock < 0)
      {
        return;
      }

      // get base address
      int ret = sceKernelGetMemBlockBase(sceBlock, &currentHandle);
      if (ret < 0)
      {
        return;
      }

      rom_translation_cache  = (u8*)currentHandle;
      ram_translation_cache  = rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE;
      rom_translation_ptr = rom_translation_cache;
      ram_translation_ptr = ram_translation_cache;
      sceKernelOpenVMDomain();
      translation_caches_inited = 1;
    }
  #endif
#endif

   init_gamepak_buffer();
   init_sound();

   if(!gba_screen_pixels)
#ifdef _3DS
      gba_screen_pixels = (uint16_t*)linearMemAlign(GBA_SCREEN_BUFFER_SIZE, 128);
#else
      gba_screen_pixels = (uint16_t*)malloc(GBA_SCREEN_BUFFER_SIZE);
#endif

   /* gba_screen_pixels is dereferenced unconditionally by both the
    * renderer (writes from update_scanline) and the post-process /
    * video_cb hand-off in video_run.  A NULL here means we cannot
    * safely run a single frame.  Log loudly; load_game will return
    * the failure to the frontend so the core unloads cleanly. */
   if (!gba_screen_pixels && log_cb)
      log_cb(RETRO_LOG_ERROR,
             "Failed to allocate frame buffer (%u bytes); core will refuse to load content\n",
             (unsigned)GBA_SCREEN_BUFFER_SIZE);

   libretro_supports_bitmasks = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   libretro_supports_ff_override = false;
   if (environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, NULL))
      libretro_supports_ff_override = true;

   // This interface is actually optional
   environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&netpacket_iface);

#ifndef GPSP_INSTANCE_B
   dual_set_frontend(environ_cb, show_warning_message);
#endif

   current_frameskip_type = no_frameskip;
   frameskip_threshold    = 0;
   frameskip_interval     = 0;
   frameskip_counter      = 0;
   audio_buff_active      = false;
   audio_buff_occupancy   = 0;
   audio_buff_underrun    = false;
   audio_latency          = 0;
   update_audio_latency   = false;
   selected_bios          = auto_detect;
   selected_boot_mode     = boot_game;
}

void retro_deinit(void)
{
#ifndef GPSP_INSTANCE_B
   dual_shutdown();
#endif
   perf_cb.perf_log();
   memory_term();

#if defined(MMAP_JIT_CACHE) && defined(HAVE_DYNAREC)
   if (rom_translation_cache)
      unmap_jit_block(rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
#endif
#if defined(_3DS) && defined(HAVE_DYNAREC)

   if (__ctr_svchax && translation_caches_inited)
   {
      uint32_t currentHandle;
      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      svcControlProcessMemory(currentHandle,
                              rom_translation_cache, rom_translation_cache_ptr,
                              ROM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcControlProcessMemory(currentHandle,
                              ram_translation_cache, ram_translation_cache_ptr,
                              RAM_TRANSLATION_CACHE_SIZE, MEMOP_UNMAP, 0b111);
      svcCloseHandle(currentHandle);
      free(rom_translation_cache_ptr);
      free(ram_translation_cache_ptr);
      translation_caches_inited = 0;
   }
#endif

#if defined(VITA) && defined(HAVE_DYNAREC)
    if(translation_caches_inited){
        translation_caches_inited = 0;
    }
#endif

#ifdef _3DS
   linearFree(gba_screen_pixels);
   if (gba_processed_pixels)
      linearFree(gba_processed_pixels);
#else
   free(gba_screen_pixels);
   if (gba_processed_pixels)
      free(gba_processed_pixels);
#endif
   if (gba_screen_pixels_prev)
      free(gba_screen_pixels_prev);

   gba_screen_pixels      = NULL;
   gba_processed_pixels   = NULL;
   gba_screen_pixels_prev = NULL;
   video_post_process     = NULL;
   post_process_cc        = false;
   post_process_mix       = false;

   if (audio_sample_buffer)
      free(audio_sample_buffer);

   audio_sample_buffer       = NULL;
   audio_samples_per_frame   = 0.0f;
   audio_samples_accumulator = 0.0f;
}

static retro_time_t retro_perf_dummy_get_time_usec() { return 0; }
static retro_perf_tick_t retro_perf_dummy_get_counter() { return 0; }
static uint64_t retro_perf_dummy_get_cpu_features() { return 0; }
static void retro_perf_dummy_log() {}
static void retro_perf_dummy_counter(struct retro_perf_counter *counter) {};

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   struct retro_vfs_interface_info vfs_iface_info;

   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   perf_cb = (struct retro_perf_callback){
      retro_perf_dummy_get_time_usec,
      retro_perf_dummy_get_counter,
      retro_perf_dummy_get_cpu_features,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_counter,
      retro_perf_dummy_log,
   };
   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);

   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) { }

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset(void)
{
#ifndef GPSP_INSTANCE_B
   if (serial_mode == SERIAL_MODE_DUAL_CABLE)
   {
      // The other console has a copy of this GBA: both restart together.
      dual_request_reset();
      return;
   }
#endif
   reset_gba();
}

size_t retro_serialize_size(void)
{
   return GBA_STATE_MEM_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

   memset (data,0, GBA_STATE_MEM_SIZE);
   gba_save_state(data);

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   if (size != GBA_STATE_MEM_SIZE)
      return false;

#ifndef GPSP_INSTANCE_B
   // The copy of this GBA on the other console cannot travel in time.
   if (dual_running() || serial_mode == SERIAL_MODE_DUAL_CABLE)
      return false;
#endif

   return gba_load_state(data);
}

void retro_cheat_reset(void)
{
   cheat_clear();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   if (!enabled)
   {
      /* Per the libretro contract, retro_cheat_set with enabled=false
       * must deactivate the cheat at this index.  Without this, a user
       * toggling a cheat off in the frontend would have no effect:
       * the cheat would stay active until the next retro_cheat_reset()
       * or content load.  Note RetroArch typically calls
       * retro_cheat_reset followed by a full reapply on every change,
       * so the visible bug surface is small, but other frontends and
       * runtime API callers can hit this directly. */
      cheat_disable(index);
      return;
   }

   switch (cheat_parse(index, code))
   {
   case CheatErrorTooMany:
      show_warning_message("Too many active cheats!", 2500);
      break;
   case CheatErrorTooBig:
      show_warning_message("Cheats are too big!", 2500);
      break;
   case CheatErrorEncrypted:
      show_warning_message("Encrypted cheats are not supported!", 2500);
      break;
   case CheatErrorNotSupported:
      show_warning_message("Cheat type is not supported!", 2500);
      break;
   case CheatNoError:
      break;
   };
}

static void extract_directory(char* buf, const char* path, size_t size)
{
   char* base;

   strlcpy(buf, path, size);

   base = strrchr(buf, '/');

   if (base)
      *base = '\0';
   else
      strlcpy(buf, ".", size);
}

#ifndef GPSP_INSTANCE_B
/* Dual Cable Link: the other consoles run copies of this GBA, made with the
 * settings it had when the session started (dual_machine_cfg). A change to
 * one of those in the menu while linked would make this GBA another machine
 * than its copies: it waits until the session is over. */
static bool cable_opts_held = false;

static bool cable_linked(void)
{
   return dual_running() || dual_cable_pending();
}

static void cable_hold_option(void)
{
   if (!cable_opts_held)
      show_warning_message("Cable Link: the change will be applied when the link ends", 3000);
   cable_opts_held = true;
}
#endif

static void check_variables(bool started_from_load)
{
   struct retro_variable var;
   bool frameskip_type_prev;
   bool post_process_cc_prev;
   bool post_process_mix_prev;
   bool linked = false;

#ifndef GPSP_INSTANCE_B
   linked = !started_from_load && cable_linked();
#endif

#ifdef HAVE_DYNAREC
   var.key = "gpsp_drc";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int prevvalue = dynarec_enable;
      int value = prevvalue;
      if (strcmp(var.value, "disabled") == 0)
         value = 0;
      else if (strcmp(var.value, "enabled") == 0)
         value = 1;

#ifndef GPSP_INSTANCE_B
      if (linked && value != prevvalue)
         cable_hold_option();
      else
#endif
         dynarec_enable = value;

      /* Flush dynarec cache to ensure we do not execute old code */
      if (dynarec_enable != prevvalue)
         flush_dynarec_caches();
   }
   else if (!linked)
      dynarec_enable = 1;

#if defined(MMAP_JIT_CACHE)
   /* Without a translation cache the dynarec cannot run; pin the
    * interpreter regardless of the core option. */
   if (!rom_translation_cache)
      dynarec_enable = 0;
#endif
#else
   dynarec_enable = 0;
#endif

   var.key = "gpsp_rtc_time_source";
   var.value = NULL;
   {
      rtc_time_source_type source = RTC_TIME_SOURCE_DETERMINISTIC;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
          !strcmp(var.value, "system"))
         source = RTC_TIME_SOURCE_SYSTEM;

      rtc_set_time_source(source);
   }

   if (started_from_load) {
     var.key                = "gpsp_bios";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "auto"))
           selected_bios = auto_detect;
        else if (!strcmp(var.value, "builtin"))
           selected_bios = builtin_bios;
        else if (!strcmp(var.value, "official"))
           selected_bios = official_bios;
     }

     var.key                = "gpsp_boot_mode";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "game"))
           selected_boot_mode = boot_game;
        else if (!strcmp(var.value, "bios"))
           selected_boot_mode = boot_bios;
     }
     if (cable_bios_boot)
        selected_boot_mode = boot_bios;

     var.key                = "gpsp_rtc";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rtc_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rtc_mode = FEAT_ENABLE;
        else
           rtc_mode = FEAT_AUTODETECT;
     }

     var.key                = "gpsp_serial";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           serial_setting = SERIAL_MODE_DISABLED;
        else if (!strcmp(var.value, "rfu"))
           serial_setting = SERIAL_MODE_RFU;
        else if (!strcmp(var.value, "mul_poke"))
           serial_setting = SERIAL_MODE_SERIAL_POKE;
        else if (!strcmp(var.value, "mul_aw1"))
           serial_setting = SERIAL_MODE_SERIAL_AW1;
        else if (!strcmp(var.value, "mul_aw2"))
           serial_setting = SERIAL_MODE_SERIAL_AW2;
        else if (!strcmp(var.value, "mul_cable"))
           serial_setting = SERIAL_MODE_SERIAL_CABLE;
        else if (!strcmp(var.value, "gbp"))
           serial_setting = SERIAL_MODE_GBP;
        else
           serial_setting = SERIAL_MODE_AUTO;
     }

     var.key                = "gpsp_rumble";
     var.value              = 0;
     if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
     {
        if (!strcmp(var.value, "disabled"))
           rumble_mode = FEAT_DISABLE;
        else if (!strcmp(var.value, "enabled"))
           rumble_mode = FEAT_ENABLE;
        else
           rumble_mode = FEAT_AUTODETECT;
     }
   }

   var.key                = "gpsp_sprlim";
   var.value              = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         sprite_limit = 1;
      else if (strcmp(var.value, "enabled") == 0)
         sprite_limit = 0;
   }
   {
      static int sound_rate_configured = 0;
      struct retro_variable svar = { "gpsp_sound_rate", NULL };
      environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &svar);
#ifndef GPSP_INSTANCE_B
      /* (the length and envelope counters the game reads step at this rate) */
      if (linked && ((svar.value && !strcmp(svar.value, "32768")) ? 15u : 16u) != sound_freq_bits)
         cable_hold_option();
      else
#endif
      gpsp_apply_sound_rate(svar.value, sound_rate_configured ? environ_cb : NULL);
      sound_rate_configured = 1;
   }


   var.key                = "gpsp_frameskip";
   var.value              = 0;
   frameskip_type_prev    = current_frameskip_type;
   current_frameskip_type = no_frameskip;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
         current_frameskip_type = auto_frameskip;
      else if (!strcmp(var.value, "auto_threshold"))
         current_frameskip_type = auto_threshold_frameskip;
      else if (!strcmp(var.value, "fixed_interval"))
         current_frameskip_type = fixed_interval_frameskip;
   }

   var.key             = "gpsp_frameskip_threshold";
   var.value           = 0;
   frameskip_threshold = 33;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   var.key   = "gpsp_frameskip_interval";
   var.value = 0;

   frameskip_interval = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_interval = strtol(var.value, NULL, 10);

   /* Cable Link players: read every time (it counts from the next netplay
    * session on). */
   read_cable_players();

   /* (Re)Initialise frame skipping, if required */
   if (started_from_load ||
       (current_frameskip_type != frameskip_type_prev))
      init_frameskip();

   var.key              = "gpsp_color_correction";
   var.value            = NULL;
   post_process_cc_prev = post_process_cc;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_cc = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_cc = true;
   }

   var.key               = "gpsp_frame_mixing";
   var.value             = NULL;
   post_process_mix_prev = post_process_mix;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         post_process_mix = false;
      else if (strcmp(var.value, "enabled") == 0)
         post_process_mix = true;
   }

   /* Check whether post processing options
    * have changed */
   if ((post_process_cc != post_process_cc_prev) ||
       (post_process_mix != post_process_mix_prev))
      init_post_processing();

   var.key           = "gpsp_turbo_period";
   var.value         = NULL;
   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      turbo_period = atoi(var.value);
      turbo_period = (turbo_period < TURBO_PERIOD_MIN) ?
            TURBO_PERIOD_MIN : turbo_period;
      turbo_period = (turbo_period > TURBO_PERIOD_MAX) ?
            TURBO_PERIOD_MAX : turbo_period;

      turbo_pulse_width = turbo_period >> 1;
      turbo_pulse_width = (turbo_pulse_width < TURBO_PULSE_WIDTH_MIN) ?
            TURBO_PULSE_WIDTH_MIN : turbo_pulse_width;
      turbo_pulse_width = (turbo_pulse_width > TURBO_PULSE_WIDTH_MAX) ?
            TURBO_PULSE_WIDTH_MAX : turbo_pulse_width;

      turbo_a_counter = 0;
      turbo_b_counter = 0;
   }
}

static void set_input_descriptors()
{
   struct retro_input_descriptor descriptors[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0 },
   };

   struct retro_input_descriptor descriptors_ff[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },
      { 0 },
   };

   if (libretro_supports_ff_override)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors_ff);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors);
}

static void set_memory_descriptors(void)
{
   const uint64_t mem = RETRO_MEMORY_SYSTEM_RAM;
   struct retro_memory_descriptor desc[2] = {
      { mem, iwram, 0x00000 + 0x8000, 0x3000000, 0, 0,  0x8000, NULL },
      { mem, ewram, 0x00000,          0x2000000, 0, 0, 0x40000, NULL },
   };
   struct retro_memory_map retromap = {
      desc,
      sizeof(desc) / sizeof(desc[0])
   };
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retromap);
}

#include "bios_patch.h"

/* The built-in BIOS, with what it lacks for the link cable
 * (bios/multiboot): SWI 25h (MultiBoot), which sends a program to the other
 * GBAs, and taking one at boot with Start+Select held. Without them a game
 * sending one (Metroid Zero Mission to Metroid Fusion, single cartridge
 * multiplayer) waited for ever, and so did the other console, in its BIOS.
 * The block goes in the free end of the BIOS, the SWI 25h entry of its
 * branch table (0xB0 + 0x25 * 4) points at it, and so does the boot. */
static void load_builtin_bios(void)
{
   const u32 entry = BIOS_MULTIBOOT_ENTRY;
   memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   memcpy(bios_rom + BIOS_MULTIBOOT_ADDR, bios_multiboot_block,
          sizeof(bios_multiboot_block));
   bios_rom[0xB0 + 0x25 * 4 + 0] = entry & 0xFF;
   bios_rom[0xB0 + 0x25 * 4 + 1] = (entry >> 8) & 0xFF;
   bios_rom[0xB0 + 0x25 * 4 + 2] = (entry >> 16) & 0xFF;
   bios_rom[0xB0 + 0x25 * 4 + 3] = (entry >> 24) & 0xFF;
   /* The boot goes on through bios_BootHook (the literal reset_vector
    * loads swi_SoftReset from): with Start+Select held it takes a program
    * from the cable, as the official BIOS does. */
   bios_rom[0x164 + 0] = BIOS_BOOTHOOK_ENTRY & 0xFF;
   bios_rom[0x164 + 1] = (BIOS_BOOTHOOK_ENTRY >> 8) & 0xFF;
   bios_rom[0x164 + 2] = 0;
   bios_rom[0x164 + 3] = 0;
}

bool retro_load_game(const struct retro_game_info* info)
{
   if (!info || !info->path)
      return false;

   /* If retro_init failed to allocate the frame buffer, refuse to load
    * rather than crash on the first video_run.  retro_init is allowed
    * to fail allocations silently; this is the point at which we have
    * to decide whether the core can usefully run. */
   if (!gba_screen_pixels)
      return false;

   /* init_gamepak_buffer() allocates the ROM buffers in 1MB blocks and
    * stops at the first failed malloc, so a memory-starved device can
    * leave none of them allocated.  load_gamepak() reads the cartridge
    * header straight out of gamepak_buffers[0], so there is no way to
    * proceed from here without dereferencing NULL. */
   if (!gamepak_buffer_count)
   {
      error_msg("Not enough memory to allocate the ROM buffer.");
      return false;
   }

   cable_bios_boot = false;
#ifndef GPSP_INSTANCE_B
   cable_opts_held = false;
#endif
   check_variables(true);
   set_input_descriptors();

   char filename_bios[MAX_PATH];
   const char* dir = NULL;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      info_msg("RGB565 is not supported.");

   extract_directory(main_path, info->path, sizeof(main_path));

   bool bios_loaded = false;
   if (selected_bios == auto_detect || selected_bios == official_bios)
   {
     /* Build '<dir>/gba_bios.bin'.  strlcpy returns the source length,
      * so we can reuse it as the offset for the suffix append - one
      * less strlen call than a strlcat-style pattern, and bounded
      * against MAX_PATH overflow either way.  If the directory path
      * was already truncated (len >= sizeof(filename_bios)), the
      * subsequent load_bios will fail and we fall back to the
      * built-in BIOS - the right outcome for a pathological frontend
      * path. */
     const char *base_dir = (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
                            ? dir : main_path;
     size_t len = strlcpy(filename_bios, base_dir, sizeof(filename_bios));
     if (len < sizeof(filename_bios))
        strlcpy(filename_bios + len, "/gba_bios.bin", sizeof(filename_bios) - len);

     bios_loaded = true;

     if (load_bios(filename_bios) != 0)
     {
        if (selected_bios == official_bios)
          show_warning_message("Could not load BIOS image file, using built-in BIOS", 2500);
        bios_loaded = false;
     }

     if (bios_loaded && bios_rom[0] != 0x18)
     {
        if (selected_bios == official_bios)
          show_warning_message("BIOS image seems incorrect, using built-in BIOS", 2500);
        bios_loaded = false;
     }
   }

   if (!bios_loaded) {
     /* Load the built-in BIOS */
     load_builtin_bios();
   }

   memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
   if (load_gamepak(info, info->path, rtc_mode, rumble_mode, serial_setting) != 0)
   {
      error_msg("Could not load the game file.");
      return false;
   }

   if (selected_bios != builtin_bios && gamepak_header_nonstandard)
   {
      show_warning_message("Non-standard ROM header detected, using built-in BIOS", 2500);
      load_builtin_bios();
   }

   extern bool require_m1_hle_bios;

   if (selected_bios != builtin_bios && require_m1_hle_bios)
   {
      show_warning_message("Pokemon ROM Hack (M1) detected, forcing built-in BIOS", 2500);
      load_builtin_bios();
   }

#ifdef GPSP_INSTANCE_B
   /* A hidden GBA of the Cable Link: linked, so as every other GBA there. */
   cable_force_bios_boot();
#endif

#ifdef HAVE_DYNAREC
   if (gamepak_mini_materialized && dynarec_enable)
   {
      dynarec_enable = 0;
      flush_dynarec_caches();
      show_warning_message("Mini ROM detected, dynamic recompiler disabled", 2500);
   }
#endif

   struct retro_rumble_interface rumbleif;
   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumbleif))
      rumble_cb = rumbleif.set_rumble_state;
   else
      rumble_cb = NULL;

   reset_gba();

   set_memory_descriptors();

#ifndef GPSP_INSTANCE_B
   dual_game_loaded(info->path, dual_machine_cfg());
#endif

   return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info* info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
#ifndef GPSP_INSTANCE_B
   dual_shutdown();
#endif
   if (libretro_ff_enabled)
      set_fastforward_override(false);

   libretro_supports_bitmasks    = false;
   libretro_supports_ff_override = false;
   libretro_ff_enabled           = false;
   libretro_ff_enabled_prev      = false;

   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   turbo_a_counter   = 0;
   turbo_b_counter   = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return gamepak_backup;
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return ewram;

   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return 0x20000;  /* Assume 128KiB, biggest possible save */
   else if (id == RETRO_MEMORY_SYSTEM_RAM)
      return 0x40000;

   return 0;
}

/* Check whether the frame about to run should be skipped (not drawn) */
static void update_frameskip(void)
{
   /* Check whether current frame should
    * be skipped */
   skip_next_frame = 0;

   if (current_frameskip_type != no_frameskip)
   {
      switch (current_frameskip_type)
      {
         case auto_frameskip:

            skip_next_frame =
                  (audio_buff_active && audio_buff_underrun) ?
                        1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case auto_threshold_frameskip:

            skip_next_frame =
                  (audio_buff_active &&
                        (audio_buff_occupancy < frameskip_threshold)) ?
                              1 : 0;

            if (!skip_next_frame ||
                (frameskip_counter >= FRAMESKIP_MAX))
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }
            else
               frameskip_counter++;

            break;
         case fixed_interval_frameskip:

            if (frameskip_counter < frameskip_interval)
            {
               skip_next_frame   = 1;
               frameskip_counter++;
            }
            else
            {
               skip_next_frame   = 0;
               frameskip_counter = 0;
            }

            break;
         default:
            skip_next_frame = 0;
            break;
      }
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

#ifdef GPSP_INSTANCE_B
   /* The copy of the other player's GBA is never seen */
   skip_next_frame = 1;
#endif
}

#ifndef GPSP_INSTANCE_B
/* Dual Cable Link: a frame of both GBAs. Ours runs here, the other player's
 * one (instance B) on its own thread; both talk through the cable. */
static void retro_run_dual(void)
{
   bool updated = false;

   input_poll_cb();
   switch (dual_frame_begin(input_local_keys()))
   {
   case DUAL_FRAME_RUN:
      break;
   case DUAL_FRAME_HOLD:
      /* Ahead of the other console: this frame waits. */
      audio_silence_run();
      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, GBA_SCREEN_PITCH * 2);
      return;
   default:
      /* The other console's buttons for this frame are not here yet. */
      video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, GBA_SCREEN_PITCH * 2);
      return;
   }

   update_input();
   rumble_frame_reset();
   update_frameskip();

   dual_b_frame_start();
   #ifdef HAVE_DYNAREC
   if (dynarec_enable)
      execute_arm_translate(execute_cycles);
   else
   #endif
   {
      clear_gamepak_stickybits();
      execute_arm(execute_cycles);
   }
   dual_b_frame_wait();
   dual_frame_end();

   if (rumble_cb) {
     u32 strength = 0xffff * rumble_active_pct();
     rumble_cb(0, RETRO_RUMBLE_WEAK,   MIN(strength, 0xffff));
     rumble_cb(0, RETRO_RUMBLE_STRONG, MIN(strength, 0xffff) / 2);
   }

   audio_run();
   video_run();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);
}
#endif

void retro_run(void)
{
   bool updated = false;
   unsigned audio_frames = 1;

#ifndef GPSP_INSTANCE_B
   if (dual_take_boot_reset())
      cable_boot_pending = true;
   /* The link is over: the options changed meanwhile apply now. */
   if (cable_opts_held && !cable_linked())
   {
      cable_opts_held = false;
      check_variables(false);
   }
#endif
   if (cable_boot_pending)
   {
      /* A netplay client just plugged the cable */
      cable_boot_pending = false;
      reset_gba();
   }

#ifndef GPSP_INSTANCE_B
   if (serial_mode == SERIAL_MODE_SERIAL_CABLE ||
       serial_mode == SERIAL_MODE_DUAL_CABLE)
   {
      /* The real time cable is plugged once the dual cable is not used
       * after all (serial_cable.c keeps it unplugged until then). */
      const bool pending = dual_cable_pending();
      if (cable_dual_pending && !pending && !dual_running())
         serialcable_replug();
      cable_dual_pending = pending;

      if (dual_setting_up())
      {
         /* Both GBAs are being set up: the game waits. */
         video_cb(NULL, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, GBA_SCREEN_PITCH * 2);
         return;
      }
      if (dual_running())
      {
         retro_run_dual();
         return;
      }
   }
#endif

   input_poll_cb();
   update_input();

   rumble_frame_reset();
   update_frameskip();

   if (serial_mode == SERIAL_MODE_SERIAL_CABLE)
      serialcable_frame_start();

   do
   {
      /* This runs just a frame */
      #ifdef HAVE_DYNAREC
      if (dynarec_enable)
         execute_arm_translate(execute_cycles);
      else
      #endif
      {
         /* Sticky bits only used in interpreter */
         clear_gamepak_stickybits();
         execute_arm(execute_cycles);
      }

      /* The cable might need the next frame right now, before we block on
       * the video/audio output: that one is then the frame to be shown. */
      if (serial_mode != SERIAL_MODE_SERIAL_CABLE || !serialcable_frame_end())
         break;
      audio_frames++;
   } while (1);

   if (rumble_cb) {
     // TODO: Add some user-option to select a rumble policy
     u32 strength = 0xffff * rumble_active_pct();
     rumble_cb(0, RETRO_RUMBLE_WEAK,   MIN(strength, 0xffff));
     rumble_cb(0, RETRO_RUMBLE_STRONG, MIN(strength, 0xffff) / 2);
   }

   while (audio_frames--)
      audio_run();
   video_run();

   switch (serial_mode) {
   case SERIAL_MODE_RFU:
     rfu_frame_update();
     break;
   case SERIAL_MODE_SERIAL_POKE:
     serialpoke_frame_update();
     break;
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(false);
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}



#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "common.h"
#include "libretro.h"
#include "libretro_core_options.h"
#include "memmap.h"

#include "gba_memory.h"
#include "gba_cc_lut.h"

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

// 59.72750057 hz
#define GBA_FPS ((float) GBC_BASE_RATE) / (308 * 228 * 4)

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
static retro_input_poll_t input_poll_cb;
static retro_environment_t environ_cb;

struct retro_perf_callback perf_cb;

int dynarec_enable;
int use_libretro_save_method = 0;
boot_mode selected_boot_mode = boot_game;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 iwram_stack_optimize = 1;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

static u16 *gba_screen_pixels_prev = NULL;
static u16 *gba_processed_pixels   = NULL;

static void (*video_post_process)(void) = NULL;
static bool post_process_cc  = false;
static bool post_process_mix = false;

#if defined(PSP)
static uint32_t next_pow2(uint32_t v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v++;
   return v;
}
#endif

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
   uint16_t *src = gba_screen_pixels;
   uint16_t *dst = gba_processed_pixels;
   size_t x, y;

   for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
   {
      for (x = 0; x < GBA_SCREEN_PITCH; x++)
      {
         u16 src_color = *(src + x);

         /* Convert colour to RGB555 and perform lookup */
         *(dst + x) = *(gba_cc_lut + (((src_color & 0xFFC0) >> 1) | (src_color & 0x1F)));
      }

      src += GBA_SCREEN_PITCH;
      dst += GBA_SCREEN_PITCH;
   }
}

static void video_post_process_mix(void)
{
   uint16_t *src_curr = gba_screen_pixels;
   uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t *dst      = gba_processed_pixels;
   size_t x, y;

   for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
   {
      for (x = 0; x < GBA_SCREEN_PITCH; x++)
      {
         /* Get colours from current + previous frames (RGB565) */
         uint16_t rgb_curr = *(src_curr + x);
         uint16_t rgb_prev = *(src_prev + x);

         /* Store colours for next frame */
         *(src_prev + x)   = rgb_curr;

         /* Mix colours
          * > "Mixing Packed RGB Pixels Efficiently"
          *   http://blargg.8bitalley.com/info/rgb_mixing.html */
         *(dst + x)        = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;
      }

      src_curr += GBA_SCREEN_PITCH;
      src_prev += GBA_SCREEN_PITCH;
      dst      += GBA_SCREEN_PITCH;
   }
}

static void video_post_process_cc_mix(void)
{
   uint16_t *src_curr = gba_screen_pixels;
   uint16_t *src_prev = gba_screen_pixels_prev;
   uint16_t *dst      = gba_processed_pixels;
   size_t x, y;

   for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
   {
      for (x = 0; x < GBA_SCREEN_PITCH; x++)
      {
         /* Get colours from current + previous frames (RGB565) */
         uint16_t rgb_curr = *(src_curr + x);
         uint16_t rgb_prev = *(src_prev + x);

         /* Store colours for next frame */
         *(src_prev + x)   = rgb_curr;

         /* Mix colours
          * > "Mixing Packed RGB Pixels Efficiently"
          *   http://blargg.8bitalley.com/info/rgb_mixing.html */
         uint16_t rgb_mix  = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;

         /* Convert colour to RGB555 and perform lookup */
         *(dst + x) = *(gba_cc_lut + (((rgb_mix & 0xFFC0) >> 1) | (rgb_mix & 0x1F)));
      }

      src_curr += GBA_SCREEN_PITCH;
      src_prev += GBA_SCREEN_PITCH;
      dst      += GBA_SCREEN_PITCH;
   }
}

static void init_post_processing(void)
{
   size_t buf_size = GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT * sizeof(u16);

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
      gba_processed_pixels = (u16*)linearMemAlign(buf_size, 128);
#else
      gba_processed_pixels = (u16*)malloc(buf_size);
#endif

      if (!gba_processed_pixels)
         return;

      memset(gba_processed_pixels, 0xFFFF, buf_size);
   }

   /* Initialise 'history' buffer, if required */
   if (!gba_screen_pixels_prev &&
       post_process_mix)
   {
      gba_screen_pixels_prev = (u16*)malloc(buf_size);

      if (!gba_screen_pixels_prev)
         return;

      memset(gba_screen_pixels_prev, 0xFFFF, buf_size);
   }

   /* Assign post processing function */
   if (post_process_cc && post_process_mix)
      video_post_process = video_post_process_cc_mix;
   else if (post_process_cc)
      video_post_process = video_post_process_cc;
   else if (post_process_mix)
      video_post_process = video_post_process_mix;
}

/* Video post processing END */

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

#if defined(PSP)
   static unsigned int __attribute__((aligned(16))) d_list[32];
   void* texture_vram_p = NULL;
   int texture_size = (GBA_SCREEN_WIDTH*GBA_SCREEN_HEIGHT*2);
   
   texture_vram_p = (void*) (0x44200000 - texture_size); /* max VRAM address - frame size */

   sceKernelDcacheWritebackRange(gba_screen_pixels_buf, texture_size);

   sceGuStart(GU_DIRECT, d_list);
   sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
   sceGuCopyImage(GU_PSM_5650, 0, 0, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT, GBA_SCREEN_WIDTH,
               gba_screen_pixels_buf, 0, 0, GBA_SCREEN_WIDTH, texture_vram_p);
   sceGuTexImage(0, next_pow2(GBA_SCREEN_WIDTH), next_pow2(GBA_SCREEN_HEIGHT), GBA_SCREEN_WIDTH, texture_vram_p);
   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
   sceGuDisable(GU_BLEND);

   sceGuFinish();

   video_cb(texture_vram_p, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
#else 
   video_cb(gba_screen_pixels_buf, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
            GBA_SCREEN_PITCH * 2);
#endif
}

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
   info->library_name = "gpSP";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = "v0.91" GIT_VERSION;
   info->need_fullpath = true;
   info->block_extract = false;
   info->valid_extensions = "gba|bin|agb|gbz" ;
}


void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = GBA_SCREEN_WIDTH;
   info->geometry.base_height = GBA_SCREEN_HEIGHT;
   info->geometry.max_width = GBA_SCREEN_WIDTH;
   info->geometry.max_height = GBA_SCREEN_HEIGHT;
   info->geometry.aspect_ratio = 0;
   info->timing.fps = ((float) GBA_FPS);
   info->timing.sample_rate = GBA_SOUND_FREQUENCY;
}

void retro_init(void)
{
#if defined(HAVE_DYNAREC)
  #if defined(HAVE_MMAP)
   rom_translation_cache = mmap(NULL, ROM_TRANSLATION_CACHE_SIZE,
                                PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
   ram_translation_cache = mmap(NULL, RAM_TRANSLATION_CACHE_SIZE,
                                PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
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

   if (!gamepak_rom)
      init_gamepak_buffer();
   init_sound(1);

   if(!gba_screen_pixels)
#ifdef _3DS
      gba_screen_pixels = (uint16_t*)linearMemAlign(GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT * sizeof(uint16_t), 128);
#else
      gba_screen_pixels = (uint16_t*)malloc(GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT * sizeof(uint16_t));
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
   perf_cb.perf_log();
   memory_term();

#if defined(HAVE_MMAP) && defined(HAVE_DYNAREC)
   munmap(rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE);
   munmap(ram_translation_cache, RAM_TRANSLATION_CACHE_SIZE);
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
}

static retro_time_t retro_perf_dummy_get_time_usec() { return 0; }
static retro_perf_tick_t retro_perf_dummy_get_counter() { return 0; }
static uint64_t retro_perf_dummy_get_cpu_features() { return 0; }
static void retro_perf_dummy_log() {}
static void retro_perf_dummy_counter(struct retro_perf_counter *counter) {};

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;

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

   libretro_set_core_options(environ_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}
void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_reset(void)
{
   update_backup();
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

   gba_load_state(data);

   return true;
}

void retro_cheat_reset(void)
{
   cheat_clear();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   if (!enabled)
      return;

   cheat_parse(index, code);
}

static void extract_directory(char* buf, const char* path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char* base = strrchr(buf, '/');

   if (base)
      *base = '\0';
   else
      strncpy(buf, ".", size);
}

static void check_variables(int started_from_load)
{
   struct retro_variable var;
   bool frameskip_type_prev;
   bool post_process_cc_prev;
   bool post_process_mix_prev;

#ifdef HAVE_DYNAREC
   var.key = "gpsp_drc";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int prevvalue = dynarec_enable;
      if (strcmp(var.value, "disabled") == 0)
         dynarec_enable = 0;
      else if (strcmp(var.value, "enabled") == 0)
         dynarec_enable = 1;

      if (dynarec_enable != prevvalue)
         init_caches();
   }
   else
      dynarec_enable = 1;
#else
   dynarec_enable = 0;
#endif

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

   if (started_from_load)
   {
      var.key = "gpsp_save_method";
      var.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         if (!strcmp(var.value, "libretro"))
            use_libretro_save_method = 1;
         else
            use_libretro_save_method = 0;
      }
   }
}

static void set_input_descriptors()
{
   struct retro_input_descriptor descriptors[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
      { 0 },
   };

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

bool retro_load_game(const struct retro_game_info* info)
{
   if (!info)
      return false;

   use_libretro_save_method = 0;
   check_variables(1);
   set_input_descriptors();

   char filename_bios[MAX_PATH];
   const char* dir = NULL;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      info_msg("RGB565 is not supported.");

   extract_directory(main_path, info->path, sizeof(main_path));

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
      strcpy(save_path, dir);
   else
      strcpy(save_path, main_path);

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
      strcpy(filename_bios, dir);
   else
      strcpy(filename_bios, main_path);

   bool bios_loaded = false;
   printf("USE %d\n", (int)selected_bios);
   if (selected_bios == auto_detect || selected_bios == official_bios)
   {
     bios_loaded = true;
     strcat(filename_bios, "/gba_bios.bin");

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
     memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
   }

   memset(gamepak_backup, -1, sizeof(gamepak_backup));
   gamepak_filename[0] = 0;
   if (load_gamepak(info, info->path) != 0)
   {
      error_msg("Could not load the game file.");
      return false;
   }

   reset_gba();

   set_memory_descriptors();

   return true;
}

bool retro_load_game_special(unsigned game_type,
                             const struct retro_game_info* info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
   update_backup();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id)
{
   switch (id)
   {
   case RETRO_MEMORY_SAVE_RAM:
      if (use_libretro_save_method)
         return gamepak_backup;
      break;
   default:
      break;
   }

   return 0;
}

size_t retro_get_memory_size(unsigned id)
{
   switch (id)
   {
   case RETRO_MEMORY_SAVE_RAM:
      if (use_libretro_save_method)
      {
         switch(backup_type)
         {
         case BACKUP_SRAM:
            if(sram_size == SRAM_SIZE_32KB)
               return 0x8000;
            else
               return 0x10000;
            break;

         case BACKUP_FLASH:
            if(flash_size == FLASH_SIZE_64KB)
               return 0x10000;
            else
               return 0x20000;
            break;

         case BACKUP_EEPROM:
            if(eeprom_size == EEPROM_512_BYTE)
               return 0x200;
            else
               return 0x2000;
            break;
         // assume 128KB save, regardless if rom supports battery saves
         // this is needed because gba cannot provide initially the backup save size 
         // until a few cycles has passed (unless provided by a database)
         case BACKUP_NONE:
         default:
            return (1024 * 128);
            break;
         }
      }
      break;
   default:
      break;
   }

   return 0;
}

void retro_run(void)
{
   bool updated = false;

   update_input();

   input_poll_cb();

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

   /* This runs just a frame */
   #ifdef HAVE_DYNAREC
   if (dynarec_enable)
      execute_arm_translate(execute_cycles);
   else
   #endif
      execute_arm(execute_cycles);

   render_audio();
   video_run();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables(0);
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

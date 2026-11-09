/* MultiBoot (SWI 25h) for the built-in BIOS.
 *
 * The open BIOS gpSP ships left SWI 25h as a stub, so a game that sends a
 * program to another GBA through the cable (Metroid Zero Mission to
 * Metroid Fusion, single cartridge multiplayer...) waited for ever with the
 * built-in BIOS, while the other console sat in its BIOS waiting for the
 * program. This is the master side of the transfer, as the official BIOS
 * does it (GBATEK "BIOS Multi Boot", checked word by word against the
 * official BIOS on the real time cable: encryption and CRC match).
 *
 * It is built on its own (no libtonc needed) at a fixed address in the
 * free space at the end of the built-in BIOS, and gpSP points the SWI 25h
 * entry of the BIOS branch table at it when it loads the built-in BIOS (see
 * Makefile and ../../bios_patch.c).
 *
 * r0: MultiBootParam, r1: transfer mode (0: normal 256 KHz 32 bit,
 * 1: multiplayer 115 KHz 16 bit, 2: normal 2 MHz 32 bit).
 * Returns 0 when every client took the program, 1 if not.
 *
 * GPL2, as the rest of the built-in BIOS.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG_VCOUNT     (*(vu16 *)0x04000006)
#define REG_SIODATA32  (*(vu32 *)0x04000120)
#define REG_SIOMULTI   ((vu16 *)0x04000120)
#define REG_SIOCNT     (*(vu16 *)0x04000128)
#define REG_SIOMLT_SEND (*(vu16 *)0x0400012A)

/* MultiBootParam (as the SDK and libgba lay it out) */
#define MBP_HANDSHAKE   0x14
#define MBP_CLIENT_DATA 0x19
#define MBP_PALETTE     0x1C
#define MBP_CLIENT_BIT  0x1E
#define MBP_SRCP        0x20
#define MBP_ENDP        0x24

#define TRIES_0075      0x4000    /* "send CRC now" polls before giving up */

/* (No globals: this runs from the BIOS ROM. normal_cnt: 0 for multiplayer,
 * else the SIOCNT of a normal 32 bit transfer.) */

/* One transfer. Multiplayer: the answers of clients 1..3 in *ans.
 * Normal: the 32 bit word that came back (upper half: the client's). */
static u32 xfer(u32 normal_cnt, u32 v, u16 *ans)
{
   u32 guard = 0x100000;
   if (!normal_cnt) {
      REG_SIOMLT_SEND = (u16)v;
      REG_SIOCNT |= 0x0080;
      while ((REG_SIOCNT & 0x0080) && --guard);
      ans[0] = REG_SIOMULTI[1];
      ans[1] = REG_SIOMULTI[2];
      ans[2] = REG_SIOMULTI[3];
      return ans[0];
   }
   REG_SIODATA32 = v;
   REG_SIOCNT = normal_cnt;
   REG_SIOCNT = normal_cnt | 0x0080;
   while ((REG_SIOCNT & 0x0080) && --guard);
   v = REG_SIODATA32;
   ans[0] = (u16)(v >> 16);
   ans[1] = ans[2] = ans[0];
   return v;
}

/* Every client in client_bit answered `want` */
static int all_said(const u16 *ans, u32 clients, u16 want)
{
   u32 i;
   for (i = 0; i < 3; i++)
      if ((clients & (2 << i)) && ans[i] != want)
         return 0;
   return 1;
}

static u32 crc_word(u32 c, u32 d, u32 poly)
{
   u32 b;
   c ^= d;
   for (b = 0; b < 32; b++)
      c = (c >> 1) ^ ((c & 1) ? poly : 0);
   return c;
}

/* About 1/16 second: the clients get ready for the program meanwhile */
static void wait_frames(u32 n)
{
   while (n--) {
      u32 guard = 0x200000;
      while (REG_VCOUNT >= 160 && --guard);
      guard = 0x200000;
      while (REG_VCOUNT < 160 && --guard);
   }
}

__attribute__((section(".text.entry")))
u32 swi_MultiBoot(u8 *p, u32 mode)
{
   const u32 hh = p[MBP_HANDSHAKE];
   const u32 pp = p[MBP_PALETTE];
   u32 clients = p[MBP_CLIENT_BIT] & 0x0E;
   const u8 *src = *(u8 **)(p + MBP_SRCP);
   const u8 *end = *(u8 **)(p + MBP_ENDP);
   const u16 saved = REG_SIOCNT;
   u32 len, cc = 0xFF, rr, m, f, c, poly, key, ptr, i, r, ok;
   u32 multi, normal_cnt = 0;
   u16 ans[3];

   if (mode > 2 || end <= src)
      return 1;
   len = (u32)(end - src);
   if (len > 0x3FF40 || (len & 3))
      return 1;

   multi = (mode == 1);
   if (multi) {
      /* Multiplayer, at the speed the game set, no serial IRQ meanwhile */
      REG_SIOCNT = (saved & 0x0003) | 0x2000;
      poly = 0xA517; key = 0x6465646F; c = 0xFFF8;
      if (!clients)
         clients = 0x02;
   } else {
      /* Normal 32 bit, internal clock; only client 1 can answer */
      normal_cnt = 0x1001 | (mode == 2 ? 0x0002 : 0);
      poly = 0xC37B; key = 0x43202F2F; c = 0xC387;
      clients = 0x02;
   }

   /* client_data: the first client's (they answer alike) */
   for (i = 0; i < 3; i++)
      if (clients & (2 << i)) {
         cc = p[MBP_CLIENT_DATA + i];
         break;
      }

   /* The game sent 64hh (final handshake) itself: the clients need a
    * moment after it before the length */
   wait_frames(3);

   /* Length, and the clients' random data back */
   r = xfer(normal_cnt, len / 4 - 0x34, ans);
   rr = multi ? (r & 0xFF) : ((r >> 16) & 0xFF);

   m = 0xFFFF0000 | (cc << 8) | pp;
   f = 0xFFFF0000 | (rr << 8) | hh;

   for (ptr = 0; ptr < len; ptr += 4) {
      const u32 d = (u32)src[ptr] | ((u32)src[ptr + 1] << 8) |
                    ((u32)src[ptr + 2] << 16) | ((u32)src[ptr + 3] << 24);
      const u32 a = 0xC0 + ptr;
      u32 e;
      c = crc_word(c, d, poly);
      m = m * 0x6F646573 + 1;
      e = d ^ (0xFE000000 - a) ^ m ^ key;
      if (multi) {
         xfer(normal_cnt, e & 0xFFFF, ans);
         xfer(normal_cnt, e >> 16, ans);
      } else
         xfer(normal_cnt, e, ans);
   }
   c = crc_word(c, f, poly) & 0xFFFF;

   /* Wait until every client has the whole program */
   ok = 0;
   for (i = 0; i < TRIES_0075; i++) {
      xfer(normal_cnt, 0x0065, ans);
      if (all_said(ans, clients, 0x0075)) {
         ok = 1;
         break;
      }
   }
   if (ok) {
      xfer(normal_cnt, 0x0066, ans);
      xfer(normal_cnt, c, ans);
      ok = all_said(ans, clients, (u16)c);
   }

   REG_SIOCNT = saved & ~0x0080;
   return ok ? 0 : 1;
}

/* ---- Client side: Start+Select held when the BIOS boots ----------------
 *
 * The official BIOS then waits for a program from the cable instead of
 * starting the cartridge (Metroid Fusion gets Zero Mission's link program
 * this way). The built-in BIOS jumped to the cartridge whatever was held.
 * gpSP points the BIOS boot at bios_BootHook (the literal swi_SoftReset is
 * taken from, after the logo): with Start+Select held it takes the program
 * in multiplayer mode, as the official BIOS answers it (seen on the real
 * time cable: 7202, the header count, 73cc, 73rr, the address of each
 * word, 0074/0075 and the CRC), and starts it at 0x020000C0. Anything
 * else boots the cartridge as before.
 */

#define REG_KEYINPUT   (*(vu16 *)0x04000130)
#define REG_RCNT       (*(vu16 *)0x04000134)
#define REG_IE         (*(vu16 *)0x04000200)
#define REG_IF         (*(vu16 *)0x04000202)
#define REG_IME        (*(vu16 *)0x04000208)
#define SOFT_RESET     0x1804      /* swi_SoftReset of the built-in BIOS */

/* Waits for the next transfer the parent makes; `reply` is our word in it.
 * Returns the parent's word. */
static u32 client_xfer(u32 reply)
{
   REG_SIOMLT_SEND = (u16)reply;
   while (!(REG_IF & 0x0080))
      ;
   REG_IF = 0x0080;
   return REG_SIOMULTI[0];
}

/* Takes one program. Returns only if the transfer failed (CRC). */
static void client_receive(void)
{
   u32 slot, bit, w, reply, i, pp = 0, hh = 0, cc = 0xC1, rr = 0x70;
   u32 len, ptr, lo, m, c, f;
   u16 *const hdr = (u16 *)0x02000000;

   REG_RCNT = 0;
   REG_SIOCNT = 0x2003;
   REG_SIOCNT = 0x6003;           /* multiplayer, serial IRQ flag */
   REG_IF = 0x0080;
   slot = (REG_SIOCNT >> 4) & 3;
   if (!slot)
      slot = 1;
   bit = 1u << slot;

   /* Handshake: 62xx until the parent says 61xx */
   reply = 0x7200 | bit;
   for (;;) {
      w = client_xfer(reply);
      if ((w & 0xFF00) == 0x6100)
         break;
      reply = 0x7200 | bit;
   }
   /* Header: 0x60 halfwords, we answer how many are left */
   reply = (0x60 << 8) | bit;
   for (i = 0; i < 0x60; i++) {
      hdr[i] = (u16)client_xfer(reply);
      reply = ((0x5F - i) << 8) | bit;
   }
   /* 62xx again, then the palette (63pp) and the handshake (64hh) */
   for (;;) {
      w = client_xfer(reply);
      if ((w & 0xFF00) == 0x6300) {
         pp = w & 0xFF;
         reply = 0x7300 | cc;
      } else if ((w & 0xFF00) == 0x6400) {
         hh = w & 0xFF;
         reply = 0x7300 | rr;
         break;
      } else
         reply = 0x7200 | bit;
   }
   /* The length, then the program */
   w = client_xfer(reply);
   len = (w + 0x34) * 4;
   if (len > 0x3FF40)
      return;
   m = 0xFFFF0000 | (cc << 8) | pp;
   f = 0xFFFF0000 | (rr << 8) | hh;
   c = 0xFFF8;
   for (ptr = 0xC0; ptr < 0xC0 + len; ptr += 4) {
      u32 d;
      lo = client_xfer(ptr & 0xFFFF);
      w = client_xfer((ptr + 2) & 0xFFFF);
      m = m * 0x6F646573 + 1;
      d = (lo | (w << 16)) ^ (0xFE000000 - ptr) ^ m ^ 0x6465646F;
      *(vu32 *)(0x02000000 + ptr) = d;
      c = crc_word(c, d, 0xA517);
   }
   c = crc_word(c, f, 0xA517) & 0xFFFF;

   /* 0065 until we say 0075, 0066, then the CRCs are exchanged */
   reply = ptr & 0xFFFF;
   for (;;) {
      w = client_xfer(reply);
      if (w == 0x0065)
         reply = 0x0075;
      else if (w == 0x0066)
         break;
   }
   w = client_xfer(c);
   if (w != c)
      return;

   /* Started by the cable, in multiplayer mode, as this client */
   *(vu8 *)0x020000C4 = 3;
   *(vu8 *)0x020000C5 = (u8)slot;
   REG_SIOCNT = 0x2003;
   REG_IF = 0x0080;
   __asm__ volatile(
      "mov r0, #0x1F\n"
      "msr cpsr_c, r0\n"
      "ldr sp, =0x03007F00\n"
      "mov r0, #0\n"
      "mov r1, #0\n"
      "mov r2, #0\n"
      "mov r3, #0\n"
      "ldr lr, =0x020000C0\n"
      "bx lr\n"
      ::: "r0", "r1", "r2", "r3", "lr", "memory");
}

__attribute__((section(".text.entry2")))
void bios_BootHook(void)
{
   /* Start (bit 3) and Select (bit 2), active low */
   if (!(REG_KEYINPUT & 0x000C)) {
      const u16 ie = REG_IE;
      REG_IME = 0;
      REG_IE = 0;
      for (;;)
         client_receive();       /* until one program gets here whole */
      REG_IE = ie;
   }
   ((void (*)(void))SOFT_RESET)();
}

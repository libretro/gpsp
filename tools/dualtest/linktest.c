/* Link cable test ROM for the Dual Cable Link.
 *
 * Every GBA (2 to 4) runs this. The parent (SI low) starts a multiplayer
 * transfer as soon as the previous one is over (tens of them per frame, more
 * than any real game), each child loads a new word after each one. The four
 * words that crossed the cable are logged in EWRAM (two log entries per
 * transfer), so the GBAs' logs can be compared, and the words depend on the
 * buttons, so they also tell whether each GBA got its own player's buttons. */

#define REG16(a)    (*(volatile unsigned short *)(a))
#define RCNT        REG16(0x04000134)
#define SIOCNT      REG16(0x04000128)
#define SIOMLT_SEND REG16(0x0400012A)
#define SIOMULTI0   REG16(0x04000120)
#define SIOMULTI1   REG16(0x04000122)
#define SIOMULTI2   REG16(0x04000124)
#define SIOMULTI3   REG16(0x04000126)
#define KEYINPUT    REG16(0x04000130)

#define LOG         ((volatile unsigned int *)0x02000010)
#define LOG_COUNT   (*(volatile unsigned int *)0x02000000)
#define LOG_SUM     (*(volatile unsigned int *)0x02000004)
#define LOG_ROLE    (*(volatile unsigned int *)0x02000008)
#define LOG_MAX     8192

static void record(unsigned int w)
{
  unsigned int n = LOG_COUNT;
  if (n < LOG_MAX)
    LOG[n] = w;
  LOG_COUNT = n + 1;
  LOG_SUM = LOG_SUM * 31 + w;
}

int main(void)
{
  unsigned int seq = 0;
  LOG_COUNT = 0;
  LOG_SUM = 0;

  RCNT = 0;
  SIOCNT = 0x2003;          /* multiplayer, 115200 bps */

  for (;;) {
    unsigned short c = SIOCNT;
    if (!(c & 0x0004)) {
      /* Parent: go when everybody is ready and nothing is going on. */
      LOG_ROLE = 1;
      if ((c & 0x0008) && !(c & 0x0080)) {
        SIOMLT_SEND = (unsigned short)(seq * 7 + (~KEYINPUT & 0x3FF));
        SIOCNT = c | 0x0080;
        while (SIOCNT & 0x0080)
          ;
        record(SIOMULTI0 | ((unsigned int)SIOMULTI1 << 16));
        record(SIOMULTI2 | ((unsigned int)SIOMULTI3 << 16));
        seq++;
      }
    } else {
      /* Child: the next word, then wait for a transfer to come and go. */
      LOG_ROLE = 2 + ((c >> 4) & 3) - 1;   /* 2, 3, 4: by its ID */
      SIOMLT_SEND = (unsigned short)((seq * 13 + (~KEYINPUT & 0x3FF)) ^
                                     (0x5A5A + LOG_ROLE));
      while (!(SIOCNT & 0x0080))
        if (!(SIOCNT & 0x0004))
          break;            /* became the parent */
      while (SIOCNT & 0x0080)
        ;
      record(SIOMULTI0 | ((unsigned int)SIOMULTI1 << 16));
      record(SIOMULTI2 | ((unsigned int)SIOMULTI3 << 16));
      seq++;
    }
  }
  return 0;
}

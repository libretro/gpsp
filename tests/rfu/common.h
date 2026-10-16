/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only hardware/frontend shim; the runner substitutes this include. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX_RFU_NETPLAYERS 32
#define REG_SIOCNT 0
#define REG_SIODATA32_H 1
#define REG_SIODATA32_L 2

static u32 cpu_ticks = 123;
static u16 regs[3];
static u16 read_ioreg(unsigned i) { return regs[i]; }
static void write_ioreg(unsigned i, u16 v) { regs[i] = v; }
static void rand_seed(u32 seed) { (void)seed; }
static u32 rand_gen(void) { return 42; }
static u32 netorder32(u32 value) {
  const u16 one = 1;
  return *(const u8 *)&one ? __builtin_bswap32(value) : value;
}

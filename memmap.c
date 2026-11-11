
#include <stdint.h>
#include <stdbool.h>

#include "memmap.h"

// The JIT cache buffer is allocated via mmap (or win equivalent) so that it
// can be RWX. On top of that, we need the bufer to be "close" to the text
// segment, so that we can perform jumps between the two code blocks.
// Android and some other platforms discourage the usage of sections in the
// binary (ie. on-disk ELF) that are marked as executable and writtable for
// security reasons. Therefore we prefer to use mmap even though it can be
// tricky to map correctly.

// To map a block close to the code, we take the function address as a proxy
// of the text section address, and try to map the cache next to it. This is
// an iterative process of trial and error that is hopefully successful.

// Jump/Call offset requirements:
// x86-64 has a +/- 2GB offset requirement.
// ARM64 has a +/-128MB offset requirement.
// ARM32 has a +/- 32MB offset requirement (gpsp does not require this).
// MIPS requires blocks to be within the same 256MB boundary (identical 4 MSB)

#ifdef MMAP_JIT_CACHE

// JIT block requirements translated to allocation code.
#if defined(MIPS_ARCH)
  #define _MAP_ITERATIONS           1024   // Test -/+2GB in 4MB steps
  #define _MAP_STEP         (4*1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_section_mips(ptr, size)
#elif defined(ARM64_ARCH)
  #define _MAP_ITERATIONS            256   // Test -/+128MB in 1MB steps
  #define _MAP_STEP           (1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_offset(ptr, size, 128)
#else
  #define _MAP_ITERATIONS           1024   // Test -/+2GB in 4MB steps
  #define _MAP_STEP         (4*1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_offset(ptr, size, 2048)
#endif

bool validate_addr_offset(void *ptr, unsigned size, unsigned max_offset_mb) {
	// Ensure that the start and the end of the block is not too far away.
	uintptr_t ref_addr = (uintptr_t)(map_jit_block);
	uintptr_t start_addr = (uintptr_t)ptr;
	uintptr_t end_addr = start_addr + size;
	uintptr_t dist1 = start_addr > ref_addr ? start_addr - ref_addr
	                                        : ref_addr - start_addr;
	uintptr_t dist2 = end_addr > ref_addr ? end_addr - ref_addr
	                                      : ref_addr - end_addr;

	return dist1 < max_offset_mb * 1024 * 1024 &&
	       dist2 < max_offset_mb * 1024 * 1024;
}

bool validate_addr_section_mips(void *ptr, unsigned size, unsigned max_offset_mb) {
	uintptr_t ref_addr = (uintptr_t)(map_jit_block);
	uintptr_t start_addr = (uintptr_t)ptr;
	uintptr_t end_addr = start_addr + size;
	const uintptr_t msk = ~0xFFFFFFFU;  // 256MB block

	return (ref_addr & msk) == (start_addr & msk) &&
	       (ref_addr & msk) == (end_addr & msk);
}

#ifdef WIN32

	#include <windows.h>
	#include <io.h>

	void *map_jit_block(unsigned size) {
		unsigned i;
		uintptr_t base = (uintptr_t)(map_jit_block) & (~(_MAP_STEP - 1ULL));
		for (i = 0; i < _MAP_ITERATIONS; i++) {
			int offset = ((i & 1) ? 1 : -1) * (i >> 1) * _MAP_STEP;
			uintptr_t baddr = base + (intptr_t)offset;
			if (!baddr)
				continue;    // Do not map NULL, bad things happen :)

			void *p = VirtualAlloc((void*)baddr, size, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p) {
				if (_VALIDATE_BLOCK_FN(p, size))
					return p;

				VirtualFree(p, 0, MEM_RELEASE);
			}

		}
		return 0;
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		VirtualFree(bufptr, 0, MEM_RELEASE);
	}

#else

	#include <sys/mman.h>

#if defined(ARM64_ARCH)

	#include <stddef.h>

	// ARM64: the block must be within 128MB of the code, and the Dual Cable
	// Link loads the hidden GBAs (each one with its own cache) only when a
	// netplay session starts. By then the memory near the code is often all
	// taken (on Android the allocator fills it with 1MB blocks), and a GBA
	// without a cache runs on the interpreter: its timing is not the same as
	// that of its copy on the other console. So the first cache (the GBA the
	// player sees, mapped when the core starts) is mapped with room for the
	// hidden GBAs right behind it, and they take their part of it.
	#define JIT_POOL_SLOTS 4

	static void *map_jit_near(unsigned size) {
		unsigned i;
		uintptr_t base = (uintptr_t)(map_jit_block) & (~(_MAP_STEP - 1ULL));
		int flags = MAP_ANON | MAP_PRIVATE;
		#ifdef __APPLE__
		flags |= MAP_JIT;
		#endif
		for (i = 0; i < _MAP_ITERATIONS; i++) {
			int offset = ((i & 1) ? 1 : -1) * (i >> 1) * _MAP_STEP;
			uintptr_t baddr = base + (intptr_t)offset;
			if (!baddr)
				continue;    // Do not map NULL, bad things happen :)

			void *p = mmap((void*)baddr, size, PROT_READ|PROT_WRITE|PROT_EXEC,
			                                   flags, -1, 0);
			if (p != MAP_FAILED) {
				if (_VALIDATE_BLOCK_FN(p, size))
					return p;

				munmap(p, size);
			}
		}
		return 0;
	}

#ifndef GPSP_INSTANCE_B
	static unsigned char *jit_pool;     // JIT_POOL_SLOTS caches, slot 0 is ours
	static unsigned jit_pool_size;      // Size of a slot
	static unsigned jit_pool_used;      // One bit per slot

	static void jit_pool_release(void) {
		if (jit_pool && !jit_pool_used) {
			munmap(jit_pool, jit_pool_size * JIT_POOL_SLOTS);
			jit_pool = NULL;
		}
	}

	// For the hidden GBAs (memmap.c of instances B, C and D).
	void *jit_pool_take(unsigned size) {
		unsigned k;
		if (!jit_pool || size != jit_pool_size)
			return NULL;
		for (k = 1; k < JIT_POOL_SLOTS; k++)
			if (!(jit_pool_used & (1u << k))) {
				jit_pool_used |= 1u << k;
				return &jit_pool[k * size];
			}
		return NULL;
	}

	// Returns 0 if `p` is not a slot of the pool.
	int jit_pool_give(void *p, unsigned size) {
		unsigned k;
		if (!jit_pool || size != jit_pool_size)
			return 0;
		for (k = 0; k < JIT_POOL_SLOTS; k++)
			if (p == &jit_pool[k * size] && (jit_pool_used & (1u << k))) {
				madvise(p, size, MADV_DONTNEED);   // Back to empty pages
				jit_pool_used &= ~(1u << k);
				jit_pool_release();
				return 1;
			}
		return 0;
	}

	void *map_jit_block(unsigned size) {
		if (!jit_pool) {
			jit_pool = (unsigned char*)map_jit_near(size * JIT_POOL_SLOTS);
			if (jit_pool) {
				jit_pool_size = size;
				jit_pool_used = 0;
			}
		}
		if (jit_pool && size == jit_pool_size && !(jit_pool_used & 1)) {
			jit_pool_used |= 1;
			return jit_pool;
		}
		return map_jit_near(size);
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		if (!jit_pool_give(bufptr, size))
			munmap(bufptr, size);
	}
#else
	// A hidden GBA (Dual Cable Link): a slot of the pool of the GBA the
	// player sees, if it is close enough to our code too.
	void *jit_pool_take(unsigned size);
	int jit_pool_give(void *p, unsigned size);

	void *map_jit_block(unsigned size) {
		void *p = jit_pool_take(size);
		if (p) {
			if (_VALIDATE_BLOCK_FN(p, size))
				return p;
			jit_pool_give(p, size);
		}
		return map_jit_near(size);
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		if (!jit_pool_give(bufptr, size))
			munmap(bufptr, size);
	}
#endif

#else

	// Posix implementation
	void *map_jit_block(unsigned size) {
		unsigned i;
		uintptr_t base = (uintptr_t)(map_jit_block) & (~(_MAP_STEP - 1ULL));
		int flags = MAP_ANON | MAP_PRIVATE;
		#ifdef __APPLE__
		/* RWX mappings require MAP_JIT under the hardened runtime and
		 * unconditionally on Apple silicon, where writes are then gated
		 * per-thread (see jit_write_enable/jit_exec_enable). */
		flags |= MAP_JIT;
		#endif
		for (i = 0; i < _MAP_ITERATIONS; i++) {
			int offset = ((i & 1) ? 1 : -1) * (i >> 1) * _MAP_STEP;
			uintptr_t baddr = base + (intptr_t)offset;
			if (!baddr)
				continue;    // Do not map NULL, bad things happen :)

			void *p = mmap((void*)baddr, size, PROT_READ|PROT_WRITE|PROT_EXEC,
			                                   flags, -1, 0);
			/* mmap signals failure with MAP_FAILED, not NULL. */
			if (p != MAP_FAILED) {
				if (_VALIDATE_BLOCK_FN(p, size))
					return p;

				munmap(p, size);
			}
		}
		return 0;
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		munmap(bufptr, size);
	}

#endif /* ARM64_ARCH */

#endif /* WIN32 */

#endif /* MMAP_JIT_CACHE */


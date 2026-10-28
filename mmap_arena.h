#ifndef clox_mmap_arena_h
#define clox_mmap_arena_h

// arena.h switches to an mmap-backed allocator when OOM_COMMIT is defined:
// virtual address space is reserved up front (PROT_NONE) and pages are
// committed on demand (mprotect) as the bump pointer advances. That gives one
// growable, contiguous arena with no malloc/realloc/free behind it.
#ifndef OOM_COMMIT
#define OOM_COMMIT
#endif

#include <sys/resource.h>

#include <arena.h>

// Virtual address range reserved for the compiler arena (source text, AST and
// cgen IR). mmap reserves it without committing: pages become real memory only
// as the bump pointer advances, so a large reservation costs no RSS until the
// input actually needs it. It only has to exceed the largest single compile's
// live set.
#if UINTPTR_MAX > 0xFFFFFFFFu
#define CLOX_ARENA_RESERVE_CAP GB(64)
#else
// A 32-bit address space cannot hold the 64-bit default (GB(64) shifts to 0);
// keep the previous 256 MB ceiling.
#define CLOX_ARENA_RESERVE_CAP MB(256)
#endif

// Effective reservation size for arena_init(NULL, ...). Starts from the cap
// above and, when RLIMIT_AS is finite (CI containers, ulimit -v), takes at
// most half of it so the reservation itself cannot be what makes mmap fail.
// A smaller reservation exhausts at allocation time and reports "Out of
// memory." instead of aborting inside arena_init.
static inline isize clox_arena_reserve(void) {
  isize want = (isize)CLOX_ARENA_RESERVE_CAP;
#ifdef RLIMIT_AS
  struct rlimit lim;
  if (getrlimit(RLIMIT_AS, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY) {
    uintmax_t half = (uintmax_t)lim.rlim_cur / 2;
    if (half < (uintmax_t)want)
      want = (isize)half;
  }
#endif
  return want;
}

#endif

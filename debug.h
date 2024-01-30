#ifndef clox_debug_h
#define clox_debug_h

// clang-format off
#ifdef GDB
#define assert(c)    if (!(c)) __builtin_unreachable()
#define breakpoint() asm("int3; nop")
#else
#include <assert.h>
#define breakpoint() (void)0;
#endif

#include "chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(Chunk* chunk, int offset);

#endif

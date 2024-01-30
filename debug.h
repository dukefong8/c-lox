#ifndef clox_debug_h
#define clox_debug_h

// clang-format off
#define assert(c) if (!(c)) __builtin_unreachable()

#ifdef GDB
#define breakpoint() asm("int3; nop")
#else
#define breakpoint() (void)0;
#endif

#include "chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(Chunk* chunk, int offset);

#endif

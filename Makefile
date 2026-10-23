EXE = main
BUILD_DIR := ./build

SRCS := $(shell find . -name '*.c')
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

WARN = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function 
SANZ = # -fsanitize=undefined

# Clang-only: print just one frame of macro expansion context on errors.
CLANG_MACRO_FLAGS := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo -fmacro-backtrace-limit=1)

CPPFLAGS += -MMD -MP -I. -I../ModernC/include
CFLAGS += $(WARN) $(SANZ) -g3 -fno-omit-frame-pointer $(CLANG_MACRO_FLAGS)
LDFLAGS += $(SANZ)

.PHONY: all
all: $(EXE)

.PHONY: clean
clean:
	rm -r $(EXE) $(BUILD_DIR)

.PHONY: watch
watch:
	find -name '*.c' -o -name '*.h' | entr -cc clang $(WARN) -fsyntax-only -ferror-limit=1 -fmacro-backtrace-limit=1 /_

$(EXE): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o : %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(DEPS)

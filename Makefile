EXE = main
BUILD_DIR := ./build

SRCS := $(shell find . -name '*.c')
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

WARN = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function 
SANZ = -fsanitize=undefined

CPPFLAGS += -MMD -MP -I. -I../ModernC/include
CFLAGS += $(WARN) $(SANZ) -g3 -fno-omit-frame-pointer
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

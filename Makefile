PROG      = velo
HEADLESS  = headless
BUILD     = build
ROM      ?= rom/nk.bin

.DEFAULT_GOAL := $(PROG)

CFLAGS  += -Isrc -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
CFLAGS  += $(shell pkg-config --cflags sdl3)
LDFLAGS += $(shell pkg-config --libs sdl3) -lm

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
SRC_MENU  = src/menu_macos.m
LDFLAGS  += -framework Cocoa
else
SRC_MENU  = src/menu_none.c
endif

SRC_MACHINE = src/mips.c src/machine.c
SRC_APP     = $(SRC_MACHINE) src/lcd.c src/main.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(BUILD)/tools/headless.o

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11,$(CFLAGS)) -fobjc-arc -c -o $@ $<

run: $(PROG)
	./$(PROG) $(ROM)

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS)

.PHONY: run clean test

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d)

test: $(HEADLESS)
	sh tests/boot.sh $(ROM)

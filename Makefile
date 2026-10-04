PROG      = sh3emu
HEADLESS  = headless
SH3RUN    = sh3-run
GATEWAYCHECK = $(BUILD)/gateway-check
APP       = SH3Emu.app
BUILD     = build
ROM      ?= rom/odo-sh3-ce212.bin

.DEFAULT_GOAL := all

CFLAGS  += -Isrc -I$(BUILD) -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
CFLAGS  += $(shell pkg-config --cflags sdl3)
THREAD_LIBS = -lpthread
LDFLAGS += $(shell pkg-config --libs sdl3) -lm -lz $(THREAD_LIBS)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
MENU     ?= macos
LDFLAGS  += -framework Cocoa
else
MENU     ?= bar
CFLAGS   += -D_GNU_SOURCE
endif
ifeq ($(MENU),macos)
SRC_MENU  = src/app/menu_macos.m src/app/dialog_macos.m
else
SRC_MENU  = src/app/menu_bar.c src/vendor/truetype.c
endif

ifeq ($(shell pkg-config --exists slirp && echo yes),yes)
SRC_NET   = src/net/net_gateway.c src/net/net_link.c
CFLAGS   += $(shell pkg-config --cflags slirp)
NET_LIBS  = $(shell pkg-config --libs slirp)
else
SRC_NET   = src/net/net_gateway_none.c src/net/net_link.c
endif

SRC_MACHINE = src/core/sh3.c src/core/sh7709.c src/core/machine.c src/core/cfcard.c src/core/ppfs.c src/core/mailbox.c src/core/agent.c src/core/ce.c src/core/gdb.c src/core/screen.c src/core/key_text.c src/util/options.c src/util/file.c
SRC_APP     = $(SRC_MACHINE) $(SRC_NET) src/core/lcd.c src/util/png.c src/app/typer.c src/app/view.c src/app/profiles.c src/app/main.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/src/core/lcd.o $(BUILD)/src/util/png.o $(BUILD)/tools/headless.o

all: $(HEADLESS) $(SH3RUN)

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS) $(NET_LIBS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm -lz $(THREAD_LIBS) $(NET_LIBS)

$(GATEWAYCHECK): $(filter-out %/net_link.o,$(SRC_NET:%.c=$(BUILD)/%.o)) $(BUILD)/tools/gateway_check.o
	$(CC) -o $@ $^ $(NET_LIBS)

$(SH3RUN): $(BUILD)/src/core/sh3.o $(BUILD)/tools/sh3_run.o
	$(CC) -o $@ $^

ICON_TOOL  = $(BUILD)/icon
ICON_SIZES = 16 32 64 128 256 512 1024

$(ICON_TOOL): $(BUILD)/tools/icon.o $(BUILD)/src/util/png.o $(BUILD)/src/vendor/svg.o
	$(CC) -o $@ $^ -lm -lz

icons: $(ICON_TOOL) assets/icon.svg
	@mkdir -p $(BUILD)/icons
	@for size in $(ICON_SIZES); do $(ICON_TOOL) assets/icon.svg $$size $(BUILD)/icons/icon-$$size.png || exit 1; done

$(BUILD)/src/vendor/%.o: CFLAGS += -w

VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)

$(BUILD)/version.h: FORCE
	@mkdir -p $(BUILD)
	@printf '#define SH3EMU_VERSION "%s"\n' "$(VERSION)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD)/src/util/options.o: $(BUILD)/version.h

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11,$(CFLAGS)) -fobjc-arc -c -o $@ $<

run: $(PROG)
	./$(PROG) $(ROM)

app: $(PROG) icons
	ICONS=$(BUILD)/icons sh tools/mkapp.sh $(APP)

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS) $(SH3RUN) $(APP)

.PHONY: all run clean test check app icons sh3-fuzz FORCE

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d) $(BUILD)/tools/icon.d $(BUILD)/tools/sh3_run.d $(BUILD)/tools/gateway_check.d

check: $(HEADLESS) $(SH3RUN) $(GATEWAYCHECK)
	sh tests/check.sh
	$(GATEWAYCHECK)

test: $(HEADLESS) $(SH3RUN)
	sh tests/sh3/run.sh
	sh tests/boot.sh $(ROM)

sh3-fuzz: $(SH3RUN)
	python3 tests/sh3/fuzz.py --runner ./$(SH3RUN)

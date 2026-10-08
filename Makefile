PROG      = sh3emu
HEADLESS  = headless
SH3RUN    = sh3-run
RAPI_TOOL = sh3emu-rapi
GATEWAYCHECK = $(BUILD)/gateway-check
PROXYCHECK = proxycheck
APP       = SH3Emu.app
BUILD     = build
UNCRUSTIFY ?= uncrustify
ROM      ?= rom/nk-hp320lx.bin
VELO_TOOLCHAIN ?= ../../velo-toolchain
C_STYLE_SOURCES = $(filter-out src/vendor/%,$(shell git ls-files 'src/**/*.c' 'src/**/*.h' 'tools/*.c'))

.DEFAULT_GOAL := all

PKG_CONFIG ?= pkg-config

CFLAGS  += -Isrc -I$(BUILD) -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
ifeq ($(SDL_STATIC),1)
SDL_PKG_CONFIG = $(PKG_CONFIG) --static
else
SDL_PKG_CONFIG = $(PKG_CONFIG)
endif

CFLAGS  += $(shell $(SDL_PKG_CONFIG) --cflags sdl3)
THREAD_LIBS = -lpthread
LDFLAGS += $(shell $(SDL_PKG_CONFIG) --libs sdl3) -lm -lz $(THREAD_LIBS)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
MENU     ?= macos
else
MENU     ?= bar
CFLAGS   += -D_GNU_SOURCE
endif
ifeq ($(MENU),macos)
SRC_MENU  = src/frontend/macos/menu.m src/frontend/macos/dialog.m
LDFLAGS  += -framework Cocoa
else ifeq ($(MENU),android)
SRC_MENU  = src/frontend/android/menu.c src/frontend/android/dialog.c src/frontend/android/keystrip.c src/frontend/android/list.c src/frontend/android/text.c src/frontend/android/toast.c src/frontend/common/menu_state.c src/frontend/android/android.c src/vendor/truetype.c
else
SRC_MENU  = src/frontend/linux/menu.c src/frontend/linux/dialog.c src/frontend/linux/ui.c src/frontend/common/menu_state.c src/vendor/truetype.c
endif

ifeq ($(shell $(PKG_CONFIG) --exists slirp && echo yes),yes)
SRC_NET   = src/net/net_gateway.c src/net/serial_link.c
CFLAGS   += $(shell $(PKG_CONFIG) --cflags slirp)
NET_LIBS  = $(shell $(PKG_CONFIG) --libs slirp)
ifeq ($(shell $(PKG_CONFIG) --exists libcurl && echo yes),yes)
SRC_NET  += src/net/web_proxy.c src/net/web_image.c src/vendor/image.c src/vendor/svg.c
CFLAGS   += $(shell $(PKG_CONFIG) --cflags libcurl)
NET_LIBS += $(shell $(PKG_CONFIG) --libs libcurl)
else
SRC_NET  += src/net/web_proxy_none.c
endif
else
SRC_NET   = src/net/net_gateway_none.c src/net/web_proxy_none.c src/net/serial_link.c
endif

SRC_MACHINE = src/core/sh3.c src/core/sh7709.c src/core/machine.c src/core/casio.c src/core/hp320lx.c src/core/cfcard.c src/core/mailbox.c src/core/agent.c src/core/ce.c src/core/gdb.c src/core/screen.c src/core/key_text.c src/util/options.c src/util/file.c
SRC_RAPI    = src/rapi/rapi.c src/rapi/rapi_load.c src/rapi/rapi_setup.c src/rapi/rapi_sync.c src/rapi/debugmgr_images.c
SRC_APP     = $(SRC_MACHINE) $(SRC_NET) $(SRC_RAPI) src/app/capture.c src/app/desktop.c src/app/host.c src/app/input.c src/app/launch.c src/app/log.c src/app/paths.c src/app/rom_catalog.c src/app/settings.c src/app/snapshot_store.c src/core/lcd.c src/util/png.c src/app/typer.c src/app/view.c src/app/profiles.c src/app/main.c src/frontend/common/dialog.c src/frontend/common/menu_queue.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/src/core/lcd.o $(BUILD)/src/util/png.o $(BUILD)/tools/headless.o

all: $(HEADLESS) $(SH3RUN) $(RAPI_TOOL)

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS) $(NET_LIBS)

$(BUILD)/libmain.so: $(OBJ_APP)
	$(CC) -shared -Wl,--no-undefined -o $@ $^ $(LDFLAGS) $(NET_LIBS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm -lz $(THREAD_LIBS) $(NET_LIBS)

$(GATEWAYCHECK): $(filter-out %/serial_link.o,$(SRC_NET:%.c=$(BUILD)/%.o)) $(BUILD)/tools/gateway_check.o
	$(CC) -o $@ $^ -lm -lz $(NET_LIBS) $(THREAD_LIBS)

$(PROXYCHECK): $(filter-out %/serial_link.o,$(SRC_NET:%.c=$(BUILD)/%.o)) $(BUILD)/tools/proxy_check.o
	$(CC) -o $@ $^ -lm -lz $(NET_LIBS) $(THREAD_LIBS)

$(SH3RUN): $(BUILD)/src/core/sh3.o $(BUILD)/tools/sh3_run.o
	$(CC) -o $@ $^

$(RAPI_TOOL): $(SRC_RAPI:%.c=$(BUILD)/%.o) $(BUILD)/src/util/options.o $(BUILD)/tools/sh3emu_rapi.o
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

app: $(PROG) $(RAPI_TOOL) icons
	ICONS=$(BUILD)/icons sh tools/mkapp.sh $(APP)

apk: icons
	ICONS=$(BUILD)/icons sh tools/mkapk.sh

apk-push:
	sh tools/mkapk.sh push

apk-install: icons
	ICONS=$(BUILD)/icons sh tools/mkapk.sh install

debugmgr:
	$(MAKE) -C $(VELO_TOOLCHAIN) debugmgr-sh3
	python3 tools/mkdebugmgr.py $(VELO_TOOLCHAIN)/build/debugmgr src/rapi/debugmgr_images.c

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS) $(SH3RUN) $(RAPI_TOOL) $(PROXYCHECK) $(APP)

.PHONY: all run clean test check format format-check app apk apk-push apk-install icons debugmgr sh3-fuzz FORCE

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d) $(BUILD)/tools/icon.d $(BUILD)/tools/sh3_run.d $(BUILD)/tools/gateway_check.d $(BUILD)/tools/sh3emu_rapi.d

check: $(HEADLESS) $(SH3RUN) $(GATEWAYCHECK) $(PROXYCHECK)
	sh tests/check.sh

format:
	$(UNCRUSTIFY) -c .uncrustify.cfg --replace --no-backup $(C_STYLE_SOURCES)

format-check:
	$(UNCRUSTIFY) -c .uncrustify.cfg --check $(C_STYLE_SOURCES)
	$(GATEWAYCHECK)

test: $(PROG) $(HEADLESS) $(SH3RUN) $(RAPI_TOOL)
	sh tests/sh3/run.sh
	sh tests/boot.sh

sh3-fuzz: $(SH3RUN)
	python3 tests/sh3/fuzz.py --runner ./$(SH3RUN)

PROG      = velo
HEADLESS  = headless
PROXYCHECK = proxycheck
VELORAPI  = velo-rapi
BUILD     = build
ROM      ?= rom/nk.bin
CE2_ROM  ?= rom/nk-ce2.bin

.DEFAULT_GOAL := all

CFLAGS  += -Isrc -I$(BUILD) -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
CFLAGS  += $(shell pkg-config --cflags sdl3)
THREAD_LIBS = -lpthread
LDFLAGS += $(shell pkg-config --libs sdl3) -lm -lz $(THREAD_LIBS)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
SRC_MENU  = src/app/menu_macos.m
LDFLAGS  += -framework Cocoa
else
SRC_MENU  = src/app/menu_none.c
CFLAGS   += -D_GNU_SOURCE
endif

ifeq ($(shell pkg-config --exists slirp && echo yes),yes)
SRC_NET  = src/net/net_gateway.c
CFLAGS  += $(shell pkg-config --cflags slirp)
NET_LIBS = $(shell pkg-config --libs slirp)
ifeq ($(shell pkg-config --exists libcurl && echo yes),yes)
SRC_NET  += src/net/web_proxy.c src/net/web_image.c src/vendor/vendor.c
CFLAGS   += $(shell pkg-config --cflags libcurl)
NET_LIBS += $(shell pkg-config --libs libcurl)
else
SRC_NET  += src/net/web_proxy_none.c
endif
LDFLAGS += $(NET_LIBS)
else
SRC_NET  = src/net/net_gateway_none.c src/net/web_proxy_none.c
endif

SRC_MACHINE = src/core/mips.c src/core/machine.c src/core/pccard.c src/core/uart.c src/core/key_text.c src/util/options.c
SRC_RAPI    = src/rapi/rapi.c src/rapi/rapi_load.c src/rapi/rapi_setup.c src/rapi/rapi_sync.c
SRC_APP     = $(SRC_MACHINE) $(SRC_NET) $(SRC_RAPI) src/app/desktop.c src/core/lcd.c src/util/png.c src/app/typer.c src/app/view.c src/app/main.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/src/core/lcd.o $(BUILD)/src/util/png.o $(BUILD)/tools/headless.o

all: $(PROG) $(VELORAPI)

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm -lz $(NET_LIBS) $(THREAD_LIBS)

$(PROXYCHECK): $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/tools/proxy_check.o
	$(CC) -o $@ $^ -lm $(NET_LIBS) $(THREAD_LIBS)

$(VELORAPI): $(SRC_RAPI:%.c=$(BUILD)/%.o) $(BUILD)/src/util/options.o $(BUILD)/tools/velo_rapi.o
	$(CC) -o $@ $^

$(BUILD)/src/vendor/%.o: CFLAGS += -w

VERSION ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)

$(BUILD)/version.h: FORCE
	@mkdir -p $(BUILD)
	@printf '#define VELO_VERSION "%s"\n' "$(VERSION)" > $@.tmp
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

app: $(PROG) $(VELORAPI)
	sh tools/mkapp.sh Velo.app

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS) $(PROXYCHECK) $(VELORAPI) Velo.app

.PHONY: all run clean test check app FORCE

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d) $(BUILD)/tools/proxy_check.d $(BUILD)/tools/velo_rapi.d

check: $(PROG) $(HEADLESS) $(PROXYCHECK) $(VELORAPI)
	sh tests/check.sh

test: $(HEADLESS) $(PROXYCHECK) $(VELORAPI)
	sh tests/boot.sh $(ROM) $(CE2_ROM)

PROG      = velo
HEADLESS  = headless
PROXYCHECK = proxycheck
VELORAPI  = velo-rapi
BUILD     = build
ROM      ?= rom/nk.bin
CE2_ROM  ?= rom/nk-ce2.bin

.DEFAULT_GOAL := all

CFLAGS  += -Isrc -Wall -Wextra -O2 -std=c11 -fno-common -MMD -MP
CFLAGS  += $(shell pkg-config --cflags sdl3)
THREAD_LIBS = -lpthread
LDFLAGS += $(shell pkg-config --libs sdl3) -lm $(THREAD_LIBS)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
SRC_MENU  = src/menu_macos.m
LDFLAGS  += -framework Cocoa
else
SRC_MENU  = src/menu_none.c
endif

ifeq ($(shell pkg-config --exists slirp && echo yes),yes)
SRC_NET  = src/netgw.c
CFLAGS  += $(shell pkg-config --cflags slirp)
NET_LIBS = $(shell pkg-config --libs slirp)
ifeq ($(shell pkg-config --exists libcurl && echo yes),yes)
SRC_NET  += src/webproxy.c src/webimage.c src/vendor/vendor.c
CFLAGS   += $(shell pkg-config --cflags libcurl)
NET_LIBS += $(shell pkg-config --libs libcurl)
else
SRC_NET  += src/webproxy_none.c
endif
LDFLAGS += $(NET_LIBS)
else
SRC_NET  = src/netgw_none.c
endif

SRC_MACHINE = src/mips.c src/machine.c src/pccard.c src/uart.c src/keytext.c
SRC_RAPI    = src/rapi.c src/rapiload.c src/rapisetup.c src/rapisync.c
SRC_APP     = $(SRC_MACHINE) $(SRC_NET) $(SRC_RAPI) src/desktop.c src/lcd.c src/typer.c src/main.c $(SRC_MENU)

OBJ_APP      = $(patsubst %.m,$(BUILD)/%.o,$(SRC_APP:%.c=$(BUILD)/%.o))
OBJ_HEADLESS = $(SRC_MACHINE:%.c=$(BUILD)/%.o) $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/tools/headless.o

all: $(PROG) $(VELORAPI)

$(PROG): $(OBJ_APP)
	$(CC) -o $@ $^ $(LDFLAGS)

$(HEADLESS): $(OBJ_HEADLESS)
	$(CC) -o $@ $^ -lm $(NET_LIBS) $(THREAD_LIBS)

$(PROXYCHECK): $(SRC_NET:%.c=$(BUILD)/%.o) $(BUILD)/tools/proxycheck.o
	$(CC) -o $@ $^ -lm $(NET_LIBS) $(THREAD_LIBS)

$(VELORAPI): $(SRC_RAPI:%.c=$(BUILD)/%.o) $(BUILD)/tools/velorapi.o
	$(CC) -o $@ $^

$(BUILD)/src/vendor/%.o: CFLAGS += -w

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -std=c11,$(CFLAGS)) -fobjc-arc -c -o $@ $<

run: $(PROG)
	./$(PROG) $(ROM)

clean:
	rm -rf $(BUILD) $(PROG) $(HEADLESS) $(PROXYCHECK) $(VELORAPI)

.PHONY: all run clean test

-include $(OBJ_APP:.o=.d) $(OBJ_HEADLESS:.o=.d) $(BUILD)/tools/proxycheck.d $(BUILD)/tools/velorapi.d

test: $(HEADLESS) $(PROXYCHECK) $(VELORAPI)
	sh tests/boot.sh $(ROM) $(CE2_ROM)

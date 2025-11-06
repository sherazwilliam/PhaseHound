# Top-level Makefile
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -pthread -fPIC
LDFLAGS ?= -ldl -pthread

PREFIX ?= /usr/local

INCS = -Iinclude

CORE_SRCS = src/core.c src/common.c
CORE_OBJS = $(CORE_SRCS:.c=.o)
CORE_BIN  = ph-core

CLI_SRCS  = tools/cli.c src/common.c
CLI_OBJS  = $(CLI_SRCS:.c=.o)
CLI_BIN   = ph-cli

GIT_SHA := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
CFLAGS  += -DPH_GIT_SHA=\"$(GIT_SHA)\"

.PHONY: all clean addons install

all: $(CORE_BIN) $(CLI_BIN) addons addons

$(CORE_BIN): $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(CLI_BIN): $(CLI_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

addons:
	@for d in $(wildcard src/addons/*); do \
	  if [ -f $$d/Makefile ]; then echo "[addons] building $$d"; $(MAKE) -C $$d; fi; \
	done

clean:
	rm -f $(CORE_OBJS) $(CLI_OBJS) $(CORE_BIN) $(CLI_BIN)
	@for d in $(wildcard src/addons/*); do \
	  if [ -f $$d/Makefile ]; then echo "[addons] cleaning $$d"; $(MAKE) -C $$d clean; fi; \
	done

install:
	install -m755 $(CORE_BIN) $(PREFIX)/bin/$(CORE_BIN)
	install -m755 $(CLI_BIN)  $(PREFIX)/bin/$(CLI_BIN)

asan:
	@echo "No asan yet"

test:
	@echo "No tests yet"


# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# binary name
APP = receiver
SIMPLE_APP = pingpong-simple

# all source are stored in SRCS-y
SRCS-y := main.c
SIMPLE_SRCS := simple_pingpong.c

PKGCONF ?= pkg-config

# Build using pkg-config variables if possible
ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

# A0:36:9F:2A:5D:28

all: shared
.PHONY: shared static simple
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)
simple: build/$(SIMPLE_APP)-shared
	ln -sf $(SIMPLE_APP)-shared build/$(SIMPLE_APP)

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O3 $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk)
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk)

CFLAGS += -DALLOW_EXPERIMENTAL_API

build/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

build/$(SIMPLE_APP)-shared: $(SIMPLE_SRCS) Makefile $(PC_FILE) | build
	$(CC) $(CFLAGS) $(SIMPLE_SRCS) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

build:
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	rm -f build/$(SIMPLE_APP) build/$(SIMPLE_APP)-shared
	test -d build && rmdir -p build || true

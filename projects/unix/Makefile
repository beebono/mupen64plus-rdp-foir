#/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
# *   mupen64plus-rdp-foir - Makefile                                       *
# *   Follows the layout of the other Mupen64Plus plugin Makefiles.         *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU General Public License as published by  *
# *   the Free Software Foundation; either version 3 of the License, or     *
# *   (at your option) any later version.                                   *
# * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
# Makefile for the foir software RDP video plugin.
#
#   make all                          native build (x86_64: scalar kernels)
#   make all NEON=0                   scalar reference path only
#   make all CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64
#                                     cross build for aarch64 (NEON kernels = M2)
#   make replay                       build the offline replay tool (no core needed)

# detect operating system
UNAME ?= $(shell uname -s)
OS := NONE
ifeq ("$(UNAME)","Linux")
  OS = LINUX
  SO_EXTENSION = so
  SHARED = -shared
endif
ifeq ("$(UNAME)","linux")
  OS = LINUX
  SO_EXTENSION = so
  SHARED = -shared
endif
ifeq ("$(OS)","NONE")
  $(error OS type "$(UNAME)" is not supported by this Makefile yet (Linux only).)
endif

# detect system architecture (override HOST_CPU when cross compiling)
HOST_CPU ?= $(shell uname -m)
CPU := NONE
ifneq ("$(filter x86_64 amd64,$(HOST_CPU))","")
  CPU := X86
  ARCH_DETECTED := 64BITS
  PIC ?= 1
endif
ifneq ("$(filter aarch64 arm64,$(HOST_CPU))","")
  CPU := AARCH
  ARCH_DETECTED := 64BITS
  PIC ?= 1
endif
ifeq ("$(CPU)","NONE")
  $(error CPU type "$(HOST_CPU)" not supported (x86_64 and aarch64 only).)
endif

# kernel / compiler selection:
#   NEON=1 builds the NEON kernel set and its precompiled per-state variants (M2 / M3)
ifeq ("$(CPU)","AARCH")
  NEON ?= 1
else
  NEON ?= 0
endif

SRCDIR = ../../src
TOOLDIR = ../../tools
OBJDIR = _obj$(if $(CROSS_COMPILE),_cross_$(CPU),)$(POSTFIX)
# cross builds put their products in _out_<cpu>/ so they never clobber the native ones
OUTDIR = $(if $(CROSS_COMPILE),_out_$(CPU)/,)

# base CFLAGS, LDLIBS, and LDFLAGS
OPTFLAGS ?= -O3
WARNFLAGS ?= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -std=gnu11 $(OPTFLAGS) $(WARNFLAGS) -fwrapv -fno-strict-aliasing -fvisibility=hidden -I$(SRCDIR)
LDFLAGS += $(SHARED)

ifeq ($(PIC), 1)
  CFLAGS += -fPIC
else
  CFLAGS += -fno-PIC
endif

ifeq ($(NEON), 1)
  ifneq ("$(CPU)","AARCH")
    $(error NEON=1 needs an aarch64 target; use NEON=0 on x86_64)
  endif
  CFLAGS += -DFOIR_NEON=1
  NEON_SOURCE = $(SRCDIR)/kneon_comb.c $(SRCDIR)/kneon_blend.c $(SRCDIR)/kneon_mem.c $(SRCDIR)/kneon_tex.c
endif

ifeq ($(OS), LINUX)
  LDFLAGS += -Wl,-version-script,$(SRCDIR)/foir_api_export.ver
  LDLIBS += -ldl -lm -lpthread
endif

# set mupen64plus core API header path
ifneq ("$(APIDIR)","")
  CFLAGS += "-I$(APIDIR)"
else
  TRYDIR = ../../../mupen64plus-core/src/api
  ifneq ("$(wildcard $(TRYDIR)/m64p_types.h)","")
    CFLAGS += -I$(TRYDIR)
  else
    TRYDIR = /usr/local/include/mupen64plus
    ifneq ("$(wildcard $(TRYDIR)/m64p_types.h)","")
      CFLAGS += -I$(TRYDIR)
    else
      TRYDIR = /usr/include/mupen64plus
      ifneq ("$(wildcard $(TRYDIR)/m64p_types.h)","")
        CFLAGS += -I$(TRYDIR)
      else
        $(error Mupen64Plus API header files not found! Use makefile parameter APIDIR to force a location.)
      endif
    endif
  endif
endif

# reduced compile output when running make without V=1
ifneq ($(findstring $(MAKEFLAGS),s),s)
ifndef V
	Q_CC  = @echo '    CC  '$@;
	Q_LD  = @echo '    LD  '$@;
endif
endif

CC        = $(CROSS_COMPILE)gcc
RM       ?= rm -f
INSTALL  ?= install
MKDIR ?= mkdir -p
COMPILE.c = $(Q_CC)$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c
LINK.o = $(Q_LD)$(CC) $(CFLAGS) $(LDFLAGS) $(TARGET_ARCH)

ifeq ($(DEBUG),1)
  CFLAGS += -g
  INSTALL_STRIP_FLAG ?=
else
  CFLAGS += -DNDEBUG
  INSTALL_STRIP_FLAG ?= -s
endif

ifeq ($(PREFIX),)
  PREFIX := /usr/local
endif
ifeq ($(LIBDIR),)
  LIBDIR := $(PREFIX)/lib
endif
ifeq ($(PLUGINDIR),)
  PLUGINDIR := $(LIBDIR)/mupen64plus
endif

# renderer core (shared between the plugin and the replay tool)
CORE_SOURCE = \
	$(SRCDIR)/config.c \
	$(SRCDIR)/log.c \
	$(SRCDIR)/parser.c \
	$(SRCDIR)/setup.c \
	$(SRCDIR)/raster.c \
	$(SRCDIR)/mt.c \
	$(SRCDIR)/stats.c \
	$(SRCDIR)/tmem.c \
	$(SRCDIR)/kernels_scalar.c \
	$(SRCDIR)/kernels_tex.c \
	$(SRCDIR)/kernels_neon.c \
	$(NEON_SOURCE) \
	$(SRCDIR)/spec.c \
	$(SRCDIR)/vi.c

PLUGIN_SOURCE = \
	$(SRCDIR)/plugin.c \
	$(SRCDIR)/capture.c \
	$(SRCDIR)/framelog.c \
	$(SRCDIR)/gl_out.c

CORE_OBJECTS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(CORE_SOURCE))
PLUGIN_OBJECTS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(PLUGIN_SOURCE))
REPLAY_OBJECTS := $(OBJDIR)/tools/replay.o
FUZZ_OBJECTS := $(OBJDIR)/tools/kfuzz.o
WALKFUZZ_OBJECTS := $(OBJDIR)/tools/walkfuzz.o
OBJECTS := $(CORE_OBJECTS) $(PLUGIN_OBJECTS) $(REPLAY_OBJECTS) $(FUZZ_OBJECTS) $(WALKFUZZ_OBJECTS)
OBJDIRS = $(dir $(OBJECTS))
$(shell $(MKDIR) $(OBJDIRS) $(OUTDIR))

TARGET = $(OUTDIR)mupen64plus-rdp-foir$(POSTFIX).$(SO_EXTENSION)
REPLAY = $(OUTDIR)foir-replay
KFUZZ = $(OUTDIR)foir-kfuzz
WALKFUZZ = $(OUTDIR)foir-walkfuzz

targets:
	@echo "mupen64plus-rdp-foir makefile. "
	@echo "  Targets:"
	@echo "    all           == Build the foir video plugin ($(TARGET))"
	@echo "    replay        == Build the offline replay tool ($(REPLAY))"
	@echo "    check         == Run the bit-exactness checks (neoncheck.py, mtfuzz.py, kfuzz incl. the COPY kernel, the batch builder, LOD / mipmap scenes, every specialised variant, texture any-format kernel and combiner shape kernel)"
	@echo "    fuzz          == Build the NEON-vs-scalar kernel fuzzer ($(KFUZZ), needs an aarch64 build)"
	@echo "    walkfuzz      == Build the per-row edge walker test ($(WALKFUZZ))"
	@echo "    clean         == remove object files"
	@echo "    rebuild       == clean and re-build all"
	@echo "    install       == Install the plugin"
	@echo "    uninstall     == Uninstall the plugin"
	@echo "  Options:"
	@echo "    NEON=(1|0)    == NEON kernels + specialised variants (default: 1 on aarch64, 0 otherwise)"
	@echo "    CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64  == cross build for aarch64"
	@echo "    APIDIR=path   == path to find Mupen64Plus Core headers"
	@echo "    OPTFLAGS=flag == compiler optimization (default: -O3)"
	@echo "    WARNFLAGS=flag == compiler warning levels"
	@echo "    POSTFIX=name  == String added to the name of the build (default: '')"
	@echo "    DEBUG=1       == add debugging symbols"
	@echo "    V=1           == show verbose compiler output"
	@echo "  Install Options:"
	@echo "    PREFIX=path   == install/uninstall prefix (default: /usr/local)"
	@echo "    LIBDIR=path   == library prefix (default: PREFIX/lib)"
	@echo "    PLUGINDIR=path == path to install plugin libraries (default: LIBDIR/mupen64plus)"
	@echo "    DESTDIR=path  == path to prepend to all installation paths (only for packagers)"

all: $(TARGET)

replay: $(REPLAY)

fuzz: $(KFUZZ)

walkfuzz: $(WALKFUZZ)

# bit-exactness of the optimised paths against the scalar reference (see tools/neoncheck.py, tools/mtfuzz.py and the
# kernel fuzzer tools/kfuzz.c, whose -S mode walks through every specialised variant);
# cross builds: make check CROSS_COMPILE=aarch64-linux-gnu- HOST_CPU=aarch64 RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
RUNNER ?=
check: replay walkfuzz $(if $(filter AARCH,$(CPU)),fuzz)
	$(RUNNER) ./$(WALKFUZZ) -n 40000
	python3 $(TOOLDIR)/neoncheck.py --replay ./$(REPLAY) --runner "$(RUNNER)"
	python3 $(TOOLDIR)/mtfuzz.py -n 60 --replay ./$(REPLAY) --runner "$(RUNNER)"
ifeq ("$(CPU)","AARCH")
	python3 $(TOOLDIR)/mtfuzz.py -n 40 -s 1000 --comb shape --replay ./$(REPLAY) --runner "$(RUNNER)"
	python3 $(TOOLDIR)/mtfuzz.py -n 40 -s 2000 --tex shape --replay ./$(REPLAY) --runner "$(RUNNER)"
	$(RUNNER) $(KFUZZ) -n 3000
	$(RUNNER) $(KFUZZ) -m 32 -n 20000 -s 700000
	$(RUNNER) $(KFUZZ) -m 64 -n 40000 -s 800000
	$(RUNNER) $(KFUZZ) -S -n 15000
	$(RUNNER) $(KFUZZ) -S -T 5 -n 15000 -s 500000
	$(RUNNER) $(KFUZZ) -S -T 0 -n 8000 -s 900000
	$(RUNNER) $(KFUZZ) -L -m 1 -n 20000 -s 600000
	$(RUNNER) $(KFUZZ) -L -S -T 0 -n 8000 -s 950000
endif

install: $(TARGET)
	$(INSTALL) -d "$(DESTDIR)$(PLUGINDIR)"
	$(INSTALL) -m 0644 $(INSTALL_STRIP_FLAG) $(TARGET) "$(DESTDIR)$(PLUGINDIR)"

uninstall:
	$(RM) "$(DESTDIR)$(PLUGINDIR)/$(notdir $(TARGET))"

clean:
	$(RM) -r _obj* _out_* mupen64plus-rdp-foir*.so foir-replay foir-kfuzz foir-walkfuzz ref-angrylion

rebuild: clean all

# angrylion reference tool (x86 development tool; needs the sibling angrylion-rdp-plus checkout)
ANGRYDIR ?= ../../../angrylion-rdp-plus/src/core
ref: ref-angrylion

ref-angrylion: $(TOOLDIR)/ref_angrylion.c $(ANGRYDIR)/n64video.c $(ANGRYDIR)/parallel.cpp
	@echo '    LD  ref-angrylion'
	@$(CROSS_COMPILE)gcc -O2 -std=gnu11 -w -I$(ANGRYDIR) -c $(ANGRYDIR)/n64video.c -o $(OBJDIR)/angry_n64video.o
	@$(CROSS_COMPILE)g++ -O2 -std=c++14 -w -I$(ANGRYDIR) -c $(ANGRYDIR)/parallel.cpp -o $(OBJDIR)/angry_parallel.o
	@$(CROSS_COMPILE)gcc -O2 -std=gnu11 -w -I$(ANGRYDIR) -c $(TOOLDIR)/ref_angrylion.c -o $(OBJDIR)/ref_angrylion.o
	@$(CROSS_COMPILE)g++ $(OBJDIR)/ref_angrylion.o $(OBJDIR)/angry_n64video.o $(OBJDIR)/angry_parallel.o -lpthread -o ref-angrylion

CFLAGS += -MD -MP
$(OBJDIR)/tools/walkfuzz.o: $(TOOLDIR)/walk_ref.inc
-include $(OBJECTS:.o=.d)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(COMPILE.c) -o $@ $<

$(OBJDIR)/tools/%.o: $(TOOLDIR)/%.c
	$(COMPILE.c) -o $@ $<

$(TARGET): $(CORE_OBJECTS) $(PLUGIN_OBJECTS)
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

$(KFUZZ): $(CORE_OBJECTS) $(FUZZ_OBJECTS)
	$(Q_LD)$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(WALKFUZZ): $(CORE_OBJECTS) $(WALKFUZZ_OBJECTS)
	$(Q_LD)$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

# the replay tool is a plain executable: no export map, no -shared, no core dependency
$(REPLAY): $(CORE_OBJECTS) $(REPLAY_OBJECTS)
	$(Q_LD)$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

.PHONY: all replay fuzz walkfuzz check ref clean install uninstall targets rebuild

# Standalone benchmark app. Never shipped -> full DaisySP (LGPL) is fine here.
# The shipping firmware is built by the ROOT Makefile and is untouched.

TARGET = bench

CPP_STANDARD = -std=c++17

LIBDAISY_DIR = ../lib/libDaisy
DAISYSP_DIR  = ../lib/DaisySP

# Full DaisySP, including the LGPL modules -- family 2 measures them.
USE_DAISYSP_LGPL = 1

# Same linkage as the shipping firmware, so the memory map the SRAM/SDRAM
# workloads observe is the map the firmware will actually have.
APP_TYPE = BOOT_SRAM
LDSCRIPT = ../alt_sram.lds

# The engine includes DaisySP headers relative to Source/ ("Effects/overdrive.h",
# "Filters/svf.h", "Utility/dsp.h"), so that directory has to be on the path.
C_INCLUDES = -I. -I../engine/ -I../third_party/ -I../lib/DaisySP/Source/
C_USR_FLAGS = -ffast-math -funroll-loops

GIT_HASH = $(shell git -C .. rev-parse --short HEAD)

CPP_SOURCES = \
	main.cpp \
	report.cpp \
	runner.cpp \
	anchor.cpp \
	mem.cpp \
	workloads_system.cpp \
	workloads_daisysp.cpp \
	workloads_memory.cpp \
	workloads_mod.cpp \
	workloads_abl.cpp \
	workloads_taps.cpp \
	workloads_sampler.cpp \
	../engine/mod/lane.cpp \
	../engine/sampler/sampler_engine.cpp \
	../engine/sampler/sample_buffer.cpp \
	../engine/mod/super_modulator.cpp \
	../engine/parts/part.cpp \
	../engine/instrument.cpp \
	../engine/center/center.cpp \
	../engine/fx/grit.cpp \
	../engine/fx/flux.cpp \
	../engine/fx/taps.cpp \
	../engine/fx/reverb.cpp \
	../engine/fx/part_fx.cpp \
	../engine/fx/comp.cpp \
	../engine/synth/voice.cpp \
	../engine/synth/synth_engine.cpp

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# --- git hash header --------------------------------------------------
# `-DBENCH_GIT_HASH=...` on the compile line is invisible to Make's
# dependency graph, so a stale build/main.o survives a HEAD move and keeps
# reporting the old hash (this bit once: a run labelled itself 5555422
# while HEAD was 0e6eeb5). Emit the hash as a real header instead, and make
# it a real prerequisite of the one object that uses it.
#
# Written via a temp file, promoted only if content differs, so the
# header's mtime -- and therefore whether main.o rebuilds -- reflects
# whether the hash actually changed, not merely whether `make` ran.
C_INCLUDES += -I$(BUILD_DIR)

.PHONY: FORCE
FORCE:

$(BUILD_DIR)/git_hash.h: FORCE | $(BUILD_DIR)
	@echo '#define BENCH_GIT_HASH "$(GIT_HASH)"' > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || mv -f $@.tmp $@
	@rm -f $@.tmp

$(BUILD_DIR)/main.o: $(BUILD_DIR)/git_hash.h

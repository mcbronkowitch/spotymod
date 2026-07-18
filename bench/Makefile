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
C_DEFS += -DBENCH_GIT_HASH=\"$(GIT_HASH)\"

CPP_SOURCES = \
	main.cpp \
	report.cpp \
	runner.cpp \
	mem.cpp \
	workloads_system.cpp \
	../engine/mod/lane.cpp \
	../engine/mod/super_modulator.cpp \
	../engine/parts/part.cpp \
	../engine/instrument.cpp \
	../engine/center/center.cpp \
	../engine/fx/grit.cpp \
	../engine/fx/flux.cpp \
	../engine/fx/reverb.cpp \
	../engine/fx/part_fx.cpp \
	../engine/fx/comp.cpp \
	../engine/synth/voice.cpp \
	../engine/synth/synth_engine.cpp

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

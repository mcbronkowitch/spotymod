# spotymod -- VCV Rack plugin (third host over the shared engine/ core).
#
#   RACK_DIR=/path/to/Rack-SDK make        # build
#   RACK_DIR=/path/to/Rack-SDK make install # build + copy to Rack user dir
#
# No engine sources are duplicated: this Makefile compiles the same
# engine/*.cpp the desktop render host (CMakeLists.txt) uses, two levels up.

RACK_DIR ?= ../../../Rack-SDK
# Absolute repo root: keeps object files for the shared engine sources inside
# host/vcv/build/ (a relative ../.. would make plugin.mk emit .o files outside
# the plugin dir). Run make from this directory.
REPO     := $(realpath ../..)

FLAGS += -I$(REPO)/engine -I$(REPO)/third_party
# DaisySP fans its private headers (dsp.h, ...) across these subdirs; the engine's
# FX chain pulls them in transitively, so all four must be on the include path.
FLAGS += -I$(REPO)/lib/DaisySP/Source \
         -I$(REPO)/lib/DaisySP/Source/Utility \
         -I$(REPO)/lib/DaisySP/Source/Control \
         -I$(REPO)/lib/DaisySP/Source/Effects \
         -I$(REPO)/lib/DaisySP/Source/Filters
CFLAGS +=
CXXFLAGS +=

# plugin glue
SOURCES += $(wildcard src/*.cpp)

# portable engine core (identical list to the render target in CMakeLists.txt)
SOURCES += \
	$(REPO)/engine/mod/lane.cpp \
	$(REPO)/engine/mod/super_modulator.cpp \
	$(REPO)/engine/parts/part.cpp \
	$(REPO)/engine/instrument.cpp \
	$(REPO)/engine/center/center.cpp \
	$(REPO)/engine/fx/grit.cpp \
	$(REPO)/engine/fx/flux.cpp \
	$(REPO)/engine/fx/reverb.cpp \
	$(REPO)/engine/fx/part_fx.cpp \
	$(REPO)/engine/fx/comp.cpp \
	$(REPO)/engine/synth/voice.cpp \
	$(REPO)/engine/synth/synth_engine.cpp

# minimal DaisySP slice the FX chain needs
SOURCES += \
	$(REPO)/lib/DaisySP/Source/Effects/overdrive.cpp \
	$(REPO)/lib/DaisySP/Source/Effects/decimator.cpp \
	$(REPO)/lib/DaisySP/Source/Effects/sampleratereducer.cpp \
	$(REPO)/lib/DaisySP/Source/Control/phasor.cpp \
	$(REPO)/lib/DaisySP/Source/Filters/svf.cpp

DISTRIBUTABLES += res plugin.json

include $(RACK_DIR)/plugin.mk

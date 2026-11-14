# The host renderer (see render.cpp): the firmware's UI and engines, built
# for the computer with stand-ins for the Pod's hardware (fake/).
SRC   = ../../../src
CXX  ?= clang++
# -MMD: each object also rebuilds when a header it includes changes.
FLAGS = -std=gnu++17 -O1 -w -MMD -MP -D'section(x)=used' -include fake/stm32h7xx.h \
        -Ifake -I$(SRC) -I$(SRC)/libDaisy/src -I$(SRC)/DaisySP/Source \
        -I$(SRC)/DaisySP/DaisySP-LGPL/Source \
        $(addprefix -I,$(shell find $(SRC)/DaisySP/Source $(SRC)/DaisySP/DaisySP-LGPL/Source -type d))
FW    = ui ui_supersaw ui_lfo ui_graphics font_tomthumb performance_store looper_layer \
        tempo_clock granular_engine dexed_synth dexed_sysex dexed_factory_data \
        dattorro_reverb ensemble_synth ensemble_factory_data supersaw_synth sample_synth \
        sequencer arpeggiator
MSFA  = exp2 freqlut sin fm_op_kernel fm_core env pitchenv lfo porta dx7note EngineMsfa
DSP   = $(shell find $(SRC)/DaisySP/Source $(SRC)/DaisySP/DaisySP-LGPL/Source -name '*.cpp')
OBJ   = $(addprefix obj/fw_,$(addsuffix .o,$(FW))) $(addprefix obj/msfa_,$(addsuffix .o,$(MSFA))) \
        $(patsubst %.cpp,obj/dsp_%.o,$(notdir $(DSP))) obj/render.o obj/host_ff.o obj/host_dir.o obj/oled_fonts.o

render: $(OBJ)
	$(CXX) -o $@ $(OBJ)

obj/fw_%.o: $(SRC)/%.cpp | obj
	$(CXX) $(FLAGS) -c $< -o $@
obj/msfa_%.o: $(SRC)/msfa/%.cpp | obj
	$(CXX) $(FLAGS) -c $< -o $@
obj/render.o: render.cpp | obj
	$(CXX) $(FLAGS) -c $< -o $@
obj/host_ff.o: host_ff.cpp | obj
	$(CXX) $(FLAGS) -c $< -o $@
obj/host_dir.o: host_dir.c | obj
	cc -O1 -c $< -o $@
obj/oled_fonts.o: $(SRC)/libDaisy/src/util/oled_fonts.c | obj
	cc -O1 -w -I$(SRC)/libDaisy/src -c $< -o $@
vpath %.cpp $(sort $(dir $(DSP)))
obj/dsp_%.o: %.cpp | obj
	$(CXX) $(FLAGS) -c $< -o $@
obj:
	mkdir -p obj

-include $(wildcard obj/*.d)
clean:
	rm -rf obj render

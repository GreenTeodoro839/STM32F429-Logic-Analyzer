# Host-side tests (gcc, AddressSanitizer + UBSan):  make -C tests
CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -I. -I../f429 -I../common
OUT      = build

TESTS = $(OUT)/test_analyze $(OUT)/test_gen $(OUT)/test_meter

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

$(OUT)/test_analyze: test_analyze.c ring_stub.c ../f429/analyze.c ../f429/decode.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

$(OUT)/test_gen: test_gen.c ../f429/gen_calc.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^

# meter.c is compiled from a copy next to a stub clock.h; the test simulates
# the CPU time of the statistics by wrapping ana_stats_feed_masked
$(OUT)/test_meter: test_meter.c ../f429/meter.c ../f429/analyze.c ring_stub.c | $(OUT)
	mkdir -p $(OUT)/meter_src && cp ../f429/meter.c meter_stub/clock.h $(OUT)/meter_src/
	$(CC) $(CFLAGS) $(CPPFLAGS) -DMETER_TEST -Wl,--wrap=ana_stats_feed_masked -o $@ test_meter.c $(OUT)/meter_src/meter.c ../f429/analyze.c ring_stub.c

# UI simulator: the real UI code on synthetic data, screens as PPM files.
# The sources are copied next to stub clock.h / tusb.h so that their own
# includes resolve to the stubs.
SIM_SRC = ui.c ui_wave.c ui_setup.c ui_meter.c ui_decode.c ui_gen.c gfx.c font.c analyze.c decode.c gen_calc.c

sim: | $(OUT)
	rm -rf $(OUT)/sim_src && mkdir -p $(OUT)/sim_src $(OUT)/screens
	cp $(addprefix ../f429/,$(SIM_SRC)) sim/clock.h sim/tusb.h $(OUT)/sim_src/
	$(CC) $(CFLAGS) -Wno-format-truncation $(CPPFLAGS) -o $(OUT)/ui_sim sim/sim_main.c sim/sim_stubs.c ring_stub.c \
	    $(addprefix $(OUT)/sim_src/,$(SIM_SRC))
	./$(OUT)/ui_sim $(OUT)/screens

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -rf $(OUT)

.PHONY: all clean sim

# Open-source Polono label printer driver (PL60, PL70e-BT, PL420):
# filter, tests and macOS installer.
#
#   make          universal (Apple Silicon + Intel) filter in build/
#   make test     check the filter against tests/cases
#   make pkg      build/Polono-Driver-$(VERSION).pkg and the uninstaller
#
# libcups and its PPD API are deprecated on macOS but still ship; the
# warnings are silenced on purpose.

VERSION = 1.0.0
CC      = clang
ARCHS   = -arch arm64 -arch x86_64
CFLAGS  = $(ARCHS) -O2 -Wall -Wextra -Wno-deprecated-declarations -mmacosx-version-min=11.0
LDLIBS  = -lcups -lcupsimage
BUILD   = build
FILTER  = $(BUILD)/rastertotspl

all: $(FILTER)

$(FILTER): src/rastertotspl.c
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
	codesign --force --sign - $@

test: $(FILTER)
	tests/run.sh $(FILTER)

# Regenerate tests/cases/<model> from the current filter (all models, or
# MODELS="PL70e PL420"). Only after a deliberate change of output that has
# been checked against Polono's driver or a real printer.
test-vectors: $(FILTER)
	tests/make-cases.sh $(FILTER) $(MODELS)

# Rewrite the PL70e-BT and PL420 PPDs from ppd/make_beeprt_ppds.py.
ppds:
	python3 ppd/make_beeprt_ppds.py

pkg: $(FILTER) test
	VERSION=$(VERSION) pkg/build.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test test-vectors ppds pkg clean

# nioh3fps: build with MinGW-w64 gcc (WinLibs UCRT) via mingw32-make.
#   mingw32-make            -> build/dinput8.dll (stripped), build/unstripped/dinput8.dll, build/dinput8.map
#   mingw32-make release    -> build/release/ artifacts; no ToggleFile or probe CSV paths
#   mingw32-make test       -> native regression, resolver/gate/heartbeat/integration and proxy smoke tests
#                              (test_trace also cross-checks tools/trace2csv.py against the DLL converter)
#   mingw32-make imports    -> objdump -p import check (PLAN section 3)
#   mingw32-make expected   -> regenerate src/expected.h from the decrypted exe
CC      := gcc
STRIP   := strip
OBJDUMP := objdump
PYTHON  := python
EXE     ?= G:/SteamLibrary/steamapps/common/Nioh3/Nioh3.exe.unpacked.exe
PROBECSV ?= docs/s05/probe_targets.csv
RELEASE ?= 0
BDIR    := $(if $(filter 1,$(RELEASE)),build/release,build)
.DEFAULT_GOAL := all

GIT_REV  := $(shell git -c core.excludesFile= rev-parse --short=8 HEAD 2>/dev/null)
GIT_DIRTY := $(if $(GIT_REV),$(shell git -c core.excludesFile= status --porcelain --untracked-files=no -- src third_party Makefile tests tools))
BUILD_ID := $(if $(GIT_REV),$(GIT_REV)$(if $(GIT_DIRTY),-dirty,),nogit)
# objects are rebuilt when the build ID changes (it is compiled into the log header and the trace)
STAMP   := $(BDIR)/obj/buildid.stamp
$(shell mkdir -p $(BDIR)/obj && (echo '$(BUILD_ID)' | cmp -s - $(STAMP) || echo '$(BUILD_ID)' > $(STAMP)))

MH      := third_party/minhook
CFLAGS  := -O2 -std=gnu11 -Wall -Wextra -fno-strict-aliasing -I$(MH)/include -DBUILD_ID=\"$(BUILD_ID)\"
MHFLAGS := -O2 -std=gnu11 -w -I$(MH)/include
LDFLAGS := -shared -static -Wl,--subsystem,windows
ifeq ($(RELEASE),1)
CFLAGS  += -DRELEASE
endif

SRC     := src/proxy.c src/init.c src/hooks.c src/factor.c src/stubs.c src/config.c src/logring.c \
           src/trace.c src/writer.c src/probe.c src/crash.c src/aob.c src/sysinfo.c src/resolve.c src/gate.c src/heartbeat.c src/lockstep.c \
           src/consumer_probe.c src/consumer_math.c
ifneq ($(RELEASE),1)
SRC     += src/toggle_file.c
endif
ASM     := src/probe_thunk.S src/heartbeat_leaf.S src/consumer_thunk.S
MHSRC   := $(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde64.c
OBJ     := $(patsubst src/%.c,$(BDIR)/obj/%.o,$(SRC)) $(BDIR)/obj/probe_thunk.o $(BDIR)/obj/heartbeat_leaf.o $(BDIR)/obj/consumer_thunk.o \
           $(patsubst $(MH)/src/%.c,$(BDIR)/obj/mh/%.o,$(MHSRC))
HDR     := $(wildcard src/*.h)

all: $(BDIR)/dinput8.dll $(BDIR)/nioh3fps.ini

release:
	$(MAKE) RELEASE=1 all

$(BDIR)/obj/%.o: src/%.c $(HDR) $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BDIR)/obj/%.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(BDIR)/obj/mh/%.o: $(MH)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MHFLAGS) -c $< -o $@

$(BDIR)/unstripped/dinput8.dll: $(OBJ) src/dinput8.def
	@mkdir -p $(BDIR)/unstripped
	$(CC) $(LDFLAGS) -o $@ $(OBJ) src/dinput8.def -Wl,-Map=$(BDIR)/dinput8.map -luser32 -lkernel32

$(BDIR)/dinput8.dll: $(BDIR)/unstripped/dinput8.dll
	$(STRIP) -s -o $@ $<
	@echo BUILD_ID=$(BUILD_ID) > $(BDIR)/BUILD_ID.txt

INI_SRC := $(if $(filter 1,$(RELEASE)),src/nioh3fps.release.ini,src/nioh3fps.ini)
$(BDIR)/nioh3fps.ini: $(INI_SRC)
	cp $< $@

# optional diagnostics only; omitted from the default S2L artifact set
build/nioh3fps_probe.csv: $(PROBECSV)
	cp $< $@

build/test_factor.exe: tests/test_factor.c src/factor.c src/factor.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -o $@ tests/test_factor.c src/factor.c

build/test_unwind.exe: tests/test_unwind.c src/stubs.c src/stubs.h src/expected.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -o $@ tests/test_unwind.c src/stubs.c

build/test_thunk.exe: tests/test_thunk.c src/probe.c src/aob.c src/probe_thunk.S src/common.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -I$(MH)/include -o $@ tests/test_thunk.c src/probe.c src/aob.c src/probe_thunk.S

build/test_stubs.exe: tests/test_stubs.c src/stubs.c src/stubs.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -o $@ tests/test_stubs.c src/stubs.c

build/test_trace.exe: tests/test_trace.c src/trace.c src/common.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -municode -I$(MH)/include -DBUILD_ID=\"test\" -o $@ tests/test_trace.c src/trace.c

build/test_probecsv.exe: tests/test_probecsv.c src/probe.c src/aob.c src/probe_thunk.S src/common.h src/expected.h
	$(CC) -O2 -std=gnu11 -Wall -Wextra -municode -I$(MH)/include -o $@ tests/test_probecsv.c src/probe.c src/aob.c src/probe_thunk.S

build/smoke_host.exe: tests/smoke_host.c
	$(CC) $(CFLAGS) -municode -o $@ $<

build/test_lockstep.exe: tests/test_lockstep.c src/hooks.c src/lockstep.c src/gate.c src/factor.c src/stubs.c src/heartbeat.c src/heartbeat_leaf.S $(HDR)
	$(CC) $(CFLAGS) -DLOCKSTEP_TEST -o $@ tests/test_lockstep.c src/hooks.c src/lockstep.c src/gate.c src/factor.c src/stubs.c src/heartbeat.c src/heartbeat_leaf.S

build/test_heartbeat.exe: tests/test_heartbeat.c src/heartbeat.c src/heartbeat_leaf.S src/stubs.c src/heartbeat.h
	$(CC) $(CFLAGS) -o $@ tests/test_heartbeat.c src/heartbeat.c src/heartbeat_leaf.S src/stubs.c

build/test_resolve.exe: tests/test_resolve.c src/resolve.c src/aob.c src/anchors.h src/resolve.h
	$(CC) $(CFLAGS) -o $@ tests/test_resolve.c src/resolve.c src/aob.c

build/test_gate.exe: tests/test_gate.c src/gate.c src/gate.h src/factor.c src/factor.h
	$(CC) $(CFLAGS) -o $@ tests/test_gate.c src/gate.c src/factor.c

build/test_consumers.exe: tests/test_consumers.c tests/test_thunk.c src/consumer_probe.c src/consumer_math.c src/consumer_thunk.S src/probe.c $(HDR) $(patsubst $(MH)/src/%.c,build/obj/mh/%.o,$(MHSRC))
	$(CC) $(CFLAGS) -o $@ tests/test_consumers.c src/consumer_math.c src/consumer_thunk.S src/probe.c src/probe_thunk.S src/aob.c src/resolve.c src/stubs.c $(patsubst $(MH)/src/%.c,build/obj/mh/%.o,$(MHSRC))

TESTS := build/test_factor.exe build/test_unwind.exe build/test_thunk.exe build/test_stubs.exe build/test_trace.exe          build/test_probecsv.exe build/test_resolve.exe build/test_gate.exe build/test_heartbeat.exe build/test_lockstep.exe build/test_consumers.exe build/smoke_host.exe

test: $(TESTS) all
	build/test_consumers.exe "$(EXE)"
	build/test_resolve.exe "$(EXE)"
	build/test_gate.exe
	build/test_lockstep.exe "$(EXE)"
	build/test_heartbeat.exe "$(EXE)"
	build/test_factor.exe > build/test_factor.log && tail -n 2 build/test_factor.log
	build/test_unwind.exe "$(EXE)"
	build/test_thunk.exe
	build/test_stubs.exe
	@mkdir -p build/tracetest
	build/test_trace.exe build/tracetest
	$(PYTHON) -I tools/trace2csv.py build/tracetest/nioh3fps_trace.bin build/tracetest/test_trace_py.csv
	cmp build/tracetest/test_trace_c.csv build/tracetest/test_trace_py.csv && echo "TRACE2CSV: PASS (identical to the DLL converter)"
	$(PYTHON) -I tools/trace2csv.py build/tracetest/legacy.bin build/tracetest/legacy_py.csv
	cmp build/tracetest/legacy_c.csv build/tracetest/legacy_py.csv && echo "TRACE2CSV_LEGACY: PASS (S1 v1 compatibility)"
	build/test_probecsv.exe "$(EXE)" "$(PROBECSV)" > build/test_probecsv.log && tail -n 3 build/test_probecsv.log
	@mkdir -p build/smoketest
	cp build/dinput8.dll build/nioh3fps.ini build/smoketest/
	build/smoke_host.exe build/smoketest 2

imports: build/dinput8.dll
	$(OBJDUMP) -p build/dinput8.dll | grep -E "DLL Name|^\s+\[ *[0-9]+\] +\+base"

expected:
	$(PYTHON) tools/gen/gen_expected.py "$(EXE)" src/expected.h

clean:
	rm -rf build

.PHONY: all release test imports expected clean

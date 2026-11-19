# libtinynv — the C userspace NVIDIA driver (M3). See docs/driver/libtinynv-design.md.
# make        build the library
# make test   run every test that needs no GPU (today: the TinyGPU backend against the python mock server)
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -fPIC -Iinclude -I../include
# Pick the same SDK the rest of the tree picks, even when this makefile is run on its own. The top level passes
# CC="cc -isysroot ..." and that still wins; this is for the direct sub-make, which otherwise takes Apple cc's default.
# It is not a style point: it produced driver test binaries stamped sdk 27.0 while every other binary on the box was
# 26.5, which is a difference nobody intended sitting underneath a measurement, and Session A found it rather than the
# build saying so. The machine has changed three times in a day; a build that chooses for itself is the only kind that
# stays right.
SDK     := $(shell sh $(dir $(lastword $(MAKEFILE_LIST)))../build/sdk.sh 2>/dev/null)
ifneq ($(SDK),)
CFLAGS  += -isysroot $(SDK)
endif
# absolute, from the source tree: a relative firmware path finds the images from one directory and not from another,
# which turns a missing fetch into a test that passes without testing anything
CFLAGS  += -DTINYNV_FW_DEFAULT_DIR=\"$(abspath third_party/firmware)\"
PYTHON  ?= python3
# Whether the tinygrad the oracle steps need is here (env.sh: TINYGRAD_SRC, branch egpu-hcq2-remote), asked once and only
# for `make test`. It probes what those steps import, through the same path they put first: a bare `import tinygrad`
# also succeeds on env.sh's PYTHONPATH tinygrad-stable, which has neither module, and the steps would then die mid-run.
# And, separately, as nv_trace.py imports it - with no path of its own, so only an installed tinygrad will do there.
# Without it those steps say SKIP (no tinygrad) and the rest runs; TINYNV_REQUIRE_TINYGRAD=1 makes its absence, and a
# missing trace, norm.cubin or vendored headers (each skipped by default, as before only with TINYNV_ALLOW_NO_*), fail.
ifneq ($(filter test,$(MAKECMDGOALS)),)
HAVE_TINYGRAD := $(shell $(PYTHON) -c 'import tinygrad.helpers' >/dev/null 2>&1 && $(PYTHON) -c 'import os, sys; sys.path.insert(0, os.environ.get("TINYGRAD_SRC", "$(abspath ../../tinygrad)")); import tinygrad.runtime.autogen.nv_570, test.unit.test_remote_pci' >/dev/null 2>&1 && echo 1)
endif
STRICT  := $(filter 1,$(TINYNV_REQUIRE_TINYGRAD))
# NVIDIA's own headers, fetched by tools/fetch_nv_headers.sh and never committed (see .gitignore)
NVSRC   := third_party/open-gpu-kernel-modules-81fe4fb417c8ac3b9bdcc1d56827d116743892a5
NVINC   := -I$(NVSRC)/src/nvidia/arch/nvalloc/common/inc -I$(NVSRC)/src/common/sdk/nvidia/inc -I$(NVSRC)/src/common/inc \
           -I$(NVSRC)/src/nvidia/inc -I$(NVSRC)/src/common/shared/msgq/inc -I$(NVSRC)/src/common/uproc/os/common/include \
           -I$(NVSRC)/src/nvidia/generated   # rmconfig.h, which gsp_fw_heap.h (the firmware heap formula test_headers pins) includes
BUILD   := build
SKIPS    = $(BUILD)/test-skips.txt   # one line per SKIP of the last `make test`; the top level counts it
# where the library finds the emergency bus-master clear (tools/tinynv_bmclear.c, built beside it): absolute, as the
# firmware directory is, because the process that needs it is somebody else's program in somebody else's directory
CFLAGS  += -DTINYNV_BMCLEAR_DEFAULT=\"$(abspath $(BUILD))/tinynv-bmclear\"
# track header dependencies. without this a change to a struct in a header rebuilds only the .c files that were edited,
# and the rest of the library keeps the old layout: the two halves then disagree about where a field is, which shows up
# as a value that was set a moment ago reading back as zero. cost me a debugging round; never again.
DEPFLAGS := -MMD -MP

# Which commit this build came from, and whether the tree was clean. Baked into one object so only that one rebuilds
# when it changes. Three runs in this project have been diagnosed against code they were not running; a binary that can
# say what it is turns "are you running the fix?" into a question with an answer.
# Scoped to this directory on purpose: the repository holds the shim as well, and a driver that called itself dirty
# because somebody else had an edit open would be reporting the wrong thing.
BUILD_ID := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)$(shell git diff --quiet -- . 2>/dev/null || echo -dirty)
$(BUILD)/src/build_id.o: CFLAGS += -DTINYNV_BUILD_ID='"$(BUILD_ID)"'

# And the object has to be rebuilt every time, which is the only way make can be right about this: nothing in the source
# changes when the commit does, only a -D on the command line, so there is no timestamp for make to compare. The first
# attempt here went through a stamp file rewritten only when the value changed - and make compares the stamp's
# timestamp, which had not changed either, so the version marker went stale within minutes of being written, which is
# precisely the failure it exists to catch. Recompiling one function on every build is not a cost worth being clever
# about.
# `always` is a target with no recipe that is never up to date, so anything depending on it is rebuilt every time.
#
# It must not become the default goal. The first target a makefile defines is what a bare `make` builds, and this rule
# sits above `all` because it belongs with the value it exists for - so the goal is named explicitly below. Without that
# line, `make` silently did nothing at all, which is a worse failure than the stale version marker this was written to
# fix, and it presented the same way: a build that looked fine and changed nothing.
.DEFAULT_GOAL := all
.PHONY: always
always:
$(BUILD)/src/build_id.o: always

UNAME := $(shell uname -s)
SRCS  := src/err.c src/build_id.c src/qmd.c src/image.c src/arena.c src/exec.c src/cubin.c src/sha256.c src/fw.c src/dev.c src/mmu.c src/tlsf.c src/pt.c src/gpu.c src/gsp.c src/flcn.c src/vbios.c src/submit.c \
         src/tensor_unlock.c src/tinynv.c src/pci_replay.c src/pci_tinygpu.c src/lost.c
ifeq ($(UNAME),Linux)
SRCS  += src/pci_sysfs.c
endif
OBJS  := $(SRCS:%.c=$(BUILD)/%.o)

.PHONY: all test clean
# The smi tool is part of a build, not of a test run. It was reachable only through `make test`, which meant the one
# command anybody would run to get it did not produce it and it sat in a directory nobody had reason to look in.
all: $(BUILD)/libtinynv.a $(BUILD)/tinynv-smi $(BUILD)/tinynv-bmclear

$(BUILD)/libtinynv.a: $(OBJS)
	@mkdir -p $(@D)
	ar rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The counts in this test's summary line are grepped out of the test itself at build time. They were literals and had
# gone stale, reporting the same two numbers whatever the file held - so the line that says how much was checked was the
# one thing in the suite not checking anything.
$(BUILD)/tinynv-smi: tools/tinynv_smi.c $(BUILD)/libtinynv.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc -o $@ tools/tinynv_smi.c $(BUILD)/libtinynv.a

# The emergency bus-master clear a client starts when it has lost its TinyGPU server (tools/tinynv_bmclear.c). Built
# with the library, which finds it at this absolute path (TINYNV_BMCLEAR_HELPER overrides it).
$(BUILD)/tinynv-bmclear: tools/tinynv_bmclear.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/test_headers: test/test_headers.c src/nv_structs.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc $(NVINC) \
	  -DTINYNV_HDR_STRUCTS=$$(grep -c '^SAME_SIZE(' test/test_headers.c) \
	  -DTINYNV_HDR_FIELDS=$$(grep -c '^SAME_AT(' test/test_headers.c) \
	  -DTINYNV_HDR_CONSTS=$$(grep -c '^_Static_assert(' test/test_headers.c) $< -o $@

$(BUILD)/test_arena: test/test_arena.c src/arena.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc -o $@ test/test_arena.c src/arena.c

$(BUILD)/test_tensor_unlock_state: test/test_tensor_unlock_state.c src/tensor_unlock_state.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc -o $@ test/test_tensor_unlock_state.c

# The check floors are grepped out of each test at build time, never written as literals: a count stated as a constant
# stops counting the moment the file changes, and stops silently. Same reason test_headers' summary is grepped.
$(BUILD)/test_submit: test/test_submit.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc -DTINYNV_SUBMIT_CHECKS=$$(grep -c 'CHECK(' test/test_submit.c) \
	  $< $(BUILD)/libtinynv.a -o $@

$(BUILD)/test_exec_mode: test/test_exec_mode.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc -DTINYNV_MODE_CHECKS=$$(grep -c 'CHECK(' test/test_exec_mode.c) \
	  $< $(BUILD)/libtinynv.a -o $@

$(BUILD)/test_delta: test/test_delta.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc -DTINYNV_DELTA_CHECKS=$$(grep -c 'CHECK(' test/test_delta.c) \
	  $< $(BUILD)/libtinynv.a -o $@

# The teardown witness compiles src/tinynv.c itself - the functions under test are static by design - against stubs
# it defines, so it links err.o ALONE: the archive would duplicate every symbol the include just compiled.
# lost.o beside it: the lost-gpu latch the teardown consults (src/lost.c), which needs nothing but err.o and the pci vtable.
$(BUILD)/test_teardown_term: test/test_teardown_term.c $(BUILD)/src/err.o $(BUILD)/src/lost.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc test/test_teardown_term.c $(BUILD)/src/err.o $(BUILD)/src/lost.o -o $@

$(BUILD)/test_teardown_skip: test/test_teardown_skip.c $(BUILD)/src/err.o $(BUILD)/src/lost.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc test/test_teardown_skip.c $(BUILD)/src/err.o $(BUILD)/src/lost.o -o $@

$(BUILD)/test_teardown_lost: test/test_teardown_lost.c $(BUILD)/src/err.o $(BUILD)/src/lost.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc test/test_teardown_lost.c $(BUILD)/src/err.o $(BUILD)/src/lost.o -o $@

# The real exec.o against recording stand-ins for the command encoders and submit (defined in the test itself) and
# stops for everything else it could reach (fake_exec_unreachable.c): the archive then supplies exec.o and err.o only.
$(BUILD)/test_upload_order: test/test_upload_order.c test/fake_exec_unreachable.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc test/test_upload_order.c test/fake_exec_unreachable.c $(BUILD)/libtinynv.a -o $@

# TINYNV_CE_ROUTE's ordering, the same way: the real exec.o against recording stand-ins, plus its own allocator and
# ring-state stand-ins for the CE bring-up (FAKE_EXEC_CE_ROUTE drops those three from the unreachable set).
$(BUILD)/test_ce_route: test/test_ce_route.c test/fake_exec_unreachable.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc -DFAKE_EXEC_CE_ROUTE test/test_ce_route.c test/fake_exec_unreachable.c $(BUILD)/libtinynv.a -o $@

# A card probe (test_hw_ce_overlap.c says what it measures and how to run it): built by `make test` so it cannot rot,
# never run by it. It compiles src/tinynv.c into itself, as test_teardown_term does, to reach the booted gpu and exec
# through the runtime's own boot and teardown; the archive's tinynv.o is then never pulled in. The kernel's default
# cubin is baked in as an absolute path because the card runner's cwd is not this directory.
$(BUILD)/test_hw_ce_overlap: test/test_hw_ce_overlap.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc -DTINYNV_CE_OVERLAP_CUBIN=\"$(abspath ../libtinycudart/keepalive.sm_86.cubin)\" \
	  $< $(BUILD)/libtinynv.a -o $@

$(BUILD)/test_%: test/test_%.c $(BUILD)/libtinynv.a
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -Isrc $< $(BUILD)/libtinynv.a -o $@

# the mock server exits on its own when the socket disappears, so a failed test cannot leave one behind
# Images cut from NVIDIA's own libraries (torch's libtorch_cuda, cuBLASLt) are not redistributed; three tests run on them
# when they are here and say SKIPPED when they are not. test/fixtures/README.md has where each comes from and its sha256.
NV_FIXTURES ?= test/fixtures-nvidia
TRACE ?= $(abspath ../../traces)/5090-boot-smoke
# the launch snapshot from the combined recording: the descriptors and command buffers the card was handed
SNAPSHOT ?= $(abspath ../../traces)/5090-launch2-launch.json

# Opening the TinyGPU socket is a card step even when no GPU work follows: the server's listen backlog is one, so a
# stray connection from an offline test takes the live server's only client slot and blocks there. It happened on
# 2026-09-14 - a probe that defaulted to $TMPDIR/tinygpu.sock sat unaccepted for five minutes behind llama-server.
#
# Nothing in this target wants a real card. The tests that need a socket are handed a mock one as an argument, which
# already wins over the environment, so this changes nothing about how they run - it removes the possibility that a
# future test forgets to pass one and silently inherits a live server instead. Structural rather than remembered.
test: export TINYNV_SOCKET :=
# and no emergency bus-master clear (src/pci_tinygpu.c: tg_rescue) from a test that latches a lost server: the arms
# that exercise it turn it on themselves, with the helper, the socket and the app all pointed at stand-ins.
test: export TINYNV_EMERGENCY_BM := 0
# and no watchdog (src/pci_tinygpu.c: tg_watch), which TINYNV_EMERGENCY_BM=0 already implies and this says outright.
# Should one start anyway, the only app it can start does not exist: a watchdog beside a test whose fake server exits
# would otherwise start the installed TinyGPU on that test's socket. test_watchdog turns both on again for itself, with
# its own binary as the app.
test: export TINYNV_WATCHDOG := 0
test: export TINYGPU_APP := /nonexistent/TinyGPU.app/Contents/MacOS/TinyGPU
test: $(BUILD)/test_tensor_unlock_state $(BUILD)/test_arena $(BUILD)/test_witness $(BUILD)/tinynv-smi $(BUILD)/test_image $(BUILD)/test_exec_mode $(BUILD)/test_delta $(BUILD)/test_qmd_cost $(BUILD)/test_pci $(BUILD)/test_tg_dead $(BUILD)/test_tg_sigpipe $(BUILD)/test_lost $(BUILD)/test_bmclear $(BUILD)/tinynv-bmclear $(BUILD)/test_watchdog $(BUILD)/test_teardown_lost $(BUILD)/test_cubin $(BUILD)/test_api $(BUILD)/test_fw $(BUILD)/test_submit $(BUILD)/test_reloc $(BUILD)/test_thread_limit $(BUILD)/test_tlsf_used $(BUILD)/test_events $(BUILD)/test_global $(BUILD)/test_kernel_attrs $(BUILD)/test_elf_abi $(BUILD)/test_module_unload $(BUILD)/test_upload_order $(BUILD)/test_ce_route $(BUILD)/test_teardown_term $(BUILD)/test_teardown_skip $(BUILD)/test_num_barriers $(BUILD)/test_hw_namedbar $(BUILD)/test_hw_ce_overlap $(BUILD)/test_mm $(BUILD)/test_qmd $(BUILD)/test_qmd_hw $(BUILD)/test_hw $(BUILD)/test_hw_kernel $(BUILD)/test_hw_smem $(BUILD)/test_hw_cbank $(BUILD)/test_hw_dims $(BUILD)/test_hw_chain $(BUILD)/test_hw_replay $(BUILD)/test_hw_gap $(BUILD)/test_hw_printf $(BUILD)/test_hw_abort $(BUILD)/test_hw_fault $(BUILD)/test_hw_wpr $(BUILD)/test_hw_fill $(BUILD)/test_dev
	@mkdir -p $(BUILD); : > $(SKIPS)
	@if [ -z "$(HAVE_TINYGRAD)" ] && [ -n "$(STRICT)" ]; then echo "FAIL: TINYNV_REQUIRE_TINYGRAD=1 but $(PYTHON) cannot import tinygrad (tinygrad.helpers, tinygrad.runtime.autogen.nv_570, test.unit.test_remote_pci; TINYGRAD_SRC=$${TINYGRAD_SRC:-$(abspath ../../tinygrad)}; see env.sh)"; exit 1; fi
	@[ -n "$(HAVE_TINYGRAD)" ] || echo "  no tinygrad (TINYGRAD_SRC, see env.sh): the steps that need it say SKIP (no tinygrad) and the rest runs; TINYNV_REQUIRE_TINYGRAD=1 fails instead"
	@# FIRST, before any test runs: do the tests in this directory still report what they find? On 2026-09-15
	@# test_rm_free.c exited 0 with FAIL printed - it consulted its failure count at line 117 of a 318-line main, the
	@# file grew past that guard, and the whole C4b decision plus the rows proving the read-only fix landed below it.
	@# `make test` was green over all of it. Session C's lint derives each file's failure counter FROM THE CODE rather
	@# than from a list of names (a name list was its own blind spot, found here with a two-line negative control), and
	@# fails the build on LOSING - evidence being discarded now. --strict ALSO fails on UNGUARDED-ACCUMULATING: a file
	@# whose assertion macro increments a counter consulted before the end, where the natural way to add a test is to
	@# append a CHECK that then silently does not count. That is the shape that bit us, and it is worth failing on
	@# BEFORE it starts losing anything. UNGUARDED-but-FAIL-FAST never fails: every site there returns 1 itself, so a
	@# new check copied from its neighbours returns too - and failing on harmless findings is how a gate gets disabled,
	@# taking the real ones with it. All five of ours are fail-fast, so --strict costs nothing today and catches the
	@# next accumulating file the day it is written. It runs first because a suite whose verdicts are discarded makes
	@# everything after it unreadable.
	$(PYTHON) tools/exit_status_lint.py --strict
	@# AND whether what is about to run was built from what is in the tree. The make here is GNU Make 3.81 (Apple's
	@# 2006 build), which compares mtimes to the SECOND while APFS records nanoseconds - so a source edited inside the
	@# same second its object was compiled is "not newer" and is NOT rebuilt, silently. That defeats every negative
	@# control, which is the practice the rest of this suite's credibility rests on: a check "seen to fail" and then
	@# "seen to pass" proves nothing if both runs were the same binary. Invisible to a build id, too - build_id.c is
	@# recompiled every time, so the id looks current while every other object is stale.
	@# with the build directory it is asked about: the top-level Makefile builds this driver at cuda-shim/build/shim/nv, and
	@# the tool's default of build/src was a stale tree from an older standalone build (found 2026-09-19)
	@$(PYTHON) tools/check_stale_objects.py $(BUILD)/src src
	@for cb in ../spike/vecadd.sm86.cubin ../spike/vecadd.sm90.cubin ../spike/vecadd.sm120.cubin; do $(BUILD)/test_cubin $$cb || exit 1; done
	@# `test -f $$cb && run || true` swallowed a real FAILURE, not just a missing file: in `A && B || C` the C runs
	@# whenever B fails, so these two cubins could have been failing since the line was written and the suite would
	@# have said nothing. Found by applying Session C's rule - grep for the reassuring construct, not for the
	@# condition - after their own test reported PASSED while testing nothing. A skip is also now announced rather
	@# than silent, because "it did not run" and "it passed" should never look the same.
	@for cb in ../spike/norm.cubin ../spike/mmvq.cubin; do \
	  if [ -f $$cb ]; then $(BUILD)/test_cubin $$cb - || exit 1; \
	  else echo "SKIP (no fixture): test_cubin $$cb — not present (ggml build output), so nothing checked it" | tee -a $(SKIPS); fi; done
	@# a kernel name longer than any in the local corpus. nothing here reaches 256 characters, which is why a fixed
	@# buffer survived until session A's 8012-kernel survey found two at 474.
	@$(PYTHON) tools/make_long_name_cubin.py ../spike/vecadd.sm120.cubin $(BUILD)/longname.cubin >/dev/null
	@$(BUILD)/test_cubin $(BUILD)/longname.cubin -
	@$(BUILD)/test_api ../spike/vecadd.sm86.cubin 0x160 && $(BUILD)/test_api ../spike/vecadd.sm120.cubin 0x380
	@$(BUILD)/test_fw
	@# which submission path an unset environment gets, and how the two knobs resolve. costs nothing and is the only
	@# place that decision is visible without a card.
	@# recycling the scratch region a piece at a time, against a byte-by-byte record of who wrote what. the failure
	@# it exists for - bytes handed back while an engine still reads them - produces a wrong answer and no fault, so
	@# there is nothing on the card that would report it.
	@$(BUILD)/test_tensor_unlock_state
	@$(BUILD)/test_arena
	@# the crash witness, which only ever runs during a crash on a live card - so a bug in it costs a whole run to
	@# find. checked here against the containment question its file exists to answer, in both directions.
	@$(BUILD)/test_witness
	@$(BUILD)/test_exec_mode
	@# which dwords of a launch's descriptor span TINYNV_DELTA_DELIVERY patches and which it trusts are already there.
	@# a run a dword short is a stale kernel parameter and nothing on the card reports it, so the covering property
	@# and the gap arithmetic are pinned here, off the card.
	@$(BUILD)/test_delta
	@# what a launch costs the processor, with no gpu. sizes any change that removes host work per launch.
	@$(BUILD)/test_qmd_cost 20000
	@# the memory manager on its own: awkward sizes, freed and reallocated, which the recorded boot never does
	@$(BUILD)/test_mm
	@# and a sequence a real caller issued - ggml's k=129 f16 matmul cases, which is where the mapping check went wrong
	@$(BUILD)/test_mm test/allocseq-k129.txt
	@# the transport after its server is gone (2026-11-18 DART panic, M1): the loss latches, every later call fails without
	@# a byte on the socket, it is said once, and the write-pointer check names the server rather than the device. No mock
	@# server: the test plays the server on a socket of its own, so it needs neither python nor tinygrad, and runs
	@# ahead of the tests that do.
	@$(BUILD)/test_tg_dead
	@# ...and a server KILLED under a program that leaves SIGPIPE at its default (step B, 2026-11-19): the send to the
	@# closed peer fails and latches; it does not kill the process before the loss can be handled.
	@$(BUILD)/test_tg_sigpipe
	@# ...and what the driver does about it (M2, M7): the first impossible readback is classified (server gone, card off
	@# the bus, BAR window lost) and latched; nothing more is staged or rung; a long wait finds the loss within half a
	@# second instead of at its 30 s timeout, while a short one costs no extra round trip; a lost gpu's teardown drains
	@# nothing and unloads nothing, and with the connection alive clears bus mastering before the socket closes.
	@$(BUILD)/test_lost
	@$(BUILD)/test_teardown_lost
	@# the emergency bus-master clear (M3) against a server this test plays: read, write without MASTER, read back;
	@# nothing written to a device that reads all ones or through a server that does not answer; the hard timeout holds.
	@# Every path it could take to the real server or app is overridden, and it is told the helper by absolute path.
	@$(BUILD)/test_bmclear $(abspath $(BUILD))/tinynv-bmclear
	@# the watchdog (M4) with real processes and SIGKILL, against a fake server it plays and counts the starts of: a
	@# killed client or a killed server gets bus mastering cleared once, a clean exit costs the server no connection, two
	@# clears at once start one app. Its stand-in app is the test binary itself, never the installed one.
	@if [ "$$(uname)" = Darwin ]; then $(BUILD)/test_watchdog $(abspath $(BUILD))/tinynv-bmclear; \
	 else echo "SKIP (not macOS): test_watchdog - the watchdog watches pids with kqueue and is spawned on macOS only" | tee -a $(SKIPS); fi
	@if [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/nv_trace.py --selftest; else echo "SKIP (no tinygrad): nv_trace.py --selftest — the boot-trace recorder's coalescing self-check" | tee -a $(SKIPS); fi
	@# what a cubin references and does not contain, both halves: the device printf resolves, anything else is refused.
	@# the cubin is build output and not in the repository, so it is made if it is missing rather than assumed.
	@if [ ! -f ../spike/printf_kernel.sm120.cubin ] && [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/build_spike_cubin.py ../spike/printf_kernel.cu \
	   || { echo "  cannot build the cubin test_reloc needs (a compiler server must be reachable; see env.sh)."; \
	        echo "  set TINYNV_ALLOW_NO_CUBINS=1 to skip this test deliberately."; [ -n "$$TINYNV_ALLOW_NO_CUBINS" ]; }; fi
	@if [ -f ../spike/printf_kernel.sm120.cubin ]; then $(BUILD)/test_reloc ../spike/printf_kernel.sm120.cubin; \
	 else echo "SKIP (no fixture): test_reloc on printf_kernel.sm120.cubin — not built (the top-level make test builds it with build/spike-cubin.sh)" | tee -a $(SKIPS); fi
	@# ...and the device assert handler, on a real torch sm_120 image that references BOTH allowed names. NVIDIA-built,
	@# so not shipped: put it in $(NV_FIXTURES) (test/fixtures/README.md says where it comes from) or this arm says SKIPPED.
	@if [ -f $(NV_FIXTURES)/torch-cu128-assertfail-vprintf.sm120.cubin ]; then $(BUILD)/test_reloc $(NV_FIXTURES)/torch-cu128-assertfail-vprintf.sm120.cubin; \
	 else echo "SKIP (no fixture): test_reloc on torch's __assertfail/vprintf image — NVIDIA-built fixture not shipped; see test/fixtures/README.md (NV_FIXTURES=$(NV_FIXTURES))" | tee -a $(SKIPS); fi
	@# the most threads a block can have: __launch_bounds__ capped by registers (NVIDIA's rule), on a torch image whose
	@# kernels declare no bound - reported by kernel_info, and one warp over it refused before any card is involved
	@if [ -f $(NV_FIXTURES)/torch-cu128-unbounded-regs.sm120.cubin ]; then $(BUILD)/test_thread_limit $(NV_FIXTURES)/torch-cu128-unbounded-regs.sm120.cubin; \
	 else echo "SKIP (no fixture): test_thread_limit on torch's unbounded-register image — NVIDIA-built fixture not shipped; see test/fixtures/README.md (NV_FIXTURES=$(NV_FIXTURES))" | tee -a $(SKIPS); fi
	@# cuFuncGetAttribute values from the image, against what NVIDIA's driver reported for the same cubins
	@$(BUILD)/test_kernel_attrs test/fixtures
	@# the architecture from the right byte of e_flags for ELF ABI 7 and 8 (one PTX through ptxas 12.8 and 13.0)
	@$(BUILD)/test_elf_abi test/fixtures
	@# a module whose device did not drain is refused and kept, not freed
	@$(BUILD)/test_module_unload test/fixtures
	@# an upload never lands before a launch that precedes it (H4): the real exec.c, no card
	@$(BUILD)/test_upload_order
	@# TINYNV_CE_ROUTE: what each batch acquires, the flush before a routed copy, no acquire on an unsubmitted value,
	@# and the fallback when the copy-engine bring-up fails
	@$(BUILD)/test_ce_route
	@# a hung exit-path drain must die of ONE SIGTERM (the 2026-09-25 field shape), with the handler shown to return;
	@# then the same witness re-armed with the PRE-FIX ordering must reproduce the wedge, or it proves nothing
	@$(BUILD)/test_teardown_term
	@TEARDOWN_MUTANT=1 $(BUILD)/test_teardown_term
	@# TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL=1: a failed drain skips gpu_close/exec_fini; unset/0 and a good drain unload as before
	@$(BUILD)/test_teardown_skip
	@# BARRIER_COUNT from EIATTR_NUM_BARRIERS, not a constant 1 (the C8 FP8 fault); 0 declared still grants 1.
	@# test_hw_namedbar is its card-side arm: BUILT above so it cannot rot, never run by this offline suite.
	@# (test_hw_ce_overlap likewise: the TINYNV_CE_GROUP overlap probe, built above, run only on a card.)
	@$(BUILD)/test_num_barriers test/fixtures/namedbar.sm120a.cubin
	@# the allocator's count of what it handed out (what tinynv_mem_info reports), checked against its own block list
	@$(BUILD)/test_tlsf_used
	@# event query/destroy/elapsed and mem_info through the public API on the null device
	@$(BUILD)/test_events
	@# a module's data object by name (cuModuleGetGlobal) on the cuBLAS image cublasCreate asks it of
	@if [ -f $(NV_FIXTURES)/cublaslt-cu128-deviceconstants.sm120.cubin ]; then $(BUILD)/test_global $(NV_FIXTURES)/cublaslt-cu128-deviceconstants.sm120.cubin; \
	 else echo "SKIP (no fixture): test_global on cuBLASLt's device-constants image — NVIDIA-built fixture not shipped; see test/fixtures/README.md (NV_FIXTURES=$(NV_FIXTURES))" | tee -a $(SKIPS); fi
	@# the command buffers the card was handed, rebuilt from the oracle's own method numbers and compared word for word
	@# without tinygrad the reference is not regenerated and the C test reads the committed one
	@if [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/nv_reference_submit.py test/reference-submit.txt >/dev/null || exit 1; \
	 else echo "SKIP (no tinygrad): nv_reference_submit.py — test/reference-submit.txt not regenerated from the oracle; test_submit reads the committed copy" | tee -a $(SKIPS); fi
	@$(BUILD)/test_submit test/reference-submit.txt
	@# and the kernel descriptor, against what tinygrad's own QMD class builds for the same cubin. the C side reads the
	@# cubin itself rather than being handed the numbers, so a divergence in the cubin reader is caught here too.
	@if [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/nv_reference_qmd.py ../spike/vecadd.sm120.cubin test/reference-qmd.txt >/dev/null || exit 1; \
	 else echo "SKIP (no tinygrad): nv_reference_qmd.py — test/reference-qmd.txt not regenerated from tinygrad's QMD; test_qmd reads the committed copy" | tee -a $(SKIPS); fi
	@$(BUILD)/test_qmd test/reference-qmd.txt ../spike/vecadd.sm120.cubin
	@# the loadable image: the stage between reading a cubin and launching from it, and the one where a mistake stays
	@# silent. A constant bank placed at the wrong offset loads fine, launches fine, and reads a table that is not there,
	@# so nothing downstream reports it - which is exactly why this has to run rather than merely exist. The corpus is a
	@# real ggml cubin (49 relocations, 4 kernels) and not the vector add, because the relocated __device__ tables every
	@# IQ-quantised kernel depends on are the thing at risk. norm.cubin is build output, so it is a guarded skip.
	@if [ -f ../spike/norm.cubin ]; then \
	   if [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/nv_reference_image.py ../spike/norm.cubin test/reference-image.txt >/dev/null || exit 1; \
	   else echo "SKIP (no tinygrad): nv_reference_image.py — test/reference-image.txt not regenerated from tinygrad's elf_loader; test_image reads the committed copy" | tee -a $(SKIPS); fi; \
	   $(BUILD)/test_image test/reference-image.txt ../spike/norm.cubin; \
	 elif [ -n "$$TINYNV_ALLOW_NO_CUBINS" ] || [ -z "$(STRICT)" ]; then echo "SKIP (no fixture): test_image — no ../spike/norm.cubin (ggml build output), so the image loader is not checked against the oracle" | tee -a $(SKIPS); \
	 else echo "  no ../spike/norm.cubin: the image loader would not be checked against the oracle at all. Build it, or set TINYNV_ALLOW_NO_CUBINS=1 to skip it deliberately."; exit 1; fi
	@# and the same two things against bytes a 5090 actually took, from the recorded session of 2026-09-14. the reference
	@# is committed because the recording is not something to repeat casually; it is regenerated when the snapshot is there.
	@if [ -f $(SNAPSHOT) ] && [ -n "$(HAVE_TINYGRAD)" ]; then $(PYTHON) tools/nv_reference_hw.py $(SNAPSHOT) test/reference-hw.txt >/dev/null; \
	 elif [ -f $(SNAPSHOT) ]; then echo "SKIP (no tinygrad): nv_reference_hw.py — test/reference-hw.txt not regenerated from the snapshot; test_qmd_hw reads the committed copy" | tee -a $(SKIPS); fi
	@$(BUILD)/test_qmd_hw test/reference-hw.txt
	@# and the same against descriptors a 3060 was handed, which is a DIFFERENT descriptor generation: 256 bytes of QMD
	@# v3 where the above is 384 of v5, with the grid under another name, the driver's parameters at the start of
	@# constant bank 0 instead of dword 188, and two fields Ampere has that Blackwell does not. Both are checked every
	@# run, because the whole point of one driver for two cards is that neither is allowed to drift.
	@$(BUILD)/test_qmd_hw test/reference-hw-sm86.txt
	@# both modes: faithful reproduces the recording operation for operation with nothing forgiven, which is what the
	@# first hardware runs use; fast skips reads whose values are proven held, which is what everything else uses.
	@# TINYNV_START_RESET=0 on both: the recording began at a card with no resident firmware, which the old rule did not
	@# reset, and every open now resets (dev.c, B 2026-10-14) - a policy above the recorded boot, not part of it. The third
	@# run is the other half: WITHOUT the knob the replay must diverge exactly where the reset begins (warm_reset's
	@# config-space snapshot, which the recording never made) - which is what says the reset is in the build at all.
	@if [ -f $(TRACE).trace ]; then TINYNV_START_RESET=0 TINYNV_FAITHFUL=1 $(BUILD)/test_dev $(TRACE) && TINYNV_START_RESET=0 $(BUILD)/test_dev $(TRACE) && \
	   { TINYNV_FAITHFUL=1 $(BUILD)/test_dev $(TRACE) > $(BUILD)/start-reset.out 2>&1; \
	     if grep -q 'first divergence: line 4: the recording asks bar:-1 at 0x4 (2 bytes) and we never did' $(BUILD)/start-reset.out && grep -q 'card reset anyway' $(BUILD)/start-reset.out; then echo "start reset: in effect (the replay diverges where the reset it adds begins), good"; \
	     else echo "  FAIL: the default open did not diverge at the start reset - is the reset-on-every-open in this build?"; grep 'divergence' $(BUILD)/start-reset.out | head -3; exit 1; fi; }; \
	   { TINYNV_UNLOAD=1 TINYNV_FAITHFUL=1 $(BUILD)/test_dev $(TRACE) > $(BUILD)/start-reset-unload.out 2>&1; \
	     if grep -q 'replay (faithful): [0-9]* of [0-9]* recorded operations reproduced, 0 tolerated, 0 divergences' $(BUILD)/start-reset-unload.out && grep -q 'opens cold without a reset' $(BUILD)/start-reset-unload.out; then echo "start reset: TINYNV_UNLOAD=1 opens cold, the recorded boot exactly (no reset on a board whose reset-on-open does not come back), good"; \
	     else echo "  FAIL: under TINYNV_UNLOAD=1 a genuine-zero open still reset the card (or the replay diverged)"; grep -E 'divergence|reset' $(BUILD)/start-reset-unload.out | head -3; exit 1; fi; }; \
	 elif [ -n "$$TINYNV_ALLOW_NO_TRACE" ] || [ -z "$(STRICT)" ]; then echo "SKIP (no fixture): test_dev boot replay — no trace at $(TRACE), so the recorded boot (and the start-reset checks) do not run" | tee -a $(SKIPS); \
	 else echo "  no trace at $(TRACE): the whole boot replay would not run. Record one with tools/nv_trace.py, or set TINYNV_ALLOW_NO_TRACE=1 to skip it deliberately."; exit 1; fi
	@# and that no control this driver sends carries a pointer, at any nesting depth. A pointer in an rm control's
	@# parameters is an address in THIS process; the card reads whatever number is there and reports nothing. Session C
	@# found their own scan for these had two blind spots - nested structs, and raw pointers that are not spelled NvP64 -
	@# so this looks for both, and reads the control list out of nv_structs.h so it cannot drift from what is sent.
	@if [ -d $(NVSRC) ]; then $(PYTHON) tools/check_ctrl_pointers.py $(NVSRC)/src/common/sdk/nvidia/inc src/nv_structs.h; \
	 else echo "SKIP (no fixture): check_ctrl_pointers.py — no vendored headers (tools/fetch_nv_headers.sh)" | tee -a $(SKIPS); fi
	@if [ -d $(NVSRC) ]; then $(MAKE) -s $(BUILD)/test_headers && $(BUILD)/test_headers; \
	 elif [ -n "$$TINYNV_ALLOW_NO_HEADERS" ] || [ -z "$(STRICT)" ]; then echo "SKIP (no fixture): test_headers — no vendored headers, so the struct cross-checks do not run (tools/fetch_nv_headers.sh)" | tee -a $(SKIPS); \
	 else echo "  no vendored headers: the struct cross-checks would not run. Run tools/fetch_nv_headers.sh, or set TINYNV_ALLOW_NO_HEADERS=1 to skip them deliberately."; exit 1; fi
	@# the way out of the driver, on a server of its own so nothing before it has already cleared bus mastering: the mock
	@# hands over a device with bus mastering on, as the dext does, and the driver has to give it back with that off.
	@# the mock server is tinygrad's own (test/unit/test_remote_pci.py), so all three of these need it
	@if [ -n "$(HAVE_TINYGRAD)" ]; then \
	sock=$$(mktemp -d)/tinygpu.sock; \
	$(PYTHON) test/mock_server.py $$sock 60 & srv=$$!; \
	for i in $$(seq 100); do [ -S $$sock ] && break; sleep 0.05; done; \
	TINYNV_HW_MOCK=1 $(BUILD)/test_hw $$sock; rc=$$?; \
	rm -f $$sock; wait $$srv 2>/dev/null; [ $$rc = 0 ] || exit $$rc; \
	sock=$$(mktemp -d)/tinygpu.sock; \
	$(PYTHON) test/mock_server.py $$sock 60 & srv=$$!; \
	for i in $$(seq 100); do [ -S $$sock ] && break; sleep 0.05; done; \
	$(BUILD)/test_pci $$sock; rc=$$?; \
	rm -f $$sock; wait $$srv 2>/dev/null; [ $$rc = 0 ] || exit $$rc; \
	$(PYTHON) test/test_wire_proxy.py $(BUILD)/test_pci; \
	else echo "SKIP (no tinygrad): test_hw on the mock server — the driver's exit path giving bus mastering back" | tee -a $(SKIPS); \
	echo "SKIP (no tinygrad): test_pci on the mock server — the TinyGPU wire client against the python driver's own server" | tee -a $(SKIPS); \
	echo "SKIP (no tinygrad): test_wire_proxy.py — the wire-level recorder (it drives the same mock server)" | tee -a $(SKIPS); fi
	@echo "libtinynv: $$(grep -c '^SKIP' $(SKIPS) | tr -d ' ') SKIP(s), each listed above"

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

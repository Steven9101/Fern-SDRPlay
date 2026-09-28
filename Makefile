# Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#
#   make                  build/fern-sdrplay with this machine's compiler
#   make test             ABI, unit, protocol, command line and package tests
#   make test-asan        the unit and protocol tests under ASan and UBSan
#   make abi-check        compare src/rsp_api.h with SDRplay's installed headers
#   make package          dist/sdrplay-VERSION-linux-x86_64.fernmod, built in a
#                         Debian 10 container (glibc 2.28) so that it runs on
#                         every distribution from 2019 on; needs docker
#   make package NATIVE=1 the same with this machine's compiler and glibc
#
# The module loads SDRplay's libsdrplay_api at run time and never links it;
# nothing here needs SDRplay's files except abi-check, which reads their
# headers where SDRplay's installer put them.

VERSION := 0.1.0
.DEFAULT_GOAL := all

ARCH := $(shell uname -m)
ifneq ($(ARCH),x86_64)
$(error this release builds for x86_64 only; aarch64 and armhf are not done yet)
endif

OPT ?= -O2
CXXFLAGS_BASE := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	-DFERN_SDRPLAY_VERSION='"$(VERSION)"' -Isrc
LDLIBS := -pthread -ldl
SDRPLAY_INCLUDE ?= /usr/local/include

MODULE_SRCS := src/json.cpp src/io.cpp src/log.cpp src/api.cpp src/models.cpp src/settings.cpp src/stream.cpp \
	src/watchdog.cpp src/presence.cpp src/receiver.cpp src/session.cpp src/listing.cpp src/gain_control.cpp
PROGRAM_SRCS := src/main.cpp
TEST_SRCS := tests/test_main.cpp tests/fake_control.cpp tests/test_json.cpp tests/test_models.cpp \
	tests/test_settings.cpp tests/test_stream.cpp tests/test_gain_control.cpp tests/test_api.cpp \
	tests/test_receiver.cpp tests/test_session.cpp tests/test_listing.cpp

NATIVE_DIR := build/native
TEST_DIR := build/test
ASAN_DIR := build/asan
BASELINE_DIR := build/baseline
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

# Compile rules for one build directory: $(1) directory, $(2) extra flags.
define compile_rules
$(1)/obj/%.o: %.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS_BASE) $(2) -MMD -MP -c $$< -o $$@
endef

$(eval $(call compile_rules,$(NATIVE_DIR),))
$(eval $(call compile_rules,$(TEST_DIR),-DFERN_SDRPLAY_TESTING -DFAKE_LIBRARY='"$(CURDIR)/$(TEST_DIR)/libfake_sdrplay_api.so"'))
$(eval $(call compile_rules,$(ASAN_DIR),$(SANITIZE) -DFERN_SDRPLAY_TESTING -DFAKE_LIBRARY='"$(CURDIR)/$(ASAN_DIR)/libfake_sdrplay_api.so"'))
$(eval $(call compile_rules,$(BASELINE_DIR),))

objs = $(patsubst %.cpp,$(1)/obj/%.o,$(2))

.PHONY: all test test-asan abi-check package baseline-inside print-version clean

all: build/fern-sdrplay

build/fern-sdrplay: $(call objs,$(NATIVE_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS))
	$(CXX) -Wl,--as-needed -Wl,-z,relro,-z,now -o $@ $^ $(LDLIBS)

# The program as the command line tests run it: it takes the fake library
# from FERN_SDRPLAY_LIBRARY, which the release build ignores.
$(TEST_DIR)/fern-sdrplay: $(call objs,$(TEST_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS))
	$(CXX) -o $@ $^ $(LDLIBS)

$(TEST_DIR)/fern-sdrplay-tests: $(call objs,$(TEST_DIR),$(MODULE_SRCS) $(TEST_SRCS))
	$(CXX) -o $@ $^ $(LDLIBS)

$(ASAN_DIR)/fern-sdrplay-tests: $(call objs,$(ASAN_DIR),$(MODULE_SRCS) $(TEST_SRCS))
	$(CXX) $(SANITIZE) -o $@ $^ $(LDLIBS)

FAKE_FLAGS := -std=c++17 -O1 -g -fPIC -shared -pthread -fvisibility=hidden -Wall -Wextra -Isrc -Itests

$(TEST_DIR)/libfake_sdrplay_api.so: tests/fake_sdrplay.cpp tests/fake_sdrplay.h src/rsp_api.h
	@mkdir -p $(@D)
	$(CXX) $(FAKE_FLAGS) -o $@ $<

$(ASAN_DIR)/libfake_sdrplay_api.so: tests/fake_sdrplay.cpp tests/fake_sdrplay.h src/rsp_api.h
	@mkdir -p $(@D)
	$(CXX) $(FAKE_FLAGS) $(SANITIZE) -o $@ $<

abi-check:
	@if [ -f $(SDRPLAY_INCLUDE)/sdrplay_api.h ]; then \
		$(CXX) -std=c++17 -Wall -Wextra -Isrc -I$(SDRPLAY_INCLUDE) -fsyntax-only tests/abi_check.cpp && \
		echo "abi-check: src/rsp_api.h matches $(SDRPLAY_INCLUDE)/sdrplay_api.h"; \
	else \
		echo "abi-check: skipped, SDRplay's headers are not installed in $(SDRPLAY_INCLUDE)"; \
	fi

test: abi-check $(TEST_DIR)/fern-sdrplay-tests $(TEST_DIR)/fern-sdrplay $(TEST_DIR)/libfake_sdrplay_api.so build/fern-sdrplay
	$(TEST_DIR)/fern-sdrplay-tests
	sh tests/cli_test.sh build/fern-sdrplay $(TEST_DIR)/fern-sdrplay $(TEST_DIR)/libfake_sdrplay_api.so
	sh tests/package_test.sh build/fern-sdrplay

test-asan: $(ASAN_DIR)/fern-sdrplay-tests $(ASAN_DIR)/libfake_sdrplay_api.so
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 $(ASAN_DIR)/fern-sdrplay-tests

PACKAGE := dist/sdrplay-$(VERSION)-linux-x86_64.fernmod
BASELINE_IMAGE := fern-sdrplay-baseline:1

ifeq ($(NATIVE),1)
PACKAGE_BIN := $(BASELINE_DIR)/native/fern-sdrplay
$(PACKAGE_BIN): build/fern-sdrplay
	@mkdir -p $(@D)
	strip -o $@ $<
else
PACKAGE_BIN := $(BASELINE_DIR)/fern-sdrplay
$(PACKAGE_BIN): $(MODULE_SRCS) $(PROGRAM_SRCS) $(wildcard src/*.h) tools/baseline.Dockerfile
	docker image inspect $(BASELINE_IMAGE) >/dev/null 2>&1 || \
		docker build -t $(BASELINE_IMAGE) -f tools/baseline.Dockerfile tools
	docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR)":/src -w /src $(BASELINE_IMAGE) make baseline-inside
endif

# Run inside the container: GCC 8 against glibc 2.28.
baseline-inside: $(call objs,$(BASELINE_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS))
	$(CXX) -Wl,--as-needed -Wl,-z,relro,-z,now -s -o $(BASELINE_DIR)/fern-sdrplay $^ $(LDLIBS)

package: $(PACKAGE)

$(PACKAGE): $(PACKAGE_BIN) build/fern-sdrplay tools/mkfernmod.py tools/check_fernmod.py
	@mkdir -p dist
	python3 tools/mkfernmod.py --executable $(PACKAGE_BIN) --describe-with build/fern-sdrplay \
		--platform linux-x86_64 --version $(VERSION) --output $@
	python3 tools/check_fernmod.py $@
	python3 tools/glibc_floor.py $(if $(filter 1,$(NATIVE)),,--max-glibc 2.28) $(PACKAGE_BIN)

print-version:
	@echo $(VERSION)

clean:
	rm -rf build dist

-include $(shell find build -name '*.d' 2>/dev/null)

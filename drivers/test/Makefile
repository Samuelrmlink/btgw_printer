# Native tests for the printer modules (gcc -m32, see fakehost/fakehost.h).
#   make -C drivers/test            build and run every test
#   make -C drivers/test T=pm220    one test
# Objects go to $(BUILD) (default build/test at the repo root).
ROOT    := $(abspath ../..)
PARENT  := $(ROOT)/btgw
BUILD   ?= $(ROOT)/build/test
GEN     := $(BUILD)/gen
OLD_PM220 ?= $(HOME)/Projects/pm220-pico2w
.DEFAULT_GOAL := all
CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS  := -m32 -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter $(SAN) \
           -I$(CURDIR)/fakehost/include -I$(CURDIR)/fakehost -I$(GEN) \
           -I$(PARENT)/pex/sdk/include -I$(ROOT)/drivers/include -I$(ROOT)/drivers \
           -I$(PARENT)/src -I$(CURDIR)
LDFLAGS := -m32 $(SAN)

FAKE_SRC := fakehost/fakehost.c fakehost/fh_classic.c fakehost/fh_ble.c
FAKE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(FAKE_SRC)) $(BUILD)/parent/cl_sess.o $(BUILD)/parent/pex_json.o

$(GEN)/pex_api.h: $(PARENT)/pex/host_api.json $(PARENT)/pex/tools/gen_host_api.py
	@mkdir -p $(GEN)
	python3 $(PARENT)/pex/tools/gen_host_api.py header $< $@

$(BUILD)/%.o: %.c $(GEN)/pex_api.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/parent/cl_sess.o: $(PARENT)/src/bt/classic/cl_sess.c $(GEN)/pex_api.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/parent/pex_json.o: $(PARENT)/pex/runtime/groups/pex_json.c $(GEN)/pex_api.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(PARENT)/pex/runtime -c $< -o $@

# A module's sources compiled with its entry points renamed, so several
# modules link into one binary: $(call module,name,sources)
define module
$(1)_OBJ := $$(patsubst $(ROOT)/%.c,$(BUILD)/mod/$(1)/%.o,$(2))
$(BUILD)/mod/$(1)/%.o: $(ROOT)/%.c $(GEN)/pex_api.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -Dpex_main=$(1)__pex_main -Dprn_drv=$(1)__prn_drv -Dprn_hub=$(1)__prn_hub -c $$< -o $$@
endef

-include modules.mk

TESTS ?= $(patsubst test_%.c,%,$(wildcard test_*.c))
T ?= $(TESTS)

all: $(addprefix run-,$(T))

$(BUILD)/test_%: $(BUILD)/test_%.o $(FAKE_OBJ) $(LIB_OBJ) $(MODS_OBJ) $(EXTRA_OBJ_$*)
	$(CC) $(LDFLAGS) $^ -o $@

run-%: $(BUILD)/test_%
	@echo "== $*"; $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean
.SECONDARY:

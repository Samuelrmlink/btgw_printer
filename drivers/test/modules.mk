# Shared, stateless code: linked once.
LIB_SRC := $(ROOT)/drivers/core/prn_core.c $(ROOT)/drivers/proto/tspl/tspl.c ref/layout.c
LIB_OBJ := $(patsubst $(ROOT)/%.c,$(BUILD)/lib/%.o,$(filter $(ROOT)/%,$(LIB_SRC))) \
           $(patsubst %.c,$(BUILD)/%.o,$(filter-out $(ROOT)/%,$(LIB_SRC)))
$(BUILD)/lib/%.o: $(ROOT)/%.c $(GEN)/pex_api.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Modules, each with its own entry point names.
$(eval $(call module,hub,$(ROOT)/drivers/hub/printer_hub.c))
$(eval $(call module,virt,$(ROOT)/drivers/virtual/virtual.c))
$(eval $(call module,pm220,$(ROOT)/drivers/vendors/nelko/pm220/pm220.c $(ROOT)/drivers/vendors/nelko/pm220/pm220_proto.c))
MODS_OBJ := $(hub_OBJ) $(virt_OBJ) $(pm220_OBJ)

# The old PM220 firmware's TSPL and layout code, renamed so it links next to ours.
OLD_RENAME := -Dtspl_build_job=old_tspl_build_job -Dtspl_mm_to_dots=old_tspl_mm_to_dots \
  -Djob_media_json=old_job_media_json -Djob_compute_layout=old_job_compute_layout \
  -Djob_from_bitmap=old_job_from_bitmap -Djob_test_frame=old_job_test_frame
OLD_OBJ := $(BUILD)/old/tspl.o $(BUILD)/old/job.o
$(BUILD)/old/%.o: $(OLD_PM220)/src/printer/%.c
	@mkdir -p $(dir $@)
	$(CC) -m32 -std=gnu11 -O1 -g -w $(SAN) -I$(OLD_PM220)/src -I$(OLD_PM220)/src/printer $(OLD_RENAME) -c $< -o $@
EXTRA_OBJ_tspl := $(OLD_OBJ)

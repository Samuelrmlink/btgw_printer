
# Nelko P21 and Phomemo D30 (feature/p21-d30)
$(eval $(call module,p21,$(ROOT)/drivers/vendors/nelko/p21/p21_app.c $(ROOT)/drivers/vendors/nelko/p21/p21_proto.c))
$(eval $(call module,d30,$(ROOT)/drivers/vendors/phomemo/d30/d30_app.c $(ROOT)/drivers/vendors/phomemo/d30/d30_proto.c))
MODS_OBJ += $(p21_OBJ) $(d30_OBJ)
CFLAGS += -DGOLDEN_DIR='"$(ROOT)/drivers/vendors/nelko/p21/test"'
# header dependencies (an included .inc.c change must rebuild its module)
CFLAGS += -MMD -MP
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

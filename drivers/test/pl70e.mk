# Nelko PL70e-BT driver test (test_pl70e.c). The module objects are an extra
# prerequisite of this one test binary, so they land in its $^.
$(eval $(call module,pl70e,$(ROOT)/drivers/vendors/nelko/pl70e/pl70e.c $(ROOT)/drivers/vendors/nelko/pl70e/pl70e_proto.c))
$(BUILD)/test_pl70e: $(pl70e_OBJ)

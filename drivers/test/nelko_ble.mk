# Nelko TSPL over BLE (nelko_ble, the P21) for test_p21.c and test_caps.c.
# nelko_proto.c is already in nelko_tspl's objects (MODS_OBJ), so only the
# driver itself is compiled here.
$(eval $(call module,nble,$(ROOT)/drivers/vendors/nelko/ble/nelko_ble.c))
$(BUILD)/test_p21: $(nble_OBJ)
$(BUILD)/test_caps: $(nble_OBJ)

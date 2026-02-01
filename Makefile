obj-m += hackberrypi-max17048.o

DT_NAME := hackberrypicm5

MODULE_NAME := hackberrypi-max17048.ko

CONFIG_TXT := /boot/firmware/config.txt

OVERLAY_DIR := /boot/firmware/overlays

MODULE_INSTALL_DIR := /lib/modules/$(shell uname -r)/kernel/drivers/power/supply

modules:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	dtc -I dts -O dtb -o $(DT_NAME).dtbo $(DT_NAME).dts

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -rf *.dtbo

install: remove
	install -m 644 -D $(MODULE_NAME) $(MODULE_INSTALL_DIR)
	install -m 644 -D $(DT_NAME).dtbo $(OVERLAY_DIR)
	depmod -a
	sed -i "/dtoverlay=vc4-kms-dpi-hyperpixel4sq/d" $(CONFIG_TXT)
	echo "dtoverlay=$(DT_NAME)" >> $(CONFIG_TXT)
	echo "Please reboot to apply changes"

remove:
	rm -rf $(MODULE_INSTALL_DIR)/$(MODULE_NAME)
	rm -rf $(OVERLAY_DIR)/$(DT_NAME).dtbo
	sed -i "/dtoverlay=$(DT_NAME)/d" $(CONFIG_TXT)
echo "dtoverlay=vc4-kms-dpi-hyperpixel4sq" >> $(CONFIG_TXT)
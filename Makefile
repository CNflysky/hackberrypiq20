obj-m += hackberrypi-max17048.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

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

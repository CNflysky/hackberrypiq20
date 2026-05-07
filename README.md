# hackberrypiq20

Battery driver and panel fix for **HackberryPi Q20** using **Raspberry Pi CM5**.

This repository installs a **MAX17048 battery fuel-gauge kernel driver** using **DKMS** and installs the required **device-tree overlay** for HackberryPi Q20 hardware.

---

## Requirements

Ubuntu or Debian-based Raspberry Pi CM5 image using:

- `/boot/firmware/config.txt`
- `/boot/firmware/overlays`

You **must** have kernel headers installed for your running kernel.

### Install dependencies

```bash
sudo apt update
sudo apt install -y \
  dkms \
  build-essential \
  device-tree-compiler \
  rsync
```

### Install kernel headers (choose one)

**Raspberry Pi OS (recommended):**
```bash
sudo apt install -y linux-headers-rpi-2712
```

**Ubuntu / generic Debian kernels:**
```bash
sudo apt install -y linux-headers-$(uname -r)
```

Verify headers are present:
```bash
ls /lib/modules/$(uname -r)/build
```

---

## Install

Install latest release:

```bash
curl -L0 https://github.com/CNflysky/hackberrypiq20/releases/latest/download/hackberrypi-max17048-dkms-all.deb -O hackberrypi-max17048-dkms-all.deb
sudo apt install ./hackberrypi-max17048-dkms-all.deb
rm hackberrypi-max17048-dkms-all.deb
sudo reboot
```

---

## Verify

After reboot:

```bash
dkms status
dmesg | grep max17048
ls /sys/class/power_supply/
```

---

## Uninstall

```bash
sudo apt purge hackberrypi-max17048-dkms*
sudo reboot
```

---

## Notes

- The kernel module is managed by **DKMS** and will automatically rebuild on kernel updates.
- The device-tree overlay is installed once and enabled in `/boot/firmware/config.txt`.
- This repository **no longer uses** `make install` or `make remove`.

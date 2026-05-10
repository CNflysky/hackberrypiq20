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
# Add repo key
curl -fsSL https://cnflysky.github.io/hackberrypiq20/hackberrypi-max17048.gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/hackberrypi-max17048.gpg
# Add apt source
sudo tee /etc/apt/sources.list.d/hackberrypi-max17048.sources <<EOF
Types: deb
URIs: https://cnflysky.github.io/hackberrypiq20/
Suites: stable
Components: main
Architectures: all
Signed-By: /usr/share/keyrings/hackberrypi-max17048.gpg
EOF
# install
sudo apt update
sudo apt install hackberrypi-max17048-dkms
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
# this will delete hackberrypi-max17048.gpg and hackberrypi-max17048.sources file.
# no manual cleanup required.
sudo apt purge hackberrypi-max17048-dkms*
sudo reboot
```

---

## Notes

- The kernel module is managed by **DKMS** and will automatically rebuild on kernel updates.
- The device-tree overlay is installed once and enabled in `/boot/firmware/config.txt`.
- This repository **no longer uses** `make install` or `make remove`.

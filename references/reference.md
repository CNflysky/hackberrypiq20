# HackberryPiCM5 Technical References

**Repository Source:** [ZitaoTech/HackberryPiCM5](https://github.com/ZitaoTech/HackberryPiCM5/tree/main)

This document consolidates technical details, pinouts, and specifications for the HackberryPiCM5 hardware, derived from repository references and supplementary technical documentation.

---

## 1. System Overview

**Battery Specifications**
- **Type:** Lipo battery (Single Cell)
- **Dimensions:** 83 x 50.5 x 10.1 mm
- **Capacity:** 5000mAh @ 3.7V (18.5Wh)
- **Voltage Range:** 2.75V - 4.2V
- **Weight:** 84.4g
- **Connector:** PH2.0 2P
- **Protection:** 5A overcurrent protection

**Source:** `references/README.md` in repo.

---

## 2. Battery Subsystem (MAX17048)

The HackberryPiCM5 uses the **MAX17048** microchip to measure battery voltage. It communicates with the CM5 Compute Module via the **I2C interface**.

### Key Specifications (MAX17048)
- **Function:** Fuel Gauge / Voltage Monitor
- **Communication:** I2C
- **I2C Address:** `0x36` (Default)
- **Operating Voltage:** Powered by the battery cell (2.5V - 4.5V range)
- **Measurement Accuracy:** ±7.5mV voltage accuracy, ±1% State-of-Charge (SOC) accuracy (ModelGauge algorithm)

### Pinout / Schematic Integration
- **I2C Bus:** The MAX17048 shares the I2C bus with the screen's touch controller.
- **Connection:**
    - **SDA/SCL:** Connected to the CM5 I2C lines (typically GPIO2/GPIO3 on standard Pi pinout, or specific pins routed on the CM5 carrier board).
    - **VDD:** Connected to System Power (3.3V/5V) or Battery depending on implementation.
    - **GND:** Common Ground.
    - **ALRT:** Interrupt pin (Active Low) - signals low SOC or voltage change.

### Software Implementation (Python)
The following script reads the voltage from the MAX17048.

```python
import smbus2
import time

class MAX17048:
    def __init__(self, i2c_bus=11, i2c_address=0x36):
        # Note: i2c_bus=11 is specific to this hardware setup
        self.bus = smbus2.SMBus(i2c_bus)
        self.address = i2c_address

    def read_voltage(self):
        try:
            # Read voltage registers (0x02 and 0x03)
            read = self.bus.read_i2c_block_data(self.address, 0x02, 2)
            
            # Combine the bytes and convert to voltage
            voltage_raw = (read[0] << 8) | read[1]
            voltage = voltage_raw * 0.078125  # 78.125μV per LSB
            
            return voltage / 1000  # Convert to volts
            
        except Exception as e:
            print(f"Error reading voltage: {e}")
            return None

    def close(self):
        self.bus.close()
```

**Source:** `references/BATTERY_MEASURE.md` and `references/get_battery_voltage.py` in repo.

---

## 3. Display Subsystem (DPI Interface)

The display appears to be a **TL040HDS** series TFT LCD (e.g., TL040HDS30-B1620C), driven using the **DPI (Display Parallel Interface)** mode on the Raspberry Pi Compute Module.

### Display Specifications (TL040HDS Series)
- **Type:** a-Si TFT Active Matrix LCD
- **Interface:** RGB / DPI (24-bit typically) + SPI (for initial config, if applicable)
- **Touch:** Capacitive Touch (Shared I2C bus with Battery monitor)
- **Backlight:** LED

### Raspberry Pi DPI Pinout (Standard RGB24 Mode)
The DPI interface uses GPIO bank 0 (GPIO 0-27) to transmit partial pixel data parallelly.

| Signal | GPIO Pin | Function |
| :--- | :--- | :--- |
| **Control** | | |
| CLK | GPIO 0 | Pixel Clock (PCLK) |
| DE | GPIO 1 | Data Enable |
| VSYNC | GPIO 2 | Vertical Sync |
| HSYNC | GPIO 3 | Horizontal Sync |
| **Blue** | | |
| B0 - B7 | GPIO 4 - 11 | Blue Data (8-bit) |
| **Green** | | |
| G0 - G7 | GPIO 12 - 19 | Green Data (8-bit) |
| **Red** | | |
| R0 - R7 | GPIO 20 - 27 | Red Data (8-bit) |

*Note: The actual pin mapping depends on the specific DPI overlay configured in `config.txt` (e.g., `dpi_output_format`).*

**Source:** Derived from `references/[Rpi Doc] Using-a-DPI-display.pdf` context and standard DPI documentation.

---

## 4. Hardware Reference Images & Schematics

For visual diagrams (Battery photo, Schematic snippets), please refer to the images linked in the original repository files:
- `references/README.md` (Battery Photo)
- `references/BATTERY_MEASURE.md` (Schematic Snippet)

**Repository Link:** [ZitaoTech/HackberryPiCM5/references](https://github.com/ZitaoTech/HackberryPiCM5/tree/main/references)

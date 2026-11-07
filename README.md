# FOTA

# ESP32 Over-the-Air (OTA) Firmware Update System

A modular firmware update framework for ESP32-based devices, designed to support secure firmware delivery, version management, firmware integrity verification, and rollback protection.

The project explores how firmware updates can be performed remotely without requiring a physical USB connection or manual firmware flashing for every release.

The initial implementation is developed using **ESP32-S3, ESP-IDF 5.5.0, and PlatformIO**. Additional ESP32 and Arduino framework implementations are planned.

---

## 1. Project Overview

Firmware updates are an important part of connected embedded systems. Once a device has been deployed, updating its firmware through a physical programming interface can become inconvenient, especially when devices are distributed across different locations.

This project implements an Over-the-Air (OTA) firmware update mechanism that allows an ESP32 device to:

- Check for firmware updates from a remote manifest.
- Compare the installed firmware version with the available version.
- Download new firmware over HTTPS.
- Verify firmware integrity using SHA-256.
- Install firmware into an inactive OTA partition.
- Reboot into the updated firmware.
- Validate the newly installed application.
- Roll back to the previously working firmware if validation fails.

The objective is to develop a reusable OTA framework that can eventually be integrated into different ESP32 projects with minimal changes to the application's existing code.

## 2. Current Development Status

| Implementation        | Status                                      |
| --------------------- | ------------------------------------------- |
| ESP32-S3 with ESP-IDF | Initial implementation developed and tested |
| ESP32 with Arduino    | Coming soon                                 |
| ESP32 with ESP-IDF    | Coming soon                                 |
| ESP32-S3 with Arduino | Coming soon                                 |

Board used ESP32-S3 N16R8V

### Implemented and tested functionality

- HTTPS communication with GitHub-hosted firmware manifests.
- Remote firmware version checking.
- Semantic version comparison using major, minor, and patch numbers.
- Automatic update scheduling using FreeRTOS tasks and queues.
- HTTPS firmware download.
- SHA-256 firmware integrity verification.
- Dual OTA partition support.
- Firmware installation using ESP-IDF OTA APIs.
- Boot partition switching.
- Pending image verification.
- Firmware acceptance and rollback APIs.
- Custom partition table configuration.

### Current development focus

The core OTA download and installation workflow has been demonstrated successfully. Startup health validation and rollback integration are being refined further to make the framework more robust under application failures and network interruptions.

---

## 3. Repository Structure

```text
FOTA/
│
├── README.md
│
├── esp32_arduino/
│   └── README.md
│
├── esp32_espidf/
│   └── README.md
│
├── esp32s3_arduino/
│   └── README.md
│
└── esp32s3_espidf/
    │
    ├── .pio/
    ├── .vscode/
    ├── build/
    │
    ├── include/
    ├── lib/
    ├── releases/
    ├── src/
    ├── test/
    │
    ├── .clangd
    ├── .gitignore
    ├── CMakeLists.txt
    ├── partitions.csv
    ├── platformio.ini
    └── sdkconfig
```

### Main implementation directory

`esp32s3_espidf/` contains the active ESP32-S3 N16R8V implementation.

| Directory / File | Purpose                                              |
| ---------------- | ---------------------------------------------------- |
| `src/`           | Main application and OTA implementation source files |
| `include/`       | Project header files and interface declarations      |
| `lib/`           | Project-specific libraries and reusable components   |
| `releases/`      | Firmware release binaries and manifest               |
| `test/`          | Test-related source files                            |
| `partitions.csv` | Custom flash partition layout                        |
| `platformio.ini` | PlatformIO environment and build configuration       |
| `sdkconfig`      | ESP-IDF configuration                                |
| `CMakeLists.txt` | ESP-IDF project configuration and firmware version   |
| `.gitignore`     | Excludes generated and local development files       |

The `.pio/` and `build/` directories are generated during development and are not intended to be maintained as source code.

---

## 4. System Architecture

The implementation is divided into three primary responsibilities.

### 4.1 Manifest checking

The device connects to a remote JSON manifest hosted on GitHub.

Example manifest:

```json
{
    "version": "0.0.7",
    "firmware_url": "https://raw.githubusercontent.com/kashyap135/FOTA/main/esp32s3_espidf/releases/firmware.bin",
    "sha256": "YOUR_FIRMWARE_SHA256_HASH"
}
```

The manifest provides:

- `version`: Latest available firmware version.
- `firmware_url`: Location of the firmware binary.
- `sha256`: Expected SHA-256 hash of the firmware binary.

The device retrieves the manifest through HTTPS and parses its contents using cJSON.

The currently installed firmware version is obtained using:

```c
const esp_app_desc_t *app_desc =
    esp_app_get_description();

const char *curr_version = app_desc->version;
```

The remote and local versions are compared before an update is scheduled.

### 4.2 OTA download and installation

Once a newer firmware version is detected, the manifest information is passed to the OTA download task using a FreeRTOS queue.

The OTA download component performs the following operations:

1. Establishes an HTTPS connection to the firmware URL.
2. Identifies the next available OTA partition.
3. Downloads the firmware binary.
4. Calculates its SHA-256 hash.
5. Compares the calculated hash with the manifest hash.
6. Completes firmware installation.
7. Selects the new partition as the boot partition.
8. Restarts the device.

Important ESP-IDF OTA APIs include:

```c
esp_ota_get_next_update_partition(NULL);
```

```c
esp_ota_begin();
```

```c
esp_ota_write();
```

```c
esp_ota_end();
```

```c
esp_ota_set_boot_partition();
```

These APIs provide the foundation for writing and selecting firmware images without overwriting the currently running OTA application.

### 4.3 Firmware validation and rollback

A downloaded firmware image should not automatically be considered reliable just because the download and integrity verification succeeded.

ESP-IDF provides OTA image-state tracking and rollback support.

When a new OTA image boots, it can enter:

```c
ESP_OTA_IMG_PENDING_VERIFY
```

The application can then perform startup checks before accepting the image.

If validation succeeds:

```c
esp_ota_mark_app_valid_cancel_rollback();
```

This marks the currently running firmware as valid and cancels the rollback process.

If validation fails:

```c
esp_ota_mark_app_invalid_rollback_and_reboot();
```

This marks the new image as invalid and requests a reboot into the previously working firmware.

The project wraps these operations inside its rollback component:

```c
ota_rollback_validate_running_app(true);
```

for successful validation, and:

```c
ota_rollback_validate_running_app(false);
```

for failed validation.

The boolean represents the result of the application's startup self-test. It should eventually be driven by actual application health checks rather than a permanently hardcoded value.

---

## 5. Custom Flash Partition Table

The ESP32-S3 implementation uses a custom partition table to support multiple firmware images.

Current partition configuration:

```csv
# Name,     Type, SubType, Offset,   Size
nvs,        data, nvs,     0x9000,   24K
otadata,    data, ota,     0xf000,   8K
phy_init,   data, phy,     0x11000,  4K
factory,    app,  factory, 0x20000,  3M
ota_0,      app,  ota_0,   0x320000, 3M
ota_1,      app,  ota_1,   0x620000, 3M
```

### Partition responsibilities

| Partition  | Purpose                               |
| ---------- | ------------------------------------- |
| `nvs`      | Non-volatile storage                  |
| `otadata`  | Stores OTA boot-selection information |
| `phy_init` | Wi-Fi PHY initialization data         |
| `factory`  | Factory firmware image                |
| `ota_0`    | First OTA application slot            |
| `ota_1`    | Second OTA application slot           |

The two OTA application slots allow the device to write a new firmware image into an inactive partition while preserving the currently running image.

For example:

```text
Current firmware: ota_0

New firmware downloaded into: ota_1

After successful installation:
Boot partition = ota_1
```

For a subsequent update, the target partition alternates depending on the currently running application.

The factory partition provides an additional fallback image. Its presence does not mean that every rollback will necessarily return to the factory image; the bootloader selects the appropriate previously valid image according to OTA state.

**Flash configuration:** ESP32-S3 N16R8 development board with 16 MB flash.

---

## 6. Firmware Version Management

Firmware versions use a three-component format:

```text
MAJOR.MINOR.PATCH
```

Examples:

```text
0.0.5
0.0.6
0.0.7
1.0.0
1.0.1
```

The project compares each numeric component independently.

Examples:

| Installed | Available | Result             |
| --------- | --------- | ------------------ |
| 0.0.5     | 0.0.7     | Update available   |
| 0.0.7     | 0.0.7     | Already up to date |
| 1.0.1     | 1.0.0     | Downgrade rejected |
| 1.0.0     | 1.0.1     | Update available   |

Version comparison is implemented in the manifest-checking component.

The firmware version is configured in the ESP-IDF project configuration:

```cmake
set(PROJECT_VER "0.0.7")
```

The value should be updated whenever a new firmware release is prepared.

---

## 7. Firmware Release Workflow

The current release workflow uses GitHub as the firmware hosting and manifest distribution platform.

```text
Develop firmware
       |
       v
Build firmware binary
       |
       v
Calculate SHA-256
       |
       v
Upload firmware.bin
       |
       v
Update manifest.json
       |
       v
Device checks manifest
       |
       v
Version comparison
       |
       v
HTTPS firmware download
       |
       v
SHA-256 verification
       |
       v
Write inactive OTA partition
       |
       v
Switch boot partition
       |
       v
Restart and validate firmware
```

### Updating the release

1. Modify the application firmware.
2. Update the project version.
3. Build the firmware using the correct PlatformIO environment.
4. Locate the generated firmware binary.
5. Calculate its SHA-256 hash.
6. Upload the binary to the release directory.
7. Update the manifest with the new version, URL, and hash.
8. Allow connected devices to check for the new release.

The firmware URL and hash in the manifest must correspond to the exact binary being distributed.

Never publish a new manifest with a hash copied from an earlier firmware release.

---

## 8. Development Environment

The initial implementation uses:

| Component              | Configuration  |
| ---------------------- | -------------- |
| Microcontroller        | ESP32-S3       |
| Development board      | ESP32-S3 N16R8 |
| Framework              | ESP-IDF        |
| ESP-IDF version        | 5.5.0          |
| Build environment      | PlatformIO     |
| Language               | C              |
| RTOS                   | FreeRTOS       |
| Network                | Wi-Fi          |
| Firmware transport     | HTTPS          |
| Manifest format        | JSON           |
| JSON parser            | cJSON          |
| Integrity verification | SHA-256        |
| Firmware hosting       | GitHub         |

### Building the project

Clone the repository:

```bash
git clone https://github.com/kashyap135/FOTA.git
```

Navigate to the ESP32-S3 ESP-IDF implementation:

```bash
cd FOTA/esp32s3_espidf
```

Open the project in VS Code with PlatformIO installed.

Select the configured environment:

```ini
[env:esp32-s3-devkitc-1-n16r8v]
```

Build the firmware:

```bash
pio run
```

The firmware can initially be flashed through the programming interface to provision the device.

Subsequent firmware updates can be performed through the OTA mechanism.

### Wi-Fi configuration

Wi-Fi credentials are maintained separately in the project's Wi-Fi configuration header.

Configure the appropriate SSID and password locally before building.

Do not commit personal Wi-Fi credentials, private keys, or other secrets to a public repository.

---

## 9. Testing and Verification

The OTA implementation has been tested using actual firmware transfers between OTA partitions on an ESP32-S3.

### Test 1: Successful OTA update

A device running firmware `0.0.5` detected firmware `0.0.7` from the remote manifest.

The device then:

- Retrieved the manifest over HTTPS.
- Detected the newer firmware version.
- Queued the update.
- Downloaded the firmware.
- Calculated the SHA-256 hash.
- Verified the expected hash.
- Installed the firmware into `ota_1`.
- Rebooted into the updated firmware.
- Passed startup validation.
- Marked the new image valid.

Representative serial output:

```text
Current firmware version: 0.0.5
Available firmware version: 0.0.7
New firmware available: 0.0.7

OTA task received update
Version: 0.0.7

target partition: ota_1
address: 0x620000

SHA-256 context initialized; starting download
Downloading 995472 bytes

Downloaded: 995472 bytes

Calculated SHA-256:
0f1aa312f3b57c6472445a355f14a36c9e88f928ce34db6aba0bf32691b61842

Firmware verified and installed. Rebooting...
```

After reboot:

```text
Project name: FOTA
App version: 0.0.7

Running partition: ota_1
Image state: PENDING_VERIFY

Startup validation passed
Firmware marked VALID
```

A subsequent manifest check confirmed:

```text
Current firmware version: 0.0.7
Available firmware version: 0.0.7

Firmware is already up to date.
Skipping update.
```

### Test 2: Rollback verification

A separate test build was configured to deliberately fail startup validation.

The test demonstrated the rollback path:

```text
TEST MODE: deliberately failing pending OTA image

New firmware failed validation.
Rollback to previously worked partition.
```

The next boot returned to the previously working firmware image.

This verified that ESP-IDF's OTA rollback mechanism could reject a pending firmware image and return to a previously valid application.

### Test 3: Subsequent firmware release

A later release test used firmware versions `1.0.0` and `1.0.1`.

The device downloaded a 992,864-byte binary and calculated the following SHA-256:

```text
9afa698a3fc317c3a1f2cca65c91e72cde0a28d50a56f12f78c439a819118415
```

The hash matched the manifest, and the firmware was installed into the inactive OTA partition.

The subsequent boot exposed an application abort during startup. This is part of the ongoing health-validation refinement and is not counted as a successful end-to-end firmware acceptance test.

---

## 10. Future Development

The long-term objective is to make this project a reusable OTA framework rather than a board-specific demonstration.

The following features are planned.

### 10.1 User application integration

A dedicated user-code area or integration interface is planned.

The intention is to allow developers to add their own application logic without modifying the internal OTA implementation.

A possible future structure:

```text
esp32s3_espidf/
│
├── ota_core/
│   ├── manifest.c
│   ├── firmware_download.c
│   ├── ota_rollback.c
│   └── ota_manager.c
│
├── user_code/
│   ├── app_main.c
│   └── application.c
│
└── releases/
```

The exact structure and public API are still under development.

### 10.2 Reusable OTA library

A reusable library or component is planned so developers can integrate OTA functionality into an existing ESP32 application.

The intended integration should eventually be simple enough to look similar to:

```c
#include "ota_manager.h"p

void app_main(void)
{
    ota_manager_init();

    // User application code
}
```

This is an illustrative future interface, not an API currently available in the repository.

### 10.3 Arduino framework support

Arduino-based implementations are planned for:

- ESP32 with Arduino.
- ESP32-S3 with Arduino.

The objective is to make the same OTA functionality accessible to developers using both ESP-IDF and Arduino.

### 10.4 Improved application health monitoring

Future improvements include:

- Application-specific self-tests.
- Peripheral and sensor initialization checks.
- Task health monitoring.
- Communication health checks.
- Configurable startup validation timeouts.
- More robust handling of failed startup checks.
- Persistent suppression of previously rejected firmware releases.

### 10.5 Additional OTA capabilities

Other planned improvements include:

- Release channels such as stable and development.
- Firmware release metadata.
- Better update scheduling.
- Configurable retry and timeout policies.
- Firmware compatibility checks.
- More extensive failure and recovery testing.
- Documentation and examples for custom partition configurations.

---

## 11. Project Goals

The project is being developed with the following goals:

1. Develop a reusable OTA firmware update mechanism for ESP32 devices.
2. Support secure remote firmware delivery.
3. Prevent corrupted or unintended firmware images from being installed.
4. Use dual-partition updates to preserve the currently running application during firmware installation.
5. Provide rollback protection for unsuccessful firmware updates.
6. Support multiple ESP32 boards and development frameworks.
7. Simplify integration into existing embedded applications.

---

## 12. Contributions and Development

This project is actively being developed.

The current focus is on improving the ESP32-S3 ESP-IDF implementation, refining startup health validation, and extending support to additional ESP32 development environments.

Suggestions, bug reports, testing feedback, and contributions are welcome.

**Repository:**
[https://github.com/kashyap135/FOTA](https://github.com/kashyap135/FOTA)

---

## License

MIT License

Copyright (c) 2026 kashyap135

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

#pragma once
#include "esp_err.h"
#include "ota_update_info.h"

esp_err_t firmware_download_and_install(
    const ota_update_info_t *update
);
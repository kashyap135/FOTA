#ifndef OTA_ROLLBACK_H
#define OTA_ROLLBACK_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ota_rollback_validate_running_app(bool self_test_passed);


bool ota_rollback_is_pending(void);

#endif
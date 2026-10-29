#include "ota_rollback.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG="OTA_ROLLBACK";

esp_err_t ota_rollback_validate_running_app(bool self_test_passed){
    const esp_partition_t *running_partition=esp_ota_get_running_partition();
    if (running_partition== NULL){
        ESP_LOGE(TAG,"unable to identify the running partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,"Running partition:%s",running_partition->label);
    ESP_LOGI(TAG,"startup self-test result:%s",self_test_passed ? "PASS" : "FAIL");

    esp_ota_img_states_t image_state;

    esp_err_t err=esp_ota_get_state_partition(running_partition,&image_state);
    ESP_LOGI(TAG, "State query result: %s",
         esp_err_to_name(err));

if (err == ESP_OK) {
    ESP_LOGI(TAG, "OTA image state: %d", image_state);
}

    if (err == ESP_ERR_NOT_FOUND ||
    (err == ESP_ERR_NOT_SUPPORTED &&
     running_partition->type == ESP_PARTITION_TYPE_APP &&
     running_partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)) {

    ESP_LOGI(TAG,
             "Factory image or image without OTA state tracking");

    if (self_test_passed) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Startup self-test failed");
    return ESP_ERR_INVALID_STATE;
}

    if (err!= ESP_OK){
        ESP_LOGE(TAG,"failed to read ota image state:%s",esp_err_to_name(err));

        return err;

    }

    if (image_state != ESP_OTA_IMG_PENDING_VERIFY){
        if (self_test_passed){
            ESP_LOGI(TAG,
                     "Image is not pending verification; "
                     "no rollback action required");
            return ESP_OK;
        }

        ESP_LOGE(TAG,
                 "Self-test failed, but the running image "
                 "is not pending verification");

        return ESP_ERR_INVALID_STATE;
    }

    if (!self_test_passed){
        ESP_LOGE(TAG,"new firmware failed validation;""requesting roll back");
        err=esp_ota_mark_app_invalid_rollback_and_reboot();


        ESP_LOGE(TAG, "Rollback request returned: %s",
                 esp_err_to_name(err));

        return err;
    }
    err=esp_ota_mark_app_valid_cancel_rollback();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark application valid: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Startup validation passed; "
             "firmware marked VALID");

    return ESP_OK;
}



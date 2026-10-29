#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "firmware_download.h"
#include "ota_update_info.h"
#include <errno.h>
#include <limits.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "wifi_config.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "ota_rollback.h"


static TaskHandle_t http_task_handle = NULL;


static QueueHandle_t ota_queue;

static const char *TAG = "OTA_CHECK";

void print_ota_status(void)
{
    const esp_partition_t *running =
        esp_ota_get_running_partition();

    const esp_partition_t *boot =
        esp_ota_get_boot_partition();

    ESP_LOGI(TAG, "Running partition: %s",
             running ? running->label : "NULL");

    ESP_LOGI(TAG, "Boot partition: %s",
             boot ? boot->label : "NULL");

    const esp_partition_t *ota0 =
        esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            ESP_PARTITION_SUBTYPE_APP_OTA_0,
            NULL);

    const esp_partition_t *ota1 =
        esp_partition_find_first(
            ESP_PARTITION_TYPE_APP,
            ESP_PARTITION_SUBTYPE_APP_OTA_1,
            NULL);

    ESP_LOGI(TAG, "ota_0: %s",
             ota0 ? "Found" : "Missing");

    ESP_LOGI(TAG, "ota_1: %s",
             ota1 ? "Found" : "Missing");
}


static bool parse_version(const char *version,
                          unsigned long parts[3])
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }

    const char *p = version;

    for (int i = 0; i < 3; i++) {

        if (*p < '0' || *p > '9') {
            return false;
        }

        errno = 0;
        char *end = NULL;

        unsigned long value = strtoul(p, &end, 10);

        if (errno == ERANGE || end == p) {
            return false;
        }

        parts[i] = value;

        if (i < 2) {
            if (*end != '.') {
                return false;
            }

            p = end + 1;
        } else {
            if (*end != '\0') {
                return false;
            }
        }
    }

    return true;
}

/*
 * Returns:
 *   1  -> remote version is newer
 *   0  -> versions are equal
 *  -1  -> remote version is older
 *  -2  -> invalid version string
 */
static int compare_versions(const char *current_version,
                            const char *remote_version)
{
    unsigned long current[3];
    unsigned long remote[3];

    if (!parse_version(current_version, current) ||
        !parse_version(remote_version, remote)) {
        return -2;
    }

    for (int i = 0; i < 3; i++) {

        if (remote[i] > current[i]) {
            return 1;
        }

        if (remote[i] < current[i]) {
            return -1;
        }
    }

    return 0;
}


static void wifi_event_handler(void *arg,esp_event_base_t event_base,int32_t event_id,void *event_data){
    if (event_base ==WIFI_EVENT){
        if (event_id== WIFI_EVENT_STA_START){
            printf("Wifi started\n");
        }

        else if (event_id == WIFI_EVENT_STA_CONNECTED){
            printf("WIFI connected\n");
        }

        else if (event_id == WIFI_EVENT_STA_DISCONNECTED){
            printf("wifi disconnected\n");
            esp_wifi_connect();
        }
    }
    else if (event_base== IP_EVENT){
        if (event_id==IP_EVENT_STA_GOT_IP){

            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            printf("GOt IP address! "IPSTR" \n",IP2STR(&event->ip_info.ip));
            vTaskDelay(pdMS_TO_TICKS(10000));
            xTaskNotifyGive(http_task_handle);
        }
    }
}


void http_task(void *arg)
{
    char buffer[512];

const esp_app_desc_t *app_desc =
    esp_app_get_description();

const char *curr_version = app_desc->version;

printf("Running firmware version: %s\n", curr_version);

    while (1) {

        // Wait until Wi-Fi has obtained an IP address
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        esp_http_client_config_t config = {
            .url = "https://raw.githubusercontent.com/kashyap135/FOTA/refs/heads/main/esp32s3_espidf/releases/manifest.json",
            .timeout_ms = 20000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = NULL;
        esp_err_t err = ESP_FAIL;

        // Retry manifest connection: 2, 4, and 8 seconds
        for (int attempt = 0; attempt < 4; attempt++) {

            client = esp_http_client_init(&config);

            if (client == NULL) {
                printf("HTTP client init failed\n");
                err = ESP_FAIL;
            } else {
                err = esp_http_client_open(client, 0);

                if (err == ESP_OK) {
                    printf("Manifest connection established\n");
                    break;
                }

                printf("Manifest connection attempt %d failed: %s\n",
                       attempt + 1, esp_err_to_name(err));

                printf("Socket errno: %d\n",
                       esp_http_client_get_errno(client));

                esp_http_client_cleanup(client);
                client = NULL;
            }

            if (attempt < 3) {
                int delay_seconds = 2 << attempt;

                printf("Retrying manifest request in %d seconds\n",
                       delay_seconds);

                vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000));
            }
        }

        if (client == NULL || err != ESP_OK) {
            printf("Manifest connection failed after retries\n");
            continue;
        }

        // Read HTTP response headers
        int64_t content_length =
            esp_http_client_fetch_headers(client);

        int status = esp_http_client_get_status_code(client);

        if (status != 200 || content_length < 0) {
            printf("Manifest request failed: HTTP %d\n", status);

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        // Read manifest response
        int len = esp_http_client_read(
            client, buffer, sizeof(buffer) - 1);

        if (len <= 0) {
            printf("Manifest read failed\n");

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        buffer[len] = '\0';
        printf("Manifest received:\n%s\n", buffer);

        cJSON *root = cJSON_Parse(buffer);

        if (root == NULL) {
            printf("JSON parse failed\n");
        } else {

            cJSON *version =
                cJSON_GetObjectItem(root, "version");

            cJSON *firmware_url =
                cJSON_GetObjectItem(root, "firmware_url");

            cJSON *sha256 =
                cJSON_GetObjectItem(root, "sha256");

            if (!cJSON_IsString(version) ||
                !cJSON_IsString(firmware_url) ||
                !cJSON_IsString(sha256) ||
                version->valuestring == NULL ||
                firmware_url->valuestring == NULL ||
                sha256->valuestring == NULL) {

                printf("Invalid manifest fields\n");

                cJSON_Delete(root);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                continue;
            }

            // Display running and available versions
            // Display the running and available versions
printf("Current firmware version: %s\n", curr_version);
printf("Available firmware version: %s\n",
       version->valuestring);

int version_result =
    compare_versions(curr_version, version->valuestring);

if (version_result == 1) {

    ota_update_info_t update = {0};

    snprintf(update.firmware_url,
             sizeof(update.firmware_url), "%s",
             firmware_url->valuestring);

    snprintf(update.sha256,
             sizeof(update.sha256), "%s",
             sha256->valuestring);

    snprintf(update.version,
             sizeof(update.version), "%s",
             version->valuestring);

    printf("New firmware available: %s\n",
           update.version);

    if (xQueueOverwrite(ota_queue, &update) == pdPASS) {
        printf("Update queued successfully\n");
    } else {
        printf("Failed to queue OTA update\n");
    }

} else if (version_result == 0) {

    printf("Firmware is already up to date. Skipping update.\n");

} else if (version_result == -1) {

    printf("Remote firmware is older than the running firmware. "
           "Downgrade rejected.\n");

} else {

    printf("Invalid firmware version. Skipping update.\n");
}

printf("Firmware URL: %s\n", firmware_url->valuestring);
printf("SHA256: %s\n", sha256->valuestring);

            cJSON_Delete(root);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        // Delay before the next manifest check
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}




void ota_task(void *arg)
{
    ota_update_info_t update;
    while(1){

        if (xQueueReceive(ota_queue, &update, portMAX_DELAY) != pdPASS) {
            continue;
        }

        printf("OTA task received update\n");
        printf("Version: %s\n", update.version);
        printf("URL: %s\n", update.firmware_url);
        printf("SHA256: %s\n", update.sha256);

        esp_err_t err = firmware_download_and_install(&update);

if (err != ESP_OK) {
    printf("OTA update failed: %s\n", esp_err_to_name(err));
}
        

    }
}


void app_main(void)
{
    esp_err_t ret;
    printf("\n===== APP MAIN STARTED =====\n");
fflush(stdout);
 ESP_LOGE("DEBUG_BOOT", "APP MAIN REACHED - ERROR LEVEL TEST");
print_ota_status();
     // OTA boot diagnostics
const esp_partition_t *running =
    esp_ota_get_running_partition();

if (running != NULL) {
    ESP_LOGI("OTA_BOOT", "Running partition: %s", running->label);
    ESP_LOGI("OTA_BOOT", "Partition address: 0x%lx",
             (unsigned long)running->address);

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);

    if (err == ESP_OK) {
        ESP_LOGI("OTA_BOOT", "Image state: %d", (int)state);

        switch (state) {
            case ESP_OTA_IMG_NEW:
                ESP_LOGI("OTA_BOOT", "State: NEW");
                break;

            case ESP_OTA_IMG_PENDING_VERIFY:
                ESP_LOGW("OTA_BOOT", "State: PENDING_VERIFY");
                break;

            case ESP_OTA_IMG_VALID:
                ESP_LOGI("OTA_BOOT", "State: VALID");
                break;

            case ESP_OTA_IMG_INVALID:
                ESP_LOGE("OTA_BOOT", "State: INVALID");
                break;

            case ESP_OTA_IMG_ABORTED:
                ESP_LOGE("OTA_BOOT", "State: ABORTED");
                break;

            case ESP_OTA_IMG_UNDEFINED:
                ESP_LOGI("OTA_BOOT", "State: UNDEFINED");
                break;

            default:
                ESP_LOGW("OTA_BOOT", "State: UNKNOWN");
                break;
        }
    } else {
        ESP_LOGW("OTA_BOOT", "Cannot read image state: %s",
                 esp_err_to_name(err));
    }
} else {
    ESP_LOGE("OTA_BOOT", "Unable to identify running partition");
}


    /* 1. Initialize NVS, recovering if its stored data is incompatible. */
    ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    bool self_test_passed = true;

#ifdef OTA_ROLLBACK_TEST_FAIL
    const esp_partition_t *test_partition =
        esp_ota_get_running_partition();

    esp_ota_img_states_t test_state;

    if (test_partition != NULL &&
        esp_ota_get_state_partition(
            test_partition, &test_state) == ESP_OK &&
        test_state == ESP_OTA_IMG_PENDING_VERIFY) {

        ESP_LOGW("OTA_BOOT",
                 "TEST MODE: deliberately failing pending OTA image");

        self_test_passed = false;
    }
#endif

ret = ota_rollback_validate_running_app(self_test_passed);

if (ret != ESP_OK) {
    printf("OTA startup validation failed: %s\n",
           esp_err_to_name(ret));
    abort();
}

    printf("Startup checks passed. Firmware validated.\n");
    /* 2. Initialize network infrastructure. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* 3. Initialize the Wi-Fi driver. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 4. Configure Wi-Fi credentials. */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD
        }
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config)
    );

    /* 5. Register event handlers. */
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    /* 6. Create the OTA queue. */
    ota_queue = xQueueCreate(1, sizeof(ota_update_info_t));

    if (ota_queue == NULL) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* 7. Create the HTTP manifest-checking task. */
    if (xTaskCreate(
            http_task,
            "http_task",
            8192,
            NULL,
            5,
            &http_task_handle
        ) != pdPASS) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* 8. Create the OTA download/install task. */
    if (xTaskCreate(
            ota_task,
            "ota_task",
            12288,
            NULL,
            5,
            NULL
        ) != pdPASS) {
        ESP_ERROR_CHECK(ESP_FAIL);
    }

   
    /* 9. Start Wi-Fi. */
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * 10. Local startup initialization succeeded.
     * If this is a pending OTA image, mark it valid and
     * cancel rollback. This does not test internet access.
     */
    

    

    /* 11. Start connecting; internet availability is not
       a condition for accepting the firmware image. */
    ret = esp_wifi_connect();

    if (ret != ESP_OK) {
        printf("Wi-Fi connection request failed: %s\n",
               esp_err_to_name(ret));
    }
}

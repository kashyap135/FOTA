#include <stdio.h>
#include <stdbool.h>
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

#include "wifi_config.h"
#include "freertos/queue.h"


static TaskHandle_t http_task_handle = NULL;


static QueueHandle_t ota_queue;


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

void app_main()
{

   
   esp_err_t ret;
   ret=nvs_flash_init();
    

    ret =esp_netif_init();
    ret=esp_event_loop_create_default();

   esp_netif_create_default_wifi_sta();

   wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    esp_wifi_init(&cfg);
    

ota_queue=xQueueCreate(1,sizeof(ota_update_info_t));
if(ota_queue==NULL){
    printf("OTA queue creation failed");
    return;
}

    

if (xTaskCreate(http_task, "http_task", 8192, NULL, 5,
                &http_task_handle) != pdPASS) {
    printf("HTTP task creation failed\n");
    return;
}



    if (xTaskCreate(
        ota_task, "ota_task", 12288, NULL, 5,
        NULL) != pdPASS) {
    printf("OTA task creation failed\n");
    return;
}

    esp_wifi_set_mode(WIFI_MODE_STA);

wifi_config_t wifi_config = {
    .sta = {
        .ssid = WIFI_SSID,
        .password = WIFI_PASSWORD
    }
};

ret=esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_event_handler_register(
    WIFI_EVENT,
    ESP_EVENT_ANY_ID,
    &wifi_event_handler,
    NULL
);
esp_event_handler_register(
    IP_EVENT,
    IP_EVENT_STA_GOT_IP,
    &wifi_event_handler,
    NULL
);
ret=esp_wifi_start();

ret=esp_wifi_connect();

   
}
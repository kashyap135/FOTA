#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "firmware_download.h"
#include "ota_update_info.h"

#include "wifi_config.h"
#include "freertos/queue.h"

#define curr_version "0.0.0"  

static TaskHandle_t http_task_handle = NULL;


static QueueHandle_t ota_queue;


bool is_version_newer(const char *remote_version){
    int curr_major,curr_minor,curr_patch;
    int remote_major,remote_minor,remote_patch;
    if(sscanf(curr_version,"%d.%d.%d",&curr_major,&curr_minor,&curr_patch) !=3 ||
    sscanf(remote_version,"%d.%d.%d",&remote_major,&remote_minor,&remote_patch) !=3){
        return false;
    }

    if (remote_major >curr_major){
        return true;
    }
    if(remote_major < curr_major)
    return false;

    if(remote_minor>curr_minor)
    return true;
    if(remote_minor<curr_minor)
    return false;
    if(remote_patch>curr_patch)
    return true;

    return false;

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
            xTaskNotifyGive(http_task_handle);
        }
    }
}

void http_task(void *arg){
    char buffer[512];

    
    
while(1){
    //http setup
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_http_client_config_t config = {
    .url = "https://raw.githubusercontent.com/kashyap135/FOTA/refs/heads/main/esp32s3_espidf/releases/manifest.json",
    .timeout_ms = 20000,
    .crt_bundle_attach = esp_crt_bundle_attach,
};

esp_http_client_handle_t client =
    esp_http_client_init(&config);
    if (client == NULL) {
    printf("HTTP client init failed\n");
    continue;
}

esp_err_t err = esp_http_client_open(client, 0);

if (err != ESP_OK) {
    printf("HTTP open failed: %s\n", esp_err_to_name(err));
    int socket_errno = esp_http_client_get_errno(client);

printf("HTTP open failed: %s\n", esp_err_to_name(err));
printf("Socket errno: %d\n", socket_errno);
    esp_http_client_cleanup(client);
    continue;
}
int64_t content_length = esp_http_client_fetch_headers(client);
int status = esp_http_client_get_status_code(client);

if (status != 200 || content_length < 0) {
    printf("Manifest request failed: HTTP %d\n", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    continue;
}

int len=esp_http_client_read(client,buffer,sizeof(buffer)-1);

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

    cJSON *version = cJSON_GetObjectItem(root, "version");
    cJSON *firmware_url = cJSON_GetObjectItem(root, "firmware_url");
    cJSON *sha256 = cJSON_GetObjectItem(root, "sha256");

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

    if (is_version_newer(version->valuestring)) {
        ota_update_info_t update={0};

        snprintf(update.firmware_url,sizeof(update.firmware_url),"%s",firmware_url->valuestring);

        snprintf(update.sha256,sizeof(update.sha256),"%s",sha256->valuestring);

        snprintf(update.version,sizeof(update.version),"%s",version->valuestring);

        printf("New firmware available!\n");

        if (xQueueOverwrite(ota_queue, &update) == pdPASS) {

            printf("Update queued successfully\n");
        }
     } 

     else {

        printf("Firmware is up to date.\n");
}
    

    printf("Version: %s\n", version->valuestring);
    printf("Firmware URL: %s\n", firmware_url->valuestring);
    printf("SHA256: %s\n", sha256->valuestring);

    cJSON_Delete(root);
}
esp_http_client_close(client);
esp_http_client_cleanup(client);
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
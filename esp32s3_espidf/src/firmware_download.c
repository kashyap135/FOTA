#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include <stdlib.h>
#include "firmware_download.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"


static const char *TAG="OTA_DOWNLOAD";

#define DOWNLOAD_BUFFER_SIZE 4096

static void test_sha256(void)
{
    mbedtls_sha256_context ctx;
    unsigned char digest[32];
   

    mbedtls_sha256_init(&ctx);

    int start_ret = mbedtls_sha256_starts(&ctx, 0);
    ESP_LOGI(TAG, "SHA start result: %d", start_ret);

    if (start_ret == 0) {
        int update_ret = mbedtls_sha256_update(
            &ctx, (const unsigned char *)"abc", 3);

        ESP_LOGI(TAG, "SHA update result: %d", update_ret);

        if (update_ret == 0) {
            int finish_ret = mbedtls_sha256_finish(&ctx, digest);

            ESP_LOGI(TAG, "SHA finish result: %d", finish_ret);
            ESP_LOG_BUFFER_HEX(TAG, digest, sizeof(digest));\


            
        }
        
    }

    

    mbedtls_sha256_free(&ctx);
}

esp_err_t firmware_download_and_install(
    const ota_update_info_t *update){

        unsigned char *buffer = NULL;

        if ( update == NULL || update->firmware_url[0]=='\0' || strlen(update->sha256)!=64 ){
            return ESP_ERR_INVALID_ARG;
        }

        test_sha256();


    //sha256 verification

    size_t len = strlen(update->sha256);

if (len != 64) {
    ESP_LOGE(TAG, "Invalid SHA256 length: %u (expected 64)",
             (unsigned int)len);
    return ESP_ERR_INVALID_ARG;
}

for (size_t i = 0; i < len; i++) {
    char c = update->sha256[i];

    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
        ESP_LOGE(TAG, "Invalid SHA256 character at index %u: 0x%02X",
                 (unsigned int)i, (unsigned char)c);
        return ESP_ERR_INVALID_ARG;
    }
}

        const esp_partition_t *partition=esp_ota_get_next_update_partition(NULL);

        if(partition==NULL){
            ESP_LOGE(TAG,"No ota update partition available");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGI(TAG,"target partition:%s",partition->label);
        ESP_LOGI(TAG,"address:0x%lx",(unsigned long)partition->address);
        ESP_LOGI(TAG,"Size %lu bytes",(unsigned long )partition->size);

        esp_http_client_config_t config={
            .url=update->firmware_url,
            .timeout_ms=15000,
            .buffer_size=DOWNLOAD_BUFFER_SIZE,
            .crt_bundle_attach=esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
        esp_err_t err=esp_http_client_open(client,0);
        if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t content_length=esp_http_client_fetch_headers(client);
    int status=esp_http_client_get_status_code(client);

    if (status!=200){
        ESP_LOGE(TAG,"firmware http status:%d",status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;

    }

    if (content_length <= 0 ||
        (uint64_t)content_length > partition->size) {

        ESP_LOGE (TAG, "Invalid firmware size: %" PRId64,
                 content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }


        esp_ota_handle_t ota_handle;
        err=esp_ota_begin(partition,OTA_SIZE_UNKNOWN,&ota_handle);

        if(err!=ESP_OK){
            ESP_LOGE(TAG,"esp_ota_begin failed : %s",esp_err_to_name(err));
             esp_http_client_close(client);
        esp_http_client_cleanup(client);
                return err;
        }


        mbedtls_sha256_context sha_ctx;
        mbedtls_sha256_init(&sha_ctx);

        if(mbedtls_sha256_starts(&sha_ctx,0)!=0){
            err=ESP_FAIL;
            goto fail;
        }

       buffer = malloc(DOWNLOAD_BUFFER_SIZE);

if (buffer == NULL) {
    err = ESP_ERR_NO_MEM;
    goto fail;
}
        unsigned char actual_hash[32];
        char actual_hex[65];
        int64_t total_written=0;
        ESP_LOGI(TAG, "SHA-256 context initialized; starting download");
        ESP_LOGI(TAG,"Downloading %" PRId64 "bytes",content_length);

        

        while(total_written<content_length){
            int to_read=DOWNLOAD_BUFFER_SIZE;
            int64_t remaining = content_length -total_written;
            if(remaining <to_read){
                to_read=(int)remaining;
            }

            int received=esp_http_client_read(client,(char *)buffer,to_read);

            if (received<=0){
                ESP_LOGE(TAG,"downloading interrupted at %"PRId64,total_written);
                err=ESP_FAIL;
                goto fail;
            }

            int sha_ret = mbedtls_sha256_update(
    &sha_ctx, buffer, (size_t)received);

if (sha_ret != 0) {
    ESP_LOGE(TAG, "SHA-256 update failed: %d", sha_ret);
    err = ESP_FAIL;
    goto fail;
}

            err=esp_ota_write(ota_handle,buffer,(size_t)received);

            if(err!=ESP_OK){
                ESP_LOGE(TAG,"esp_ota_write failed:%s",esp_err_to_name(err));
                goto fail;
            }

            total_written+=received;

        }

        int ret = mbedtls_sha256_finish(&sha_ctx, actual_hash);
if (ret != 0) {
    ESP_LOGE(TAG, "SHA-256 finish failed: %d", ret);
    err = ESP_FAIL;
    goto fail;
}

ESP_LOG_BUFFER_HEX(TAG, actual_hash, sizeof(actual_hash));

const char hex_chars[] = "0123456789abcdef";

for (int i = 0; i < 32; i++) {
    actual_hex[i * 2] = hex_chars[actual_hash[i] >> 4];
    actual_hex[i * 2 + 1] = hex_chars[actual_hash[i] & 0x0F];
}
actual_hex[64] = '\0';

mbedtls_sha256_free(&sha_ctx);

ESP_LOGI(TAG, "Downloaded: %" PRId64 " bytes", total_written);
ESP_LOGI(TAG, "Calculated SHA-256: %s", actual_hex);


    if (strcasecmp(actual_hex, update->sha256) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch; rejecting firmware");
        err = ESP_ERR_INVALID_CRC;
        goto fail_after_hash;
    }

err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot partition selection failed: %s",
                 esp_err_to_name(err));
        goto cleanup;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Firmware verified and installed. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; // Normally unreachable.

fail:
    mbedtls_sha256_free(&sha_ctx);

fail_after_hash:
    esp_ota_abort(ota_handle);

cleanup:
    if (buffer != NULL) {
        free(buffer);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
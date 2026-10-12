#pragma once
typedef struct {
    char firmware_url[256];
    char sha256[65];
    char version[32];
}ota_update_info_t;
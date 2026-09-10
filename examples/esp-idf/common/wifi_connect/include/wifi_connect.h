/* Minimal Wi-Fi station helper shared by the Art-Net examples. MIT licensed. */

#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise NVS, the network stack and Wi-Fi, then join the given network.
 * Blocks until an IPv4 address is assigned, retrying for as long as it takes.
 * Returns ESP_ERR_INVALID_ARG for an SSID over 32 bytes or a password over 64.
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONNECT_H */

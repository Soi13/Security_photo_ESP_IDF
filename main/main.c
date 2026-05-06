#include <stdio.h>
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "driver/gpio.h"


#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0      5

#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

#define FLASH_GPIO 4

#define WIFI_SSID "Soi13"
#define WIFI_PASS ""

//Telegram credentials
#define BOT_TOKEN ""
#define CHAT_ID ""

static const char *TAG = "ESP32-CAM";

void flash_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << FLASH_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
}

void telegram_send_photo(void)
{
    const char *host = "api.telegram.org";
    const int port = 443;

    gpio_set_level(FLASH_GPIO, 1); //Turn on flash
    vTaskDelay(pdMS_TO_TICKS(100));

    // 1. Capture image from camera
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        gpio_set_level(FLASH_GPIO, 0);
        return;
    }

    gpio_set_level(FLASH_GPIO, 0); //Turn off flash

    ESP_LOGI(TAG, "Image captured: %d bytes", fb->len);

    // 2. TLS config
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    struct esp_tls *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "TLS init failed");
        esp_camera_fb_return(fb);
        return;
    }

    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "TLS connect failed: %d", ret);
        esp_tls_conn_destroy(tls);
        esp_camera_fb_return(fb);
        return;
    }

    ESP_LOGI(TAG, "TLS connected");

    // 3. Multipart boundary
    const char *boundary = "----esp32cam";

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary, CHAT_ID, boundary
    );

    char footer[128];
    int footer_len = snprintf(footer, sizeof(footer),
        "\r\n--%s--\r\n",
        boundary
    );

    // 4. HTTP request header
    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "POST /bot%s/sendPhoto HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: esp32\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        BOT_TOKEN,
        host,
        boundary,
        header_len + fb->len + footer_len
    );

    // 5. Send HTTP header
    esp_tls_conn_write(tls, req, req_len);
    esp_tls_conn_write(tls, header, header_len);

    // 6. Send image (stream-safe)
    size_t sent = 0;
    while (sent < fb->len) {
        int w = esp_tls_conn_write(tls, (char *)fb->buf + sent, fb->len - sent);
        if (w <= 0) {
            ESP_LOGE(TAG, "Image write failed");
            goto cleanup;
        }
        sent += w;
    }

    // 7. Send footer
    esp_tls_conn_write(tls, footer, footer_len);

    ESP_LOGI(TAG, "Photo sent");

    // 8. Read response (optional)
    char buf[512];
    int r;
    do {
        r = esp_tls_conn_read(tls, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = 0;
            printf("%s", buf);
        }
    } while (r > 0);

cleanup:
    esp_tls_conn_destroy(tls);
    esp_camera_fb_return(fb);

    ESP_LOGI(TAG, "Done");
}

// Wifi event handler for displaying parameters of connection
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying to reconnect. The reason is %d ", event->reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi connected");
        ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Subnet Mask: " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
    }
}

// Initializing Wifi connection
static void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    ESP_ERROR_CHECK(esp_wifi_start());
    //ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(40)); //This method specifically for ESP32-C3, otherwise it will not connect to WiFi.
}

//Camera initialization
static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk  = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,

        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,

        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,

        .grab_mode = CAMERA_GRAB_WHEN_EMPTY
    };

    return esp_camera_init(&config);
}

void app_main(void)
{
    flash_init();
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed!");
        return;
    }
    ESP_LOGI(TAG, "Camera initialized");

    telegram_send_photo();
}

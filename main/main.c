#include <stdio.h>
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

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

#define WIFI_SSID "Soi13"
#define WIFI_PASS ""

//Telegram credentials
#define BOT_TOKEN ""
#define CHAT_ID ""

static const char *TAG = "ESP32-CAM";

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

        .pixel_format = PIXFORMAT_JPEG,  // important

        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,

        .grab_mode = CAMERA_GRAB_WHEN_EMPTY
    };

    return esp_camera_init(&config);
}

void send_photo(camera_fb_t *fb)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/sendPhoto",
        BOT_TOKEN);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .buffer_size_tx = 2048,
        //.skip_cert_common_name_check = true, // simplify TLS
        //.crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    const char *boundary = "----esp32boundary";

    char head[512];
    int head_len = snprintf(head, sizeof(head),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"CHAT_ID\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"cam.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary, CHAT_ID, boundary);

    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail),
        "\r\n--%s--\r\n", boundary);

    int total_len = head_len + fb->len + tail_len;

    esp_http_client_set_header(client,
        "Content-Type",
        "multipart/form-data; boundary=----esp32boundary");

    esp_http_client_open(client, total_len);

    esp_http_client_write(client, head, head_len);
    esp_http_client_write(client, (const char *)fb->buf, fb->len);
    esp_http_client_write(client, tail, tail_len);

    int status = esp_http_client_fetch_headers(client);
    ESP_LOGI("HTTP", "Status = %d", status);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void camera_task(void *pv)
{
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();

        if (fb) {
            send_photo(fb);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGE(TAG, "Capture failed");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/*//Capture image
void capture_photo(void)
{
    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        printf("Camera capture failed\n");
        return;
    }

    printf("Captured image: %d bytes\n", fb->len);

    // Example: just print first few bytes
    for (int i = 0; i < 10 && i < fb->len; i++) {
        printf("%02X ", fb->buf[i]);
    }
    printf("\n");

    esp_camera_fb_return(fb);
}*/

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed!");
        return;
    }
    ESP_LOGI(TAG, "Camera initialized");

    xTaskCreate(camera_task, "cam_task", 8192, NULL, 5, NULL);

    //capture_photo();
}

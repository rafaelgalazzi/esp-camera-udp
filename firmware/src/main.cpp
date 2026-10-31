#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "cJSON.h"
#include "protocol.hpp"
#include "xiao_sense.hpp"
#include "ov2640_profile.hpp"
#include "wifi_build_config.h"
#if WIFI_INI_CONFIGURED
#define INITIAL_WIFI_SSID WIFI_INI_SSID
#define INITIAL_WIFI_PASSWORD WIFI_INI_PASSWORD
#elif __has_include("secrets.h")
#include "secrets.h"
#else
#define INITIAL_WIFI_SSID ""
#define INITIAL_WIFI_PASSWORD ""
#endif

static const char *TAG = "iris_udp";
static EventGroupHandle_t events;
static SemaphoreHandle_t target_mutex;
static constexpr EventBits_t wifi_ready = BIT0;
static uint64_t camera_id;
static uint32_t boot_id;
static nvs_handle_t storage;
struct Target
{
    sockaddr_in address{};
    uint64_t server = 0;
    int64_t accepted_at = 0;
    unsigned fps = camera_fps;
};
static Target target;
static uint64_t paired_server = 0;
static std::atomic<bool> wifi_started{false};
static std::atomic<bool> scanning{false};

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && !scanning.load())
        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(events, wifi_ready);
        xSemaphoreTake(target_mutex, portMAX_DELAY);
        target.accepted_at = 0;
        xSemaphoreGive(target_mutex);
        const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%u; %s", unsigned(event->reason),
                 scanning.load() ? "diagnostic scan" : "retrying");
        if (!scanning.load())
            esp_wifi_connect();
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(events, wifi_ready);
        const auto *event = static_cast<const ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Wi-Fi ready; IP=" IPSTR, IP2STR(&event->ip_info.ip));
    }
}
static void scan_wifi()
{
    if (!wifi_started.load())
    {
        ESP_LOGW(TAG, "Configure Wi-Fi first, then run scan");
        return;
    }
    scanning.store(true);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    wifi_scan_config_t config{};
    config.show_hidden = true;
    ESP_LOGI(TAG, "Scanning Wi-Fi; streaming pauses until reconnection");
    esp_err_t result = esp_wifi_scan_start(&config, true);
    if (result == ESP_OK)
    {
        uint16_t total = 0;
        result = esp_wifi_scan_get_ap_num(&total);
        auto records = static_cast<wifi_ap_record_t *>(calloc(20, sizeof(wifi_ap_record_t)));
        uint16_t count = 20;
        if (result == ESP_OK && records)
        {
            result = esp_wifi_scan_get_ap_records(&count, records);
            if (result == ESP_OK)
            {
                ESP_LOGI(TAG, "Scan found %u networks; showing %u", unsigned(total), unsigned(count));
                for (uint16_t i = 0; i < count; ++i)
                    ESP_LOGI(TAG, "AP ssid=\"%.*s\" channel=%u rssi=%d auth=%u",
                             32, reinterpret_cast<const char *>(records[i].ssid),
                             unsigned(records[i].primary), int(records[i].rssi), unsigned(records[i].authmode));
            }
        }
        else if (!records)
            result = ESP_ERR_NO_MEM;
        free(records);
    }
    esp_wifi_clear_ap_list();
    if (result != ESP_OK)
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(result));
    scanning.store(false);
    esp_wifi_connect();
}

static void console_task(void *)
{
    // USB Serial/JTAG console accepts bounded newline-delimited JSON; no password echo.
    char line[384];
    size_t used = 0;
    bool overflow = false;
    for (;;)
    {
        int ch = getchar();
        if (ch == EOF)
        {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r')
            continue;
        if (ch != '\n')
        {
            if (used < sizeof(line) - 1)
                line[used++] = char(ch);
            else
                overflow = true;
            continue;
        }
        line[used] = 0;
        used = 0;
        if (overflow)
        {
            overflow = false;
            ESP_LOGW(TAG, "Command too long");
            continue;
        }
        cJSON *obj = cJSON_Parse(line);
        if (!obj)
        {
            ESP_LOGW(TAG, "Invalid JSON command");
            continue;
        }
        auto cmd = cJSON_GetObjectItemCaseSensitive(obj, "command");
        if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "wifi"))
        {
            auto ssid = cJSON_GetObjectItemCaseSensitive(obj, "ssid"), pass = cJSON_GetObjectItemCaseSensitive(obj, "password");
            if (cJSON_IsString(ssid) && cJSON_IsString(pass) && strlen(ssid->valuestring) > 0 && strlen(ssid->valuestring) <= 32 && strlen(pass->valuestring) <= 63)
            {
                ESP_ERROR_CHECK(nvs_set_str(storage, "ssid", ssid->valuestring));
                ESP_ERROR_CHECK(nvs_set_str(storage, "password", pass->valuestring));
                ESP_ERROR_CHECK(nvs_commit(storage));
                cJSON_Delete(obj);
                ESP_LOGI(TAG, "Wi-Fi saved; rebooting");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            else
                ESP_LOGW(TAG, "Expected ssid (1..32 bytes), password (0..63 bytes)");
        }
        else if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "unpair"))
        {
            ESP_ERROR_CHECK(nvs_set_u64(storage, "server", 0));
            ESP_ERROR_CHECK(nvs_commit(storage));
            cJSON_Delete(obj);
            ESP_LOGI(TAG, "Pairing cleared; rebooting");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        else if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "scan"))
            scan_wifi();
        else if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "status"))
        {
            char configured_ssid[33] = INITIAL_WIFI_SSID;
            size_t size = sizeof(configured_ssid);
            nvs_get_str(storage, "ssid", configured_ssid, &size);
            ESP_LOGI(TAG, "Configured SSID=\"%s\"", configured_ssid);
            ESP_LOGI(TAG, "camera=%016llx boot=%lu wifi=%d free_heap=%lu", (unsigned long long)camera_id, (unsigned long)boot_id,
                     !!(xEventGroupGetBits(events) & wifi_ready), (unsigned long)esp_get_free_heap_size());
        }
        else
            ESP_LOGW(TAG, "Commands: wifi, status, scan, unpair");
        cJSON_Delete(obj);
    }
}
static void discovery_task(void *)
{
    for (;;)
    {
        xEventGroupWaitBits(events, wifi_ready, pdFALSE, pdTRUE, portMAX_DELAY);
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        timeval timeout{0, 200000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
        {
            close(fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        sockaddr_in probe{};
        probe.sin_family = AF_INET;
        probe.sin_port = htons(discovery_port);
        probe.sin_addr.s_addr = INADDR_BROADCAST;
        if (*manual_bridge_ip && inet_pton(AF_INET, manual_bridge_ip, &probe.sin_addr) != 1)
            ESP_LOGE(TAG, "Invalid manual bridge IP; using broadcast");
        int64_t last_probe = 0, last_warning = 0;
        while (xEventGroupGetBits(events) & wifi_ready)
        {
            auto now = esp_timer_get_time();
            if (now - last_probe >= 1000000 || !last_probe)
            {
                iris::Header h;
                h.type = iris::Type::hello;
                h.camera = camera_id;
                h.boot = boot_id;
                h.server = paired_server ? paired_server : preferred_server;
                const char metadata[] = "xiao-esp32s3-sense;OV2640;iris-udp/1.0;JPEG;240x240;monochrome";
                h.payload = sizeof(metadata) - 1;
                uint8_t packet[iris::header_size + sizeof(metadata) - 1];
                iris::encode(packet, h);
                memcpy(packet + iris::header_size, metadata, h.payload);
                sendto(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&probe), sizeof(probe));
                last_probe = now;
            }
            uint8_t packet[256];
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            int n = recvfrom(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &len);
            iris::Header ack;
            if (n > 0 && iris::decode(packet, n, ack) && ack.type == iris::Type::accept && ack.camera == camera_id && ack.boot == boot_id && ack.server &&
                (!paired_server || ack.server == paired_server) && (!preferred_server || ack.server == preferred_server) && from.sin_port == htons(discovery_port))
            {
                const auto port = iris::read_be(packet + iris::header_size, 2), fps = iris::read_be(packet + iris::header_size + 2, 2);
                if (!port || fps < 1 || fps > 90)
                    continue;
                if (!paired_server)
                {
                    ESP_ERROR_CHECK(nvs_set_u64(storage, "server", ack.server));
                    ESP_ERROR_CHECK(nvs_commit(storage));
                    paired_server = ack.server;
                    ESP_LOGI(TAG, "Paired server=%016llx", (unsigned long long)paired_server);
                }
                from.sin_port = htons(uint16_t(port));
                xSemaphoreTake(target_mutex, portMAX_DELAY);
                target.address = from;
                target.server = ack.server;
                target.fps = std::min(unsigned(fps), camera_fps);
                target.accepted_at = esp_timer_get_time();
                xSemaphoreGive(target_mutex);
            }
            xSemaphoreTake(target_mutex, portMAX_DELAY);
            auto last_ack = target.accepted_at;
            xSemaphoreGive(target_mutex);
            if (now - last_ack > 5000000 && now - last_warning > 10000000)
            {
                ESP_LOGW(TAG, "No bridge ACK: check bridge, UDP 5001 firewall, client isolation, or saved server pairing");
                last_warning = now;
            }
        }
        close(fd);
    }
}
static void stream_task(void *)
{
    uint32_t frame_id = 0;
    int fd = -1;
    for (;;)
    {
        xSemaphoreTake(target_mutex, portMAX_DELAY);
        Target dest = target;
        xSemaphoreGive(target_mutex);
        if (!(xEventGroupGetBits(events) & wifi_ready) || !dest.accepted_at || esp_timer_get_time() - dest.accepted_at > 4000000)
        {
            if (fd >= 0)
            {
                close(fd);
                fd = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (fd < 0)
        {
            fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (fd < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            fcntl(fd, F_SETFL, O_NONBLOCK);
        }
        int64_t started = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
        {
            ESP_LOGW(TAG, "Capture failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (fb->format == PIXFORMAT_JPEG && fb->len >= 4 && fb->len <= iris::max_frame_size)
        {
            iris::Header h;
            h.type = iris::Type::jpeg;
            h.camera = camera_id;
            h.boot = boot_id;
            h.server = dest.server;
            h.frame = ++frame_id;
            h.capture_us = uint64_t(fb->timestamp.tv_sec) * 1000000 + fb->timestamp.tv_usec;
            h.total = fb->len;
            h.count = (fb->len + iris::chunk_size - 1) / iris::chunk_size;
            uint8_t packet[iris::header_size];
            iovec parts[2]{};
            parts[0].iov_base = packet;
            parts[0].iov_len = sizeof(packet);
            msghdr message{};
            message.msg_name = &dest.address;
            message.msg_namelen = sizeof(dest.address);
            message.msg_iov = parts;
            message.msg_iovlen = 2;
            for (h.index = 0; h.index < h.count; ++h.index)
            {
                size_t offset = h.index * iris::chunk_size;
                h.payload = std::min(iris::chunk_size, fb->len - offset);
                iris::encode(packet, h);
                // lwIP builds one datagram from both slices; avoid an extra app-side copy.
                // Keep the framebuffer until all synchronous sendmsg calls finish.
                parts[1].iov_base = fb->buf + offset;
                parts[1].iov_len = h.payload;
                auto n = sendmsg(fd, &message, MSG_DONTWAIT);
                if (n != int(iris::header_size + h.payload))
                    break; // Drop rest of frame; never queue/retry old frames.
            }
        }
        esp_camera_fb_return(fb);
        int64_t wait = 1000000 / dest.fps - (esp_timer_get_time() - started);
        vTaskDelay(pdMS_TO_TICKS(std::max<int64_t>(1, wait / 1000)));
    }
}
extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("iris", NVS_READWRITE, &storage));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    camera_id = iris::read_be(mac, 6);
    boot_id = esp_random();
    if (!boot_id)
        boot_id = 1;
    nvs_get_u64(storage, "server", &paired_server);
    events = xEventGroupCreate();
    target_mutex = xSemaphoreCreateMutex();
    if (!events || !target_mutex)
        abort();
    ESP_LOGI(TAG, "camera=%016llx boot=%lu; configure Wi-Fi over serial JSON", (unsigned long long)camera_id, (unsigned long)boot_id);
#if WIFI_INI_CONFIGURED
    char applied_build[33]{};
    size_t applied_size = sizeof(applied_build);
    const auto applied_result = nvs_get_str(storage, "wifi_build", applied_build, &applied_size);
    if (applied_result != ESP_OK || strcmp(applied_build, WIFI_BUILD_ID) != 0)
    {
        ESP_ERROR_CHECK(nvs_set_str(storage, "ssid", WIFI_INI_SSID));
        ESP_ERROR_CHECK(nvs_set_str(storage, "password", WIFI_INI_PASSWORD));
        ESP_ERROR_CHECK(nvs_set_str(storage, "wifi_build", WIFI_BUILD_ID));
        ESP_ERROR_CHECK(nvs_commit(storage));
        ESP_LOGI(TAG, "New build: Wi-Fi configuration applied from project wifi.ini");
    }
#endif
    char ssid[33] = INITIAL_WIFI_SSID, password[65] = INITIAL_WIFI_PASSWORD;
    size_t len = sizeof(ssid);
    nvs_get_str(storage, "ssid", ssid, &len);
    len = sizeof(password);
    nvs_get_str(storage, "password", password, &len);
    ESP_LOGI(TAG, "Configured SSID=\"%s\"", ssid);
    if (xTaskCreate(console_task, "console", 4096, nullptr, 3, nullptr) != pdPASS)
        abort();
    if (!*ssid)
    {
        ESP_LOGW(TAG, "No Wi-Fi configured. Send {\"command\":\"wifi\",\"ssid\":\"...\",\"password\":\"...\"}");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t wifi{};
    memcpy(wifi.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi.sta.password, password, strlen(password));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_started.store(true);
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    // Registration remains available even if camera initialization needs retries.
    if (xTaskCreate(discovery_task, "discovery", 4096, nullptr, 4, nullptr) != pdPASS)
        abort();
    auto cam = camera_config();
    while (esp_camera_init(&cam) != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera init failed; check OV2640, ribbon and PSRAM; retry in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    auto sensor = esp_camera_sensor_get();
    ESP_LOGI(TAG, "Sensor PID=0x%04x (OV2640 expected: 0x%04x)", sensor->id.PID, OV2640_PID);
    if (!apply_ov2640_profile(sensor))
    {
        ESP_LOGE(TAG, "OV2640 monochrome profile failed; video disabled, USB remains available");
        return;
    }
    sensor->set_hmirror(sensor, camera_mirror);
    sensor->set_vflip(sensor, camera_flip);
    // Discard frames captured while the sensor profile was being changed.
    for (unsigned i = 0; i < 2; ++i) {
        auto stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }
    ESP_LOGI(TAG, "OV2640 monochrome JPEG 240x240 ready; quality=%d, FPS ceiling=%u",
             camera_jpeg_quality, camera_fps);
    if (xTaskCreate(stream_task, "stream", 6144, nullptr, 5, nullptr) != pdPASS)
        abort();
}

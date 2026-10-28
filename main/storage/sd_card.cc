// sd_card.cc — SD 卡服务实现（SPI 模式；从 dms-pager 移植并裁剪）
#include "storage/sd_card.h"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "ff.h"

#include "boards/esp32s3_lucky/config.h"

#define TAG "SdCard"

namespace sd_card {
namespace {

bool s_mounted = false;
bool s_initialized = false;
sdmmc_card_t* s_card = nullptr;
esp_timer_handle_t s_timer = nullptr;
char s_summary[40] = "";

void FillSummary() {
    s_summary[0] = '\0';
    if (s_card == nullptr) return;
    snprintf(s_summary, sizeof(s_summary), "%s %.1fGB", s_card->cid.name,
             (double)s_card->csd.capacity / (1024.0 * 1024.0 * 2.0));   // 扇区数×512B
}

int MountOnce() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_PIN_MOSI;
    bus.miso_io_num = SD_PIN_MISO;
    bus.sclk_io_num = SD_PIN_SCK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize((spi_host_device_t)SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return (int)err;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_PIN_CS;
    slot.host_id = (spi_host_device_t)SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;
    mcfg.max_files = 10;
    mcfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载失败: %s（无卡/接触不良/格式不支持均可能）",
                 esp_err_to_name(err));
        spi_bus_free((spi_host_device_t)SD_SPI_HOST);
        s_card = nullptr;
        return (int)err;
    }
    s_mounted = true;
    FillSummary();
    ESP_LOGI(TAG, "SD 卡已挂载：%s", s_summary);
    return 0;
}

void DeferredMount(void* arg) {
    (void)arg;
    if (s_mounted) return;
    for (int attempt = 1; attempt <= 3 && !s_mounted; ++attempt) {
        int ret = MountOnce();
        if (ret == 0) return;
        ESP_LOGW(TAG, "SD 卡尝试 %d/3 失败", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "SD 卡不可用 → 继续运行（素材回落 flash 槽）");
}

}  // namespace

void Initialize() {
    if (s_initialized) return;
    s_initialized = true;
    esp_timer_create_args_t args = {};
    args.callback = DeferredMount;
    args.name = "sd_mount";
    args.dispatch_method = ESP_TIMER_TASK;
    args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        ESP_LOGW(TAG, "挂载定时器创建失败，改为立即尝试");
        DeferredMount(nullptr);
        return;
    }
    esp_timer_start_once(s_timer, 1000 * 1000);
}

bool IsMounted() { return s_mounted; }
const char* MountPoint() { return SD_MOUNT_POINT; }
const char* CardSummary() { return s_summary; }

int Mount() {
    if (s_mounted) return 0;
    int ret = MountOnce();
    if (ret != 0) {
        for (int i = 0; i < 2 && !s_mounted; ++i) {
            vTaskDelay(pdMS_TO_TICKS(300));
            ret = MountOnce();
        }
    }
    return ret;
}

int Unmount() {
    if (!s_mounted || s_card == nullptr) return 0;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = nullptr;
        s_mounted = false;
        s_summary[0] = '\0';
        spi_bus_free((spi_host_device_t)SD_SPI_HOST);
        ESP_LOGI(TAG, "SD 卡已卸载");
    } else {
        ESP_LOGW(TAG, "SD 卡卸载失败: %s", esp_err_to_name(err));
    }
    return (int)err;
}

bool Capacity(uint64_t* total, uint64_t* used, uint64_t* free_bytes) {
    if (!s_mounted) return false;
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || fs == nullptr) return false;
    const uint64_t t = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
    const uint64_t f = (uint64_t)free_clusters * fs->csize * 512;
    if (total) *total = t;
    if (free_bytes) *free_bytes = f;
    if (used) *used = t - f;
    return true;
}

}  // namespace sd_card

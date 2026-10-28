// sd_card.h — SD 卡服务（SPI 模式，2026-10-28 移植自 dms-pager）
//
// 与 dms-pager 的差异：S3 只保留 SPI 后端（SDMMC 引脚不成组），保留
// 「延迟挂载 + 3 次重试 + 失败降级 + 容量查询」的行为。
// 任何失败都只打日志：SD 卡不可用绝不影响抽奖/打印/Web（规则 7）。
#ifndef LUCKY_PAGE_STORAGE_SD_CARD_H_
#define LUCKY_PAGE_STORAGE_SD_CARD_H_

#include <driver/sdmmc_types.h>

#include <cstdint>

namespace sd_card {

/// 启动后延迟 1s 挂载（避免阻塞开机）；重复调用安全
void Initialize();
bool IsMounted();
const char* MountPoint();                 // "/sdcard"

/// 手动挂载/卸载（Web 接口用）；返回 esp_err_t 但用 int 暴露避免头文件污染
int Mount();
int Unmount();

/// 容量（字节）；未挂载返回 false
bool Capacity(uint64_t* total, uint64_t* used, uint64_t* free_bytes);

/// 卡信息摘要（"SD16G 29.7GB"），未挂载返回空
const char* CardSummary();

}  // namespace sd_card

#endif  // LUCKY_PAGE_STORAGE_SD_CARD_H_

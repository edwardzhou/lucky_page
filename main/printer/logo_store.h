#ifndef _LOGO_STORE_H_
#define _LOGO_STORE_H_

#include <cstddef>
#include <cstdint>

#include "printer/printer_logo.h"

/// 用户素材（logo/边框图）存储 —— **独立 raw flash 分区**（设计 §6.4 / D15）。
///
/// 为什么不用 SPIFFS：assets 分区是**构建期生成**的镜像，每次 `idf.py flash`
/// 都会整块重写 → 用户上传的图被冲掉（真机多次踩坑：打测试页发现 logo 没了）。
/// 放进独立分区后，烧固件/OTA 都不影响；单张图也不需要一个文件系统。
///
/// 结构：`LOGO` + len(u32) + crc32(u32) + LGO1 数据（不足部分为 0xFF）。
/// 写入前先擦除，写后校验；任一步失败都不会破坏已有图（D15）。
class LogoStore {
public:
    /// 旧位置（仅用于一次性迁移；迁移后不再使用）
    static constexpr const char* kLegacyPath = "/spiffs/logo.bin";

    /// 写入（覆盖）。len 必须 ≤ PrinterLogo::kMaxLogoBytes。
    static bool Save(const uint8_t* buf, size_t len);

    /// 读到 out（调用方保证容量 ≥ kMaxLogoBytes）；out_len 返回实际长度。
    static bool Load(uint8_t* out, size_t out_cap, size_t* out_len);

    static bool Remove();
    static bool Exists();

    /// 仅读头部做完整校验（不加载整张位图）
    static LogoInfo ReadInfo();

    /// 从旧的 SPIFFS 位置迁移一次（存在旧图且新分区为空时）
    static void MigrateFromSpiffsIfNeeded();
};

#endif  // _LOGO_STORE_H_

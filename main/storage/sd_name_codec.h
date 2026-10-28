// sd_name_codec.h — SD 文件名编码转换（UTF-8 ↔ GBK/CP936），纯函数可单测
// 为什么需要：FATFS 配置 CP936（与 dms-pager 卡上的中文名一致）时，
// VFS 期待 GBK 字节；而 Web 层全是 UTF-8。
#ifndef LUCKY_PAGE_STORAGE_SD_NAME_CODEC_H_
#define LUCKY_PAGE_STORAGE_SD_NAME_CODEC_H_

#include <string>

namespace sd_name_codec {

/// UTF-8 → GBK（不可编码字符降级为 '?'，与打印链路同一套表）
std::string ToFs(const std::string& utf8);

/// GBK → UTF-8（非法字节/未收录 → '?'）
std::string FromFs(const std::string& gbk);

}  // namespace sd_name_codec

#endif  // LUCKY_PAGE_STORAGE_SD_NAME_CODEC_H_

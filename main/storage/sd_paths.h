// sd_paths.h — SD 路径与素材文件名的纯逻辑（可单测）
// 规则：一切路径都限制在 /sdcard 下；素材固定目录 /sdcard/lucky/{assets,sounds}
#ifndef LUCKY_PAGE_STORAGE_SD_PATHS_H_
#define LUCKY_PAGE_STORAGE_SD_PATHS_H_

#include <string>

namespace sd_paths {

constexpr const char* kRoot = "/sdcard";
constexpr const char* kAssetDir = "/sdcard/lucky/assets";
constexpr const char* kSoundDir = "/sdcard/lucky/sounds";

/// 相对路径规范化：必须以 / 开头，拒绝 ..、空段、反斜杠；返回 "" 表示非法
std::string Normalize(const std::string& rel);

/// 规范化后的相对路径 → /sdcard 下的绝对路径（非法返回 ""）
std::string Full(const std::string& rel);

/// 素材图路径：/sdcard/lucky/assets/<id>.lgo（id 非法返回 ""）
std::string AssetFile(int id);

/// 音效路径：/sdcard/lucky/sounds/<role>.pcm（role 只能是 welcome/win/lose）
std::string SoundFile(const char* role);

/// 文件名合法性（上传/重命名用）：非空、≤120 字节、无 / \ : * ? " < > | 与控制字符
bool ValidName(const std::string& name);

}  // namespace sd_paths

#endif  // LUCKY_PAGE_STORAGE_SD_PATHS_H_

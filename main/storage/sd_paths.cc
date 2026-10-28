// sd_paths.cc — SD 路径纯逻辑实现
#include "storage/sd_paths.h"

#include <cctype>
#include <vector>

namespace sd_paths {
namespace {

bool SafeChar(unsigned char c) {
    if (c < 0x20 || c == 0x7F) return false;
    switch (c) {
        case '/': case '\\': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            return false;
        default:
            return true;
    }
}

}  // namespace

std::string Normalize(const std::string& rel) {
    if (rel.empty() || rel[0] != '/') return "";
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 1; i <= rel.size(); ++i) {
        if (i == rel.size() || rel[i] == '/') {
            if (cur.empty()) {
                if (i == rel.size()) break;
                continue;                      // 折叠重复 /
            }
            if (cur == "." ) { cur.clear(); continue; }
            if (cur == "..") return "";        // 禁止上跳
            for (unsigned char c : cur) {
                if (!SafeChar(c) && c != '/') return "";
            }
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(rel[i]);
        }
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string Full(const std::string& rel) {
    std::string norm = Normalize(rel);
    if (norm.empty()) return "";
    return std::string(kRoot) + (norm == "/" ? "" : norm);
}

std::string AssetFile(int id) {
    if (id < 0 || id >= 48) return "";
    return std::string(kAssetDir) + "/" + std::to_string(id) + ".lgo";
}

std::string SoundFile(const char* role) {
    if (role == nullptr) return "";
    std::string r(role);
    if (r != "welcome" && r != "win" && r != "lose") return "";
    return std::string(kSoundDir) + "/" + r + ".pcm";
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 120) return false;
    if (name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (!SafeChar(c)) return false;
    }
    return true;
}

}  // namespace sd_paths

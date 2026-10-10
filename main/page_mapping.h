#ifndef _PAGE_MAPPING_H_
#define _PAGE_MAPPING_H_

#include <cstdint>
#include <string>
#include <vector>

/// 页码→DMX 行程值映射管理。
/// 内存缓存 + SPIFFS JSON 持久化，GetMapping() 零开销。
class PageMapping {
public:
    static PageMapping& GetInstance();

    /// 返回内存缓存引用，O(1)。首次调用时自动从 SPIFFS 加载。
    const std::vector<uint8_t>& GetMapping();

    /// 返回页码名称列表引用，O(1)。首次调用时自动从 SPIFFS 加载。
    const std::vector<std::string>& GetPageNames();

    /// 用自定义值数组重建映射并保存 SPIFFS。
    /// names 非空时同时更新页码名称。
    void SetMapping(const std::vector<uint8_t>& values,
                    const std::vector<std::string>& names = {});

    /// 设置页码名称并保存 SPIFFS。
    void SetPageNames(const std::vector<std::string>& names);

    /// 根据名称查找页码索引，未找到返回 -1。大小写敏感。
    int GetMappingByName(const std::string& name);

    /// 按公式均匀分配（255/N），保存 SPIFFS。
    void SetDefaultMapping(int total_pages);

    /// 从 SPIFFS JSON 加载到内存。文件不存在则使用默认值。
    void LoadMapping();

    /// 将当前内存映射保存到 SPIFFS JSON。
    void SaveMapping();

    PageMapping(const PageMapping&) = delete;
    PageMapping& operator=(const PageMapping&) = delete;

private:
    PageMapping() = default;

    /// 生成默认页码名称 "P1", "P2", "P3", ...
    void GenerateDefaultNames(int total_pages);

    std::vector<uint8_t> mapping_;
    std::vector<std::string> page_names_;
    bool loaded_ = false;
};

#endif  // _PAGE_MAPPING_H_

/// 行程表校验（纯函数，设计 §4.2b 断言 / Task 2.1d Step 5）：
/// ① 长度必须 == total_pages ② 两两互异 ③ 相邻差 >= min_gap
/// ④ 值落在 [stroke_min, stroke_max]。
/// @return 空串 = 通过；否则中文错误原因（Web 400 直接回传）
std::string ValidatePageMapping(const std::vector<uint8_t>& values, int total_pages,
                                int min_gap, int stroke_min, int stroke_max);

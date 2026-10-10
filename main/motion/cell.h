#ifndef _CELL_H_
#define _CELL_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

/// DMX 通道数据单元。
///
/// 每个 Cell 表示一个 DMX 起始通道号及其后续最多 7 个连续通道的值。
/// 多个 Action 同时写入同一 Cell 时，使用 MergeFrom 的"非零值覆盖"语义合并。
/// Cell 同时携带网格坐标 (y, x)，用于标识其在像素矩阵中的位置。
class Cell {
public:
    /// 单个 Cell 最大支持的连续通道数。
    /// 每个灯具 7 个通道（Page + CH1-CH6），适配当前灯具协议。
    static constexpr size_t kMaxChannels = 7;

    Cell() : channel_number_(0), y_(0), x_(0) { memset(data_, 0, sizeof(data_)); }

    /// 构造指定起始通道号的空 Cell（所有通道值为 0）。
    /// @param channel_number DMX 起始通道号 (1-512)
    explicit Cell(int channel_number) : channel_number_(channel_number), y_(0), x_(0) {
        memset(data_, 0, sizeof(data_));
    }

    /// 从数据数组构造 Cell。
    /// @param channel_number DMX 起始通道号 (1-512)
    /// @param data 通道值数组
    /// @param len 数据长度，超过 kMaxChannels 的部分会被截断
    Cell(int channel_number, const uint8_t* data, size_t len)
        : channel_number_(channel_number), y_(0), x_(0) {
        memset(data_, 0, sizeof(data_));
        if (data && len > 0) {
            size_t copy_len = len > kMaxChannels ? kMaxChannels : len;
            memcpy(data_, data, copy_len);
        }
    }

    Cell(const Cell& other)
        : channel_number_(other.channel_number_), y_(other.y_), x_(other.x_) {
        memcpy(data_, other.data_, sizeof(data_));
    }

    Cell& operator=(const Cell& other) {
        if (this != &other) {
            channel_number_ = other.channel_number_;
            y_ = other.y_;
            x_ = other.x_;
            memcpy(data_, other.data_, sizeof(data_));
        }
        return *this;
    }

    /// 将所有通道值清零。
    void Reset() { memset(data_, 0, sizeof(data_)); }

    /// 使用"非零值覆盖"语义合并另一个 Cell 的数据。
    ///
    /// 遍历所有通道位置，仅当 other 的通道值为非零时覆盖当前值。
    /// 这确保了零值（表示"不改变"）不会覆盖已有的非零值。
    /// 常用于 Pipeline 合并多个并行 Action 的输出。
    ///
    /// @param other 要合并的源 Cell
    void MergeFrom(const Cell& other) {
        for (size_t i = 0; i < kMaxChannels; ++i) {
            if (other.data_[i] != 0) {
                data_[i] = other.data_[i];
            }
        }
    }

    /// 比较两个 Cell 是否完全相同（通道号和所有通道值）。
    /// @param other 要比较的 Cell
    /// @return true 表示完全相同
    bool Equals(const Cell& other) const {
        return channel_number_ == other.channel_number_ &&
               memcmp(data_, other.data_, sizeof(data_)) == 0;
    }

    bool operator==(const Cell& other) const { return Equals(other); }
    bool operator!=(const Cell& other) const { return !Equals(other); }

    /// 设置指定偏移位置的通道值。
    /// @param index 通道偏移（0 = 起始通道，1 = 起始通道+1，...）
    /// @param value 通道值 (0-255)
    void SetData(size_t index, uint8_t value) {
        if (index < kMaxChannels) {
            data_[index] = value;
        }
    }

    /// 获取指定偏移位置的通道值。
    /// @param index 通道偏移
    /// @return 通道值，越界返回 0
    uint8_t GetData(size_t index) const {
        return index < kMaxChannels ? data_[index] : 0;
    }

    /// 获取内部数据数组指针。
    /// @return 指向 kMaxChannels 字节的只读数组
    const uint8_t* Data() const { return data_; }

    /// 获取 DMX 起始通道号。
    int ChannelNumber() const { return channel_number_; }

    /// 设置 DMX 起始通道号。
    void SetChannelNumber(int channel_number) {
        channel_number_ = channel_number;
    }

    int Y() const { return y_; }
    int X() const { return x_; }
    void SetY(int y) { y_ = y; }
    void SetX(int x) { x_ = x; }

private:
    uint8_t data_[kMaxChannels] = {};  // 连续通道值数组
    int channel_number_ = 0;            // DMX 起始通道号 (1-512)
    int y_ = 0;                         // 行坐标
    int x_ = 0;                         // 列坐标
};

#endif  // _CELL_H_

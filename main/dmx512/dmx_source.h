#ifndef _DMX_SOURCE_H_
#define _DMX_SOURCE_H_

#include <cstdint>

/// DMX512 数据源抽象接口。
///
/// SendTask 每帧调用 GetFrame() 获取数据，统一发送路径。
/// 实现：MotionEngineSource、DmxFileSource、ArtNetSource。
class DmxSource {
public:
    virtual ~DmxSource() = default;

    /// 获取一帧 DMX 数据。
    /// @param bufs 输出 buffer 数组，每路 512 字节
    /// @param channel_count 输出实际路数（1-4）
    /// @return true=有新数据可发，false=空闲（SendTask 发上一帧）
    virtual bool GetFrame(uint8_t bufs[][512], int* channel_count) = 0;
};

#endif  // _DMX_SOURCE_H_

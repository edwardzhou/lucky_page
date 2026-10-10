// test_dmx_source.cc — DMX 帧源（真机踩坑回归：GetFrame 的 channel_count 空指针）
#include "lucky_test.h"

#include "dmx512/motion_engine_source.h"
#include "motion/motion_engine.h"
#include "settings.h"

TEST(DmxSource, NullEngineReturnsFalse) {
    // 引擎为空 → 必须安全返回 false（不得解引用）
    MotionEngineSource src(nullptr);
    uint8_t bufs[1][512] = {};
    int count = -1;
    EXPECT_FALSE(src.GetFrame(bufs, &count));
}

TEST(DmxSource, ValidEngineFillsFrameAndCount) {
    MotionEngine engine(1, (uint8_t)Settings::GetInstance().paging_channel_count());
    engine.Start();
    MotionEngineSource src(&engine);
    uint8_t bufs[1][512] = {};
    int count = 0;
    EXPECT_TRUE(src.GetFrame(bufs, &count));
    EXPECT_EQ(1, count);            // 单机模式：固定 1 路
    engine.Stop();
}

TEST(DmxSource, NullCountPointerDoesNotCrash) {
    // 真机回归：曾给 GetFrame 传 nullptr 当 channel_count → StoreProhibited（写地址 0）
    MotionEngine engine(1, (uint8_t)Settings::GetInstance().paging_channel_count());
    engine.Start();
    MotionEngineSource src(&engine);
    uint8_t bufs[1][512] = {};
    EXPECT_TRUE(src.GetFrame(bufs, nullptr));   // 必须容错，不得崩溃
    engine.Stop();
}

TEST(DmxSource, NullBufferReturnsFalse) {
    MotionEngine engine(1, (uint8_t)Settings::GetInstance().paging_channel_count());
    engine.Start();
    MotionEngineSource src(&engine);
    int count = 0;
    EXPECT_FALSE(src.GetFrame(nullptr, &count));
    engine.Stop();
}

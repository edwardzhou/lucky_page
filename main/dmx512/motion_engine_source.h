#ifndef _MOTION_ENGINE_SOURCE_H_
#define _MOTION_ENGINE_SOURCE_H_

#include "dmx512/dmx_source.h"

class MotionEngine;

class MotionEngineSource : public DmxSource {
public:
    explicit MotionEngineSource(MotionEngine* engine) : engine_(engine) {}

    bool GetFrame(uint8_t bufs[][512], int* channel_count) override;

private:
    MotionEngine* engine_;
};

#endif  // _MOTION_ENGINE_SOURCE_H_

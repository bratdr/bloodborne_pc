// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Intel XeSS (Xe Super Sampling) as an alternative to FSR 3.1/4, DLSS and TAA,
// loaded at run time through libxess.dll (Windows) or libxess.so (Linux): no Intel SDK
// headers or libraries needed at build time.
//
// It consumes the FSR 4 frame (vk_fsr4.h): scene color, depth, motion vectors in render pixels
// and the jitter offset; the output is written in General layout.

#pragma once

#include <memory>
#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

class XessUpscaler {
public:
    XessUpscaler(const Instance& instance, Scheduler& scheduler);
    ~XessUpscaler();

    /// libxess initialized and XeSS is available on this GPU and driver.
    [[nodiscard]] bool Available() const noexcept;

    /// Records XeSS into `frame.cmdbuf` (the frame's preset picks the XeSS quality mode).
    /// False when XeSS cannot run; Problem() says why.
    bool Record(const Fsr4Upscaler::Frame& frame);

    [[nodiscard]] const char* Problem() const noexcept;
    [[nodiscard]] bool Fatal() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan

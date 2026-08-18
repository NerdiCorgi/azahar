// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_headless/headless_window.h"

#include <algorithm>
#include <cstdio>
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
namespace {

constexpr u32 OutputWidth = 400;
constexpr u32 OutputHeight = 480;
constexpr size_t RgbaFrameBytes = static_cast<size_t>(OutputWidth) * OutputHeight * 4;
constexpr size_t YuvFrameBytes = static_cast<size_t>(OutputWidth) * OutputHeight * 3 / 2;

u8 ClampToByte(int value) {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

void ConvertRgbaToYuv420p(const std::vector<u8>& rgba_frame, std::vector<u8>& yuv_frame) {
    u8* const y_plane = yuv_frame.data();
    u8* const u_plane = y_plane + OutputWidth * OutputHeight;
    u8* const v_plane = u_plane + (OutputWidth / 2) * (OutputHeight / 2);

    for (u32 y = 0; y < OutputHeight; ++y) {
        for (u32 x = 0; x < OutputWidth; ++x) {
            const size_t offset = (static_cast<size_t>(y) * OutputWidth + x) * 4;
            const int r = rgba_frame[offset + 0];
            const int g = rgba_frame[offset + 1];
            const int b = rgba_frame[offset + 2];
            y_plane[static_cast<size_t>(y) * OutputWidth + x] =
                ClampToByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (u32 y = 0; y < OutputHeight; y += 2) {
        for (u32 x = 0; x < OutputWidth; x += 2) {
            int r_sum = 0;
            int g_sum = 0;
            int b_sum = 0;

            for (u32 block_y = 0; block_y < 2; ++block_y) {
                for (u32 block_x = 0; block_x < 2; ++block_x) {
                    const size_t offset =
                        (static_cast<size_t>(y + block_y) * OutputWidth + (x + block_x)) * 4;
                    r_sum += rgba_frame[offset + 0];
                    g_sum += rgba_frame[offset + 1];
                    b_sum += rgba_frame[offset + 2];
                }
            }

            const int r = r_sum / 4;
            const int g = g_sum / 4;
            const int b = b_sum / 4;
            const size_t chroma_offset = static_cast<size_t>(y / 2) * (OutputWidth / 2) + x / 2;
            u_plane[chroma_offset] = ClampToByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            v_plane[chroma_offset] = ClampToByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

} // namespace

HeadlessWindow::HeadlessWindow(
    bool raw_video_stdout_enabled_,
    std::optional<std::chrono::steady_clock::duration> raw_video_frame_period_)
    : raw_video_stdout_enabled{raw_video_stdout_enabled_},
      raw_video_frame_period{raw_video_frame_period_} {
    window_info.type = Frontend::WindowSystemType::Headless;
    UpdateCurrentFramebufferLayout(Core::kScreenTopWidth,
                                   Core::kScreenTopHeight + Core::kScreenBottomHeight);
    if (raw_video_stdout_enabled) {
        rgba_frame.resize(RgbaFrameBytes);
        yuv_frame.resize(YuvFrameBytes);
    }
}

void HeadlessWindow::PollEvents() {
    ProcessConfigurationChanges();
    MaybeWriteRawVideoFrame();
}

void HeadlessWindow::SwapBuffers() {}

void HeadlessWindow::MaybeWriteRawVideoFrame() {
    if (!raw_video_stdout_enabled) {
        return;
    }

    auto& system = Core::System::GetInstance();
    auto& renderer_base = system.GPU().Renderer();
    const s32 current_frame = renderer_base.GetCurrentFrame();
    if (current_frame == last_raw_video_frame) {
        return;
    }
    last_raw_video_frame = current_frame;

    const auto now = std::chrono::steady_clock::now();
    if (raw_video_frame_period.has_value() && last_raw_video_frame_write_time.has_value() &&
        now - *last_raw_video_frame_write_time < *raw_video_frame_period) {
        return;
    }

    if (!renderer_base.TryCaptureFrameRGBA(GetFramebufferLayout(), rgba_frame)) {
        return;
    }

    ConvertRgbaToYuv420p(rgba_frame, yuv_frame);

    if (std::fwrite(yuv_frame.data(), 1, yuv_frame.size(), stdout) != yuv_frame.size() ||
        std::fflush(stdout) != 0) {
        raw_video_stdout_enabled = false;
        std::clearerr(stdout);
        std::fprintf(stderr, "Raw video stdout disabled after write failure\n");
        return;
    }

    last_raw_video_frame_write_time = now;
}

void HeadlessWindow::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    UpdateCurrentFramebufferLayout(minimal_size.first, minimal_size.second);
}

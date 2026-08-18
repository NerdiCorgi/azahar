// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "citra_headless/headless_window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/renderer_software/renderer_software.h"

namespace {

constexpr u32 OutputWidth = 400;
constexpr u32 OutputHeight = 480;
constexpr u32 TopScreenHeight = 240;
constexpr u32 BottomScreenX = 40;
constexpr size_t RgbaFrameBytes = static_cast<size_t>(OutputWidth) * OutputHeight * 4;
constexpr size_t YuvFrameBytes = static_cast<size_t>(OutputWidth) * OutputHeight * 3 / 2;

u8 ClampToByte(int value) {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

void WritePixel(std::vector<u8>& rgba_frame, u32 x, u32 y, const u8* rgba) {
    const size_t offset = (static_cast<size_t>(y) * OutputWidth + x) * 4;
    std::memcpy(rgba_frame.data() + offset, rgba, 4);
}

void BlitScreen(std::vector<u8>& rgba_frame, const SwRenderer::ScreenInfo& info, u32 dst_x,
                u32 dst_y, u32 dst_width, u32 dst_height) {
    if (info.pixels.empty() || info.width == 0 || info.height == 0 || dst_width == 0 ||
        dst_height == 0 || dst_x >= OutputWidth || dst_y >= OutputHeight) {
        return;
    }

    const u32 blit_width = std::min(dst_width, OutputWidth - dst_x);
    const u32 blit_height = std::min(dst_height, OutputHeight - dst_y);
    const u32 native_width = info.height;
    const u32 native_height = info.width;

    for (u32 y = 0; y < blit_height; ++y) {
        const u32 src_y = static_cast<u32>((static_cast<u64>(y) * native_height) / blit_height);
        for (u32 x = 0; x < blit_width; ++x) {
            const u32 src_x = static_cast<u32>((static_cast<u64>(x) * native_width) / blit_width);
            const size_t src_offset = (static_cast<size_t>(src_y) * info.height + src_x) * 4;
            if (src_offset + 3 >= info.pixels.size()) {
                continue;
            }
            WritePixel(rgba_frame, dst_x + x, dst_y + y, info.pixels.data() + src_offset);
        }
    }
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

    const auto& renderer = static_cast<const SwRenderer::RendererSoftware&>(renderer_base);

    std::fill(rgba_frame.begin(), rgba_frame.end(), 0);
    for (size_t i = 3; i < rgba_frame.size(); i += 4) {
        rgba_frame[i] = 255;
    }

    BlitScreen(rgba_frame, renderer.Screen(VideoCore::ScreenId::TopLeft), 0, 0, OutputWidth,
               TopScreenHeight);
    BlitScreen(rgba_frame, renderer.Screen(VideoCore::ScreenId::Bottom), BottomScreenX,
               TopScreenHeight, 320, 240);
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

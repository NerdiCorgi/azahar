// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include "common/color.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_software/renderer_software.h"

namespace SwRenderer {

namespace {

void WritePixel(std::vector<u8>& rgba_frame, u32 width, u32 x, u32 y, const u8* rgba) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
    std::memcpy(rgba_frame.data() + offset, rgba, 4);
}

void BlitScreen(std::vector<u8>& rgba_frame, const Layout::FramebufferLayout& layout,
                const Common::Rectangle<u32>& target, const ScreenInfo& info) {
    if (info.pixels.empty() || info.width == 0 || info.height == 0 || target.GetWidth() == 0 ||
        target.GetHeight() == 0 || target.left >= layout.width || target.top >= layout.height) {
        return;
    }

    const u32 blit_width = std::min(target.GetWidth(), layout.width - target.left);
    const u32 blit_height = std::min(target.GetHeight(), layout.height - target.top);
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
            WritePixel(rgba_frame, layout.width, target.left + x, target.top + y,
                       info.pixels.data() + src_offset);
        }
    }
}

} // namespace

RendererSoftware::RendererSoftware(Core::System& system, Pica::PicaCore& pica_,
                                   Frontend::EmuWindow& window)
    : VideoCore::RendererBase{system, window, nullptr}, memory{system.Memory()}, pica{pica_},
      rasterizer{memory, pica} {}

RendererSoftware::~RendererSoftware() = default;

void RendererSoftware::SwapBuffers() {
    system.perf_stats->StartSwap();
    PrepareRenderTarget();
    system.perf_stats->EndSwap();
    EndFrame();
}

bool RendererSoftware::TryCaptureFrameRGBA(const Layout::FramebufferLayout& layout,
                                           std::vector<u8>& out) {
    const size_t required_size = static_cast<size_t>(layout.width) * layout.height * 4;
    if (out.size() != required_size) {
        out.resize(required_size);
    }

    std::fill(out.begin(), out.end(), 0);
    for (size_t i = 3; i < out.size(); i += 4) {
        out[i] = 255;
    }

    if (layout.top_screen_enabled) {
        BlitScreen(out, layout, layout.top_screen, Screen(VideoCore::ScreenId::TopLeft));
    }
    if (layout.bottom_screen_enabled) {
        BlitScreen(out, layout, layout.bottom_screen, Screen(VideoCore::ScreenId::Bottom));
    }

    return true;
}

void RendererSoftware::PrepareRenderTarget() {
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        LoadFBToScreenInfo(i, color_fill);
    }
}

void RendererSoftware::LoadFBToScreenInfo(int i, const Pica::ColorFill& color_fill) {
    const u32 fb_id = i == 2 ? 1 : 0;
    const auto& framebuffer = pica.regs.framebuffer_config[fb_id];
    auto& info = screen_infos[i];

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0 ? framebuffer.address_left1 : framebuffer.address_left2;
    const s32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const u8* framebuffer_data = memory.GetPhysicalPointer(framebuffer_addr);

    const s32 pixel_stride = framebuffer.stride / bpp;
    info.height = framebuffer.height;
    info.width = pixel_stride;
    info.pixels.resize(info.width * info.height * 4);

    for (u32 y = 0; y < info.height; y++) {
        for (u32 x = 0; x < info.width; x++) {
            const u8* pixel = framebuffer_data + (y * pixel_stride + pixel_stride - x - 1) * bpp;
            Common::Vec4 color = [&] {
                if (color_fill.is_enabled) {
                    return Common::Vec4<u8>(color_fill.color_r, color_fill.color_g,
                                            color_fill.color_b, 255);
                }

                switch (framebuffer.color_format) {
                case Pica::PixelFormat::RGBA8:
                    return Common::Color::DecodeRGBA8(pixel);
                case Pica::PixelFormat::RGB8:
                    return Common::Color::DecodeRGB8(pixel);
                case Pica::PixelFormat::RGB565:
                    return Common::Color::DecodeRGB565(pixel);
                case Pica::PixelFormat::RGB5A1:
                    return Common::Color::DecodeRGB5A1(pixel);
                case Pica::PixelFormat::RGBA4:
                    return Common::Color::DecodeRGBA4(pixel);
                }
                UNREACHABLE();
            }();
            const u32 output_offset = (x * info.height + y) * 4;
            u8* dest = info.pixels.data() + output_offset;
            std::memcpy(dest, color.AsArray(), sizeof(color));
        }
    }
}

} // namespace SwRenderer

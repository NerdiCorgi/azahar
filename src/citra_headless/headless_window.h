// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include "core/frontend/emu_window.h"

class HeadlessWindow final : public Frontend::EmuWindow {
public:
    explicit HeadlessWindow(bool raw_video_stdout_enabled = false,
                            std::optional<std::chrono::steady_clock::duration> raw_video_frame_period =
                                std::nullopt);
    ~HeadlessWindow() override = default;

    void PollEvents() override;
    void SwapBuffers() override;

private:
    void MaybeWriteRawVideoFrame();
    void OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) override;

    bool raw_video_stdout_enabled{};
    std::optional<std::chrono::steady_clock::duration> raw_video_frame_period;
    std::optional<std::chrono::steady_clock::time_point> last_raw_video_frame_write_time;
    s32 last_raw_video_frame = 0;
    std::vector<u8> rgba_frame;
    std::vector<u8> yuv_frame;
};

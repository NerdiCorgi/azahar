// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "audio_core/sink.h"

namespace AudioCore {

class TCPSink final : public Sink {
public:
    explicit TCPSink(std::string port_text);
    ~TCPSink() override;

    unsigned int GetNativeSampleRate() const override;

    void SetCallback(std::function<void(s16*, std::size_t)>) override {}

    bool ImmediateSubmission() override {
        return true;
    }

    void PushSamples(const void* data, std::size_t num_samples) override;

private:
    struct Impl;
    Impl* impl;
};

std::vector<std::string> ListTCPSinkDevices();

} // namespace AudioCore

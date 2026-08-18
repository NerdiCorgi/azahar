// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace InputCommon::RetroCorgiIPC {

struct StateRequest {
    enum class Type {
        SaveState,
        LoadState,
    };

    std::uint64_t token = 0;
    Type type = Type::SaveState;
    std::string id;
    std::string path;
};

class State {
public:
    State();
    ~State();

    void EnsureListener(int port);
    std::optional<StateRequest> PopStateRequest();
    void CompleteStateRequest(const StateRequest& request, bool success, std::size_t bytes,
                              const std::string& message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::unique_ptr<State> Init();

} // namespace InputCommon::RetroCorgiIPC

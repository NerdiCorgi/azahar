// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
namespace InputCommon::RetroCorgiIPC {

class State {
public:
    State();
    ~State();

    void EnsureListener(int port);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::unique_ptr<State> Init();

} // namespace InputCommon::RetroCorgiIPC

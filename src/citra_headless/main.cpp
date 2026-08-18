// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cctype>
#include <csignal>
#include <exception>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "audio_core/sink_details.h"
#include "citra_headless/headless_window.h"
#include "common/common_paths.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "input_common/main.h"
#include "video_core/gpu.h"

namespace {

enum class New3dsMode {
    Auto,
    Old,
    New,
};

void PrintUsage(const char* program_name) {
    std::cerr << "Usage: " << program_name
              << " <rom-path> [--seconds <n>] [--user-dir <path>] [--new-3ds <old|new|auto>]"
                 " [--raw-video-stdout] [--raw-video-fps <n>] [--audio-tcp-port <port>]"
                 " [--ipc-port <port>]\n"
              << "  --user-dir <path>         Use the specified Azahar user directory\n"
              << "  --new-3ds <old|new|auto>  Set system mode before loading the ROM\n"
              << "                             auto keeps the existing setting\n"
              << "  --renderer <backend>     Use software or Vulkan rendering (default: software)\n"
              << "  --raw-video-stdout        Write composed 400x480 yuv420p frames to stdout\n"
              << "  --raw-video-fps <n>       Cap raw video stdout to 1..60 frames per second\n"
              << "  --audio-tcp-port <port>   Stream PCM16 stereo audio on 127.0.0.1:<port>\n"
              << "  --ipc-port <port>         Listen for RetroCorgi IPC on 127.0.0.1:<port>\n";
}

bool ParseBoundedRun(int argc, char* argv[], int& index,
                     std::optional<std::chrono::seconds>& run_for) {
    if (std::string_view(argv[index]) != "--seconds") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    char* end = nullptr;
    const long long seconds = std::strtoll(argv[index + 1], &end, 10);
    if (end == argv[index + 1] || *end != '\0' || seconds < 0) {
        return false;
    }

    run_for = std::chrono::seconds(seconds);
    ++index;
    return true;
}

bool ParseUserDir(int argc, char* argv[], int& index, std::optional<std::string>& user_dir) {
    if (std::string_view(argv[index]) != "--user-dir") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    user_dir = argv[index + 1];
    if (user_dir->find_first_not_of(" \f\n\r\t\v") == std::string::npos) {
        std::cerr << "Error: --user-dir must not be empty or whitespace-only\n";
        return false;
    }
    ++index;
    return true;
}

bool ParseNew3dsMode(int argc, char* argv[], int& index, New3dsMode& new_3ds_mode) {
    if (std::string_view(argv[index]) != "--new-3ds") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    const std::string_view mode = argv[index + 1];
    if (mode == "auto") {
        new_3ds_mode = New3dsMode::Auto;
    } else if (mode == "old") {
        new_3ds_mode = New3dsMode::Old;
    } else if (mode == "new") {
        new_3ds_mode = New3dsMode::New;
    } else {
        return false;
    }

    ++index;
    return true;
}

bool ParseRenderer(int argc, char* argv[], int& index, Settings::GraphicsAPI& graphics_api) {
    if (std::string_view(argv[index]) != "--renderer") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    const std::string_view backend = argv[index + 1];
    if (backend == "software") {
        graphics_api = Settings::GraphicsAPI::Software;
    } else if (backend == "vulkan") {
        graphics_api = Settings::GraphicsAPI::Vulkan;
    } else {
        return false;
    }

    ++index;
    return true;
}

bool ParseAudioTcpPort(int argc, char* argv[], int& index, std::optional<u16>& audio_tcp_port) {
    if (std::string_view(argv[index]) != "--audio-tcp-port") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    char* end = nullptr;
    const unsigned long port = std::strtoul(argv[index + 1], &end, 10);
    if (end == argv[index + 1] || *end != '\0' || port > 65535UL) {
        return false;
    }

    audio_tcp_port = static_cast<u16>(port);
    ++index;
    return true;
}

bool ParseIpcPort(int argc, char* argv[], int& index, std::optional<u16>& ipc_port) {
    if (std::string_view(argv[index]) != "--ipc-port") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    char* end = nullptr;
    const unsigned long port = std::strtoul(argv[index + 1], &end, 10);
    if (end == argv[index + 1] || *end != '\0' || port < 1UL || port > 65535UL) {
        return false;
    }

    ipc_port = static_cast<u16>(port);
    ++index;
    return true;
}

bool ParseRawVideoFps(int argc, char* argv[], int& index, std::optional<int>& raw_video_fps) {
    if (std::string_view(argv[index]) != "--raw-video-fps") {
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }

    char* end = nullptr;
    const long fps = std::strtol(argv[index + 1], &end, 10);
    if (end == argv[index + 1] || *end != '\0' || fps < 1 || fps > 60) {
        return false;
    }

    raw_video_fps = static_cast<int>(fps);
    ++index;
    return true;
}

void NormalizeCustomUserDir(std::string& user_dir) {
    while (!user_dir.empty() && (user_dir.back() == '/' || user_dir.back() == '\\')) {
        user_dir.pop_back();
    }
    user_dir += DIR_SEP;
}

bool IsEmptyOrWhitespace(std::string_view value) {
    for (const unsigned char ch : value) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

int PrintCoreError(Core::System::ResultStatus status, const Core::System& system) {
    std::cerr << "Azahar headless failed with status " << static_cast<int>(status);
    const auto& details = system.GetStatusDetails();
    if (!details.empty()) {
        std::cerr << ": " << details;
    }
    std::cerr << '\n';
    return 1;
}

std::string MakeRetroCorgiButtonParam(std::string_view control, u16 port) {
    Common::ParamPackage param;
    param.Set("engine", "retrocorgi_ipc");
    param.Set("control", std::string{control});
    param.Set("port", std::to_string(port));
    return param.Serialize();
}

std::string MakeRetroCorgiAnalogParam(std::string_view control, u16 port) {
    Common::ParamPackage param;
    param.Set("engine", "retrocorgi_ipc");
    param.Set("control", std::string{control});
    param.Set("port", std::to_string(port));
    return param.Serialize();
}

std::string MakeRetroCorgiTouchParam(u16 port) {
    Common::ParamPackage param;
    param.Set("engine", "retrocorgi_ipc");
    param.Set("port", std::to_string(port));
    return param.Serialize();
}

std::optional<u16> GetRetroCorgiEnvIpcPort() {
    const char* ipc_port = std::getenv("RETROCORGI_AZAHAR_IPC_PORT");
    if (ipc_port == nullptr || *ipc_port == '\0' || IsEmptyOrWhitespace(ipc_port)) {
        return std::nullopt;
    }

    char* end = nullptr;
    const unsigned long port = std::strtoul(ipc_port, &end, 10);
    if (end == ipc_port || *end != '\0' || port < 1UL || port > 65535UL) {
        return std::nullopt;
    }

    return static_cast<u16>(port);
}

void ApplyHeadlessRetroCorgiInputDefaults(std::optional<u16> ipc_port) {
    const auto resolved_ipc_port = ipc_port.has_value() ? ipc_port : GetRetroCorgiEnvIpcPort();
    if (!resolved_ipc_port.has_value()) {
        return;
    }

    auto& profile = Settings::values.current_input_profile;
    const auto set_button = [&profile, &resolved_ipc_port](Settings::NativeButton::Values button,
                                                           std::string_view control) {
        profile.buttons[button] = MakeRetroCorgiButtonParam(control, *resolved_ipc_port);
    };
    const auto set_analog = [&profile, &resolved_ipc_port](Settings::NativeAnalog::Values analog,
                                                           std::string_view control) {
        profile.analogs[analog] = MakeRetroCorgiAnalogParam(control, *resolved_ipc_port);
    };

    set_button(Settings::NativeButton::Up, "dpad_up");
    set_button(Settings::NativeButton::Down, "dpad_down");
    set_button(Settings::NativeButton::Left, "dpad_left");
    set_button(Settings::NativeButton::Right, "dpad_right");
    set_button(Settings::NativeButton::A, "a");
    set_button(Settings::NativeButton::B, "b");
    set_button(Settings::NativeButton::X, "x");
    set_button(Settings::NativeButton::Y, "y");
    set_button(Settings::NativeButton::L, "l");
    set_button(Settings::NativeButton::R, "r");
    set_button(Settings::NativeButton::ZL, "zl");
    set_button(Settings::NativeButton::ZR, "zr");
    set_button(Settings::NativeButton::Select, "select");
    set_button(Settings::NativeButton::Start, "start");
    set_analog(Settings::NativeAnalog::CirclePad, "circlepad");
    set_analog(Settings::NativeAnalog::CStick, "cstick");
    profile.touch_device = MakeRetroCorgiTouchParam(*resolved_ipc_port);
}

bool WaitForAsyncOperationsToDrain(Core::System& system) {
    if (!system.KernelRunning() || !system.Kernel().AreAsyncOperationsPending()) {
        return true;
    }

    const auto start = std::chrono::steady_clock::now();
    while (system.Kernel().AreAsyncOperationsPending()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            return false;
        }

        const auto result = system.RunLoop();
        if (result != Core::System::ResultStatus::Success) {
            return false;
        }
    }

    return true;
}

void EnsureStateFileParentPath(const std::string& path) {
    if (!FileUtil::CreateFullPath(path)) {
        throw std::runtime_error("Could not create path " + path);
    }
}

std::vector<u8> ReadRequiredFile(const std::string& path) {
    if (!FileUtil::Exists(path)) {
        throw std::runtime_error("Could not open file " + path);
    }

    FileUtil::IOFile file(path, "rb");
    if (!file.IsOpen()) {
        throw std::runtime_error("Could not open file " + path);
    }

    const auto size = static_cast<std::size_t>(FileUtil::GetSize(path));
    std::vector<u8> buffer(size);
    if (size > 0 && file.ReadBytes(buffer.data(), size) != size) {
        throw std::runtime_error("Could not read file " + path);
    }

    return buffer;
}

void WriteRequiredFile(const std::string& path, const std::vector<u8>& buffer) {
    EnsureStateFileParentPath(path);

    FileUtil::IOFile file(path, "wb");
    if (!file.IsOpen()) {
        throw std::runtime_error("Could not open file " + path);
    }

    if (!buffer.empty() && file.WriteBytes(buffer.data(), buffer.size()) != buffer.size()) {
        throw std::runtime_error("Could not write file " + path);
    }
}

void ProcessRetroCorgiStateRequest(Core::System& system, HeadlessWindow& window) {
    const auto request = InputCommon::PopRetroCorgiIPCRequest();
    if (!request.has_value()) {
        return;
    }

    try {
        if (!system.IsPoweredOn()) {
            throw std::runtime_error("System is not powered on");
        }
        if (!system.GetAppLoader().SupportsSaveStates()) {
            throw std::runtime_error("The current app loader doesn't support save states");
        }
        if (!WaitForAsyncOperationsToDrain(system)) {
            throw std::runtime_error("Timed out waiting for async operations to complete");
        }

        if (request->type == InputCommon::RetroCorgiIPC::StateRequest::Type::SaveState) {
            const auto buffer = system.SaveStateBuffer();
            WriteRequiredFile(request->path, buffer);
            InputCommon::CompleteRetroCorgiIPCRequest(*request, true, buffer.size(),
                                                      "save-state completed");
            return;
        }

        auto buffer = ReadRequiredFile(request->path);
        const auto bytes = buffer.size();
        window.SetSuppressRawVideoCapture(true);
        SCOPE_EXIT({ window.SetSuppressRawVideoCapture(false); });
        if (!system.LoadStateBuffer(std::move(buffer))) {
            const auto& details = system.GetStatusDetails();
            throw std::runtime_error(details.empty() ? "Native load-state operation failed"
                                                     : details);
        }
        window.OnStateRestoreComplete();
        system.frame_limiter.AdvanceFrame();
        InputCommon::CompleteRetroCorgiIPCRequest(*request, true, bytes,
                                                  "load-state completed");
    } catch (const std::exception& exception) {
        InputCommon::CompleteRetroCorgiIPCRequest(*request, false, 0, exception.what());
    }
}

} // namespace

int main(int argc, char* argv[]) {
    Common::DetachedTasks detached_tasks;

    if (argc < 2 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        PrintUsage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    std::string rom_path;
    std::optional<std::chrono::seconds> run_for;
    std::optional<std::string> user_dir;
    std::optional<u16> audio_tcp_port;
    std::optional<u16> ipc_port;
    std::optional<int> raw_video_fps;
    New3dsMode new_3ds_mode = New3dsMode::Auto;
    Settings::GraphicsAPI graphics_api = Settings::GraphicsAPI::Software;
    bool raw_video_stdout_enabled = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--seconds") {
            if (!ParseBoundedRun(argc, argv, i, run_for)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--user-dir") {
            if (!ParseUserDir(argc, argv, i, user_dir)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--new-3ds") {
            if (!ParseNew3dsMode(argc, argv, i, new_3ds_mode)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--renderer") {
            if (!ParseRenderer(argc, argv, i, graphics_api)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--raw-video-stdout") {
            raw_video_stdout_enabled = true;
            continue;
        }

        if (std::string_view(argv[i]) == "--raw-video-fps") {
            if (!ParseRawVideoFps(argc, argv, i, raw_video_fps)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--audio-tcp-port") {
            if (!ParseAudioTcpPort(argc, argv, i, audio_tcp_port)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (std::string_view(argv[i]) == "--ipc-port") {
            if (!ParseIpcPort(argc, argv, i, ipc_port)) {
                PrintUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (!rom_path.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }

        rom_path = argv[i];
    }

    if (rom_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    if (!user_dir.has_value()) {
        if (const char* env_user_dir = std::getenv("RETROCORGI_AZAHAR_USER_DIR");
            env_user_dir != nullptr && !IsEmptyOrWhitespace(env_user_dir)) {
            user_dir = env_user_dir;
        }
    }

    if (user_dir.has_value()) {
        NormalizeCustomUserDir(*user_dir);
    }

    FileUtil::SetUserPath(user_dir.value_or(""));
    Common::Log::Initialize();
    Common::Log::Start();

#ifdef _WIN32
    if (raw_video_stdout_enabled && _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::cerr << "Error: failed to switch stdout to binary mode\n";
        return 1;
    }
#else
    if (raw_video_stdout_enabled) {
        std::signal(SIGPIPE, SIG_IGN);
    }
#endif

    Settings::values.graphics_api = graphics_api;
    Settings::values.output_type = audio_tcp_port.has_value() ? AudioCore::SinkType::TCP
                                                              : AudioCore::SinkType::Null;
    if (audio_tcp_port.has_value()) {
        Settings::values.output_device = std::to_string(*audio_tcp_port);
    }
    Settings::values.is_new_3ds.SetGlobal(true);
    if (new_3ds_mode == New3dsMode::Old) {
        Settings::values.is_new_3ds = false;
    } else if (new_3ds_mode == New3dsMode::New) {
        Settings::values.is_new_3ds = true;
    }

    InputCommon::Init();
    ApplyHeadlessRetroCorgiInputDefaults(ipc_port);

    auto& system = Core::System::GetInstance();
    HeadlessWindow window{
        raw_video_stdout_enabled,
        raw_video_stdout_enabled && raw_video_fps.has_value()
            ? std::optional{std::chrono::steady_clock::duration{std::chrono::seconds{1}} /
                            *raw_video_fps}
            : std::nullopt,
    };

    SCOPE_EXIT({ InputCommon::Shutdown(); });
    SCOPE_EXIT({
        if (system.IsPoweredOn()) {
            system.Shutdown();
        }
    });
    SCOPE_EXIT({ detached_tasks.WaitForAllTasks(); });

    Frontend::RegisterDefaultApplets(system);

    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }

    const auto load_result = system.Load(window, rom_path);
    if (load_result != Core::System::ResultStatus::Success) {
        return PrintCoreError(load_result, system);
    }

    u64 program_id{};
    system.GetAppLoader().ReadProgramId(program_id);
    system.GPU().ApplyPerProgramSettings(program_id);

    const auto deadline = run_for.has_value() ? std::optional{std::chrono::steady_clock::now() + *run_for}
                                               : std::optional<std::chrono::steady_clock::time_point>{};

    while (!deadline.has_value() || std::chrono::steady_clock::now() < *deadline) {
        window.PollEvents();
        ProcessRetroCorgiStateRequest(system, window);
        const auto result = system.RunLoop();
        if (result == Core::System::ResultStatus::Success) {
            continue;
        }
        if (result == Core::System::ResultStatus::ShutdownRequested) {
            break;
        }
        return PrintCoreError(result, system);
    }

    return 0;
}

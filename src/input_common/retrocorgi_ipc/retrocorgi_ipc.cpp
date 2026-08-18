// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <boost/asio.hpp>
#include "common/param_package.h"
#include "common/logging/log.h"
#include "core/frontend/input.h"
#include "input_common/retrocorgi_ipc/retrocorgi_ipc.h"

namespace InputCommon::RetroCorgiIPC {

using boost::asio::ip::tcp;

namespace {

int GetEnvListenerPort() {
    const char* value = std::getenv("RETROCORGI_AZAHAR_IPC_PORT");
    if (value == nullptr || *value == '\0') {
        return 0;
    }

    char* end = nullptr;
    const long port = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || port <= 0 || port > 65535) {
        return 0;
    }

    return static_cast<int>(port);
}

struct SharedState {
    mutable std::mutex update_mutex;
    std::map<std::string, bool> buttons;
    std::map<std::string, std::pair<float, float>> analogs;
    std::tuple<float, float, bool> touch{};
};

class LineServer {
public:
    LineServer(std::shared_ptr<SharedState> shared_, u16 port_)
        : shared(std::move(shared_)),
          endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), port_) {}

    ~LineServer() {
        Stop();
    }

    void Start() {
        thread = std::thread([this] { Run(); });
    }

    void Stop() {
        stopping.store(true);

        std::shared_ptr<tcp::socket> socket_to_close;
        {
            std::lock_guard guard(socket_mutex);
            socket_to_close = active_socket;
        }

        boost::system::error_code ec;
        acceptor.cancel(ec);
        acceptor.close(ec);
        if (socket_to_close) {
            socket_to_close->close(ec);
        }
        io_context.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    void Run() {
        boost::system::error_code ec;
        acceptor.open(tcp::v4(), ec);
        if (ec) {
            LOG_ERROR(Input, "retrocorgi_ipc failed to open TCP listener: {}", ec.message());
            return;
        }
        acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
        if (ec) {
            LOG_ERROR(Input, "retrocorgi_ipc failed to set TCP listener options: {}",
                      ec.message());
            return;
        }
        acceptor.bind(endpoint, ec);
        if (ec) {
            LOG_ERROR(Input, "retrocorgi_ipc failed to bind localhost listener on port {}: {}",
                      endpoint.port(), ec.message());
            return;
        }
        acceptor.listen(tcp::acceptor::max_listen_connections, ec);
        if (ec) {
            LOG_ERROR(Input, "retrocorgi_ipc failed to listen on port {}: {}", endpoint.port(),
                      ec.message());
            return;
        }

        // Minimal localhost line protocol:
        //   button <control> <0|1>
        //   analog <control> <x> <y>
        //   touch <x> <y> <0|1>
        //   reset
        // One command is processed per newline. Unknown or malformed lines are ignored.
        while (!stopping.load()) {
            auto socket = std::make_shared<tcp::socket>(io_context);
            {
                std::lock_guard guard(socket_mutex);
                active_socket = socket;
            }

            acceptor.accept(*socket, ec);
            if (ec) {
                {
                    std::lock_guard guard(socket_mutex);
                    if (active_socket == socket) {
                        active_socket.reset();
                    }
                }
                if (!stopping.load()) {
                    LOG_WARNING(Input, "retrocorgi_ipc accept failed: {}", ec.message());
                }
                continue;
            }

            boost::asio::streambuf buffer;
            while (!stopping.load()) {
                boost::asio::read_until(*socket, buffer, '\n', ec);
                if (ec) {
                    break;
                }

                std::istream input(&buffer);
                std::string line;
                std::getline(input, line);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                HandleLine(line);
            }

            socket->close(ec);
            {
                std::lock_guard guard(socket_mutex);
                if (active_socket == socket) {
                    active_socket.reset();
                }
            }
        }
    }

    void HandleLine(const std::string& line) {
        std::istringstream stream(line);
        std::string command;
        if (!(stream >> command)) {
            return;
        }

        if (command == "reset") {
            std::lock_guard guard(shared->update_mutex);
            shared->buttons.clear();
            shared->analogs.clear();
            shared->touch = {};
            return;
        }

        if (command == "button") {
            std::string control;
            int pressed = 0;
            if (!(stream >> control >> pressed)) {
                return;
            }
            if (pressed != 0 && pressed != 1) {
                return;
            }

            std::lock_guard guard(shared->update_mutex);
            shared->buttons[control] = pressed == 1;
            return;
        }

        if (command == "analog") {
            std::string control;
            float x = 0.0f;
            float y = 0.0f;
            if (!(stream >> control >> x >> y)) {
                return;
            }

            std::lock_guard guard(shared->update_mutex);
            shared->analogs[control] = {std::clamp(x, -1.0f, 1.0f),
                                        std::clamp(y, -1.0f, 1.0f)};
            return;
        }

        if (command == "touch") {
            float x = 0.0f;
            float y = 0.0f;
            int pressed = 0;
            if (!(stream >> x >> y >> pressed)) {
                return;
            }
            if (pressed != 0 && pressed != 1) {
                return;
            }

            std::lock_guard guard(shared->update_mutex);
            shared->touch = {std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f),
                             pressed == 1};
        }
    }

    std::shared_ptr<SharedState> shared;
    boost::asio::io_context io_context;
    tcp::endpoint endpoint;
    tcp::acceptor acceptor{io_context};
    std::atomic_bool stopping{false};
    std::mutex socket_mutex;
    std::shared_ptr<tcp::socket> active_socket;
    std::thread thread;
};

} // namespace

struct State::Impl {
    std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
    std::mutex listener_mutex;
    std::unique_ptr<LineServer> listener;
    int listener_port = 0;
};

class ButtonDevice final : public Input::ButtonDevice {
public:
    ButtonDevice(std::shared_ptr<SharedState> shared_, std::string control_)
        : shared(std::move(shared_)), control(std::move(control_)) {}

    bool GetStatus() const override {
        std::lock_guard guard(shared->update_mutex);
        const auto it = shared->buttons.find(control);
        return it != shared->buttons.end() ? it->second : false;
    }

private:
    std::shared_ptr<SharedState> shared;
    std::string control;
};

class AnalogDevice final : public Input::AnalogDevice {
public:
    AnalogDevice(std::shared_ptr<SharedState> shared_, std::string control_)
        : shared(std::move(shared_)), control(std::move(control_)) {}

    std::tuple<float, float> GetStatus() const override {
        std::lock_guard guard(shared->update_mutex);
        const auto it = shared->analogs.find(control);
        if (it == shared->analogs.end()) {
            return {};
        }
        return it->second;
    }

private:
    std::shared_ptr<SharedState> shared;
    std::string control;
};

class TouchDevice final : public Input::TouchDevice {
public:
    explicit TouchDevice(std::shared_ptr<SharedState> shared_) : shared(std::move(shared_)) {}

    std::tuple<float, float, bool> GetStatus() const override {
        std::lock_guard guard(shared->update_mutex);
        return shared->touch;
    }

private:
    std::shared_ptr<SharedState> shared;
};

class ButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    ButtonFactory(State* state_, std::shared_ptr<SharedState> shared_)
        : state(state_), shared(std::move(shared_)) {}

    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        state->EnsureListener(params.Get("port", 0));
        return std::make_unique<ButtonDevice>(shared, params.Get("control", ""));
    }

private:
    State* state;
    std::shared_ptr<SharedState> shared;
};

class AnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    AnalogFactory(State* state_, std::shared_ptr<SharedState> shared_)
        : state(state_), shared(std::move(shared_)) {}

    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        state->EnsureListener(params.Get("port", 0));
        return std::make_unique<AnalogDevice>(shared, params.Get("control", ""));
    }

private:
    State* state;
    std::shared_ptr<SharedState> shared;
};

class TouchFactory final : public Input::Factory<Input::TouchDevice> {
public:
    TouchFactory(State* state_, std::shared_ptr<SharedState> shared_)
        : state(state_), shared(std::move(shared_)) {}

    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage& params) override {
        state->EnsureListener(params.Get("port", 0));
        return std::make_unique<TouchDevice>(shared);
    }

private:
    State* state;
    std::shared_ptr<SharedState> shared;
};

State::State() : impl(std::make_unique<Impl>()) {
    Input::RegisterFactory<Input::ButtonDevice>("retrocorgi_ipc",
                                                std::make_shared<ButtonFactory>(this, impl->shared));
    Input::RegisterFactory<Input::AnalogDevice>("retrocorgi_ipc",
                                                std::make_shared<AnalogFactory>(this, impl->shared));
    Input::RegisterFactory<Input::TouchDevice>("retrocorgi_ipc",
                                               std::make_shared<TouchFactory>(this, impl->shared));
}

State::~State() {
    Input::UnregisterFactory<Input::ButtonDevice>("retrocorgi_ipc");
    Input::UnregisterFactory<Input::AnalogDevice>("retrocorgi_ipc");
    Input::UnregisterFactory<Input::TouchDevice>("retrocorgi_ipc");

    std::unique_ptr<LineServer> listener;
    {
        std::lock_guard guard(impl->listener_mutex);
        listener = std::move(impl->listener);
        impl->listener_port = 0;
    }
}

void State::EnsureListener(int port) {
    // If a device config does not provide a valid port, fall back to the shared env setting.
    if (port <= 0 || port > 65535) {
        port = GetEnvListenerPort();
    }

    if (port <= 0 || port > 65535) {
        return;
    }

    {
        std::lock_guard guard(impl->listener_mutex);
        if (impl->listener_port == port) {
            return;
        }
        if (impl->listener_port != 0) {
            LOG_WARNING(Input,
                        "retrocorgi_ipc ignoring requested port {} because listener is already "
                        "latched to port {}",
                        port, impl->listener_port);
            return;
        }

        impl->listener = std::make_unique<LineServer>(impl->shared, static_cast<u16>(port));
        impl->listener->Start();
        impl->listener_port = port;
    }
}

std::unique_ptr<State> Init() {
    return std::make_unique<State>();
}

} // namespace InputCommon::RetroCorgiIPC

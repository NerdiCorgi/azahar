// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
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

    struct ConnectionState {
        explicit ConnectionState(std::shared_ptr<tcp::socket> socket_) : socket(std::move(socket_)) {}

        std::shared_ptr<tcp::socket> socket;
        std::mutex write_mutex;
        std::atomic_bool open{true};
    };

    struct PendingStateRequest {
        StateRequest request;
        std::shared_ptr<ConnectionState> connection;
    };

    std::mutex request_mutex;
    std::deque<StateRequest> request_queue;
    std::map<std::uint64_t, PendingStateRequest> pending_state_requests;
    std::uint64_t next_request_token = 1;
};

std::string TrimLeadingWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \f\n\r\t\v");
    if (first == std::string::npos) {
        value.clear();
        return value;
    }
    value.erase(0, first);
    return value;
}

std::string EscapeJsonString(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20) {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::uppercase << std::setw(4)
                       << std::setfill('0') << static_cast<int>(ch);
                escaped += stream.str();
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

std::string MakeStateResponseJson(const StateRequest& request, bool success, std::size_t bytes,
                                  std::string_view message) {
    std::ostringstream stream;
    stream << "{\"id\":\"" << EscapeJsonString(request.id) << "\",\"status\":\""
           << (success ? "ok" : "error") << "\",\"path\":\""
           << EscapeJsonString(request.path) << "\",\"bytes\":" << bytes
           << ",\"message\":\"" << EscapeJsonString(message) << "\"}\n";
    return stream.str();
}

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

        std::vector<std::shared_ptr<SharedState::ConnectionState>> connections_to_close;
        {
            std::lock_guard guard(connection_mutex);
            connections_to_close = connections;
        }

        boost::system::error_code ec;
        acceptor.cancel(ec);
        acceptor.close(ec);
        for (const auto& connection : connections_to_close) {
            CloseConnection(connection);
        }
        io_context.stop();
        if (thread.joinable()) {
            thread.join();
        }

        std::vector<std::thread> workers;
        {
            std::lock_guard guard(worker_mutex);
            workers.swap(worker_threads);
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
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

            acceptor.accept(*socket, ec);
            if (ec) {
                if (!stopping.load()) {
                    LOG_WARNING(Input, "retrocorgi_ipc accept failed: {}", ec.message());
                }
                continue;
            }

            auto connection = std::make_shared<SharedState::ConnectionState>(std::move(socket));
            RegisterConnection(connection);
            {
                std::lock_guard guard(worker_mutex);
                worker_threads.emplace_back([this, connection] { HandleConnection(connection); });
            }
        }
    }

    void HandleConnection(const std::shared_ptr<SharedState::ConnectionState>& connection) {
        boost::asio::streambuf buffer;
        boost::system::error_code ec;

        while (!stopping.load() && connection->open.load()) {
            boost::asio::read_until(*connection->socket, buffer, '\n', ec);
            if (ec) {
                break;
            }

            std::istream input(&buffer);
            std::string line;
            std::getline(input, line);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            HandleLine(line, connection);
        }

        CloseConnection(connection);
        UnregisterConnection(connection);
    }

    void RegisterConnection(const std::shared_ptr<SharedState::ConnectionState>& connection) {
        std::lock_guard guard(connection_mutex);
        connections.push_back(connection);
    }

    void UnregisterConnection(const std::shared_ptr<SharedState::ConnectionState>& connection) {
        std::lock_guard guard(connection_mutex);
        std::erase(connections, connection);
    }

    void CloseConnection(const std::shared_ptr<SharedState::ConnectionState>& connection) {
        if (!connection) {
            return;
        }

        connection->open.store(false);
        if (!connection->socket) {
            return;
        }

        boost::system::error_code ec;
        connection->socket->shutdown(tcp::socket::shutdown_both, ec);
        connection->socket->close(ec);
    }

    void HandleLine(const std::string& line,
                    const std::shared_ptr<SharedState::ConnectionState>& connection) {
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
            return;
        }

        if (command == "save-state" || command == "load-state") {
            std::string id;
            if (!(stream >> id)) {
                return;
            }

            std::string path;
            std::getline(stream, path);
            path = TrimLeadingWhitespace(path);
            if (path.empty()) {
                return;
            }

            StateRequest request;
            request.type = command == "save-state" ? StateRequest::Type::SaveState
                                                     : StateRequest::Type::LoadState;
            request.id = std::move(id);
            request.path = std::move(path);

            std::lock_guard guard(shared->request_mutex);
            request.token = shared->next_request_token++;
            shared->request_queue.push_back(request);
            shared->pending_state_requests.emplace(
                request.token, SharedState::PendingStateRequest{request, connection});
        }
    }

    std::shared_ptr<SharedState> shared;
    boost::asio::io_context io_context;
    tcp::endpoint endpoint;
    tcp::acceptor acceptor{io_context};
    std::atomic_bool stopping{false};
    std::mutex connection_mutex;
    std::vector<std::shared_ptr<SharedState::ConnectionState>> connections;
    std::mutex worker_mutex;
    std::vector<std::thread> worker_threads;
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

std::optional<StateRequest> State::PopStateRequest() {
    std::lock_guard guard(impl->shared->request_mutex);
    if (impl->shared->request_queue.empty()) {
        return std::nullopt;
    }

    StateRequest request = impl->shared->request_queue.front();
    impl->shared->request_queue.pop_front();
    return request;
}

void State::CompleteStateRequest(const StateRequest& request, bool success, std::size_t bytes,
                                 const std::string& message) {
    std::shared_ptr<SharedState::ConnectionState> connection;
    {
        std::lock_guard guard(impl->shared->request_mutex);
        const auto it = impl->shared->pending_state_requests.find(request.token);
        if (it == impl->shared->pending_state_requests.end()) {
            return;
        }
        connection = it->second.connection;
        impl->shared->pending_state_requests.erase(it);
    }

    if (!connection || !connection->open.load() || !connection->socket) {
        return;
    }

    const std::string response = MakeStateResponseJson(request, success, bytes, message);
    std::lock_guard guard(connection->write_mutex);
    if (!connection->open.load()) {
        return;
    }

    boost::system::error_code ec;
    boost::asio::write(*connection->socket, boost::asio::buffer(response), ec);
    if (ec) {
        connection->open.store(false);
        LOG_WARNING(Input, "retrocorgi_ipc failed to send state response: {}", ec.message());
    }
}

std::unique_ptr<State> Init() {
    return std::make_unique<State>();
}

} // namespace InputCommon::RetroCorgiIPC

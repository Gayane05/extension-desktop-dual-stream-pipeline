// desktop/src/net/ws_server.h
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "core/protocol.h"

namespace ix { class WebSocketServer; }

namespace dsp {

class WsServer {
public:
    struct Callbacks {
        std::function<void(AudioFrame&&)> onAudio;
        std::function<void(const HelloInfo&)> onHello;
        std::function<void()> onClientGone;
    };

    WsServer(int port, Callbacks cb);
    ~WsServer();
    bool start(std::string& error);
    void stop();
    void broadcast(const std::string& textJson);

private:
    int port_;
    Callbacks cb_;
    std::unique_ptr<ix::WebSocketServer> server_;

    // Per-connection hello state, keyed by ConnectionState::getId().
    std::mutex helloMu_;
    std::map<std::string, bool> helloSeen_;
};

}  // namespace dsp

// desktop/src/net/ws_server.h
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "core/protocol.h"

namespace ix {
class WebSocketServer;
}

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
    // Also guards activeId_/rejected_ below: the SPSC rings downstream of
    // onAudio require a single producer per stream, so only one client may
    // be "active" (accepted) at a time -- a second connection that sends
    // hello while one is already active is rejected with an error+close
    // rather than being allowed to feed audio alongside the first.
    std::mutex helloMu_;
    std::map<std::string, bool> helloSeen_;
    std::string activeId_;            // id of the currently accepted client; empty if none
    std::set<std::string> rejected_;  // ids closed by us for "already connected"; suppresses
                                      // their Close event from firing onClientGone (it isn't
                                      // the active client going away)
};

}  // namespace dsp

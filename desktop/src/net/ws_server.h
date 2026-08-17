// desktop/src/net/ws_server.h
//
// The localhost WebSocket endpoint the extension (extension/offscreen.js)
// connects to. Wraps ixwebsocket, parses the wire protocol (core/protocol.h)
// on each message, and hands parsed frames/events to Pipeline via Callbacks.
//
// SECURITY MODEL: the server binds to 127.0.0.1 only and speaks plain,
// unencrypted ws:// -- acceptable today because audio never leaves the
// machine (loopback traffic is not observable off-host). If a REMOTE server
// is ever supported, this is the seam to upgrade: bind a routable address,
// switch to wss:// (ixwebsocket supports TLS) with a real certificate, and
// add client authentication -- the hello handshake below is a client-count
// gate, not an auth mechanism.
// Single-client rule: only one connection may be "active" (past hello) at a
// time, since the AudioFrame rings downstream require a single producer per
// stream (see activeId_/rejected_ below). All Callbacks fire on whichever
// ixwebsocket connection thread received the message -- never on the thread
// that called start().
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "core/protocol.h"

namespace ix
{
class WebSocketServer;
}

namespace dsp
{

class WsServer
{
public:
    struct Callbacks
    {
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
    std::string activeId_;            // Id of the currently accepted client; empty if none.
    std::set<std::string> rejected_;  // Ids closed by us for "already connected"; suppresses
                                      // their Close event from firing onClientGone (it isn't
                                      // the active client going away).
};

}  // namespace dsp

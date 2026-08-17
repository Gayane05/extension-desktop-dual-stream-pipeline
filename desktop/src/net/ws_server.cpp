// desktop/src/net/ws_server.cpp
//
// See ws_server.h for the class-level role/threading summary. This file
// implements the per-connection hello state machine: every connection must
// send a valid "hello" JSON before its binary audio frames are accepted, and
// only one connection may be active (hello-accepted) at a time.
#include "net/ws_server.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

namespace dsp
{

WsServer::WsServer(int port, Callbacks cb) : port_(port), cb_(std::move(cb)) {}
WsServer::~WsServer()
{
    stop();
}

bool WsServer::start(std::string& error)
{
    ix::initNetSystem();
    // Loopback-only, plain ws:// (no TLS): fine while the extension and this
    // app share one machine; see the security note in ws_server.h for what a
    // future remote/encrypted deployment would change here.
    server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
    // Per-connection state machine, keyed by ConnectionState::getId():
    // Open -> helloSeen_[id]=false (not yet authorized) -> a valid "hello"
    // flips it true (and, if no other client is active, makes this id
    // activeId_) -> binary frames are only forwarded to onAudio once
    // helloSeen_[id] is true -> Close erases the id's bookkeeping. Bound to
    // 127.0.0.1 only: this is not meant to be reachable off-machine.
    server_->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState> state,
                                               ix::WebSocket& ws,
                                               const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
        {
            std::lock_guard<std::mutex> lk(helloMu_);
            helloSeen_[state->getId()] = false;
        }
        else if (msg->type == ix::WebSocketMessageType::Close)
        {
            bool wasRejected = false;
            {
                std::lock_guard<std::mutex> lk(helloMu_);
                helloSeen_.erase(state->getId());
                if (activeId_ == state->getId())
                {
                    activeId_.clear();
                }
                wasRejected = rejected_.erase(state->getId()) > 0;
            }
            // Suppress onClientGone for a connection we ourselves closed
            // for being a second client: the real (active) client hasn't
            // gone anywhere, so Pipeline must not see this as a disconnect.
            if (!wasRejected && cb_.onClientGone)
            {
                cb_.onClientGone();
            }
        }
        else if (msg->type == ix::WebSocketMessageType::Message)
        {
            if (!msg->binary)
            {
                if (auto hello = parseHello(msg->str))
                {
                    if (hello->version != 1 || hello->sampleRate != kSampleRateHz ||
                        hello->format != "s16le")
                    {
                        ws.sendText(buildErrorJson("unsupported version or format"));
                        ws.close();
                        return;
                    }
                    // Single-producer enforcement: the SPSC rings behind
                    // onAudio require exactly one producer per stream.
                    // Accept this hello only if no other client is
                    // currently active (or this connection already is
                    // the active one, e.g. a duplicate hello); otherwise
                    // reject it without disturbing the active client.
                    bool accepted = false;
                    {
                        std::lock_guard<std::mutex> lk(helloMu_);
                        if (activeId_.empty() || activeId_ == state->getId())
                        {
                            activeId_ = state->getId();
                            helloSeen_[state->getId()] = true;
                            accepted = true;
                        }
                    }
                    if (!accepted)
                    {
                        ws.sendText(buildErrorJson("another client is already connected"));
                        {
                            std::lock_guard<std::mutex> lk(helloMu_);
                            rejected_.insert(state->getId());
                        }
                        ws.close();
                        return;
                    }
                    if (cb_.onHello)
                    {
                        cb_.onHello(*hello);
                    }
                }
                else if (isBye(msg->str))
                {
                    if (cb_.onClientGone)
                    {
                        cb_.onClientGone();
                    }
                }
                return;
            }
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(helloMu_);
                auto it = helloSeen_.find(state->getId());
                ok = it != helloSeen_.end() && it->second;
            }
            if (!ok)
            {
                return;  // Audio before hello: dropped.
            }
            auto frame = parseBinaryFrame(reinterpret_cast<const uint8_t*>(msg->str.data()),
                                          msg->str.size());
            if (frame && cb_.onAudio)
            {
                cb_.onAudio(std::move(*frame));
            }
        }
    });
    auto res = server_->listen();
    if (!res.first)
    {
        error = res.second;
        return false;
    }
    server_->start();
    return true;
}

void WsServer::stop()
{
    if (server_)
    {
        server_->stop();
        server_.reset();
    }
    std::lock_guard<std::mutex> lk(helloMu_);
    helloSeen_.clear();
}

void WsServer::broadcast(const std::string& textJson)
{
    if (!server_)
    {
        return;
    }
    for (auto&& client : server_->getClients())
    {
        client->sendText(textJson);
    }
}

}  // namespace dsp

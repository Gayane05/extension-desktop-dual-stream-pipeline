// desktop/src/net/ws_server.cpp
#include "net/ws_server.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

namespace dsp {

WsServer::WsServer(int port, Callbacks cb) : port_(port), cb_(std::move(cb)) {}
WsServer::~WsServer() { stop(); }

bool WsServer::start(std::string& error) {
    ix::initNetSystem();
    server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
    server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws,
               const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                std::lock_guard<std::mutex> lk(helloMu_);
                helloSeen_[state->getId()] = false;
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                {
                    std::lock_guard<std::mutex> lk(helloMu_);
                    helloSeen_.erase(state->getId());
                }
                if (cb_.onClientGone) cb_.onClientGone();
            } else if (msg->type == ix::WebSocketMessageType::Message) {
                if (!msg->binary) {
                    if (auto hello = parseHello(msg->str)) {
                        if (hello->sampleRate != 16000 || hello->format != "s16le") {
                            ws.sendText(buildErrorJson("unsupported format"));
                            ws.close();
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lk(helloMu_);
                            helloSeen_[state->getId()] = true;
                        }
                        if (cb_.onHello) cb_.onHello(*hello);
                    } else if (isBye(msg->str)) {
                        if (cb_.onClientGone) cb_.onClientGone();
                    }
                    return;
                }
                bool ok = false;
                {
                    std::lock_guard<std::mutex> lk(helloMu_);
                    auto it = helloSeen_.find(state->getId());
                    ok = it != helloSeen_.end() && it->second;
                }
                if (!ok) return;  // audio before hello: dropped
                auto frame = parseBinaryFrame(
                    reinterpret_cast<const uint8_t*>(msg->str.data()), msg->str.size());
                if (frame && cb_.onAudio) cb_.onAudio(std::move(*frame));
            }
        });
    auto res = server_->listen();
    if (!res.first) {
        error = res.second;
        return false;
    }
    server_->start();
    return true;
}

void WsServer::stop() {
    if (server_) {
        server_->stop();
        server_.reset();
    }
    std::lock_guard<std::mutex> lk(helloMu_);
    helloSeen_.clear();
}

void WsServer::broadcast(const std::string& textJson) {
    if (!server_) return;
    for (auto&& client : server_->getClients()) client->sendText(textJson);
}

}  // namespace dsp

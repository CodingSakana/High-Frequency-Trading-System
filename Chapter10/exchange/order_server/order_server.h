#pragma once

/**
 * 这个是 Order Gateway Server 的主组件
 * 控制 sequencer 和 TCP connection manager
 */

#include <functional>

#include "common/macros.h"
#include "common/tcp_server.h"
#include "common/thread_utils.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/fifo_sequencer.h"

namespace Exchange
{
class OrderServer {
public:
    OrderServer(ClientRequestLFQueue* client_requests, ClientResponseLFQueue* client_responses,
                const std::string& iface, int port);

    ~OrderServer();

    /// Start and stop the order server main thread.
    auto start() -> void;

    auto stop() -> void;

    /// Main run loop for this thread - accepts new client connections, receives client requests from them and sends
    /// client responses to them.
    auto run() noexcept {
        logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        while (run_) {
            /* 轮询的时候遇到新的连接会创建新的 socket，并且把回调函数设置为和 order_server 一样 */
            tcp_server_.poll();

            /** 
             * 接收数据如下：
             * 这个会调用每个 socket 的 TCPSocket::sendAndRecv()
             * 而每个 TCPSocket::sendAndRecv() 里又会调用回调函数
             * 每个回调函数就是下文的 recvCallback(TCPSocket* socket, Nanos rx_time)
             * 这里面包含向 sequencer 的接口通道
             * 
             * 以下这个 TCPServer::sendAndRecv() 过程中首先检查 read 的 socket，
             * 如果成功读到内容，还会调用 recv_finished_callback_()
             * 而 recv_finished_callback_() 就是调用 sequenceAndPublish()，把 sequencer 的 data 推送到最上层（可以直接和ME交互的那一层）
             * 排序就是在 sequenceAndPublish() 这个过程中进行的
             * 推送是通过写 LFQueue 的方式，这个 LFQueue 就可以直接被 ME 取了
             */
            tcp_server_.sendAndRecv();

            /**
             * 以下代码主要是处理 send，但是不会立马发送，而是写在缓冲区中。待下一轮 run() 循环才真正随前面代码发送。
             * 这里是直接取的 ME 的讯息了。通过 outgoing_responses_。
             * 所以 ME 发送 responses 的情况下是直接一步就到 socket 了，不需要像 requests 那样还要先经过 sequencer。
             */
            for (auto client_response = outgoing_responses_->getNextToRead();
                 outgoing_responses_->size() && client_response;
                 client_response = outgoing_responses_->getNextToRead()) {
                auto& next_outgoing_seq_num = cid_next_outgoing_seq_num_[client_response->client_id_];
                logger_.log("%:% %() % Processing cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_), client_response->client_id_, next_outgoing_seq_num,
                            client_response->toString());

                ASSERT(cid_tcp_socket_[client_response->client_id_] != nullptr,
                       "Dont have a TCPSocket for ClientId:" + std::to_string(client_response->client_id_));
                
                /* 拆两步发送成 OMClientResponse */
                cid_tcp_socket_[client_response->client_id_]->send(&next_outgoing_seq_num,
                                                                   sizeof(next_outgoing_seq_num));
                cid_tcp_socket_[client_response->client_id_]->send(client_response, sizeof(MEClientResponse));

                outgoing_responses_->updateReadIndex();

                ++next_outgoing_seq_num;
            }
        }
    }

    /// Read client request from the TCP receive buffer, check for sequence gaps and forward it to the FIFO sequencer.
    auto recvCallback(TCPSocket* socket, Nanos rx_time) noexcept {
        logger_.log("%:% %() % Received socket:% len:% rx:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);

        if (socket->next_rcv_valid_index_ >= sizeof(OMClientRequest)) {
            size_t i = 0;
            for (; i + sizeof(OMClientRequest) <= socket->next_rcv_valid_index_; i += sizeof(OMClientRequest)) {
                auto request = reinterpret_cast<const OMClientRequest*>(socket->inbound_data_.data() + i);
                logger_.log("%:% %() % Received %\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_), request->toString());

                if (UNLIKELY(cid_tcp_socket_[request->me_client_request_.client_id_] ==
                             nullptr)) { // first message from this ClientId.
                    cid_tcp_socket_[request->me_client_request_.client_id_] = socket;
                }

                if (cid_tcp_socket_[request->me_client_request_.client_id_] !=
                    socket) { // TODO - change this to send a reject back to the client.
                    logger_.log("%:% %() % Received ClientRequest from ClientId:% on different socket:% expected:%\n",
                                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                                request->me_client_request_.client_id_, socket->socket_fd_,
                                cid_tcp_socket_[request->me_client_request_.client_id_]->socket_fd_);
                    continue;
                }

                auto& next_exp_seq_num = cid_next_exp_seq_num_[request->me_client_request_.client_id_];
                if (request->seq_num_ != next_exp_seq_num) { // TODO - change this to send a reject back to the client.
                    logger_.log("%:% %() % Incorrect sequence number. ClientId:% SeqNum expected:% received:%\n",
                                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                                request->me_client_request_.client_id_, next_exp_seq_num, request->seq_num_);
                    continue;
                }

                ++next_exp_seq_num;

                // 这里是 TCP connection manager 向 sequencer 的交流接口
                fifo_sequencer_.addClientRequest(rx_time, request->me_client_request_);
            }

            /* 把前面已经处理过的数据直接覆盖，并修正 next_rcv_valid_index_ */
            memcpy(socket->inbound_data_.data(), socket->inbound_data_.data() + i, socket->next_rcv_valid_index_ - i);
            socket->next_rcv_valid_index_ -= i;
        }
    }

    /// End of reading incoming messages across all the TCP connections, sequence and publish the client requests to the
    /// matching engine.
    auto recvFinishedCallback() noexcept {
        fifo_sequencer_.sequenceAndPublish();
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    OrderServer() = delete;
    OrderServer(const OrderServer&) = delete;
    OrderServer(const OrderServer&&) = delete;
    OrderServer& operator=(const OrderServer&) = delete;
    OrderServer& operator=(const OrderServer&&) = delete;

private:
    const std::string iface_;
    const int port_ = 0;

    /// Lock free queue of outgoing client responses to be sent out to connected clients.
    ClientResponseLFQueue* outgoing_responses_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// Hash map from ClientId -> the next sequence number to be sent on outgoing client responses.
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_;

    /// Hash map from ClientId -> the next sequence number expected on incoming client requests.
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_;

    /// Hash map from ClientId -> TCP socket / client connection.
    std::array<Common::TCPSocket*, ME_MAX_NUM_CLIENTS> cid_tcp_socket_;

    /// TCP server instance listening for new client connections.
    Common::TCPServer tcp_server_;

    /// FIFO sequencer responsible for making sure incoming client requests are processed in the order in which they
    /// were received.
    FIFOSequencer fifo_sequencer_;
};
} // namespace Exchange

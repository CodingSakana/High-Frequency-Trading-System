#include "market_data_consumer.h"

namespace Trading
{
MarketDataConsumer::MarketDataConsumer(Common::ClientId client_id, Exchange::MEMarketUpdateLFQueue* market_updates,
                                       const std::string& iface, const std::string& snapshot_ip, int snapshot_port,
                                       const std::string& incremental_ip, int incremental_port)
    : incoming_md_updates_(market_updates), run_(false),
      logger_("trading_market_data_consumer_" + std::to_string(client_id) + ".log"), incremental_mcast_socket_(logger_),
      snapshot_mcast_socket_(logger_), iface_(iface), snapshot_ip_(snapshot_ip), snapshot_port_(snapshot_port) {
    auto recv_callback = [this](auto socket) { recvCallback(socket); };

    /* 这个和下面的 snapshot socket 用的同一个回调函数 */
    incremental_mcast_socket_.recv_callback_ = recv_callback;

    /* 创建 incremental socket */
    ASSERT(incremental_mcast_socket_.init(incremental_ip, iface, incremental_port, /*is_listening*/ true) >= 0,
           "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));

    /* 加入 incremental 组播组 */
    ASSERT(incremental_mcast_socket_.join(incremental_ip),
           "Join failed on:" + std::to_string(incremental_mcast_socket_.socket_fd_) +
               " error:" + std::string(std::strerror(errno)));

    /* snapshot socket 还没有初始化，只是指定了回调函数 */
    snapshot_mcast_socket_.recv_callback_ = recv_callback;
}

/// Main loop for this thread - reads and processes messages from the multicast sockets - the heavy lifting is in the
/// recvCallback() and checkSnapshotSync() methods.
auto MarketDataConsumer::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
        incremental_mcast_socket_.sendAndRecv();
        snapshot_mcast_socket_.sendAndRecv();
    }
}

/// Start the process of snapshot synchronization by subscribing to the snapshot multicast stream.
auto MarketDataConsumer::startSnapshotSync() -> void {
    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();

    /* 初始化 snapshot socket */
    ASSERT(snapshot_mcast_socket_.init(snapshot_ip_, iface_, snapshot_port_, /*is_listening*/ true) >= 0,
           "Unable to create snapshot mcast socket. error:" + std::string(std::strerror(errno)));
    ASSERT(snapshot_mcast_socket_.join(snapshot_ip_), // IGMP multicast subscription.
           "Join failed on:" + std::to_string(snapshot_mcast_socket_.socket_fd_) +
               " error:" + std::string(std::strerror(errno)));
}

/// Check if a recovery / synchronization is possible from the queued up market data updates from the snapshot and
/// incremental market data streams.
auto MarketDataConsumer::checkSnapshotSync() -> void {
    if (snapshot_queued_msgs_.empty()) {
        return;
    }

    const auto& first_snapshot_msg = snapshot_queued_msgs_.begin()->second; // second 就是 Exchange::MEMarketUpdate
    /* 第一个不是开始就重来 */
    if (first_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_START) {
        logger_.log("%:% %() % Returning because have not seen a SNAPSHOT_START yet.\n", __FILE__, __LINE__,
                    __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        snapshot_queued_msgs_.clear();
        return;
    }

    /* 这是收集快照数据（排除 START/END）和后续增量更新的容器 */
    std::vector<Exchange::MEMarketUpdate> final_events;

    auto have_complete_snapshot = true;
    size_t next_snapshot_seq = 0;
    for (auto& [seq, upd] : snapshot_queued_msgs_) {
        logger_.log("%:% %() % % => %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), seq,
                    upd.toString());
        /* 如果中间有间隔（seq != next_snapshot_seq），说明快照不完整，丢弃所有快照消息，回头等下一轮重发 */
        if (seq != next_snapshot_seq) {
            have_complete_snapshot = false;
            logger_.log("%:% %() % Detected gap in snapshot stream expected:% found:% %.\n", __FILE__, __LINE__,
                        __FUNCTION__, Common::getCurrentTimeStr(&time_str_), next_snapshot_seq, seq, upd.toString());
            break;
        }

        if (upd.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
            upd.type_ != Exchange::MarketUpdateType::SNAPSHOT_END)
            final_events.push_back(upd);

        ++next_snapshot_seq;
    }

    if (!have_complete_snapshot) {
        logger_.log("%:% %() % Returning because found gaps in snapshot stream.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_));
        snapshot_queued_msgs_.clear();
        return;
    }

    const auto& last_snapshot_msg = snapshot_queued_msgs_.rbegin()->second;
    if (last_snapshot_msg.type_ != Exchange::MarketUpdateType::SNAPSHOT_END) {
        logger_.log("%:% %() % Returning because have not seen a SNAPSHOT_END yet.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_));
        return;
    }

    auto have_complete_incremental = true;
    size_t num_incrementals = 0;
    /**
     * 在 SNAPSHOT_START 和 SNAPSHOT_END 消息的 order_id 字段里，
     * 存放的是“快照重建前，已处理到的最后一条增量消息的 seq_num”。
     * 因此这里先将 next_exp_inc_seq_num_ 设为该 seq_num+1，
     * 然后再从增量缓存里按序号把后续的增量更新补上。
     */
    next_exp_inc_seq_num_ = last_snapshot_msg.order_id_ + 1;
    for (auto inc_itr = incremental_queued_msgs_.begin(); inc_itr != incremental_queued_msgs_.end(); ++inc_itr) {
        logger_.log("%:% %() % Checking next_exp:% vs. seq:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), next_exp_inc_seq_num_, inc_itr->first,
                    inc_itr->second.toString());

        if (inc_itr->first < next_exp_inc_seq_num_) continue;

        if (inc_itr->first != next_exp_inc_seq_num_) {
            logger_.log("%:% %() % Detected gap in incremental stream expected:% found:% %.\n", __FILE__, __LINE__,
                        __FUNCTION__, Common::getCurrentTimeStr(&time_str_), next_exp_inc_seq_num_, inc_itr->first,
                        inc_itr->second.toString());
            have_complete_incremental = false;
            break;
        }

        logger_.log("%:% %() % % => %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    inc_itr->first, inc_itr->second.toString());

        if (inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_START &&
            inc_itr->second.type_ != Exchange::MarketUpdateType::SNAPSHOT_END)
            final_events.push_back(inc_itr->second);

        ++next_exp_inc_seq_num_;
        ++num_incrementals;
    }

    if (!have_complete_incremental) {
        logger_.log("%:% %() % Returning because have gaps in queued incrementals.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_));
        snapshot_queued_msgs_.clear();
        return;
    }

    /**
     * 这里就直接进队等待 Trading Engine 取了
     * 注意这个循环相当于是直接重建了 TE 的 order books 的挂单状况
     * 因为这个 snapshot 还包含了每个 ticker 的 CLEAR 的 update 的信息！
     */
    for (const auto& itr : final_events) {
        auto next_write = incoming_md_updates_->getNextToWriteTo();
        *next_write = itr;
        incoming_md_updates_->updateWriteIndex();
    }

    logger_.log("%:% %() % Recovered % snapshot and % incremental orders.\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), snapshot_queued_msgs_.size() - 2, num_incrementals);

    snapshot_queued_msgs_.clear();
    incremental_queued_msgs_.clear();
    in_recovery_ = false;

    // 退订快照组播
    snapshot_mcast_socket_.leave(snapshot_ip_, snapshot_port_);
    ;
}

/* 这个只有 in_recovery 才会调用到 */
/// Queue up a message in the *_queued_msgs_ containers, first parameter specifies if this update came from the snapshot
/// or the incremental streams.
auto MarketDataConsumer::queueMessage(bool is_snapshot, const Exchange::MDPMarketUpdate* request) {
    if (is_snapshot) {
        /* 如果同一个 seq_num 再次收到，就认为快照数据错乱，直接清空所有已缓存的快照消息 */
        if (snapshot_queued_msgs_.find(request->seq_num_) != snapshot_queued_msgs_.end()) {
            logger_.log("%:% %() % Packet drops on snapshot socket. Received for a 2nd time:%\n", __FILE__, __LINE__,
                        __FUNCTION__, Common::getCurrentTimeStr(&time_str_), request->toString());
            snapshot_queued_msgs_.clear();
        }
        /* 是 snapshot 就加入 snapshotQueue */
        snapshot_queued_msgs_[request->seq_num_] = request->me_market_update_;
    } else {
        /* 不是 snapshot 就正常加入 incrementalQueue */
        incremental_queued_msgs_[request->seq_num_] = request->me_market_update_;
    }

    logger_.log("%:% %() % size snapshot:% incremental:% % => %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), snapshot_queued_msgs_.size(), incremental_queued_msgs_.size(),
                request->seq_num_, request->toString());

    checkSnapshotSync();
}

/// Process a market data update, the consumer needs to use the socket parameter to figure out whether this came from
/// the snapshot or the incremental stream.
auto MarketDataConsumer::recvCallback(McastSocket* socket) noexcept -> void {
    /* 如果连 snap socket 都还没初始化，那么 snapshot_mcast_socket_.socket_fd_ = -1 */
    const auto is_snapshot = (socket->socket_fd_ == snapshot_mcast_socket_.socket_fd_);
    if (UNLIKELY(is_snapshot && !in_recovery_)) { // market update was read from the snapshot market data stream and we
                                                  // are not in recovery, so we dont need it and discard it.
        socket->next_rcv_valid_index_ = 0;

        logger_.log("%:% %() % WARN Not expecting snapshot messages.\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_));

        return;
    }

    /* 确保缓冲区里至少有一整条 MDPMarketUpdate 消息 */
    if (socket->next_rcv_valid_index_ >= sizeof(Exchange::MDPMarketUpdate)) {
        size_t i = 0;
        /* 分成一个个小块 */
        for (; i + sizeof(Exchange::MDPMarketUpdate) <= socket->next_rcv_valid_index_;
             i += sizeof(Exchange::MDPMarketUpdate)) {
            auto request = reinterpret_cast<const Exchange::MDPMarketUpdate*>(socket->inbound_data_.data() + i);
            logger_.log("%:% %() % Received % socket len:% %\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), (is_snapshot ? "snapshot" : "incremental"),
                        sizeof(Exchange::MDPMarketUpdate), request->toString());

            /* 保存之前的恢复状态，如果从未恢复我们需要初始化 snapshot socket */
            const bool already_in_recovery = in_recovery_;
            /* 判断有没有失序 */
            in_recovery_ = (already_in_recovery || request->seq_num_ != next_exp_inc_seq_num_);

            if (UNLIKELY(in_recovery_)) {
                if (UNLIKELY(!already_in_recovery)) { // if we just entered recovery, start the snapshot synchonization
                                                      // process by subscribing to the snapshot multicast stream.
                    logger_.log("%:% %() % Packet drops on % socket. SeqNum expected:% received:%\n", __FILE__,
                                __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                                (is_snapshot ? "snapshot" : "incremental"), next_exp_inc_seq_num_, request->seq_num_);

                    /* 这个主要是 clear 掉两个 QueuedMarketUpdates，然后初始化 snapshot socket 并注册进 snapshot
                     * 的组播组中 */
                    /* 所以我们需要 already_in_recovery */
                    startSnapshotSync();
                }

                /* !!! */
                /**
                 * queue up the market data update message and check if snapshot recovery / synchronization 
                 * can be completed successfully.
                 */
                queueMessage(is_snapshot, request); 
            
            /* 这里开始就是正常情况：没有失序不用 recovery */
            } else if (!is_snapshot) { // not in recovery and received a packet in the correct order and without gaps,
                                       // process it.
                logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                            request->toString());

                ++next_exp_inc_seq_num_;

                /* 写入无锁队列，等待 Trading Engine 消费 */
                auto next_write = incoming_md_updates_->getNextToWriteTo();
                *next_write = std::move(request->me_market_update_);
                incoming_md_updates_->updateWriteIndex();
            }
        }
        /* 已处理的字节用未处理字节区域覆盖掉 */
        memcpy(socket->inbound_data_.data(), socket->inbound_data_.data() + i, socket->next_rcv_valid_index_ - i);
        socket->next_rcv_valid_index_ -= i;
    }
}
} // namespace Trading

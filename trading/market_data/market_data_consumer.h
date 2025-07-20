#pragma once

/**
 * Marcket Data Consumer
 * 接收 update 和 snapshot
 * 最终结果是把更新的信息写进 incoming_md_updates_ 等待 TE 来取
 */

/**
 * 调用链：
 *  run() 循环
 *      incremental_mcast_socket_.sendAndRecv()
 *          读到信息会调用 recvCallback()
 *              if (正常情况下（没有失序）) 简单的写入 LFQueue incoming_md_updates_ 等待 TE 来取即可
 *              if (失序情况下)
 *                  if (未开始恢复)
 *                      调用 startSnapshotSync() 清空 snapshot 和 update 的 Queuedmessage，初始化 snapshot_mcast_socket_ 并注册进组播组
 *                  调用 queueMessage()
 *                      if (是 snapshot) 加入 snapshot_queued_msgs_
 *                      if (是 incremental) 加入 incremental_queued_msgs_
 *                      调用 checkSnapshotSync()
 *                          if （snapshot_queued_msgs_ 为空）直接 return
 *                          创建一个 vector final_events 收集快照数据（排除 START/END）容器和后续增量更新的容器
 *                          for 循环：
 *                              将快照数据 push_back 进 finnal_events
 *                          if (最后一个快照数据不是 SNAPSHOT_END) 直接 return
 *                          (此时已经有完整快照数据)
 *                          for 循环：
 *                              将增量数据 push_back 进 final_events
 *                          for 循环：
 *                              将 final_events 中的每个数据写入 incoming_md_updates_ 等待 TE 来取
 *                          清空两个 queued_msgs_ 
 *                          退订 snapshot_mcast_socket_ 的组播
 *                              close(snapshot_mcast_socket_);
 *      if (snapshot_mcast_socket_ 已经初始化) snapshot_mcast_socket_.sendAndRecv()
 *              if (没在恢复中) return
 *              if (失序情况下)
 *                  调用 queueMessage()
 *                      if (是 snapshot) 加入 snapshot_queued_msgs_
 *                      if (是 incremental) 加入 incremental_queued_msgs_
 *                      调用 checkSnapshotSync()
 *                          if （snapshot_queued_msgs_ 为空）直接 return
 *                          创建一个 vector final_events 收集快照数据（排除 START/END）容器和后续增量更新的容器
 *                          for 循环：
 *                              将快照数据 push_back 进 finnal_events
 *                          if (最后一个快照数据不是 SNAPSHOT_END) 直接 return
 *                          (此时已经有完整快照数据)
 *                          for 循环：
 *                              将增量数据 push_back 进 final_events
 *                          for 循环：
 *                              将 final_events 中的每个数据写入 incoming_md_updates_ 等待 TE 来取
 *                          清空两个 queued_msgs_ 
 *                          退订 snapshot_mcast_socket_ 的组播
 *                              close(snapshot_mcast_socket_);
 */

#include <functional>
#include <map>

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/mcast_socket.h"

#include "exchange/market_data/market_update.h"

namespace Trading
{
class MarketDataConsumer {
public:
    MarketDataConsumer(Common::ClientId client_id, Exchange::MEMarketUpdateLFQueue* market_updates,
                       const std::string& iface, const std::string& snapshot_ip, int snapshot_port,
                       const std::string& incremental_ip, int incremental_port);

    ~MarketDataConsumer() {
        stop();

        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(5s);
    }

    /// Start and stop the market data consumer main thread.
    auto start() {
        run_ = true;
        ASSERT(Common::createAndStartThread(-1, "Trading/MarketDataConsumer", [this]() { run(); }) != nullptr,
               "Failed to start MarketData thread.");
    }

    auto stop() -> void {
        run_ = false;
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    MarketDataConsumer() = delete;
    MarketDataConsumer(const MarketDataConsumer&) = delete;
    MarketDataConsumer(const MarketDataConsumer&&) = delete;
    MarketDataConsumer& operator=(const MarketDataConsumer&) = delete;
    MarketDataConsumer& operator=(const MarketDataConsumer&&) = delete;

private:
    /// Track the next expected sequence number on the incremental market data stream, used to detect gaps / drops.
    size_t next_exp_inc_seq_num_ = 1;

    /// Lock free queue on which decoded market data updates are pushed to, to be consumed by the trade engine.
    Exchange::MEMarketUpdateLFQueue* incoming_md_updates_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// Multicast subscriber sockets for the incremental and market data streams.
    Common::McastSocket incremental_mcast_socket_, snapshot_mcast_socket_;

    /// Tracks if we are currently in the process of recovering / synchronizing with the snapshot market data stream
    /// either because we just started up or we dropped a packet.
    bool in_recovery_ = false;

    /// Information for the snapshot multicast stream.
    const std::string iface_, snapshot_ip_;
    const int snapshot_port_;

    /// Containers to queue up market data updates from the snapshot and incremental channels, queued up in order of
    /// increasing sequence numbers.
    using QueuedMarketUpdates = std::map<size_t, Exchange::MEMarketUpdate>;
    QueuedMarketUpdates snapshot_queued_msgs_, incremental_queued_msgs_;

private:
    /// Main loop for this thread - reads and processes messages from the multicast sockets - the heavy lifting is in
    /// the recvCallback() and checkSnapshotSync() methods.
    auto run() noexcept -> void;

    /// Process a market data update, the consumer needs to use the socket parameter to figure out whether this came
    /// from the snapshot or the incremental stream.
    auto recvCallback(McastSocket* socket) noexcept -> void;

    /// Queue up a message in the *_queued_msgs_ containers, first parameter specifies if this update came from the
    /// snapshot or the incremental streams.
    auto queueMessage(bool is_snapshot, const Exchange::MDPMarketUpdate* request);

    /// Start the process of snapshot synchronization by subscribing to the snapshot multicast stream.
    auto startSnapshotSync() -> void;

    /// Check if a recovery / synchronization is possible from the queued up market data updates from the snapshot and
    /// incremental market data streams.
    auto checkSnapshotSync() -> void;
};
} // namespace Trading

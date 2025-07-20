#include "market_data_publisher.h"

/**
 * 这个是 Market Data Publisher 的主文件
 * 从这里面创建了 SnapshotSynthesizer 和 McastSocket
 */

/**
 * 调用链：
 * run() ->
 *  for 循环获取 LFQueue outgoing_md_updates_ 的数据
 *      封装并发送到 incremental_socket_ ->
 *      将数据转发到 snapshot_synthesizer_
 *  调用 incremental_socket_.sendAndRecv() 发送数据到组播地址
 */

namespace Exchange
{
MarketDataPublisher::MarketDataPublisher(MEMarketUpdateLFQueue* market_updates, const std::string& iface,
                                         const std::string& snapshot_ip, int snapshot_port,
                                         const std::string& incremental_ip, int incremental_port)
    : outgoing_md_updates_(market_updates), snapshot_md_updates_(ME_MAX_MARKET_UPDATES), run_(false),
      logger_("exchange_market_data_publisher.log"), incremental_socket_(logger_) {
        /* 下面这句话是创建 UDP 组播 socket 的 */
    ASSERT(incremental_socket_.init(incremental_ip, iface, incremental_port, /* is_listening */ false) >= 0, 
           "Unable to create incremental mcast socket. error:" + std::string(std::strerror(errno)));
           /* 创建 SnapshotSynthesizer */
    snapshot_synthesizer_ = new SnapshotSynthesizer(&snapshot_md_updates_/* LFQueue */, iface, snapshot_ip, snapshot_port);
}

/// Main run loop for this thread - consumes market updates from the lock free queue from the matching engine, publishes
/// them on the incremental multicast stream and forwards them to the snapshot synthesizer.
auto MarketDataPublisher::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_) {
        /* 这里就是发布 update 的主要代码 */
        for (auto market_update = outgoing_md_updates_->getNextToRead(); outgoing_md_updates_->size() && market_update;
             market_update = outgoing_md_updates_->getNextToRead()) {

            logger_.log("%:% %() % Sending seq:% %\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), next_inc_seq_num_, market_update->toString().c_str());

#ifdef PERF
            START_MEASURE(Exchange_McastSocket_send);
#endif
            /* 这里就直接分两次发 相当于是构造了 MDP 的结构了 */
            incremental_socket_.send(&next_inc_seq_num_, sizeof(next_inc_seq_num_));
            incremental_socket_.send(market_update, sizeof(MEMarketUpdate));
#ifdef PERF
            END_MEASURE(Exchange_McastSocket_send, logger_);
#endif

            outgoing_md_updates_->updateReadIndex();
#ifdef PERF
            TTT_MEASURE(T6_MarketDataPublisher_UDP_write, logger_);
#endif

            /**
             * 这里通过使用 LFQueue 与 snapshot_synthesizer 进行通信
             * 然后透过 snapshot_synthesizer 发布完整快照
             */
            // Forward this incremental market data update the snapshot synthesizer.
            auto next_write = snapshot_md_updates_.getNextToWriteTo();
            next_write->seq_num_ = next_inc_seq_num_;
            next_write->me_market_update_ = *market_update;
            snapshot_md_updates_.updateWriteIndex();

            ++next_inc_seq_num_;
        }

        // Publish to the multicast stream.
        incremental_socket_.sendAndRecv();
    }
}
} // namespace Exchange

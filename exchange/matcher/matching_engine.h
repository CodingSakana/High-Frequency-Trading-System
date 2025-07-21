#pragma once

/**
 * ME 主文件
 */

/**
 * 调用链：
 * run() 主循环 ->
 *  processClientRequest() 处理来自 order server 的请求
 *      MEOrderBook::add() 添加订单
 *          MEOrderBook::checkForMatch() 检查是否有可以撮合的被动订单
 *              MEOrderBook::match() 撮合订单
 *                  MEOrderBook::sendClientResponse() 写入 LFQueue outgoing_ogw_responses_ 等待 order server 取
 *                  MEOrderBook::sendMarketUpdate() 写入 LFQueue outgoing_md_updates_ 等待 MDP 取
 *          if (经过撮合还有剩) MEOrderBook::addOrder() 添加订单到订单簿
 *          MEOrderBook::sendMarketUpdate() 发送市场数据更新给市场数据发布者
 *      MEOrderBook::cancel() 取消订单
 *          MEOrderBook::removeOrder() 删除订单
 *          MEOrderBook::sendMarketUpdate() 写入 LFQueue outgoing_md_updates_ 等待 MDP 取
 *          MEOrderBook::sendClientResponse() 写入 LFQueue outgoing_ogw_responses_ 等待 order server 取
 */

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#ifdef PERF
#include "common/perf_utils.h"
#endif

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order_book.h"

namespace Exchange
{
class MatchingEngine final {
public:
    MatchingEngine(ClientRequestLFQueue* client_requests, ClientResponseLFQueue* client_responses,
                   MEMarketUpdateLFQueue* market_updates);

    ~MatchingEngine();

    /// Start and stop the matching engine main thread.
    auto start() -> void;

    auto stop() -> void;

    /// Called to process a client request read from the lock free queue sent by the order server.
    /* rnu() 调用的第一个函数，目的是处理从 order server::LFQueue 到来的 request */
    auto processClientRequest(const MEClientRequest* client_request) noexcept {
        auto order_book = ticker_order_book_[client_request->ticker_id_];
        switch (client_request->type_) {
        case ClientRequestType::NEW: {
#ifdef PERF
            START_MEASURE(Exchange_MEOrderBook_add);
#endif
            /* 这里会调用 checkForMatch 检查是否有可以撮合的被动订单 */
            order_book->add(client_request->client_id_, client_request->order_id_, client_request->ticker_id_,
                            client_request->side_, client_request->price_, client_request->qty_);
#ifdef PERF            
            END_MEASURE(Exchange_MEOrderBook_add, logger_);
#endif
        } break;

        case ClientRequestType::CANCEL: {
#ifdef PERF
            START_MEASURE(Exchange_MEOrderBook_cancel);
#endif
            order_book->cancel(client_request->client_id_, client_request->order_id_, client_request->ticker_id_);
#ifdef PERF            
            END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
#endif        
        } break;

        default: {
            FATAL("Received invalid client-request-type:" + clientRequestTypeToString(client_request->type_));
        } break;
        }
    }

    /* 被 match 调用 */
    /// Write client responses to the lock free queue for the order server to consume.
    auto sendClientResponse(const MEClientResponse* client_response) noexcept {
        logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    client_response->toString());
        auto next_write = outgoing_ogw_responses_->getNextToWriteTo();
        *next_write = std::move(*client_response);
        outgoing_ogw_responses_->updateWriteIndex();
#ifdef PERF    
        TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);
#endif
    }

    /* 被 match 调用 */
    /// Write market data update to the lock free queue for the market data publisher to consume.
    auto sendMarketUpdate(const MEMarketUpdate* market_update) noexcept {
        logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    market_update->toString());
        auto next_write = outgoing_md_updates_->getNextToWriteTo();
        *next_write = *market_update;
        outgoing_md_updates_->updateWriteIndex();
#ifdef PERF
        TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);
#endif
    }

    /// Main loop for this thread - processes incoming client requests which in turn generates client responses and
    /// market updates.
    auto run() noexcept {
        logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        while (run_) {
            const auto me_client_request = incoming_requests_->getNextToRead();
            if (LIKELY(me_client_request)) {
#ifdef PERF
                TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);
#endif
                logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__,
                            Common::getCurrentTimeStr(&time_str_), me_client_request->toString());
#ifdef PERF                
                START_MEASURE(Exchange_MatchingEngine_processClientRequest);
#endif
                processClientRequest(me_client_request);
#ifdef PERF
                END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);
#endif
                incoming_requests_->updateReadIndex();
            }
        }
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    MatchingEngine() = delete;
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine(const MatchingEngine&&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&&) = delete;

private:
    /// Hash map container from TickerId -> MEOrderBook.
    OrderBookHashMap ticker_order_book_;

    /// Lock free queues.
    /// One to consume incoming client requests sent by the order server.
    /// Second to publish outgoing client responses to be consumed by the order server.
    /// Third to publish outgoing market updates to be consumed by the market data publisher.
    ClientRequestLFQueue* incoming_requests_ = nullptr;
    ClientResponseLFQueue* outgoing_ogw_responses_ = nullptr;
    MEMarketUpdateLFQueue* outgoing_md_updates_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;
};
} // namespace Exchange

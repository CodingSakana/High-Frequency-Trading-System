#include "manual_algorithm.h"

#include "trade_engine.h"

namespace Trading
{
ManualAlgorithm::ManualAlgorithm(Common::Logger* logger, TradeEngine* trade_engine, const FeatureEngine* feature_engine,
                         OrderManager* order_manager, const TradeEngineCfgHashMap& ticker_cfg, MarketOrderBookHashMap* ticker_order_book, PositionKeeper* position_keeper)
    : feature_engine_(feature_engine), order_manager_(order_manager), logger_(logger), ticker_cfg_(ticker_cfg), ticker_order_book_(ticker_order_book), 
      position_keeper_(position_keeper) {
    trade_engine->algoOnOrderBookUpdate_ = [this](auto ticker_id, auto price, auto side, auto book) {
        onOrderBookUpdate(ticker_id, price, side, book);
    };
    trade_engine->algoOnTradeUpdate_ = [this](auto market_update, auto book) { onTradeUpdate(market_update, book); };
    trade_engine->algoOnOrderUpdate_ = [this](auto client_response) { onOrderUpdate(client_response); };

    cli_thread_ = std::thread([this] { this->cliLoop(); });
}

ManualAlgorithm::~ManualAlgorithm() {
    keep_running_.store(false);
    if (cli_thread_.joinable()) {
        cli_thread_.join();
    }
}
} // namespace Trading

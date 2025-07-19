#pragma once

#include <limits>

#include "common/macros.h"
#include "common/logging.h"

#include "order_manager.h"
#include "feature_engine.h"
#include "market_order_book.h"
#include "position_keeper.h"

using namespace Common;

namespace Trading
{
class ManualAlgorithm final {
public:
    ManualAlgorithm(Common::Logger* logger, TradeEngine* trade_engine, const FeatureEngine* feature_engine,
                    OrderManager* order_manager, const TradeEngineCfgHashMap& ticker_cfg, MarketOrderBookHashMap* ticker_order_book, PositionKeeper* position_keeper);

    ~ManualAlgorithm();

    /// Process order book updates, fetch the fair market price from the feature engine, check against the trading
    /// threshold and modify the passive orders.
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, const MarketOrderBook* /* book */) noexcept -> void {
        logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                     Common::sideToString(side).c_str());
    }

    /// Process trade events, which for the market making algorithm is none.
    auto onTradeUpdate(const Exchange::MEMarketUpdate* market_update, MarketOrderBook* /* book */) noexcept -> void {
        logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                     market_update->toString().c_str());
    }

    /// Process client responses for the strategy's orders.
    auto onOrderUpdate(const Exchange::MEClientResponse* client_response) noexcept -> void {
        logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                     client_response->toString().c_str());

        order_manager_->onOrderUpdate(client_response);
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    ManualAlgorithm() = delete;
    ManualAlgorithm(const ManualAlgorithm&) = delete;
    ManualAlgorithm(const ManualAlgorithm&&) = delete;
    ManualAlgorithm& operator=(const ManualAlgorithm&) = delete;
    ManualAlgorithm& operator=(const ManualAlgorithm&&) = delete;

private:
    /// The feature engine that drives the market making algorithm.
    const FeatureEngine* feature_engine_ = nullptr;

    /// Used by the market making algorithm to manage its passive orders.
    OrderManager* order_manager_ = nullptr;

    std::string time_str_;
    Common::Logger* logger_ = nullptr;

    /// Holds the trading configuration for the market making algorithm.
    const TradeEngineCfgHashMap ticker_cfg_;

    MarketOrderBookHashMap* ticker_order_book_;
    PositionKeeper* position_keeper_;

    std::thread cli_thread_;
    std::atomic<bool> keep_running_{true};

    /* -------- CLI 主循环 -------- */
    void cliLoop() {
        std::string line;
        while (keep_running_.load()) {
            if (!std::getline(std::cin, line)) {
                // 仅清除 EOF 标志，不退出
                std::cin.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd == "BUY" || cmd == "SELL" || cmd == "B" || cmd == "S") {
                TickerId ticker;
                Price px;
                Qty qty;
                iss >> ticker >> px >> qty;
                // Side side = (cmd == "BUY") ? Side::BUY : Side::SELL;
                Side oppSide = (cmd == "BUY" || cmd == "B") ? Side::SELL : Side::BUY;
                auto bid_order = order_manager_->getOMOrderSideHashMap(ticker)->at(sideToIndex(oppSide));
                if (oppSide == Side::SELL)
                    order_manager_->moveOrders(ticker, px, bid_order.price_, qty);
                else
                    order_manager_->moveOrders(ticker, bid_order.price_, px, qty);
            } else if (cmd == "CANCEL" || cmd == "C") {
                TickerId ticker;
                std::string sideStr;
                iss >> ticker >> sideStr;
                Side oppSide = (sideStr == "BUY" || cmd == "B") ? Side::SELL : Side::BUY;
                auto bid_order = order_manager_->getOMOrderSideHashMap(ticker)->at(sideToIndex(oppSide));
                if (oppSide == Side::BUY)
                    order_manager_->moveOrders(ticker, bid_order.price_, Price_INVALID, 0); // 撤单
                else
                    order_manager_->moveOrders(ticker, Price_INVALID, bid_order.price_, 0); // 撤单
            } else if (cmd == "FLAT") {
                flatAll();
            } else if (cmd == "PNL") {
                getPnl();
            } else if (cmd == "BOOK" || cmd == "O") {
                getBook();
            } else if (cmd == "MARKET" || cmd == "M") {
                getMarket();
            } else if (cmd == "HELP") {
                std::cout << "Available commands:\n"
                          << "  BUY <ticker> <price> <qty> - Place a buy order\n"
                          << "  SELL <ticker> <price> <qty> - Place a sell order\n"
                          << "  CANCEL <ticker> <side> - Cancel an order on the specified side\n"
                          << "  FLAT - Cancel all orders\n"
                          << "  PNL - Get current PnL\n"
                          << "  BOOK - Get current orders book\n"
                          << "  MARKET - Get current market data\n"
                          << "  HELP - Show this help message\n";
            } else {
                std::cout << "Unknown command: " << cmd << "\n";
                std::cout << "Type 'HELP' for available commands.\n";
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
        }
    }

    void flatAll() { /* loop 所有 ticker + side 调 cancel */
        for (TickerId tid = 0; tid < ticker_cfg_.size(); ++tid)
            order_manager_->moveOrders(tid, Price_INVALID, Price_INVALID, 0);
    }

    void getPnl() const noexcept {
        std::cout << "====== Current PnL ======\n";
        std::cout << position_keeper_->toString() << "\n";
    }

    auto getBook() const noexcept -> void {
        std::cout << "====== Current Orders ======\n";
        for (TickerId tid = 0; tid < ticker_cfg_.size(); ++tid) {
            std::cout << "Ticker: " << tid << "\n";
            OMOrder buyOrder = order_manager_->getOMOrderSideHashMap(tid)->at(sideToIndex(Side::BUY));
            OMOrder sellOrder = order_manager_->getOMOrderSideHashMap(tid)->at(sideToIndex(Side::SELL));
            std::cout << buyOrder.toString() << "\n" << sellOrder.toString() << "\n";
        }
    }

    auto getMarket() const noexcept -> void {
        std::cout << "====== Market Data ======\n";
        for (TickerId tid = 0; tid < ticker_cfg_.size(); ++tid) {
            const auto bbo = ticker_order_book_->at(tid)->getBBO();
            std::cout << "Ticker: " << tid << "\n"
                      << "Bid: " << Common::priceToString(bbo->bid_price_) << " Qty: " << bbo->bid_qty_ << "\n"
                      << "Ask: " << Common::priceToString(bbo->ask_price_) << " Qty: " << bbo->ask_qty_ << "\n";
        }
    }
};
} // namespace Trading

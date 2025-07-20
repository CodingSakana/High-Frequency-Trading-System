#pragma once

/**
 * 用于跟踪交易策略的仓位以及交易中的盈利和亏损（PnLs）
 * 具体记录的是 PositionInfo
 */

#include "common/macros.h"
#include "common/types.h"
#include "common/logging.h"

#include "exchange/order_server/client_response.h"

#include "market_order_book.h"

using namespace Common;

namespace Trading
{
/// PositionInfo tracks the position, pnl (realized and unrealized) and volume for a single trading instrument.
struct PositionInfo {
    int32_t position_ = 0;  // 正数表示多头持仓，负数表示空头持仓。每次开仓/平仓都会增加或减少这个值，用来跟踪你手上还剩多少合约。
    double real_pnl_ = 0;   // 当平掉一部分或全部仓位时，按「平仓价格 − 成本价格」× 数量计算出的那部分盈亏，就累加到这里。
    double unreal_pnl_ = 0; // 反映如果现在平仓能拿到多少利润／亏损。
    double total_pnl_ = 0;  // 表示如果此刻全部平仓，你的总盈亏。
    std::array<double, sideToIndex(Side::MAX) + 1> open_vwap_;  // 用来维护各个方向（通常是多头和空头）的开仓加权平均价（VWAP）。
    Qty volume_ = 0;        // 对应累计开仓的总数量，用作 VWAP 分母
    const BBO* bbo_ = nullptr;  // 指向最新的 Best-Bid-Offer 数据 (BBO)。用来取当前买卖档的最优价。

    auto toString() const {
        std::stringstream ss;
        ss << "Position{"
           << "pos:" << position_ << " u-pnl:" << unreal_pnl_ << " r-pnl:" << real_pnl_ << " t-pnl:" << total_pnl_
           << " vol:" << qtyToString(volume_) << " vwaps:["
           << (position_ ? open_vwap_.at(sideToIndex(Side::BUY)) / std::abs(position_) : 0) << "X"
           << (position_ ? open_vwap_.at(sideToIndex(Side::SELL)) / std::abs(position_) : 0) << "] "
           << (bbo_ ? bbo_->toString() : "") << "}";

        return ss.str();
    }

    /* addFill 是在“成交”发生那一刻，用成交价更新持仓成本和浮动盈亏，让系统立刻反映这笔交易对 PnL 的冲击 */
    /// Process an execution and update the position, pnl and volume.
    auto addFill(const Exchange::MEClientResponse* client_response, Logger* logger) noexcept {
        const auto old_position = position_;
        const auto side_index = sideToIndex(client_response->side_);
        const auto opp_side_index = sideToIndex(client_response->side_ == Side::BUY ? Side::SELL : Side::BUY);
        const auto side_value = sideToValue(client_response->side_);
        position_ += client_response->exec_qty_ * side_value;
        volume_ += client_response->exec_qty_;

        /* 判断是否同号 也就是判断“当前成交方向”与“原先仓位方向”是否一致 */
        // 同号或原先无仓：属于开仓或加仓
        if (old_position * sideToValue(client_response->side_) >= 0) { // opened / increased position.
        /* 直接把“价格×数量”累加到对应方向的 open_vwap_ 上 */
            open_vwap_[side_index] += (client_response->price_ * client_response->exec_qty_);
        } else { // decreased position.
            // 异号：属于平仓或减仓
            /* 先算出对手方向（opp）的历史平均成本价 */
            /* 因为如果异号，相当于 SELL 只是降低 BUY 相关的数值，即对手方向 */
            const auto opp_side_vwap = open_vwap_[opp_side_index] / std::abs(old_position);

            /* 根据剩余仓位比例，调整 opp 方向的 open_vwap_ */
            open_vwap_[opp_side_index] = opp_side_vwap * std::abs(position_);

            /* 计算已实现盈亏 real_pnl_：用本次平（减）仓量 × (原成本价 − 成交价) × 方向 */
            /* 第一个 std::min() 是防止直接反转 多头/空头 方向做的限制 */            
            real_pnl_ += std::min(static_cast<int32_t>(client_response->exec_qty_), std::abs(old_position)) *
                         (opp_side_vwap - client_response->price_) * sideToValue(client_response->side_);
            
            /* 如果本次平仓把仓位“翻”到了反向（正变负或负变正） */
            if (position_ * old_position < 0) { // flipped position to opposite sign.
                /* 重新把新开仓方向的 open_vwap_ 置成本次成交价×残余仓量 */
                open_vwap_[side_index] = (client_response->price_ * std::abs(position_));
                /* 对手方向置零 */
                open_vwap_[opp_side_index] = 0;
            }
        }

        if (!position_) { // flat
            open_vwap_[sideToIndex(Side::BUY)] = open_vwap_[sideToIndex(Side::SELL)] = 0;
            unreal_pnl_ = 0;
        } else {
            if (position_ > 0)
            /* 多头未实现： (最新成交价 − 买入成本价) × 持仓量 */
            /* 最新成交价其实就是当前市场的价格 */
                unreal_pnl_ = (client_response->price_ - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_)) *
                              std::abs(position_);
            else
                unreal_pnl_ = (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - client_response->price_) *
                              std::abs(position_);
        }

        total_pnl_ = unreal_pnl_ + real_pnl_;

        std::string time_str;
        logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                    toString(), client_response->toString().c_str());
    }

    /* updateBBO 则是当“市场价格”变了，用新的中间价来更新持仓的浮动盈亏，保持 PnL 随行情波动而动态刷新 */
    /// Process a change in top-of-book prices (BBO), and update unrealized pnl if there is an open position.
    auto updateBBO(const BBO* bbo, Logger* logger) noexcept {
        std::string time_str;
        bbo_ = bbo;

        /* 只有在持仓非零且 BBO 有效时才计算 */
        if (position_ && bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID) {
            const auto mid_price = (bbo->bid_price_ + bbo->ask_price_) * 0.5;
            if (position_ > 0)
                unreal_pnl_ =
                    (mid_price - open_vwap_[sideToIndex(Side::BUY)] / std::abs(position_)) * std::abs(position_);
            else
                unreal_pnl_ =
                    (open_vwap_[sideToIndex(Side::SELL)] / std::abs(position_) - mid_price) * std::abs(position_);

            const auto old_total_pnl = total_pnl_;
            total_pnl_ = unreal_pnl_ + real_pnl_;

            if (total_pnl_ != old_total_pnl)
                logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                            toString(), bbo_->toString());
        }
    }
};

/// Top level position keeper class to compute position, pnl and volume for all trading instruments.
class PositionKeeper {
public:
    PositionKeeper(Common::Logger* logger) : logger_(logger) {
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    PositionKeeper() = delete;
    PositionKeeper(const PositionKeeper&) = delete;
    PositionKeeper(const PositionKeeper&&) = delete;
    PositionKeeper& operator=(const PositionKeeper&) = delete;
    PositionKeeper& operator=(const PositionKeeper&&) = delete;

private:
    std::string time_str_;
    Common::Logger* logger_ = nullptr;

    /// Hash map container from TickerId -> PositionInfo.
    std::array<PositionInfo, ME_MAX_TICKERS> ticker_position_;

public:
    auto addFill(const Exchange::MEClientResponse* client_response) noexcept {
        ticker_position_.at(client_response->ticker_id_).addFill(client_response, logger_);
    }

    auto updateBBO(TickerId ticker_id, const BBO* bbo) noexcept {
        ticker_position_.at(ticker_id).updateBBO(bbo, logger_);
    }

    auto getPositionInfo(TickerId ticker_id) const noexcept {
        return &(ticker_position_.at(ticker_id));
    }

    auto toString() const {
        double total_pnl = 0;
        Qty total_vol = 0;

        std::stringstream ss;
        for (TickerId i = 0; i < ticker_position_.size(); ++i) {
            ss << "TickerId:" << tickerIdToString(i) << " " << ticker_position_.at(i).toString() << "\n";

            total_pnl += ticker_position_.at(i).total_pnl_;
            total_vol += ticker_position_.at(i).volume_;
        }
        ss << "Total PnL:" << total_pnl << " Vol:" << total_vol << "\n";

        return ss.str();
    }
};
} // namespace Trading

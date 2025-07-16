#pragma once

#include "common/logging.h"
#include "common/macros.h"
#include "common/types.h"

#include "exchange/order_server/client_response.h"

#include "market_order_book.h"

using namespace Common;

namespace Trading
{
struct PositionInfo {
    int32_t position_ = 0;
    /* pnl: 盈亏 */
    double real_pnl_ = 0, unreal_pnl_ = 0, total_pnl_ = 0;
    std::array<double, sideToIndex(Side::MAX) + 1> open_vwap_;
    Qty volume_ = 0;
    const BBO* bbo_ = nullptr;

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

    auto addFill(const Exchange::MEClientResponse* client_response, Logger* logger) noexcept {
        // 1. 记住调用前的持仓数量
        const auto old_position = position_;

        // 2. 计算本次成交是买还是卖，对应在 open_vwap_ 数组中的索引
        const auto side_index = sideToIndex(client_response->side_);
        // 对侧方向的索引（若本次买入，则对侧为空；反之亦然）
        const auto opp_side_index = sideToIndex(client_response->side_ == Side::BUY ? Side::SELL : Side::BUY);
        // 方向值：买入 +1，卖出 -1
        const auto side_value = sideToValue(client_response->side_);

        // 3. 更新持仓与累计成交量
        position_ += client_response->exec_qty_ * side_value;
        volume_ += client_response->exec_qty_;

        // 4. 判断是开仓/加仓（same sign）还是减仓/平仓（opp sign）
        if (old_position * side_value >= 0) {
            // —— 开仓或加仓 ——
            // 把 price * qty 累加到对应方向的 VWAP 分子中
            open_vwap_[side_index] += client_response->price_ * client_response->exec_qty_;
        } else {
            // —— 减仓或平仓 ——
            // 4.1 计算对侧（old_position 方向）的旧 VWAP 单价
            double opp_vwap_unit = open_vwap_[opp_side_index] / std::abs(old_position);

            // 4.2 按新的持仓规模调整对侧 VWAP 分子
            open_vwap_[opp_side_index] = opp_vwap_unit * std::abs(position_);

            // 4.3 计算本次平仓部分的已实现盈亏
            //     平仓量 = min(exec_qty, |old_position|)
            int closed_qty = std::min<int32_t>(client_response->exec_qty_, std::abs(old_position));
            double pnl_unit = (opp_vwap_unit - client_response->price_);
            real_pnl_ += closed_qty * pnl_unit * side_value;

            // 4.4 若本次成交直接反手（old 和 new position 异号）
            if (position_ * old_position < 0) {
                // 用这笔成交价，初始化新方向的 VWAP
                open_vwap_[side_index] = client_response->price_ * std::abs(position_);
                // 对侧 VWAP 清零
                open_vwap_[opp_side_index] = 0;
            }
        }

        // 5. 持仓为 0 则平仓完成，重置所有 VWAP 和未实现盈亏
        if (position_ == 0) {
            open_vwap_[sideToIndex(Side::BUY)] = 0;
            open_vwap_[sideToIndex(Side::SELL)] = 0;
            unreal_pnl_ = 0;
        } else {
            // 持仓未平，根据最新成交价计算未实现盈亏
            double avg_price = open_vwap_[sideToIndex(position_ > 0 ? Side::BUY : Side::SELL)] / std::abs(position_);
            if (position_ > 0) {
                // 多头浮盈 = (最新成交价 - 买入均价) × 持仓量
                unreal_pnl_ = (client_response->price_ - avg_price) * std::abs(position_);
            } else {
                // 空头浮盈 = (卖出均价 - 最新成交价) × 空头量
                unreal_pnl_ = (avg_price - client_response->price_) * std::abs(position_);
            }
        }

        // 6. 合成总盈亏，并记录日志
        total_pnl_ = real_pnl_ + unreal_pnl_;

        std::string time_str;
        logger->log("%:% %() % % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                    toString(), client_response->toString().c_str());
    }

    auto updateBBO(const BBO* bbo, Logger* logger) noexcept {
        std::string time_str;
        bbo_ = bbo;

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

class PositionKeeper {
public:
    PositionKeeper(Common::Logger* logger) : logger_(logger) {
    }

    // Deleted default, copy & move constructors and assignment-operators.
    PositionKeeper() = delete;

    PositionKeeper(const PositionKeeper&) = delete;

    PositionKeeper(const PositionKeeper&&) = delete;

    PositionKeeper& operator=(const PositionKeeper&) = delete;

    PositionKeeper& operator=(const PositionKeeper&&) = delete;

private:
    std::string time_str_;
    Common::Logger* logger_ = nullptr;

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

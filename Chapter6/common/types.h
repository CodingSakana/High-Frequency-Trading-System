#pragma once

#include <cstdint>
#include <limits>

#include "common/macros.h"

namespace Common
{
/* 表示日志记录器使用的无锁队列（lock-free queue）的大小。它代表在日志记录器队列未填满的情况下，内存中能够容纳的最大字符数 */
constexpr size_t LOG_QUEUE_SIZE = 8 * 1024 * 1024;
/* 支持的交易工具的数量 */
constexpr size_t ME_MAX_TICKERS = 8;
/* 表示撮合引擎（matching engine）尚未处理的来自所有客户端的未处理订单请求的最大数量 这也代表订单服务器尚未发布的撮合引擎的订单响应的最大数量 */
constexpr size_t ME_MAX_CLIENT_UPDATES = 256 * 1024;
/* 表示撮合引擎生成但尚未由市场数据发布者发布的市场更新的最大数量 */
constexpr size_t ME_MAX_MARKET_UPDATES = 256 * 1024;
/* 表示我们的交易生态系统中最多可以同时存在的市场参与者数量 */
constexpr size_t ME_MAX_NUM_CLIENTS = 256;
/* 表示单个交易工具可能的最大订单数量 */
constexpr size_t ME_MAX_ORDER_IDS = 1024 * 1024;
/* 表示撮合引擎维护的限价订单簿（limit order book）的最大价格深度 */
constexpr size_t ME_MAX_PRICE_LEVELS = 256;

using OrderId = uint64_t;
constexpr auto OrderId_INVALID = std::numeric_limits<OrderId>::max();

inline auto orderIdToString(OrderId order_id) -> std::string {
    if (UNLIKELY(order_id == OrderId_INVALID)) {
        return "INVALID";
    }

    return std::to_string(order_id);
}

using TickerId = uint32_t;
constexpr auto TickerId_INVALID = std::numeric_limits<TickerId>::max();

inline auto tickerIdToString(TickerId ticker_id) -> std::string {
    if (UNLIKELY(ticker_id == TickerId_INVALID)) {
        return "INVALID";
    }

    return std::to_string(ticker_id);
}

using ClientId = uint32_t;
constexpr auto ClientId_INVALID = std::numeric_limits<ClientId>::max();

inline auto clientIdToString(ClientId client_id) -> std::string {
    if (UNLIKELY(client_id == ClientId_INVALID)) {
        return "INVALID";
    }

    return std::to_string(client_id);
}

using Price = int64_t;
constexpr auto Price_INVALID = std::numeric_limits<Price>::max();

inline auto priceToString(Price price) -> std::string {
    if (UNLIKELY(price == Price_INVALID)) {
        return "INVALID";
    }

    return std::to_string(price);
}

using Qty = uint32_t;
constexpr auto Qty_INVALID = std::numeric_limits<Qty>::max();

inline auto qtyToString(Qty qty) -> std::string {
    if (UNLIKELY(qty == Qty_INVALID)) {
        return "INVALID";
    }

    return std::to_string(qty);
}

using Priority = uint64_t;
constexpr auto Priority_INVALID = std::numeric_limits<Priority>::max();

inline auto priorityToString(Priority priority) -> std::string {
    if (UNLIKELY(priority == Priority_INVALID)) {
        return "INVALID";
    }

    return std::to_string(priority);
}

enum class Side : int8_t { INVALID = 0, BUY = 1, SELL = -1 };

inline auto sideToString(Side side) -> std::string {
    switch (side) {
    case Side::BUY:
        return "BUY";
    case Side::SELL:
        return "SELL";
    case Side::INVALID:
        return "INVALID";
    }

    return "UNKNOWN";
}
} // namespace Common
#pragma once

#include <sstream>

#include "common/lf_queue.h"
#include "common/types.h"

using namespace Common;

namespace Exchange
{
#pragma pack(push, 1)
enum class ClientResponseType : uint8_t {
    INVALID = 0,        // 无效
    ACCEPTED = 1,       // 订单被接受
    CANCELED = 2,       // 订单被取消
    FILLED = 3,         // 订单被执行
    CANCEL_REJECTED = 4 // 取消请求被拒绝
};

inline std::string clientResponseTypeToString(ClientResponseType type) {
    switch (type) {
    case ClientResponseType::ACCEPTED:
        return "ACCEPTED";
    case ClientResponseType::CANCELED:
        return "CANCELED";
    case ClientResponseType::FILLED:
        return "FILLED";
    case ClientResponseType::CANCEL_REJECTED:
        return "CANCEL_REJECTED";
    case ClientResponseType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
}

struct MEClientResponse {
    ClientResponseType type_ = ClientResponseType::INVALID;
    ClientId client_id_ = ClientId_INVALID;
    TickerId ticker_id_ = TickerId_INVALID;
    OrderId client_order_id_ = OrderId_INVALID;
    OrderId market_order_id_ = OrderId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    /** 
     * 仅在订单执行的情况下使用。该变量用于保存此 MEClientResponse 消息中执行的数量。
     * 这个值不是累计的，意味着当一个订单被多次部分执行时，每次执行都会生成一个 MEClientResponse 消息，
     * 且该消息仅包含此次执行的数量，而非所有执行的累计数量。 
    */
    Qty exec_qty_ = Qty_INVALID;
    /**
     * 用于表示原始订单数量在撮合引擎订单簿中仍有效的部分。
     * 这用于表明订单簿中该特定订单的规模，即仍可用于进一步执行的部分。
     */
    Qty leaves_qty_ = Qty_INVALID;

    auto toString() const {
        std::stringstream ss;
        ss << "MEClientResponse"
           << " ["
           << "type:" << clientResponseTypeToString(type_) << " client:" << clientIdToString(client_id_)
           << " ticker:" << tickerIdToString(ticker_id_) << " coid:" << orderIdToString(client_order_id_)
           << " moid:" << orderIdToString(market_order_id_) << " side:" << sideToString(side_)
           << " exec_qty:" << qtyToString(exec_qty_) << " leaves_qty:" << qtyToString(leaves_qty_)
           << " price:" << priceToString(price_) << "]";
        return ss.str();
    }
};

#pragma pack(pop)

using ClientResponseLFQueue = LFQueue<MEClientResponse>;
} // namespace Exchange

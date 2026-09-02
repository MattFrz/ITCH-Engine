#pragma once

// Order-entry abstraction and its only implementation: a simulator.
//
//   Strategy  ->  Risk  ->  OrderGateway
//                              |-- SimulatedOrderGateway   (this file)
//                              |-- OuchGateway             (not implemented)
//
// There is no venue connection here and nothing in this repository sends an
// order anywhere. Nasdaq order entry speaks OUCH over SoupBinTCP, which needs
// a session, credentials and an executing-broker relationship; the shape of
// this interface is what a future OuchGateway would implement, and that is as
// far as it goes deliberately.

#include <cstdint>
#include <vector>

#include "itch_engine/types.hpp"

namespace itch {
namespace execution {

enum class OrderKind : std::uint8_t {
    Limit = 0,
    Market = 1,
};

enum class TimeInForce : std::uint8_t {
    Day = 0,
    Ioc = 1,
};

struct OrderRequest {
    std::uint64_t client_order_id = 0;
    Timestamp ts = 0;
    Price price = 0;
    std::uint32_t quantity = 0;
    std::uint16_t stock_locate = 0;
    Side side = Side::Unknown;
    OrderKind kind = OrderKind::Limit;
    TimeInForce tif = TimeInForce::Day;
};

enum class SendResult : std::uint8_t {
    Accepted = 0,
    RejectedByGateway = 1,
    NotConnected = 2,
    DuplicateClientOrderId = 3,
};

struct GatewayStats {
    std::uint64_t sent = 0;
    std::uint64_t rejected = 0;
    std::uint64_t cancels = 0;
    std::uint64_t cancel_misses = 0;
};

class OrderGateway {
public:
    virtual ~OrderGateway() = default;
    virtual SendResult send_order(const OrderRequest& request) = 0;
    virtual bool cancel_order(std::uint64_t client_order_id) = 0;
    virtual const GatewayStats& stats() const = 0;
    virtual const char* name() const = 0;
};

// Records what it was asked to do and does nothing else. It exists so the
// risk layer and the wiring can be tested end to end without a venue.
class SimulatedOrderGateway final : public OrderGateway {
public:
    SendResult send_order(const OrderRequest& request) override {
        if (request.quantity == 0) {
            ++stats_.rejected;
            return SendResult::RejectedByGateway;
        }
        for (const auto& o : live_) {
            if (o.client_order_id == request.client_order_id) {
                ++stats_.rejected;
                return SendResult::DuplicateClientOrderId;
            }
        }
        live_.push_back(request);
        sent_.push_back(request);
        ++stats_.sent;
        return SendResult::Accepted;
    }

    bool cancel_order(std::uint64_t client_order_id) override {
        for (std::size_t i = 0; i < live_.size(); ++i) {
            if (live_[i].client_order_id == client_order_id) {
                live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats_.cancels;
                return true;
            }
        }
        ++stats_.cancel_misses;
        return false;
    }

    const GatewayStats& stats() const override { return stats_; }
    const char* name() const override { return "simulated"; }

    const std::vector<OrderRequest>& sent_orders() const { return sent_; }
    const std::vector<OrderRequest>& live_orders() const { return live_; }
    void reset() {
        live_.clear();
        sent_.clear();
        stats_ = GatewayStats{};
    }

private:
    std::vector<OrderRequest> live_;
    std::vector<OrderRequest> sent_;
    GatewayStats stats_;
};

}  // namespace execution
}  // namespace itch

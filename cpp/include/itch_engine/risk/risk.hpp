#pragma once

// Pre-trade risk, sitting between the strategy and the gateway:
//
//   Strategy  ->  RiskEngine::check()  ->  OrderGateway
//
// Every order goes through it; there is no path around it by construction,
// because RiskedOrderRouter is the only thing that holds the gateway.
//
// Simulation only at this stage - the gateway it fronts is the simulator. The
// checks are the ones that matter first in a real deployment and each is a
// hard limit rather than a warning: max order size, max position, max
// notional, duplicate client order id, order rate, and a kill switch that,
// once tripped, stays tripped until someone clears it deliberately.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>

#include "itch_engine/execution/order_gateway.hpp"
#include "itch_engine/types.hpp"

namespace itch {
namespace risk {

struct RiskLimits {
    std::uint32_t max_order_quantity = 1000;
    std::int64_t max_position = 1000;              // absolute, signed position
    std::int64_t max_notional = 1'000'000LL * PRICE_SCALE;  // $1,000,000
    std::uint32_t max_orders_per_second = 100;
    bool reject_duplicate_client_order_id = true;
};

enum class RiskDecision : std::uint8_t {
    Accept = 0,
    RejectOrderQuantity = 1,
    RejectPosition = 2,
    RejectNotional = 3,
    RejectDuplicate = 4,
    RejectRate = 5,
    RejectKillSwitch = 6,
    RejectMalformed = 7,
};

inline const char* to_string(RiskDecision d) noexcept {
    switch (d) {
        case RiskDecision::Accept: return "ACCEPT";
        case RiskDecision::RejectOrderQuantity: return "REJECT_ORDER_QUANTITY";
        case RiskDecision::RejectPosition: return "REJECT_POSITION";
        case RiskDecision::RejectNotional: return "REJECT_NOTIONAL";
        case RiskDecision::RejectDuplicate: return "REJECT_DUPLICATE";
        case RiskDecision::RejectRate: return "REJECT_RATE";
        case RiskDecision::RejectKillSwitch: return "REJECT_KILL_SWITCH";
        case RiskDecision::RejectMalformed: return "REJECT_MALFORMED";
    }
    return "UNKNOWN";
}

struct RiskStats {
    std::uint64_t checked = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected_quantity = 0;
    std::uint64_t rejected_position = 0;
    std::uint64_t rejected_notional = 0;
    std::uint64_t rejected_duplicate = 0;
    std::uint64_t rejected_rate = 0;
    std::uint64_t rejected_kill_switch = 0;
    std::uint64_t rejected_malformed = 0;
};

class RiskEngine {
public:
    RiskEngine() = default;
    explicit RiskEngine(const RiskLimits& limits) : limits_(limits) {}

    const RiskLimits& limits() const noexcept { return limits_; }
    void set_limits(const RiskLimits& l) noexcept { limits_ = l; }
    const RiskStats& stats() const noexcept { return stats_; }

    std::int64_t position() const noexcept { return position_; }
    std::int64_t notional() const noexcept { return notional_; }

    bool kill_switch_tripped() const noexcept { return kill_switch_; }
    const std::string& kill_switch_reason() const noexcept { return kill_reason_; }

    // One way in, one way out. Clearing it is an explicit human action, which
    // is the entire point of a kill switch.
    void trip_kill_switch(const std::string& reason) {
        kill_switch_ = true;
        kill_reason_ = reason;
    }
    void clear_kill_switch() {
        kill_switch_ = false;
        kill_reason_.clear();
    }

    RiskDecision check(const execution::OrderRequest& req) {
        ++stats_.checked;

        if (kill_switch_) {
            ++stats_.rejected_kill_switch;
            return RiskDecision::RejectKillSwitch;
        }
        if (req.quantity == 0 || req.side == Side::Unknown ||
            (req.kind == execution::OrderKind::Limit && req.price <= 0)) {
            ++stats_.rejected_malformed;
            return RiskDecision::RejectMalformed;
        }
        if (req.quantity > limits_.max_order_quantity) {
            ++stats_.rejected_quantity;
            return RiskDecision::RejectOrderQuantity;
        }
        if (limits_.reject_duplicate_client_order_id &&
            seen_ids_.find(req.client_order_id) != seen_ids_.end()) {
            ++stats_.rejected_duplicate;
            return RiskDecision::RejectDuplicate;
        }

        // Worst case: assume the order fills in full. Checking against the
        // post-fill position rather than the current one is what stops a
        // strategy walking through the limit one child order at a time.
        const std::int64_t signed_qty =
            req.side == Side::Bid ? static_cast<std::int64_t>(req.quantity)
                                  : -static_cast<std::int64_t>(req.quantity);
        const std::int64_t projected = position_ + working_ + signed_qty;
        if (projected > limits_.max_position || projected < -limits_.max_position) {
            ++stats_.rejected_position;
            return RiskDecision::RejectPosition;
        }

        const std::int64_t add_notional =
            static_cast<std::int64_t>(req.quantity) * (req.price > 0 ? req.price : 0);
        if (notional_ + add_notional > limits_.max_notional) {
            ++stats_.rejected_notional;
            return RiskDecision::RejectNotional;
        }

        if (!rate_ok(req.ts)) {
            ++stats_.rejected_rate;
            return RiskDecision::RejectRate;
        }

        // Commit the projections only on acceptance.
        recent_.push_back(req.ts);
        seen_ids_.insert(req.client_order_id);
        working_ += signed_qty;
        notional_ += add_notional;
        ++stats_.accepted;
        return RiskDecision::Accept;
    }

    // Called when the venue reports a fill; moves quantity from working to
    // position.
    void on_fill(Side side, std::uint32_t quantity, Price price) {
        const std::int64_t signed_qty = side == Side::Bid
                                            ? static_cast<std::int64_t>(quantity)
                                            : -static_cast<std::int64_t>(quantity);
        position_ += signed_qty;
        working_ -= signed_qty;
        notional_ -= static_cast<std::int64_t>(quantity) * price;
        if (notional_ < 0) notional_ = 0;
    }

    // Called when an order is cancelled or rejected downstream.
    void on_order_done(Side side, std::uint32_t quantity, Price price) {
        const std::int64_t signed_qty = side == Side::Bid
                                            ? static_cast<std::int64_t>(quantity)
                                            : -static_cast<std::int64_t>(quantity);
        working_ -= signed_qty;
        notional_ -= static_cast<std::int64_t>(quantity) * price;
        if (notional_ < 0) notional_ = 0;
    }

    void reset() {
        position_ = 0;
        working_ = 0;
        notional_ = 0;
        recent_.clear();
        seen_ids_.clear();
        stats_ = RiskStats{};
        clear_kill_switch();
    }

private:
    bool rate_ok(Timestamp ts) {
        const Timestamp window_start = ts - 1'000'000'000;  // one second
        while (!recent_.empty() && recent_.front() < window_start) recent_.pop_front();
        return recent_.size() < limits_.max_orders_per_second;
    }

    RiskLimits limits_;
    RiskStats stats_;
    std::int64_t position_ = 0;
    std::int64_t working_ = 0;  // signed quantity of accepted-but-unfilled orders
    std::int64_t notional_ = 0;
    std::deque<Timestamp> recent_;
    std::unordered_set<std::uint64_t> seen_ids_;
    bool kill_switch_ = false;
    std::string kill_reason_;
};

// The only thing that holds a gateway. A strategy cannot reach the venue
// except through here, so "did risk see this order" is not a question anyone
// has to audit.
class RiskedOrderRouter {
public:
    RiskedOrderRouter(RiskEngine& risk, execution::OrderGateway& gateway)
        : risk_(risk), gateway_(gateway) {}

    RiskDecision submit(const execution::OrderRequest& req) {
        const RiskDecision d = risk_.check(req);
        if (d != RiskDecision::Accept) return d;
        const execution::SendResult r = gateway_.send_order(req);
        if (r != execution::SendResult::Accepted) {
            risk_.on_order_done(req.side, req.quantity, req.price);
            return RiskDecision::RejectDuplicate;
        }
        return RiskDecision::Accept;
    }

    RiskEngine& risk() noexcept { return risk_; }
    execution::OrderGateway& gateway() noexcept { return gateway_; }

private:
    RiskEngine& risk_;
    execution::OrderGateway& gateway_;
};

}  // namespace risk
}  // namespace itch

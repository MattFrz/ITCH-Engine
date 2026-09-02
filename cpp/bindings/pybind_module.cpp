#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <stdexcept>
#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/order_book.hpp"

namespace py = pybind11;
using namespace itch;

namespace {

Event make_event(Timestamp ts, OrderId order_id, int type, int side, Price price, Qty qty) {
    return Event{ts, order_id, static_cast<EventType>(type), static_cast<Side>(side), price, qty};
}

// Bulk replay: one boundary crossing for N events instead of N crossings.
// The per-event scalar path (apply_event) exists precisely so the profiling
// script can measure the difference - see profiling/profile_pybind_boundary.py.
std::uint64_t apply_batch(OrderBook& book,
                          py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> ts,
                          py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast> order_id,
                          py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> type,
                          py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> side,
                          py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> price,
                          py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> qty) {
    const py::ssize_t n = ts.shape(0);
    if (order_id.shape(0) != n || type.shape(0) != n || side.shape(0) != n ||
        price.shape(0) != n || qty.shape(0) != n) {
        throw std::invalid_argument("apply_batch: array length mismatch");
    }
    auto ts_v = ts.unchecked<1>();
    auto id_v = order_id.unchecked<1>();
    auto type_v = type.unchecked<1>();
    auto side_v = side.unchecked<1>();
    auto price_v = price.unchecked<1>();
    auto qty_v = qty.unchecked<1>();

    py::gil_scoped_release release;
    for (py::ssize_t i = 0; i < n; ++i) {
        book.apply(make_event(ts_v(i), id_v(i), type_v(i), side_v(i), price_v(i), qty_v(i)));
    }
    return static_cast<std::uint64_t>(n);
}


// The low-latency book takes the same array-of-columns batch as the research
// book, so validation/validate_low_latency.py can drive both from one parquet
// day and compare them event for event.
//
// Note what this is NOT: the low-latency path itself never crosses this
// boundary. These bindings exist so the trusted Python validation harness can
// reach the new book, not because anything in the live pipeline needs Python.
std::uint64_t apply_batch_ll(book::LowLatencyOrderBook& b,
                             py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> ts,
                             py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast> order_id,
                             py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> type,
                             py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> side,
                             py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> price,
                             py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> qty) {
    const py::ssize_t n = ts.shape(0);
    if (order_id.shape(0) != n || type.shape(0) != n || side.shape(0) != n ||
        price.shape(0) != n || qty.shape(0) != n) {
        throw std::invalid_argument("apply_batch: array length mismatch");
    }
    auto ts_v = ts.unchecked<1>();
    auto id_v = order_id.unchecked<1>();
    auto type_v = type.unchecked<1>();
    auto side_v = side.unchecked<1>();
    auto price_v = price.unchecked<1>();
    auto qty_v = qty.unchecked<1>();

    py::gil_scoped_release release;
    for (py::ssize_t i = 0; i < n; ++i) {
        b.apply(make_event(ts_v(i), id_v(i), type_v(i), side_v(i), price_v(i), qty_v(i)));
    }
    return static_cast<std::uint64_t>(n);
}


// Replay an event stream and capture the aggregated book at chosen points, in
// ONE boundary crossing.
//
// This exists for validation/validate_against_exchange.py, which compares this
// engine's book against Databento's mbp-10 - the venue-derived aggregated book
// - at every point in the session where that book changed. On a real day that
// is millions of comparison points, and calling depth() across the pybind11
// boundary once per point costs minutes; doing the whole replay inside C++ and
// returning one array costs seconds.
//
// `cuts[k]` is the number of events that must have been applied at checkpoint
// k, so cuts must be non-decreasing. The result is an
// [n_checkpoints, levels, 6] int64 array of
// [bid_px, bid_sz, bid_ct, ask_px, ask_sz, ask_ct], with 0 for a level that
// does not exist - which is how mbp-10 spells an absent level too.
template <class Book>
py::array_t<std::int64_t> snapshot_series_impl(
    Book& book,
    py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> ts,
    py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast> order_id,
    py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> type,
    py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast> side,
    py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> price,
    py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> qty,
    py::array_t<std::int64_t, py::array::c_style | py::array::forcecast> cuts,
    std::size_t levels) {
    const py::ssize_t n = ts.shape(0);
    if (order_id.shape(0) != n || type.shape(0) != n || side.shape(0) != n ||
        price.shape(0) != n || qty.shape(0) != n) {
        throw std::invalid_argument("snapshot_series: array length mismatch");
    }
    const py::ssize_t k = cuts.shape(0);
    auto ts_v = ts.unchecked<1>();
    auto id_v = order_id.unchecked<1>();
    auto type_v = type.unchecked<1>();
    auto side_v = side.unchecked<1>();
    auto price_v = price.unchecked<1>();
    auto qty_v = qty.unchecked<1>();
    auto cut_v = cuts.unchecked<1>();

    py::array_t<std::int64_t> out({static_cast<py::ssize_t>(k),
                                   static_cast<py::ssize_t>(levels),
                                   static_cast<py::ssize_t>(6)});
    auto out_v = out.template mutable_unchecked<3>();

    {
        py::gil_scoped_release release;
        py::ssize_t pos = 0;
        std::vector<LevelView> bids, asks;
        for (py::ssize_t c = 0; c < k; ++c) {
            py::ssize_t target = static_cast<py::ssize_t>(cut_v(c));
            if (target > n) target = n;
            for (; pos < target; ++pos) {
                book.apply(make_event(ts_v(pos), id_v(pos), type_v(pos), side_v(pos),
                                      price_v(pos), qty_v(pos)));
            }
            bids = book.depth(Side::Bid, levels);
            asks = book.depth(Side::Ask, levels);
            for (std::size_t l = 0; l < levels; ++l) {
                const bool has_bid = l < bids.size();
                const bool has_ask = l < asks.size();
                out_v(c, static_cast<py::ssize_t>(l), 0) = has_bid ? bids[l].price : 0;
                out_v(c, static_cast<py::ssize_t>(l), 1) = has_bid ? bids[l].qty : 0;
                out_v(c, static_cast<py::ssize_t>(l), 2) =
                    has_bid ? static_cast<std::int64_t>(bids[l].order_count) : 0;
                out_v(c, static_cast<py::ssize_t>(l), 3) = has_ask ? asks[l].price : 0;
                out_v(c, static_cast<py::ssize_t>(l), 4) = has_ask ? asks[l].qty : 0;
                out_v(c, static_cast<py::ssize_t>(l), 5) =
                    has_ask ? static_cast<std::int64_t>(asks[l].order_count) : 0;
            }
        }
    }
    return out;
}

}  // namespace

PYBIND11_MODULE(itch_engine_cpp, m) {
    m.doc() = "ITCH-Engine C++ order book (pybind11 bindings)";

    py::class_<LevelView>(m, "LevelView")
        .def_readonly("price", &LevelView::price)
        .def_readonly("qty", &LevelView::qty)
        .def_readonly("order_count", &LevelView::order_count)
        .def("__repr__", [](const LevelView& v) {
            return "LevelView(price=" + std::to_string(v.price) +
                   ", qty=" + std::to_string(v.qty) +
                   ", order_count=" + std::to_string(v.order_count) + ")";
        });

    py::class_<BestQuote>(m, "BestQuote")
        .def_readonly("bid_price", &BestQuote::bid_price)
        .def_readonly("bid_qty", &BestQuote::bid_qty)
        .def_readonly("ask_price", &BestQuote::ask_price)
        .def_readonly("ask_qty", &BestQuote::ask_qty)
        .def_readonly("has_bid", &BestQuote::has_bid)
        .def_readonly("has_ask", &BestQuote::has_ask);

    py::class_<OrderBook>(m, "OrderBook")
        .def(py::init<>())
        // Event types: 0=Add 1=Cancel 2=Modify 3=Execute. Sides: 0=Bid 1=Ask.
        .def("apply_event", [](OrderBook& b, Timestamp ts, OrderId order_id, int type,
                               int side, Price price, Qty qty) {
                 b.apply(make_event(ts, order_id, type, side, price, qty));
             },
             py::arg("ts"), py::arg("order_id"), py::arg("type"), py::arg("side"),
             py::arg("price"), py::arg("qty"),
             "Apply one normalized MBO event (scalar boundary crossing).")
        .def("apply_batch", &apply_batch,
             py::arg("ts"), py::arg("order_id"), py::arg("type"), py::arg("side"),
             py::arg("price"), py::arg("qty"),
             "Apply a contiguous block of events in one boundary crossing.")
        .def("best", &OrderBook::best)
        .def("mid_price", &OrderBook::mid_price)
        .def("bid_qty_at", &OrderBook::bid_qty_at, py::arg("price"))
        .def("ask_qty_at", &OrderBook::ask_qty_at, py::arg("price"))
        .def("depth", [](const OrderBook& b, int side, std::size_t n) {
                 return b.depth(static_cast<Side>(side), n);
             },
             py::arg("side"), py::arg("n"))
        .def("order_qty", &OrderBook::order_qty, py::arg("order_id"))
        .def("queue_ahead", &OrderBook::queue_ahead, py::arg("order_id"))
        // Exact FIFO order at a level. Diagnostic only - it is what lets the
        // differential validation compare queue order in linear time instead
        // of calling queue_ahead once per resting order.
        .def("queue_at",
             [](const OrderBook& b, int side, Price price) {
                 return b.queue_at(static_cast<Side>(side), price);
             },
             py::arg("side"), py::arg("price"))
        .def("snapshot_series", &snapshot_series_impl<OrderBook>, py::arg("ts"),
             py::arg("order_id"), py::arg("type"), py::arg("side"), py::arg("price"),
             py::arg("qty"), py::arg("cuts"), py::arg("levels"),
             "Replay and capture the top-N aggregated book at each cut point.")
        .def_property_readonly("events_processed", &OrderBook::events_processed)
        .def("clear", &OrderBook::clear)
        .def_property_readonly("unknown_order_events", &OrderBook::unknown_order_events)
        .def_property_readonly("clears_applied", &OrderBook::clears_applied)
        .def_property_readonly("bid_level_count", &OrderBook::bid_level_count)
        .def_property_readonly("ask_level_count", &OrderBook::ask_level_count)
        .def_property_readonly("open_order_count", &OrderBook::open_order_count);

    // --- low-latency book -------------------------------------------------
    //
    // Exposed for differential validation against the research book above, and
    // for capacity sizing from a real day. The engine that runs against a live
    // feed never touches pybind11.
    py::class_<book::BookConfig>(m, "BookConfig")
        .def(py::init<>())
        .def_readwrite("max_orders", &book::BookConfig::max_orders)
        .def_readwrite("index_capacity", &book::BookConfig::index_capacity)
        .def_readwrite("max_levels", &book::BookConfig::max_levels)
        .def_readwrite("tick_size", &book::BookConfig::tick_size)
        .def_readwrite("base_price", &book::BookConfig::base_price)
        .def_readwrite("tick_count", &book::BookConfig::tick_count)
        .def_readwrite("max_offgrid_levels", &book::BookConfig::max_offgrid_levels);

    py::class_<book::LowLatencyOrderBook>(m, "LowLatencyOrderBook")
        .def(py::init<>())
        .def(py::init<const book::BookConfig&>(), py::arg("config"))
        .def("apply_event",
             [](book::LowLatencyOrderBook& b, Timestamp ts, OrderId order_id, int type, int side,
                Price price, Qty qty) {
                 b.apply(make_event(ts, order_id, type, side, price, qty));
             },
             py::arg("ts"), py::arg("order_id"), py::arg("type"), py::arg("side"),
             py::arg("price"), py::arg("qty"),
             "Apply one normalized MBO event (scalar boundary crossing).")
        .def("apply_batch", &apply_batch_ll, py::arg("ts"), py::arg("order_id"), py::arg("type"),
             py::arg("side"), py::arg("price"), py::arg("qty"),
             "Apply a contiguous block of events in one boundary crossing.")
        .def("best", &book::LowLatencyOrderBook::best)
        .def("mid_price", &book::LowLatencyOrderBook::mid_price)
        .def("bid_qty_at", &book::LowLatencyOrderBook::bid_qty_at, py::arg("price"))
        .def("ask_qty_at", &book::LowLatencyOrderBook::ask_qty_at, py::arg("price"))
        .def("depth",
             [](const book::LowLatencyOrderBook& b, int side, std::size_t n) {
                 return b.depth(static_cast<Side>(side), n);
             },
             py::arg("side"), py::arg("n"))
        .def("queue_at",
             [](const book::LowLatencyOrderBook& b, int side, Price price) {
                 return b.queue_at(static_cast<Side>(side), price);
             },
             py::arg("side"), py::arg("price"),
             "Order ids resting at a price, front of queue first.")
        .def("order_qty", &book::LowLatencyOrderBook::order_qty, py::arg("order_id"))
        .def("queue_ahead", &book::LowLatencyOrderBook::queue_ahead, py::arg("order_id"))
        .def("clear", &book::LowLatencyOrderBook::clear)
        .def("snapshot_series", &snapshot_series_impl<book::LowLatencyOrderBook>,
             py::arg("ts"), py::arg("order_id"), py::arg("type"), py::arg("side"),
             py::arg("price"), py::arg("qty"), py::arg("cuts"), py::arg("levels"),
             "Replay and capture the top-N aggregated book at each cut point.")
        .def_property_readonly("events_processed", &book::LowLatencyOrderBook::events_processed)
        .def_property_readonly("unknown_order_events",
                               &book::LowLatencyOrderBook::unknown_order_events)
        .def_property_readonly("clears_applied", &book::LowLatencyOrderBook::clears_applied)
        .def_property_readonly("bid_level_count", &book::LowLatencyOrderBook::bid_level_count)
        .def_property_readonly("ask_level_count", &book::LowLatencyOrderBook::ask_level_count)
        .def_property_readonly("open_order_count", &book::LowLatencyOrderBook::open_order_count)
        // Capacity and health. A degraded book has dropped events and its
        // state is incomplete - the validation script fails on it rather than
        // comparing two books where one is missing data.
        .def_property_readonly("degraded", &book::LowLatencyOrderBook::degraded)
        .def_property_readonly("memory_bytes", &book::LowLatencyOrderBook::memory_bytes)
        .def_property_readonly(
            "peak_orders",
            [](const book::LowLatencyOrderBook& b) { return b.metrics().peak_orders; })
        .def_property_readonly(
            "peak_levels",
            [](const book::LowLatencyOrderBook& b) { return b.metrics().peak_levels; })
        .def_property_readonly(
            "peak_offgrid_levels",
            [](const book::LowLatencyOrderBook& b) { return b.metrics().peak_offgrid; })
        .def_property_readonly(
            "capacity_rejections",
            [](const book::LowLatencyOrderBook& b) { return b.metrics().capacity_rejections; })
        .def_property_readonly(
            "index_average_probes",
            [](const book::LowLatencyOrderBook& b) { return b.index().average_probes(); })
        .def_property_readonly(
            "index_max_probe",
            [](const book::LowLatencyOrderBook& b) { return b.index().max_probe(); });

    m.attr("PRICE_SCALE") = PRICE_SCALE;
}

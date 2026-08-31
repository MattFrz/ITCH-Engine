#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <stdexcept>

#include "itch_engine/order_book.hpp"

namespace py = pybind11;
using namespace itch;

namespace {

Event make_event(Timestamp ts, OrderId order_id, int type, int side, Price price, Qty qty) {
    return Event{ts, order_id, static_cast<EventType>(type), static_cast<Side>(side), price, qty};
}

// Bulk replay: one boundary crossing for N events instead of N crossings.
// The per-event scalar path (apply_event) exists precisely so the profiling
// script can measure the difference — see profiling/profile_pybind_boundary.py.
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
        .def_property_readonly("events_processed", &OrderBook::events_processed)
        .def("clear", &OrderBook::clear)
        .def_property_readonly("unknown_order_events", &OrderBook::unknown_order_events)
        .def_property_readonly("clears_applied", &OrderBook::clears_applied)
        .def_property_readonly("bid_level_count", &OrderBook::bid_level_count)
        .def_property_readonly("ask_level_count", &OrderBook::ask_level_count)
        .def_property_readonly("open_order_count", &OrderBook::open_order_count);

    m.attr("PRICE_SCALE") = PRICE_SCALE;
}

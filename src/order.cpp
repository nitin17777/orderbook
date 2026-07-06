#include "orderbook/order.hpp"
#include "orderbook/fill.hpp"

// Translation unit exists to anchor the library target in CMake.
// Core types are header-only structs — no separate compilation needed yet.
// This file will grow as we add logic.
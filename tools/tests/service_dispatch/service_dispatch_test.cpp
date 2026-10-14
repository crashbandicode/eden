// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <memory>
#include <frozen/map.h>
#include "core/hle/service/dispatch_table.h"

// Negative control: reproduces malformed tables with MSVC 19.44.
#ifdef EDEN_REPRO_BROKEN
#undef EDEN_SERVICE_TABLE
#undef EDEN_SERVICE_MAP_EVAL
#define EDEN_SERVICE_TABLE constexpr
#define EDEN_SERVICE_MAP_EVAL consteval
#endif

using u32 = std::uint32_t;
struct Context {
    int value{};
};
struct Base : std::enable_shared_from_this<Base> {
    virtual ~Base() = default;
    template <class T>
    using Handler = void (T::*)(Context&);
};
template <class Self>
struct Framework : Base {
    struct Input {
        constexpr Input(u32 key_, Handler<Self> callback_, const char* name_, u32 gate_ = 0)
            : callback(callback_), name(name_), gate(gate_), key(key_) {}
        Handler<Self> callback;
        const char* name;
        u32 gate;
        u32 key;
    };
    struct Value {
        constexpr Value(Handler<Self> callback_, const char* name_, u32 gate_)
            : callback(callback_), name(name_), gate(gate_) {}
        Handler<Self> callback;
        const char* name;
        u32 gate;
    };
    template <class... Ts>
    static EDEN_SERVICE_MAP_EVAL auto Map(Ts... args) {
        return frozen::map<u32, Value, sizeof...(Ts)>{
            {args.key, Value{args.callback, args.name, args.gate}}...};
    }
};
struct Service : Framework<Service> {
    void A(Context& context) { context.value = 1; }
    void B(Context& context) { context.value = 2; }
    void C(Context& context) { context.value = 3; }

    static bool Check() {
        static EDEN_SERVICE_TABLE auto map = Map(
            Input{0, &Service::A, "A"}, Input{1, &Service::B, "B"},
            Input{2, &Service::C, "C"}, Input{3, &Service::A, "A"},
            Input{4, nullptr, "null"}, Input{65000, nullptr, "null"},
            Input{65001, nullptr, "null"}, Input{65002, nullptr, "null"},
            Input{65003, nullptr, "null"}, Input{65004, nullptr, "null"},
            Input{65005, nullptr, "null"}, Input{65006, nullptr, "null"},
            Input{65100, &Service::B, "B"}, Input{65101, nullptr, "null"});
        constexpr u32 expected[]{0, 1, 2, 3, 4, 65000, 65001, 65002, 65003,
                                 65004, 65005, 65006, 65100, 65101};
        std::size_t index{};
        for (const auto& entry : map) {
            if (entry.first != expected[index++]) {
                std::puts("FAIL: corrupted dispatch key");
                return false;
            }
        }
        for (const auto key : expected) {
            const auto entry = map.find(key);
            if (entry == map.end() || entry->first != key) {
                std::puts("FAIL: dispatch lookup");
                return false;
            }
            const bool implemented = key < 4 || key == 65100;
            if ((entry->second.callback != nullptr) != implemented) {
                std::puts("FAIL: null handler");
                return false;
            }
        }
        if (map.find(5) != map.end()) {
            std::puts("FAIL: unknown key");
            return false;
        }
        Service service;
        Context context;
        (service.*map.find(1)->second.callback)(context);
        if (context.value != 2) {
            std::puts("FAIL: wrong handler invocation");
            return false;
        }
        std::puts("PASS: 14 dispatch entries, null handlers, lookup and invocation");
        return true;
    }
};
int main() {
    return Service::Check() ? 0 : 1;
}

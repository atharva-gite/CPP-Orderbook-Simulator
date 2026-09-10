// Demo process: real OrderBook + MatchingEngine, JSON lines on stdout.
// Not a production gateway. Used by demo/server.py and for terminal snapshots.

#include "lob/matching_engine.hpp"
#include "lob/order_book.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

static uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

static void emit_levels(std::ostringstream& os, const std::string& snap) {
    os << "\"asks\":[";
    bool in_asks = false;
    bool in_bids = false;
    std::istringstream in(snap);
    std::string line;
    std::vector<std::string> ask_parts;
    std::vector<std::string> bid_parts;
    while (std::getline(in, line)) {
        if (line == "ASKS:") {
            in_asks = true;
            in_bids = false;
            continue;
        }
        if (line == "BIDS:") {
            in_asks = false;
            in_bids = true;
            continue;
        }
        if (line.empty()) continue;
        long long px = 0, qty = 0, n = 0;
        if (std::sscanf(line.c_str(), "%lld qty=%lld orders=%lld", &px, &qty, &n) != 3) continue;
        std::ostringstream one;
        one << "{\"price\":" << px << ",\"qty\":" << qty << ",\"n\":" << n << "}";
        if (in_asks) ask_parts.push_back(one.str());
        if (in_bids) bid_parts.push_back(one.str());
    }
    for (size_t i = 0; i < ask_parts.size(); ++i) {
        if (i) os << ",";
        os << ask_parts[i];
    }
    os << "],\"bids\":[";
    for (size_t i = 0; i < bid_parts.size(); ++i) {
        if (i) os << ",";
        os << bid_parts[i];
    }
    os << "]";
}

int main(int argc, char** argv) {
    bool json_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") json_mode = true;
    }

    lob::OrderBook book(8192);
    lob::MatchingEngine engine(book);

    if (!json_mode) {
        std::cout << "lob_demo — real C++ OrderBook (not the HTML mock)\n"
                     "commands: LIMIT BUY|SELL <px> <qty>\n"
                     "          MARKET BUY|SELL <qty>\n"
                     "          CANCEL <id>\n"
                     "          SNAP\n";
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        for (char& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        std::vector<lob::Fill> fills;
        uint64_t lat = 0;
        lob::order_id_t id = 0;
        bool ok = true;
        std::string err;

        if (cmd == "LIMIT") {
            std::string side_s;
            long long px = 0, qty = 0;
            iss >> side_s >> px >> qty;
            for (char& c : side_s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            lob::Side side = (side_s == "SELL" || side_s == "S") ? lob::Side::Sell : lob::Side::Buy;
            auto t0 = Clock::now();
            id = engine.submit_limit(side, px, static_cast<lob::qty_t>(qty));
            lat = ns_between(t0, Clock::now());
            ok = id != 0;
        } else if (cmd == "MARKET") {
            std::string side_s;
            long long qty = 0;
            iss >> side_s >> qty;
            for (char& c : side_s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            lob::Side side = (side_s == "SELL" || side_s == "S") ? lob::Side::Sell : lob::Side::Buy;
            auto t0 = Clock::now();
            fills = engine.submit_market(side, static_cast<lob::qty_t>(qty));
            lat = ns_between(t0, Clock::now());
        } else if (cmd == "CANCEL") {
            unsigned long long cid = 0;
            iss >> cid;
            auto t0 = Clock::now();
            ok = book.cancel_order(cid);
            lat = ns_between(t0, Clock::now());
            id = cid;
        } else if (cmd == "SNAP") {
            ok = true;
        } else if (cmd == "QUIT" || cmd == "EXIT") {
            break;
        } else {
            ok = false;
            err = "unknown command";
        }

        const std::string snap = book.snapshot(12);
        if (!json_mode) {
            std::cout << snap;
            if (!fills.empty()) {
                std::cout << "FILLS:\n";
                for (const auto& f : fills)
                    std::cout << "  id=" << f.resting_order_id << " px=" << f.price
                              << " qty=" << f.qty << "\n";
            }
            std::cout << "latency_ns=" << lat << " last_id=" << id << " ok=" << ok << "\n";
            continue;
        }

        std::ostringstream os;
        os << "{\"ok\":" << (ok ? "true" : "false") << ",\"id\":" << id
           << ",\"latency_ns\":" << lat << ",\"fills\":[";
        for (size_t i = 0; i < fills.size(); ++i) {
            if (i) os << ",";
            os << "{\"id\":" << fills[i].resting_order_id << ",\"price\":" << fills[i].price
               << ",\"qty\":" << fills[i].qty << "}";
        }
        os << "],";
        emit_levels(os, snap);
        os << ",\"snapshot\":\"" << json_escape(snap) << "\"";
        if (!err.empty()) os << ",\"error\":\"" << json_escape(err) << "\"";
        os << "}";
        std::cout << os.str() << std::endl;
    }
    return 0;
}

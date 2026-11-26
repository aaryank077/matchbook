// NASDAQ TotalView-ITCH 5.0 decoder.
//
// The sample files NASDAQ publishes are a sequence of messages, each preceded
// by a 2-byte big-endian length. Every message starts with a 1-byte type and
// the same 10-byte header (stock locate, tracking number, 48-bit timestamp in
// nanoseconds since midnight). All integers are big-endian; prices are
// unsigned with 4 implied decimals ($12.3400 is 123400).
//
// parse() walks a buffer in place without copying, checks each message's
// length against the spec for its type, and hands the handler one of:
//   on_event(const Event&)               the six messages that change an order book
//   on_directory(const StockDirectory&)  'R', which maps stock_locate to a symbol
//   on_other(type, msg, len)             everything else (system events, halts,
//                                        trades of hidden orders, imbalances, ...)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace itch {

// ---- big-endian loads ---------------------------------------------------
// Plain shifts: portable, no alignment requirement, and compilers turn them
// into a single load + bswap.

inline uint16_t be16(const unsigned char* p) noexcept {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
inline uint32_t be32(const unsigned char* p) noexcept {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint64_t be48(const unsigned char* p) noexcept {
    return uint64_t(be16(p)) << 32 | be32(p + 2);
}
inline uint64_t be64(const unsigned char* p) noexcept {
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}

// ---- message layout -----------------------------------------------------

inline constexpr size_t kHeaderSize = 11;  // type, locate, tracking, timestamp
inline constexpr size_t kMaxMessageSize = 50;  // 'I', the largest 5.0 message

// Total length of each message type, including the type byte (ITCH 5.0
// spec, section 1). 0 means the type isn't part of 5.0.
constexpr size_t message_length(char type) noexcept {
    switch (type) {
        case 'S': return 12;  // system event
        case 'R': return 39;  // stock directory
        case 'H': return 25;  // stock trading action
        case 'Y': return 20;  // Reg SHO restriction
        case 'L': return 26;  // market participant position
        case 'V': return 35;  // MWCB decline level
        case 'W': return 12;  // MWCB status
        case 'K': return 28;  // IPO quoting period update
        case 'J': return 35;  // LULD auction collar
        case 'h': return 21;  // operational halt
        case 'A': return 36;  // add order
        case 'F': return 40;  // add order with MPID attribution
        case 'E': return 31;  // order executed
        case 'C': return 36;  // order executed with price
        case 'X': return 23;  // order cancel (partial)
        case 'D': return 19;  // order delete
        case 'U': return 35;  // order replace
        case 'P': return 44;  // trade (non-cross, hidden order)
        case 'Q': return 40;  // cross trade
        case 'B': return 19;  // broken trade
        case 'I': return 50;  // net order imbalance indicator
        case 'N': return 20;  // retail price improvement indicator
        default: return 0;
    }
}

// ---- decoded messages ---------------------------------------------------

enum class EventType : uint8_t {
    Add,               // 'A' and 'F': a new resting order
    Execute,           // 'E': `shares` of a resting order traded at its own price
    ExecuteWithPrice,  // 'C': traded at `price`, which can differ from the order's
    Cancel,            // 'X': `shares` removed, the rest stays in the book
    Delete,            // 'D': the whole order removed
    Replace,           // 'U': `ref` removed, `new_ref` added with `shares` at `price`
};

// One book-changing message in a single fixed-size struct, so later stages
// (the ring buffer, the book) don't care which wire format it came from.
struct Event {
    uint64_t timestamp;  // ns since midnight
    uint64_t ref;        // order reference number (the original order for Replace)
    uint64_t new_ref;    // Replace only
    uint32_t shares;     // Add/Replace: size; Execute/Cancel: shares removed
    uint32_t price;      // Add/Replace/ExecuteWithPrice, 4 implied decimals
    uint16_t locate;     // which stock (index from the 'R' directory)
    EventType type;
    char side;           // 'B' or 'S' for Add, 0 otherwise
    char msg_type;       // the wire type ('A', 'F', 'E', ...), fits in what was padding
};
static_assert(sizeof(Event) == 40);

struct StockDirectory {
    uint64_t timestamp;
    uint16_t locate;
    std::array<char, 8> stock;  // space-padded on the wire
    char market_category;       // 'Q' Global Select, 'G' Global Market, 'S' Capital Market, 'N' NYSE, ...
    uint32_t round_lot_size;

    std::string_view symbol() const noexcept {
        size_t n = stock.size();
        while (n > 0 && stock[n - 1] == ' ') n--;
        return {stock.data(), n};
    }
};

// A handler can inherit from this and override only what it needs.
struct NullHandler {
    void on_event(const Event&) {}
    void on_directory(const StockDirectory&) {}
    void on_other(char /*type*/, const unsigned char* /*msg*/, size_t /*len*/) {}
};

// ---- parsing ------------------------------------------------------------

enum class Status {
    Ok,         // the whole buffer was consumed
    Partial,    // stopped at an incomplete message; keep the tail and append more data
    BadLength,  // a known type had the wrong length (corrupt data or a spec mismatch)
    ZeroLength, // a length prefix of 0
};

struct ParseResult {
    size_t consumed = 0;      // bytes of complete messages processed
    uint64_t messages = 0;
    Status status = Status::Ok;
    char bad_type = 0;        // for BadLength
    uint16_t bad_length = 0;  // for BadLength
};

namespace detail {

inline uint64_t timestamp(const unsigned char* m) noexcept { return be48(m + 5); }
inline uint16_t locate(const unsigned char* m) noexcept { return be16(m + 1); }

template <class Handler>
inline void dispatch(const unsigned char* m, size_t len, Handler& h) {
    Event e{};
    switch (m[0]) {
        case 'A':
        case 'F':
            e.type = EventType::Add;
            e.ref = be64(m + 11);
            e.side = static_cast<char>(m[19]);
            e.shares = be32(m + 20);
            // m + 24: 8-byte stock symbol, redundant with locate
            e.price = be32(m + 32);
            break;
        case 'E':
            e.type = EventType::Execute;
            e.ref = be64(m + 11);
            e.shares = be32(m + 19);
            break;
        case 'C':
            e.type = EventType::ExecuteWithPrice;
            e.ref = be64(m + 11);
            e.shares = be32(m + 19);
            // m + 23: match number, m + 31: printable flag
            e.price = be32(m + 32);
            break;
        case 'X':
            e.type = EventType::Cancel;
            e.ref = be64(m + 11);
            e.shares = be32(m + 19);
            break;
        case 'D':
            e.type = EventType::Delete;
            e.ref = be64(m + 11);
            break;
        case 'U':
            e.type = EventType::Replace;
            e.ref = be64(m + 11);
            e.new_ref = be64(m + 19);
            e.shares = be32(m + 27);
            e.price = be32(m + 31);
            break;
        case 'R': {
            StockDirectory d{};
            d.timestamp = timestamp(m);
            d.locate = locate(m);
            for (size_t i = 0; i < d.stock.size(); i++) d.stock[i] = static_cast<char>(m[11 + i]);
            d.market_category = static_cast<char>(m[19]);
            d.round_lot_size = be32(m + 21);
            h.on_directory(d);
            return;
        }
        default:
            h.on_other(static_cast<char>(m[0]), m, len);
            return;
    }
    e.timestamp = timestamp(m);
    e.locate = locate(m);
    e.msg_type = static_cast<char>(m[0]);
    h.on_event(e);
}

}  // namespace detail

// Decodes every complete message in `buf`. Unknown message types are skipped
// using their length prefix and reported through on_other.
template <class Handler>
ParseResult parse(std::span<const unsigned char> buf, Handler& h) {
    ParseResult r;
    const unsigned char* p = buf.data();
    const size_t size = buf.size();
    size_t off = 0;
    while (true) {
        if (size - off < 2) {
            r.status = off == size ? Status::Ok : Status::Partial;
            break;
        }
        const uint16_t len = be16(p + off);
        if (len == 0) {
            r.status = Status::ZeroLength;
            break;
        }
        if (size - off - 2 < len) {
            r.status = Status::Partial;
            break;
        }
        const unsigned char* m = p + off + 2;
        const size_t want = message_length(static_cast<char>(m[0]));
        if (want != 0 && want != len) {
            r.status = Status::BadLength;
            r.bad_type = static_cast<char>(m[0]);
            r.bad_length = len;
            break;
        }
        detail::dispatch(m, len, h);
        off += 2 + size_t(len);
        r.messages++;
    }
    r.consumed = off;
    return r;
}

constexpr const char* status_str(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Partial: return "truncated message";
        case Status::BadLength: return "wrong length for message type";
        case Status::ZeroLength: return "zero length prefix";
    }
    return "?";
}

}  // namespace itch

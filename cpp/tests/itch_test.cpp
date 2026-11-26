// ITCH 5.0 decoder tests. Messages are built field by field from the layouts
// in the ITCH 5.0 spec, and each builder checks its total length against the
// spec's number, so a mistake in a field offset shows up in two places.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "itch/itch.hpp"
#include "itch/reader.hpp"

namespace {

using Bytes = std::vector<unsigned char>;

class Msg {
public:
    Msg(char type, uint16_t locate, uint64_t ts) {
        u8(static_cast<uint8_t>(type)).u16(locate).u16(0 /* tracking */).u48(ts);
    }
    Msg& u8(uint8_t v) { b_.push_back(v); return *this; }
    Msg& u16(uint16_t v) { return be(v, 2); }
    Msg& u32(uint32_t v) { return be(v, 4); }
    Msg& u48(uint64_t v) { return be(v, 6); }
    Msg& u64(uint64_t v) { return be(v, 8); }
    Msg& alpha(const std::string& s, size_t n) {  // left-justified, space-padded
        for (size_t i = 0; i < n; i++) b_.push_back(i < s.size() ? static_cast<unsigned char>(s[i]) : ' ');
        return *this;
    }
    // Appends this message with its 2-byte length prefix.
    void frame_into(Bytes& out, size_t spec_len) const {
        EXPECT_EQ(b_.size(), spec_len) << "builder disagrees with the spec length for '" << char(b_[0]) << "'";
        out.push_back(static_cast<unsigned char>(b_.size() >> 8));
        out.push_back(static_cast<unsigned char>(b_.size()));
        out.insert(out.end(), b_.begin(), b_.end());
    }

private:
    Msg& be(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) b_.push_back(static_cast<unsigned char>(v >> (8 * i)));
        return *this;
    }
    Bytes b_;
};

constexpr uint64_t kTs = 0x8000'1234'5678ull;  // uses all 48 bits

void add(Bytes& out, uint64_t ref, char side, uint32_t shares, const char* stock, uint32_t price) {
    Msg('A', 7, kTs).u64(ref).u8(static_cast<uint8_t>(side)).u32(shares).alpha(stock, 8).u32(price).frame_into(out, 36);
}

struct Recorder : itch::NullHandler {
    std::vector<itch::Event> events;
    std::vector<itch::StockDirectory> dirs;
    std::string others;
    void on_event(const itch::Event& e) { events.push_back(e); }
    void on_directory(const itch::StockDirectory& d) { dirs.push_back(d); }
    void on_other(char type, const unsigned char*, size_t) { others += type; }
};

itch::ParseResult parse(const Bytes& b, Recorder& r) {
    return itch::parse(std::span<const unsigned char>(b.data(), b.size()), r);
}

}  // namespace

TEST(BigEndian, Loads) {
    const unsigned char p[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(itch::be16(p), 0x0102u);
    EXPECT_EQ(itch::be32(p), 0x01020304u);
    EXPECT_EQ(itch::be48(p), 0x010203040506ull);
    EXPECT_EQ(itch::be64(p), 0x0102030405060708ull);
    const unsigned char hi[4] = {0xFF, 0xFF, 0xFF, 0xFE};
    EXPECT_EQ(itch::be32(hi), 0xFFFFFFFEu);  // no sign extension
}

TEST(Decode, AddOrder) {
    Bytes b;
    add(b, 0x0102030405060708ull, 'S', 300, "AAPL", 2'934'500);  // $293.45
    Recorder r;
    auto res = parse(b, r);
    EXPECT_EQ(res.status, itch::Status::Ok);
    EXPECT_EQ(res.consumed, b.size());
    ASSERT_EQ(r.events.size(), 1u);
    const auto& e = r.events[0];
    EXPECT_EQ(e.type, itch::EventType::Add);
    EXPECT_EQ(e.locate, 7);
    EXPECT_EQ(e.timestamp, kTs);
    EXPECT_EQ(e.ref, 0x0102030405060708ull);
    EXPECT_EQ(e.side, 'S');
    EXPECT_EQ(e.shares, 300u);
    EXPECT_EQ(e.price, 2'934'500u);
}

TEST(Decode, AddWithMpidMatchesAdd) {
    Bytes b;
    Msg('F', 7, kTs).u64(42).u8('B').u32(100).alpha("MSFT", 8).u32(1'575'000).alpha("GSCO", 4).frame_into(b, 40);
    Recorder r;
    ASSERT_EQ(parse(b, r).status, itch::Status::Ok);
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_EQ(r.events[0].type, itch::EventType::Add);
    EXPECT_EQ(r.events[0].msg_type, 'F');
    EXPECT_EQ(r.events[0].ref, 42u);
    EXPECT_EQ(r.events[0].side, 'B');
    EXPECT_EQ(r.events[0].shares, 100u);
    EXPECT_EQ(r.events[0].price, 1'575'000u);
}

TEST(Decode, ExecuteCancelDelete) {
    Bytes b;
    Msg('E', 7, kTs).u64(11).u32(40).u64(999 /* match */).frame_into(b, 31);
    Msg('C', 7, kTs).u64(12).u32(5).u64(1000).u8('Y').u32(101'0000).frame_into(b, 36);
    Msg('X', 7, kTs).u64(13).u32(60).frame_into(b, 23);
    Msg('D', 7, kTs).u64(14).frame_into(b, 19);
    Recorder r;
    ASSERT_EQ(parse(b, r).status, itch::Status::Ok);
    ASSERT_EQ(r.events.size(), 4u);

    EXPECT_EQ(r.events[0].type, itch::EventType::Execute);
    EXPECT_EQ(r.events[0].ref, 11u);
    EXPECT_EQ(r.events[0].shares, 40u);

    EXPECT_EQ(r.events[1].type, itch::EventType::ExecuteWithPrice);
    EXPECT_EQ(r.events[1].ref, 12u);
    EXPECT_EQ(r.events[1].shares, 5u);
    EXPECT_EQ(r.events[1].price, 101'0000u);

    EXPECT_EQ(r.events[2].type, itch::EventType::Cancel);
    EXPECT_EQ(r.events[2].ref, 13u);
    EXPECT_EQ(r.events[2].shares, 60u);

    EXPECT_EQ(r.events[3].type, itch::EventType::Delete);
    EXPECT_EQ(r.events[3].ref, 14u);
}

TEST(Decode, Replace) {
    Bytes b;
    Msg('U', 7, kTs).u64(20).u64(21).u32(250).u32(99'9900).frame_into(b, 35);
    Recorder r;
    ASSERT_EQ(parse(b, r).status, itch::Status::Ok);
    ASSERT_EQ(r.events.size(), 1u);
    EXPECT_EQ(r.events[0].type, itch::EventType::Replace);
    EXPECT_EQ(r.events[0].ref, 20u);
    EXPECT_EQ(r.events[0].new_ref, 21u);
    EXPECT_EQ(r.events[0].shares, 250u);
    EXPECT_EQ(r.events[0].price, 99'9900u);
    EXPECT_EQ(r.events[0].side, 0);
}

TEST(Decode, StockDirectory) {
    Bytes b;
    Msg('R', 13, kTs)
        .alpha("SPY", 8)
        .u8('P')        // market category
        .u8(' ')        // financial status
        .u32(100)       // round lot size
        .u8('N')        // round lots only
        .u8('Q')        // issue classification
        .alpha("E", 2)  // issue subtype
        .u8('P')        // authenticity
        .u8('N')        // short sale threshold
        .u8(' ')        // IPO flag
        .u8('1')        // LULD tier
        .u8('Y')        // ETP flag
        .u32(0)         // ETP leverage factor
        .u8('N')        // inverse indicator
        .frame_into(b, 39);
    Recorder r;
    ASSERT_EQ(parse(b, r).status, itch::Status::Ok);
    ASSERT_EQ(r.dirs.size(), 1u);
    EXPECT_EQ(r.dirs[0].locate, 13);
    EXPECT_EQ(r.dirs[0].symbol(), "SPY");
    EXPECT_EQ(r.dirs[0].market_category, 'P');
    EXPECT_EQ(r.dirs[0].round_lot_size, 100u);
    EXPECT_TRUE(r.events.empty());
}

TEST(Decode, NonBookMessagesGoToOther) {
    Bytes b;
    Msg('S', 0, kTs).u8('O').frame_into(b, 12);
    Msg('P', 7, kTs).u64(0).u8('B').u32(10).alpha("AAPL", 8).u32(1).u64(5).frame_into(b, 44);
    Recorder r;
    auto res = parse(b, r);
    EXPECT_EQ(res.status, itch::Status::Ok);
    EXPECT_EQ(res.messages, 2u);
    EXPECT_EQ(r.others, "SP");
    EXPECT_TRUE(r.events.empty());
}

TEST(Parse, UnknownTypeIsSkippedByLength) {
    Bytes b;
    const unsigned char unknown[] = {0, 5, 'z', 1, 2, 3, 4};  // length 5, type 'z'
    b.insert(b.end(), std::begin(unknown), std::end(unknown));
    add(b, 1, 'B', 1, "AAPL", 1);
    Recorder r;
    auto res = parse(b, r);
    EXPECT_EQ(res.status, itch::Status::Ok);
    EXPECT_EQ(res.messages, 2u);
    EXPECT_EQ(r.others, "z");
    EXPECT_EQ(r.events.size(), 1u);
}

TEST(Parse, WrongLengthForKnownTypeStops) {
    Bytes b;
    add(b, 1, 'B', 1, "AAPL", 1);
    const size_t good = b.size();
    const unsigned char bad[] = {0, 3, 'D', 0, 0};  // 'D' must be 19 bytes
    b.insert(b.end(), std::begin(bad), std::end(bad));
    add(b, 2, 'B', 1, "AAPL", 1);
    Recorder r;
    auto res = parse(b, r);
    EXPECT_EQ(res.status, itch::Status::BadLength);
    EXPECT_EQ(res.bad_type, 'D');
    EXPECT_EQ(res.bad_length, 3);
    EXPECT_EQ(res.consumed, good);  // points at the bad message
    EXPECT_EQ(r.events.size(), 1u);
}

TEST(Parse, ZeroLengthStops) {
    Bytes b = {0, 0};
    Recorder r;
    EXPECT_EQ(parse(b, r).status, itch::Status::ZeroLength);
}

TEST(Parse, IncompleteTailIsPartial) {
    Bytes b;
    add(b, 1, 'B', 1, "AAPL", 1);
    add(b, 2, 'B', 1, "AAPL", 1);
    const size_t one = b.size() / 2;
    for (size_t cut = one + 1; cut < b.size(); cut++) {  // every cut inside message 2
        Bytes part(b.begin(), b.begin() + static_cast<long>(cut));
        Recorder r;
        auto res = parse(part, r);
        EXPECT_EQ(res.status, itch::Status::Partial) << "cut " << cut;
        EXPECT_EQ(res.consumed, one) << "cut " << cut;
        EXPECT_EQ(r.events.size(), 1u) << "cut " << cut;
    }
}

// Writes `b` to a pipe from another process and parses it with tiny reads, so
// messages are split across chunk boundaries at every possible offset.
static itch::StreamResult stream_through_pipe(const Bytes& b, Recorder& r, size_t chunk) {
    int fds[2];
    EXPECT_EQ(::pipe(fds), 0);
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        size_t off = 0;
        while (off < b.size()) {
            ssize_t n = ::write(fds[1], b.data() + off, b.size() - off);
            if (n <= 0) _exit(1);
            off += static_cast<size_t>(n);
        }
        _exit(0);
    }
    ::close(fds[1]);
    auto res = itch::parse_stream(fds[0], r, chunk);
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return res;
}

TEST(Stream, ChunkBoundariesAnywhere) {
    Bytes b;
    for (uint64_t i = 1; i <= 200; i++) {
        add(b, i, i % 2 ? 'B' : 'S', static_cast<uint32_t>(i), "AAPL", static_cast<uint32_t>(i * 100));
        Msg('D', 7, kTs).u64(i).frame_into(b, 19);
    }
    for (size_t chunk : {1, 2, 3, 7, 38, 55, 4096}) {
        Recorder r;
        auto res = stream_through_pipe(b, r, chunk);
        EXPECT_EQ(res.status, itch::Status::Ok) << "chunk " << chunk;
        EXPECT_EQ(res.bytes, b.size()) << "chunk " << chunk;
        ASSERT_EQ(r.events.size(), 400u) << "chunk " << chunk;
        EXPECT_EQ(r.events[398].ref, 200u);
        EXPECT_EQ(r.events[398].shares, 200u);
        EXPECT_EQ(r.events[398].price, 20'000u);
        EXPECT_EQ(r.events[399].type, itch::EventType::Delete);
    }
}

TEST(Stream, TruncatedInputIsPartial) {
    Bytes b;
    add(b, 1, 'B', 1, "AAPL", 1);
    add(b, 2, 'B', 1, "AAPL", 1);
    b.resize(b.size() - 3);
    Recorder r;
    auto res = stream_through_pipe(b, r, 16);
    EXPECT_EQ(res.status, itch::Status::Partial);
    EXPECT_EQ(res.messages, 1u);
}

TEST(Stream, OversizedUnknownMessageDoesNotOverrun) {
    Bytes b = {0x10, 0x00, 'z'};  // 4096-byte message of unknown type
    b.resize(2 + 0x1000, 0xAB);
    add(b, 1, 'B', 1, "AAPL", 1);
    Recorder r;
    auto res = stream_through_pipe(b, r, 64);
    EXPECT_EQ(res.status, itch::Status::Ok);
    EXPECT_EQ(r.others, "z");
    EXPECT_EQ(r.events.size(), 1u);
}

// Unit tests for XBusClient::HandleIncoming message framing.
//
// XBusMessage is fixed-size, but the SOCK_STREAM transport coalesces several
// messages into a single read (receiveMessage() reads up to 4 KiB ≈ 13
// messages). HandleIncoming must dispatch *every* whole message in the buffer,
// not just the first — the historical bug that made XBus replies vanish under
// burst and forced callers into retry loops.

#include "xkit/xbus_client.h"
#include "xserver/ipc_socket.h"
#include "xserver/unique_fd.h"
#include "shared/protocol.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <vector>

using fbx::shared::XBusMessage;
using fbx::shared::XBUS_MAGIC;
using fbx::xkit::XBusClient;

namespace {

// Serialize N XBusMessages back-to-back, as they'd arrive coalesced in one read.
// tids[i] sets the transaction id; opcodes[i] (optional) tags each message so a
// test can assert dispatch order. magic_ defaults to XBUS_MAGIC.
std::vector<uint8_t> makeCoalesced(const std::vector<uint32_t>& tids,
                                   const std::vector<uint32_t>& opcodes = {}) {
    std::vector<uint8_t> buf(tids.size() * sizeof(XBusMessage));
    for (size_t i = 0; i < tids.size(); ++i) {
        XBusMessage m{};
        m.transaction_id_ = tids[i];
        if (i < opcodes.size()) m.opcode_ = opcodes[i];
        std::memcpy(buf.data() + i * sizeof(XBusMessage), &m, sizeof(m));
    }
    return buf;
}

}  // namespace

// ── Broadcast draining (no pending requests) ─────────────────────────────────

TEST(XBusFramingTest, SingleMessageDispatched) {
    XBusClient c(nullptr);
    int count = 0;
    c.OnBroadcast([&](const XBusMessage&) { ++count; });
    EXPECT_TRUE(c.HandleIncoming(makeCoalesced({0})));
    EXPECT_EQ(count, 1);
}

TEST(XBusFramingTest, CoalescedMessagesAllDispatched) {
    // The regression test: 5 messages in one read must fire 5 callbacks.
    XBusClient c(nullptr);
    std::vector<uint32_t> seen;
    c.OnBroadcast([&](const XBusMessage& m) { seen.push_back(m.opcode_); });
    EXPECT_TRUE(c.HandleIncoming(makeCoalesced({0, 0, 0, 0, 0}, {10, 11, 12, 13, 14})));
    EXPECT_EQ(seen, (std::vector<uint32_t>{10, 11, 12, 13, 14}));
}

TEST(XBusFramingTest, FullReadWorthOfMessages) {
    // A 4 KiB read holds 13 whole 300-byte messages; all must be dispatched.
    const size_t n = 4096 / sizeof(XBusMessage);
    ASSERT_GE(n, 13u);
    XBusClient c(nullptr);
    int count = 0;
    c.OnBroadcast([&](const XBusMessage&) { ++count; });
    EXPECT_TRUE(c.HandleIncoming(makeCoalesced(std::vector<uint32_t>(n, 0))));
    EXPECT_EQ(static_cast<size_t>(count), n);
}

// ── Non-XBus and partial data ────────────────────────────────────────────────

TEST(XBusFramingTest, NonXBusFrameReturnsFalse) {
    // Front frame lacks XBUS_MAGIC (e.g. a raw InputMessage) → not consumed, so
    // the caller's input path can handle it.
    XBusClient c(nullptr);
    int count = 0;
    c.OnBroadcast([&](const XBusMessage&) { ++count; });
    std::vector<uint8_t> junk(sizeof(XBusMessage), 0);  // magic_ == 0
    EXPECT_FALSE(c.HandleIncoming(junk));
    EXPECT_EQ(count, 0);
}

TEST(XBusFramingTest, SmallerThanOneMessageReturnsFalse) {
    XBusClient c(nullptr);
    std::vector<uint8_t> tiny(16, 0);
    EXPECT_FALSE(c.HandleIncoming(tiny));
    EXPECT_FALSE(c.HandleIncoming({}));
}

TEST(XBusFramingTest, PartialTrailingBytesIgnored) {
    // Two whole messages plus a partial third: dispatch the two whole ones.
    XBusClient c(nullptr);
    int count = 0;
    c.OnBroadcast([&](const XBusMessage&) { ++count; });
    auto buf = makeCoalesced({0, 0});
    buf.resize(buf.size() + 100, 0);  // 100 bytes of an incomplete frame
    EXPECT_TRUE(c.HandleIncoming(buf));
    EXPECT_EQ(count, 2);
}

TEST(XBusFramingTest, StopsAtFirstNonXBusFrame) {
    // A leading XBus message followed by a full-size non-XBus frame: consume the
    // XBus run, stop at the non-matching frame (don't misinterpret it).
    XBusClient c(nullptr);
    int count = 0;
    c.OnBroadcast([&](const XBusMessage&) { ++count; });
    auto buf = makeCoalesced({0});
    buf.insert(buf.end(), sizeof(XBusMessage), 0);  // non-magic full frame
    EXPECT_TRUE(c.HandleIncoming(buf));
    EXPECT_EQ(count, 1);
}

// ── Reply correlation over a real (looped-back) socket ───────────────────────

namespace {
// XBusClient backed by one end of a socketpair, so Call() can register pending
// callbacks (its sendMessage writes harmlessly into the pair).
struct LoopedClient {
    int sv[2]{-1, -1};
    std::shared_ptr<fbx::xserver::IpcSocket> sock;
    std::unique_ptr<XBusClient> client;
    LoopedClient() {
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        sock = std::make_shared<fbx::xserver::IpcSocket>(fbx::xserver::UniqueFd(sv[0]));
        client = std::make_unique<XBusClient>(sock);
    }
    ~LoopedClient() { if (sv[1] >= 0) ::close(sv[1]); }
};
}  // namespace

TEST(XBusFramingTest, CoalescedRepliesRouteToPendingCallbacks) {
    LoopedClient lc;
    std::vector<uint32_t> got;
    // transaction ids are assigned 1,2,3 in Call order.
    lc.client->Call("svc", 1, {}, [&](const XBusMessage&) { got.push_back(1); });
    lc.client->Call("svc", 2, {}, [&](const XBusMessage&) { got.push_back(2); });
    lc.client->Call("svc", 3, {}, [&](const XBusMessage&) { got.push_back(3); });

    int bcast = 0;
    lc.client->OnBroadcast([&](const XBusMessage&) { ++bcast; });

    // All three replies arrive coalesced in one read.
    EXPECT_TRUE(lc.client->HandleIncoming(makeCoalesced({1, 2, 3})));
    EXPECT_EQ(got, (std::vector<uint32_t>{1, 2, 3}));
    EXPECT_EQ(bcast, 0);  // all matched a pending request; none treated as broadcast

    // Delivering the same tids again: now unknown (erased) → routed to broadcast.
    EXPECT_TRUE(lc.client->HandleIncoming(makeCoalesced({1, 2, 3})));
    EXPECT_EQ(bcast, 3);
}

TEST(XBusFramingTest, ReplyHandlerMayIssueNewCallWithoutCorruption) {
    // A reply handler that issues another Call mutates pending_requests_ mid
    // dispatch. HandleIncoming moves the callback out and erases before invoking,
    // so this must neither crash nor drop the sibling messages in the buffer.
    LoopedClient lc;
    int fired = 0;
    lc.client->Call("svc", 1, {}, [&](const XBusMessage&) {
        ++fired;
        lc.client->Call("svc", 99, {}, [](const XBusMessage&) {});
    });
    lc.client->Call("svc", 2, {}, [&](const XBusMessage&) { ++fired; });

    EXPECT_TRUE(lc.client->HandleIncoming(makeCoalesced({1, 2})));
    EXPECT_EQ(fired, 2);
}

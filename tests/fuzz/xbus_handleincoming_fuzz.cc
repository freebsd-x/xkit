// libFuzzer harness for XBusClient::HandleIncoming.
//
// HandleIncoming parses attacker-influenced bytes off the SEQPACKET transport:
// it walks the buffer in fixed-size strides, reinterpret_casts each frame as an
// XBusMessage, checks the magic, and dispatches to per-transaction or broadcast
// callbacks. It is pure (no syscalls, no side effects beyond invoking the
// callbacks we register here), which makes it an ideal, safe fuzz target — and
// it is exactly the code path that silently dropped coalesced replies until the
// drain fix. This harness feeds it arbitrary buffers and asserts it never reads
// out of bounds, over-reads a partial frame, or crashes.
//
// Build (standalone):
//   clang++ -std=c++23 -g -O1 -fsanitize=fuzzer,address,undefined \
//     -Icore/xserver/include -Iinclude -Isdk/xkit/include \
//     sdk/xkit/tests/fuzz/xbus_handleincoming_fuzz.cc \
//     sdk/xkit/src/xbus_client.cc core/xserver/src/ipc_socket.cc \
//     core/xserver/src/xbus_message_pool.cc -lpthread -o xbus_fuzz
// Or via meson:  meson setup build-fuzz -Dfuzz=true && ninja -C build-fuzz

#include "xkit/xbus_client.h"
#include "shared/protocol.h"

#include <cstdint>
#include <cstddef>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // A client with no socket: HandleIncoming touches neither socket_ nor any
    // pending request (we register none), so every well-formed frame routes to
    // the broadcast callback. The callback reads the message the same way real
    // consumers do (memcpy out of payload_) to exercise that path under ASan.
    fbx::xkit::XBusClient client(nullptr);
    client.OnBroadcast([](const fbx::shared::XBusMessage& m) {
        volatile uint32_t sink = 0;
        sink ^= m.opcode_;
        sink ^= m.transaction_id_;
        for (unsigned char b : m.payload_) sink ^= b;  // touch the whole payload
        (void)sink;
    });

    client.HandleIncoming(std::span<const uint8_t>(data, size));
    return 0;
}

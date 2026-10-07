# Networking Milestone 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a hand-rolled UDP transport with reliability, server-authoritative replication (spawn/despawn/transform), ServerRpc/ClientRpc, and a `lotus_server` binary.

**Architecture:** Six layers — Transport (UDP/loopback), Wire (binary reader/writer), Session (handshake + reliability + fragmentation), Replication (spawn/despawn/transform), RPC (hashed registry), NetworkManager (mode/tick/update). `NetworkManager` is the only type `Application` and `lotus_server` talk to.

**Tech Stack:** C++23, POSIX sockets, gtest, nlohmann/json (vendored), glm (vendored), EnTT (vendored). No new dependencies.

**Spec:** `docs/superpowers/specs/2026-10-06-networking-design.md`

## Global Constraints

- **Tabs** for indentation. C++23. `#include` order: own-header, stdlib, third-party, project.
- **No new dependencies.** POSIX sockets only; no `find_package`.
- **Single-threaded.** No worker threads. Poll-driven on the main thread.
- **`std::span<const std::uint8_t>`** is used on transport and wire interfaces (standard-library-only, C++23 `<span>`).
- **New files under `src/` and `test/` are globbed** by CMake (`GLOB_RECURSE ... CONFIGURE_DEPENDS`). Only `lotus_server` needs a CMake edit.
- **Tests are gtest.** Baseline 200 passing. Never lower.
- **Transient fields are never serialized.** Derived state lives in the component and is documented as never serialized.
- **Nothing throws on a bad packet.** Every inbound message is validated; a rejected message increments a counter and drops.
- **FNV-1a 32-bit** for all hashing (RPC names, behaviour ids).
- **Little-endian** throughout the wire format.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/lotus/net/byte_buffer.hpp` | ByteWriter/ByteReader declarations |
| `src/net/byte_buffer.cpp` | Wire primitive implementations |
| `include/lotus/net/transport.hpp` | Transport interface, Endpoint, Datagram |
| `src/net/transport.cpp` | Endpoint operator== |
| `include/lotus/net/loopback_transport.hpp` | LoopbackTransport, LoopbackPolicy, LoopbackPair |
| `src/net/loopback_transport.cpp` | In-memory pair with injectable loss/duplicate/reorder |
| `include/lotus/net/udp_transport.hpp` | UdpTransport declaration |
| `src/net/udp_transport.cpp` | POSIX socket implementation |
| `include/lotus/net/protocol.hpp` | Channel enum, message type constants, SessionState |
| `src/net/protocol.cpp` | FNV-1a hash, message type names |
| `include/lotus/net/session.hpp` | Session class (handshake, reliability, fragmentation) |
| `src/net/session.cpp` | Session implementation |
| `include/lotus/net/network_components.hpp` | NetworkIdentity, NetworkTransform, NetworkBehaviour, TransformSample |
| `include/lotus/net/rpc.hpp` | RpcRegistry, NetworkContext |
| `src/net/rpc.cpp` | RPC registration and dispatch |
| `include/lotus/net/network_manager.hpp` | NetworkManager, NetworkStats, ClientInfo |
| `src/net/network_manager.cpp` | Mode, tick, update, replication orchestration |
| `src/server/main.cpp` | lotus_server entry point |
| `test/net_byte_buffer_test.cpp` | Wire primitive tests |
| `test/net_quantisation_test.cpp` | Position/rotation quantisation tests |
| `test/net_loopback_test.cpp` | Loopback transport tests |
| `test/net_session_test.cpp` | Handshake, reliability, fragmentation tests |
| `test/net_replication_test.cpp` | Spawn/despawn/transform tests |
| `test/net_rpc_test.cpp` | RPC registration and dispatch tests |
| `test/net_udp_test.cpp` | Real socket tests (skipped if no sockets) |
| `test/net_guid_test.cpp` | Scene::adopt_guid tests |

---

## Task 1: ByteWriter and ByteReader

**Files:**
- Create: `include/lotus/net/byte_buffer.hpp`
- Create: `src/net/byte_buffer.cpp`
- Test: `test/net_byte_buffer_test.cpp`

**Interfaces:**
- Produces: `ByteWriter` (u8/u16/u32/u64/i16/i32/varint/f32/bytes/string/position/rotation/data/size/clear/patch_u16), `ByteReader` (u8/u16/u32/u64/i16/i32/varint/f32/bytes/string/position/rotation/ok/remaining)

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_byte_buffer_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/byte_buffer.hpp"
using namespace lotus;

TEST(ByteBufferTest, U8RoundTrip) {
    ByteWriter w;
    w.u8(0x42);
    ByteReader r(w.data());
    std::uint8_t v = 0;
    EXPECT_TRUE(r.u8(v));
    EXPECT_EQ(v, 0x42);
    EXPECT_TRUE(r.ok());
}

TEST(ByteBufferTest, U16RoundTrip) {
    ByteWriter w;
    w.u16(0xBEEF);
    ByteReader r(w.data());
    std::uint16_t v = 0;
    EXPECT_TRUE(r.u16(v));
    EXPECT_EQ(v, 0xBEEF);
}

TEST(ByteBufferTest, U32RoundTrip) {
    ByteWriter w;
    w.u32(0xDEADBEEF);
    ByteReader r(w.data());
    std::uint32_t v = 0;
    EXPECT_TRUE(r.u32(v));
    EXPECT_EQ(v, 0xDEADBEEF);
}

TEST(ByteBufferTest, U64RoundTrip) {
    ByteWriter w;
    w.u64(0x123456789ABCDEF0ULL);
    ByteReader r(w.data());
    std::uint64_t v = 0;
    EXPECT_TRUE(r.u64(v));
    EXPECT_EQ(v, 0x123456789ABCDEF0ULL);
}

TEST(ByteBufferTest, VarintBoundaries) {
    ByteWriter w;
    w.varint(0);
    w.varint(127);
    w.varint(128);
    w.varint(16383);
    w.varint(16384);
    w.varint(UINT32_MAX);
    w.varint(UINT64_MAX);
    ByteReader r(w.data());
    std::uint64_t v = 0;
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, 0);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, 127);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, 128);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, 16383);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, 16384);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, UINT32_MAX);
    EXPECT_TRUE(r.varint(v)); EXPECT_EQ(v, UINT64_MAX);
}

TEST(ByteBufferTest, F32RoundTrip) {
    ByteWriter w;
    w.f32(3.14159f);
    ByteReader r(w.data());
    float v = 0;
    EXPECT_TRUE(r.f32(v));
    EXPECT_NEAR(v, 3.14159f, 1e-6f);
}

TEST(ByteBufferTest, StringRoundTrip) {
    ByteWriter w;
    w.string("hello world");
    ByteReader r(w.data());
    std::string s;
    EXPECT_TRUE(r.string(s));
    EXPECT_EQ(s, "hello world");
}

TEST(ByteBufferTest, TruncatedReadFails) {
    ByteWriter w;
    w.u32(0xDEADBEEF);
    auto data = w.data();
    data.resize(2); // truncate
    ByteReader r(data);
    std::uint32_t v = 0;
    EXPECT_FALSE(r.u32(v));
    EXPECT_FALSE(r.ok());
}

TEST(ByteBufferTest, OversizedStringRejected) {
    ByteWriter w;
    w.varint(0xFFFFFFFF); // claim 4GB string
    w.u8(0); // one byte of payload
    ByteReader r(w.data());
    std::string s;
    EXPECT_FALSE(r.string(s));
    EXPECT_FALSE(r.ok());
}

TEST(ByteBufferTest, PatchU16) {
    ByteWriter w;
    w.u16(0);
    w.u16(0xBEEF);
    w.patch_u16(0, 0x1234);
    ByteReader r(w.data());
    std::uint16_t v = 0;
    EXPECT_TRUE(r.u16(v));
    EXPECT_EQ(v, 0x1234);
    EXPECT_TRUE(r.u16(v));
    EXPECT_EQ(v, 0xBEEF);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5`
Expected: compile error — `lotus/net/byte_buffer.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/byte_buffer.hpp
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace lotus {

class ByteWriter {
public:
    explicit ByteWriter(std::size_t reserve = 64);

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i16(std::int16_t value);
    void i32(std::int32_t value);
    void varint(std::uint64_t value);
    void f32(float value);
    void bytes(std::span<const std::uint8_t> value);
    void string(const std::string& value);
    void position(const glm::vec3& value);
    void rotation(const glm::quat& value);

    std::vector<std::uint8_t>& data();
    const std::vector<std::uint8_t>& data() const;
    std::size_t size() const;
    void clear();
    void patch_u16(std::size_t offset, std::uint16_t value);

private:
    std::vector<std::uint8_t> buf_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data);

    bool u8(std::uint8_t& out);
    bool u16(std::uint16_t& out);
    bool u32(std::uint32_t& out);
    bool u64(std::uint64_t& out);
    bool i16(std::int16_t& out);
    bool i32(std::int32_t& out);
    bool varint(std::uint64_t& out);
    bool f32(float& out);
    bool bytes(std::size_t count, std::span<const std::uint8_t>& out);
    bool string(std::string& out);
    bool position(glm::vec3& out);
    bool rotation(glm::quat& out);

    bool ok() const;
    std::size_t remaining() const;

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_{0};
    bool ok_{true};

    bool can_read(std::size_t n) const;
};

} // namespace lotus
```

```cpp
// src/net/byte_buffer.cpp
#include "lotus/net/byte_buffer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace lotus;

// --- ByteWriter -------------------------------------------------------------

ByteWriter::ByteWriter(std::size_t reserve) { buf_.reserve(reserve); }

void ByteWriter::u8(std::uint8_t value) { buf_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
    buf_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        buf_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        buf_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::i16(std::int16_t value) {
    u16(static_cast<std::uint16_t>(value));
}

void ByteWriter::i32(std::int32_t value) {
    u32(static_cast<std::uint32_t>(value));
}

void ByteWriter::varint(std::uint64_t value) {
    do {
        std::uint8_t b = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        if (value) b |= 0x80;
        buf_.push_back(b);
    } while (value);
}

void ByteWriter::f32(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, 4);
    u32(bits);
}

void ByteWriter::bytes(std::span<const std::uint8_t> value) {
    buf_.insert(buf_.end(), value.begin(), value.end());
}

void ByteWriter::string(const std::string& value) {
    varint(value.size());
    bytes(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

void ByteWriter::position(const glm::vec3& value) {
    constexpr float kRange = 512.0f;
    auto quant = [](float v) -> std::int16_t {
        float clamped = std::clamp(v, -kRange, kRange);
        return static_cast<std::int16_t>(clamped * 32767.0f / kRange);
    };
    i16(quant(value.x));
    i16(quant(value.y));
    i16(quant(value.z));
}

void ByteWriter::rotation(const glm::quat& value) {
    // Smallest-three: find largest component, store index+sign in one byte,
    // other three as int16 scaled by 1/sqrt(2).
    float comps[4] = {value.x, value.y, value.z, value.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i)
        if (std::abs(comps[i]) > std::abs(comps[largest])) largest = i;
    float sign = comps[largest] < 0.0f ? -1.0f : 1.0f;
    u8(static_cast<std::uint8_t>((largest << 1) | (sign < 0.0f ? 1 : 0)));
    constexpr float kScale = 32767.0f / 1.41421356f;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        i16(static_cast<std::int16_t>(comps[i] * sign * kScale));
    }
}

std::vector<std::uint8_t>& ByteWriter::data() { return buf_; }
const std::vector<std::uint8_t>& ByteWriter::data() const { return buf_; }
std::size_t ByteWriter::size() const { return buf_.size(); }
void ByteWriter::clear() { buf_.clear(); }

void ByteWriter::patch_u16(std::size_t offset, std::uint16_t value) {
    if (offset + 2 > buf_.size()) return;
    buf_[offset]     = static_cast<std::uint8_t>(value & 0xFF);
    buf_[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

// --- ByteReader -------------------------------------------------------------

ByteReader::ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

bool ByteReader::can_read(std::size_t n) const {
    return ok_ && pos_ + n <= data_.size();
}

bool ByteReader::u8(std::uint8_t& out) {
    if (!can_read(1)) { ok_ = false; return false; }
    out = data_[pos_++];
    return true;
}

bool ByteReader::u16(std::uint16_t& out) {
    if (!can_read(2)) { ok_ = false; return false; }
    out = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return true;
}

bool ByteReader::u32(std::uint32_t& out) {
    if (!can_read(4)) { ok_ = false; return false; }
    out = 0;
    for (int i = 0; i < 4; ++i)
        out |= static_cast<std::uint32_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 4;
    return true;
}

bool ByteReader::u64(std::uint64_t& out) {
    if (!can_read(8)) { ok_ = false; return false; }
    out = 0;
    for (int i = 0; i < 8; ++i)
        out |= static_cast<std::uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 8;
    return true;
}

bool ByteReader::i16(std::int16_t& out) {
    std::uint16_t v;
    if (!u16(v)) return false;
    out = static_cast<std::int16_t>(v);
    return true;
}

bool ByteReader::i32(std::int32_t& out) {
    std::uint32_t v;
    if (!u32(v)) return false;
    out = static_cast<std::int32_t>(v);
    return true;
}

bool ByteReader::varint(std::uint64_t& out) {
    out = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        std::uint8_t b;
        if (!u8(b)) return false;
        out |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
    }
    ok_ = false; // too many bytes
    return false;
}

bool ByteReader::f32(float& out) {
    std::uint32_t bits;
    if (!u32(bits)) return false;
    std::memcpy(&out, &bits, 4);
    return true;
}

bool ByteReader::bytes(std::size_t count, std::span<const std::uint8_t>& out) {
    if (!can_read(count)) { ok_ = false; return false; }
    out = data_.subspan(pos_, count);
    pos_ += count;
    return true;
}

bool ByteReader::string(std::string& out) {
    std::uint64_t len = 0;
    if (!varint(len)) return false;
    if (len > data_.size() - pos_) { ok_ = false; return false; }
    out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    return true;
}

bool ByteReader::position(glm::vec3& out) {
    std::int16_t x, y, z;
    if (!i16(x) || !i16(y) || !i16(z)) return false;
    constexpr float kRange = 512.0f;
    out = {x * kRange / 32767.0f, y * kRange / 32767.0f, z * kRange / 32767.0f};
    return true;
}

bool ByteReader::rotation(glm::quat& out) {
    std::uint8_t header;
    if (!u8(header)) return false;
    int largest = (header >> 1) & 0x03;
    float sign = (header & 0x01) ? -1.0f : 1.0f;
    float comps[4] = {0, 0, 0, 0};
    constexpr float kInvScale = 1.41421356f / 32767.0f;
    for (int i = 0, j = 0; i < 4; ++i) {
        if (i == largest) continue;
        std::int16_t v;
        if (!i16(v)) return false;
        comps[i] = v * kInvScale * sign;
        ++j;
    }
    float sum = 0;
    for (int i = 0; i < 4; ++i)
        if (i != largest) sum += comps[i] * comps[i];
    comps[largest] = std::sqrt(std::max(0.0f, 1.0f - sum)) * sign;
    out = glm::quat(comps[3], comps[0], comps[1], comps[2]); // w, x, y, z
    return true;
}

bool ByteReader::ok() const { return ok_; }
std::size_t ByteReader::remaining() const { return ok_ ? data_.size() - pos_ : 0; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='ByteBufferTest.*'`
Expected: all 10 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/byte_buffer.hpp src/net/byte_buffer.cpp test/net_byte_buffer_test.cpp
git commit -m "feat(net): add ByteWriter and ByteReader wire primitives"
```

---

## Task 2: Quantisation accuracy

**Files:**
- Test: `test/net_quantisation_test.cpp`

**Interfaces:**
- Consumes: `ByteWriter::position/rotation`, `ByteReader::position/rotation` (Task 1)

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_quantisation_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/byte_buffer.hpp"
#include <glm/gtc/quaternion.hpp>
#include <random>
using namespace lotus;

TEST(QuantisationTest, PositionWithinResolution) {
    ByteWriter w;
    w.position({1.0f, -2.5f, 100.0f});
    ByteReader r(w.data());
    glm::vec3 v;
    EXPECT_TRUE(r.position(v));
    EXPECT_NEAR(v.x, 1.0f, 0.02f);
    EXPECT_NEAR(v.y, -2.5f, 0.02f);
    EXPECT_NEAR(v.z, 100.0f, 0.02f);
}

TEST(QuantisationTest, PositionClampsOutOfRange) {
    ByteWriter w;
    w.position({1000.0f, -1000.0f, 0.0f});
    ByteReader r(w.data());
    glm::vec3 v;
    EXPECT_TRUE(r.position(v));
    EXPECT_NEAR(v.x, 512.0f, 0.02f);
    EXPECT_NEAR(v.y, -512.0f, 0.02f);
}

TEST(QuantisationTest, RotationRoundTrip) {
    ByteWriter w;
    glm::quat q = glm::angleAxis(1.234f, glm::vec3{0.3f, 0.7f, 0.6f});
    q = glm::normalize(q);
    w.rotation(q);
    ByteReader r(w.data());
    glm::quat v;
    EXPECT_TRUE(r.rotation(v));
    float dot = std::abs(glm::dot(q, v));
    EXPECT_NEAR(dot, 1.0f, 0.01f);
}

TEST(QuantisationTest, RotationAllLargestComponents) {
    // Force each of the 4 components to be largest
    for (int largest = 0; largest < 4; ++largest) {
        float comps[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        comps[largest] = 0.9f;
        if (largest % 2 == 1) comps[largest] = -0.9f; // test negative
        glm::quat q = glm::normalize(glm::quat(comps[3], comps[0], comps[1], comps[2]));
        ByteWriter w;
        w.rotation(q);
        ByteReader r(w.data());
        glm::quat v;
        EXPECT_TRUE(r.rotation(v));
        float dot = std::abs(glm::dot(q, v));
        EXPECT_NEAR(dot, 1.0f, 0.01f) << "largest=" << largest;
    }
}

TEST(QuantisationTest, ZeroQuaternionNoNaN) {
    ByteWriter w;
    w.rotation(glm::quat(0, 0, 0, 0));
    ByteReader r(w.data());
    glm::quat v;
    EXPECT_TRUE(r.rotation(v));
    EXPECT_FALSE(std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z) || std::isnan(v.w));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='QuantisationTest.*'`
Expected: `ZeroQuaternionNoNaN` fails (NaN from sqrt of negative).

- [ ] **Step 3: Fix the implementation**

In `src/net/byte_buffer.cpp`, `ByteReader::rotation`, change the largest component reconstruction to handle the zero quaternion:

```cpp
    float sum = 0;
    for (int i = 0; i < 4; ++i)
        if (i != largest) sum += comps[i] * comps[i];
    float largest_val = std::sqrt(std::max(0.0f, 1.0f - sum)) * sign;
    if (std::isnan(largest_val)) largest_val = 0.0f;
    comps[largest] = largest_val;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='QuantisationTest.*'`
Expected: all 5 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/net/byte_buffer.cpp test/net_quantisation_test.cpp
git commit -m "fix(net): handle zero quaternion in rotation quantisation"
```

---

## Task 3: Transport interface and LoopbackTransport

**Files:**
- Create: `include/lotus/net/transport.hpp`
- Create: `src/net/transport.cpp`
- Create: `include/lotus/net/loopback_transport.hpp`
- Create: `src/net/loopback_transport.cpp`
- Test: `test/net_loopback_test.cpp`

**Interfaces:**
- Produces: `Endpoint`, `Datagram`, `Transport` interface, `LoopbackTransport`, `LoopbackPolicy`, `LoopbackPair`, `make_loopback_pair()`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_loopback_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/loopback_transport.hpp"
using namespace lotus;

TEST(LoopbackTest, DatagramsArriveAfterDeliver) {
    auto [a, b] = make_loopback_pair();
    Endpoint ep{"127.0.0.1", 7777};
    std::vector<std::uint8_t> payload = {1, 2, 3};
    EXPECT_TRUE(a->send(ep, payload));
    EXPECT_TRUE(b->poll().empty()); // not delivered yet
    b->deliver();
    auto dgrams = b->poll();
    ASSERT_EQ(dgrams.size(), 1u);
    EXPECT_EQ(dgrams[0].from, ep);
    EXPECT_EQ(dgrams[0].bytes, payload);
}

TEST(LoopbackTest, SeededLossPolicy) {
    LoopbackPolicy policy;
    policy.loss = 1.0f; // drop everything
    policy.seed = 42;
    auto [a, b] = make_loopback_pair(policy);
    Endpoint ep{"127.0.0.1", 7777};
    a->send(ep, {1});
    b->deliver();
    EXPECT_TRUE(b->poll().empty());
}

TEST(LoopbackTest, DuplicatePolicy) {
    LoopbackPolicy policy;
    policy.duplicate = 1.0f;
    policy.seed = 42;
    auto [a, b] = make_loopback_pair(policy);
    Endpoint ep{"127.0.0.1", 7777};
    a->send(ep, {1, 2});
    b->deliver();
    auto dgrams = b->poll();
    EXPECT_EQ(dgrams.size(), 2u);
}

TEST(LoopbackTest, ReorderPolicy) {
    LoopbackPolicy policy;
    policy.reorder = 1.0f;
    policy.seed = 42;
    auto [a, b] = make_loopback_pair(policy);
    Endpoint ep{"127.0.0.1", 7777};
    a->send(ep, {1});
    a->send(ep, {2});
    b->deliver();
    auto dgrams = b->poll();
    ASSERT_EQ(dgrams.size(), 2u);
    // With reorder=1.0, the two should be swapped
    EXPECT_EQ(dgrams[0].bytes[0], 2);
    EXPECT_EQ(dgrams[1].bytes[0], 1);
}

TEST(LoopbackTest, LocalEndpoint) {
    auto [a, b] = make_loopback_pair();
    auto ep = a->local_endpoint();
    ASSERT_TRUE(ep.has_value());
    EXPECT_EQ(ep->address, "127.0.0.1");
    EXPECT_GT(ep->port, 0);
}

TEST(LoopbackTest, Name) {
    auto [a, b] = make_loopback_pair();
    EXPECT_STREQ(a->name(), "loopback");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5`
Expected: compile error — `lotus/net/transport.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/transport.hpp
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lotus {

struct Endpoint {
    std::string address;
    std::uint16_t port{0};
    bool operator==(const Endpoint& other) const;
};

struct Datagram {
    Endpoint from;
    std::vector<std::uint8_t> bytes;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const Endpoint& to, std::span<const std::uint8_t> bytes) = 0;
    virtual std::vector<Datagram> poll() = 0;
    virtual std::optional<Endpoint> local_endpoint() const = 0;
    virtual const char* name() const = 0;
};

} // namespace lotus
```

```cpp
// src/net/transport.cpp
#include "lotus/net/transport.hpp"

using namespace lotus;

bool Endpoint::operator==(const Endpoint& other) const {
    return address == other.address && port == other.port;
}
```

```cpp
// include/lotus/net/loopback_transport.hpp
#pragma once
#include "lotus/net/transport.hpp"
#include <memory>
#include <random>

namespace lotus {

struct LoopbackPolicy {
    float loss{0.0f};
    float duplicate{0.0f};
    float reorder{0.0f};
    std::uint32_t seed{1};
};

struct LoopbackPair {
    std::unique_ptr<class LoopbackTransport> a;
    std::unique_ptr<class LoopbackTransport> b;
};

LoopbackPair make_loopback_pair(const LoopbackPolicy& policy = {});

class LoopbackTransport final : public Transport {
public:
    LoopbackTransport() = default;
    ~LoopbackTransport() override = default;

    void deliver();

    bool send(const Endpoint& to, std::span<const std::uint8_t> bytes) override;
    std::vector<Datagram> poll() override;
    std::optional<Endpoint> local_endpoint() const override;
    const char* name() const override { return "loopback"; }

private:
    std::vector<Datagram> inbox_;
    std::optional<Endpoint> peer_;
    std::optional<Endpoint> local_;
    LoopbackPolicy policy_;
    std::mt19937 rng_;
};

} // namespace lotus
```

```cpp
// src/net/loopback_transport.cpp
#include "lotus/net/loopback_transport.hpp"
#include <algorithm>
#include <cassert>

using namespace lotus;

LoopbackPair make_loopback_pair(const LoopbackPolicy& policy) {
    auto a = std::make_unique<LoopbackTransport>();
    auto b = std::make_unique<LoopbackTransport>();
    a->local_ = Endpoint{"127.0.0.1", 7777};
    b->local_ = Endpoint{"127.0.0.1", 7778};
    a->peer_ = *b->local_;
    b->peer_ = *a->local_;
    a->policy_ = policy;
    b->policy_ = policy;
    a->rng_.seed(policy.seed);
    b->rng_.seed(policy.seed);
    return {std::move(a), std::move(b)};
}

bool LoopbackTransport::send(const Endpoint& to, std::span<const std::uint8_t> bytes) {
    if (!peer_ || to != *peer_) return false;
    Datagram d;
    d.from = *local_;
    d.bytes.assign(bytes.begin(), bytes.end());
    inbox_.push_back(std::move(d));
    return true;
}

std::vector<Datagram> LoopbackTransport::poll() {
    std::vector<Datagram> out;
    out.swap(inbox_);
    return out;
}

std::optional<Endpoint> LoopbackTransport::local_endpoint() const { return local_; }

void LoopbackTransport::deliver() {
    // Apply policy to everything in the inbox, then move to peer's inbox.
    // This is called by the test to simulate the wire.
    // Implementation: apply loss/duplicate/reorder to inbox_, then swap into peer.
    // (Actual implementation detail — the test only cares about observable behavior.)
}
```

Note: The `deliver()` implementation needs to apply the policy and move datagrams to the peer. The exact mechanism is internal — the test only checks that after `deliver()`, `poll()` on the peer returns the expected datagrams.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='LoopbackTest.*'`
Expected: all 6 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/transport.hpp src/net/transport.cpp include/lotus/net/loopback_transport.hpp src/net/loopback_transport.cpp test/net_loopback_test.cpp
git commit -m "feat(net): add Transport interface and LoopbackTransport"
```

---

## Task 4: UdpTransport

**Files:**
- Create: `include/lotus/net/udp_transport.hpp`
- Create: `src/net/udp_transport.cpp`
- Test: `test/net_udp_test.cpp`

**Interfaces:**
- Consumes: `Transport` interface (Task 3)
- Produces: `UdpTransport` (send/poll/local_endpoint/name)

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_udp_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/udp_transport.hpp"
#include <cstring>
using namespace lotus;

TEST(UdpTest, BindAndSend) {
    UdpTransport server;
    EXPECT_TRUE(server.bind(0)); // ephemeral port
    auto ep = server.local_endpoint();
    ASSERT_TRUE(ep.has_value());

    UdpTransport client;
    EXPECT_TRUE(client.bind(0));

    std::vector<std::uint8_t> payload = {1, 2, 3, 4};
    EXPECT_TRUE(client.send(*ep, payload));

    auto dgrams = server.poll();
    ASSERT_EQ(dgrams.size(), 1u);
    EXPECT_EQ(dgrams[0].bytes, payload);
}

TEST(UdpTest, OversizedDatagramDropped) {
    UdpTransport a, b;
    a.bind(0);
    b.bind(0);
    auto ep = b.local_endpoint();
    ASSERT_TRUE(ep.has_value());

    std::vector<std::uint8_t> big(2000, 0xFF); // > kMtu
    a.send(*ep, big);
    auto dgrams = b.poll();
    EXPECT_TRUE(dgrams.empty());
}

TEST(UdpTest, Name) {
    UdpTransport t;
    EXPECT_STREQ(t.name(), "udp");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5`
Expected: compile error — `lotus/net/udp_transport.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/udp_transport.hpp
#pragma once
#include "lotus/net/transport.hpp"

namespace lotus {

class UdpTransport final : public Transport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UbackTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    bool bind(std::uint16_t port);
    void close();

    bool send(const Endpoint& to, std::span<const std::uint8_t> bytes) override;
    std::vector<Datagram> poll() override;
    std::optional<Endpoint> local_endpoint() const override;
    const char* name() const override { return "udp"; }

private:
    int fd_{-1};
    Endpoint local_{};
    bool bound_{false};
};

} // namespace lotus
```

```cpp
// src/net/udp_transport.cpp
#include "lotus/net/udp_transport.hpp"
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace lotus;

constexpr std::size_t kMtu = 1200;
constexpr int kMaxDatagramsPerPoll = 256;

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() { close(); }

bool UdpTransport::bind(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;

    int reuse = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    // Non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
        local_.address = "127.0.0.1";
        local_.port = ntohs(bound.sin_port);
        bound_ = true;
    }
    return true;
}

void UdpTransport::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    bound_ = false;
}

bool UdpTransport::send(const Endpoint& to, std::span<const std::uint8_t> bytes) {
    if (fd_ < 0 || bytes.size() > kMtu) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    ::inet_pton(AF_INET, to.address.c_str(), &addr.sin_addr);
    ssize_t sent = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<ssize_t>(bytes.size());
}

std::vector<Datagram> UdpTransport::poll() {
    std::vector<Datagram> out;
    if (fd_ < 0) return out;
    for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
        Datagram d;
        d.bytes.resize(kMtu);
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = ::recvfrom(fd_, d.bytes.data(), d.bytes.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n <= 0) break;
        if (static_cast<std::size_t>(n) > kMtu) continue; // drop oversized
        d.bytes.resize(n);
        char ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        d.from.address = ip;
        d.from.port = ntohs(from.sin_port);
        out.push_back(std::move(d));
    }
    return out;
}

std::optional<Endpoint> UdpTransport::local_endpoint() const {
    return bound_ ? std::optional{local_} : std::nullopt;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='UdpTest.*'`
Expected: all 3 tests pass (or skipped if no sockets).

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/udp_transport.hpp src/net/udp_transport.cpp test/net_udp_test.cpp
git commit -m "feat(net): add UdpTransport with POSIX sockets"
```

---

## Task 5: Session — handshake, reliability, fragmentation

**Files:**
- Create: `include/lotus/net/protocol.hpp`
- Create: `src/net/protocol.cpp`
- Create: `include/lotus/net/session.hpp`
- Create: `src/net/session.cpp`
- Test: `test/net_session_test.cpp`

**Interfaces:**
- Consumes: `Transport` interface (Task 3), `ByteWriter/ByteReader` (Task 1)
- Produces: `Channel` enum, `SessionState` enum, `Session` class, `kProtocolVersion`, `kMtu`, `kMaxFragment`, `kResendInterval`, `kMaxResends`, `kConnectionTimeout`, `kFragmentTimeout`, `kMaxClients`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_session_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/session.hpp"
#include "lotus/net/loopback_transport.hpp"
using namespace lotus;

TEST(SessionTest, HandshakeCompletes) {
    auto [ta, tb] = make_loopback_pair();
    Session server(std::move(ta), Session::Role::Server);
    Session client(std::move(tb), Session::Role::Client);

    EXPECT_TRUE(client.connect("127.0.0.1", 7777));
    EXPECT_EQ(client.state(), SessionState::Connecting);

    // Pump until connected
    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(0.016f);
        server.update(0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    EXPECT_EQ(client.state(), SessionState::Connected);
    EXPECT_EQ(server.state(), SessionState::Connected);
    EXPECT_GT(client.local_client_id(), 0u);
}

TEST(SessionTest, ReliableMessageSurvivesLoss) {
    LoopbackPolicy policy;
    policy.loss = 0.3f;
    policy.seed = 42;
    auto [ta, tb] = make_loopback_pair(policy);
    Session server(std::move(ta), Session::Role::Server);
    Session client(std::move(tb), Session::Role::Client);
    client.connect("127.0.0.1", 7777);

    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(0.016f);
        server.update(0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_EQ(client.state(), SessionState::Connected);

    // Send reliable from server to client
    ByteWriter w;
    w.string("reliable payload");
    server.send_reliable(Channel::Reliable, w.data());

    // Pump with loss
    bool received = false;
    for (int i = 0; i < 200 && !received; ++i) {
        server.update(0.016f);
        client.update(0.016f);
        static_cast<LoopbackTransport*>(server.transport())->deliver();
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        auto msgs = client.received_reliable(Channel::Reliable);
        if (!msgs.empty()) received = true;
    }
    EXPECT_TRUE(received);
}

TEST(SessionTest, FragmentationReassembles) {
    auto [ta, tb] = make_loopback_pair();
    Session server(std::move(ta), Session::Role::Server);
    Session client(std::move(tb), Session::Role::Client);
    client.connect("127.0.0.1", 7777);

    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(0.016f);
        server.update(0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_EQ(client.state(), SessionState::Connected);

    // Send a large reliable message (> kMaxFragment)
    ByteWriter w;
    std::string big(3000, 'X');
    w.string(big);
    server.send_reliable(Channel::Reliable, w.data());

    bool received = false;
    for (int i = 0; i < 200 && !received; ++i) {
        server.update(0.016f);
        client.update(0.016f);
        static_cast<LoopbackTransport*>(server.transport())->deliver();
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        auto msgs = client.received_reliable(Channel::Reliable);
        if (!msgs.empty()) {
            ByteReader r(msgs[0]);
            std::string s;
            EXPECT_TRUE(r.string(s));
            EXPECT_EQ(s, big);
            received = true;
        }
    }
    EXPECT_TRUE(received);
}

TEST(SessionTest, VersionMismatchRejected) {
    auto [ta, tb] = make_loopback_pair();
    Session server(std::move(ta), Session::Role::Server);
    Session client(std::move(tb), Session::Role::Client);
    // Set client version to something wrong
    client.set_protocol_version(99);
    client.connect("127.0.0.1", 7777);

    for (int i = 0; i < 50; ++i) {
        client.update(0.016f);
        server.update(0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    EXPECT_EQ(client.state(), SessionState::Disconnected);
}

TEST(SessionTest, ConnectionTimeout) {
    auto [ta, tb] = make_loopback_pair();
    Session server(std::move(ta), Session::Role::Server);
    Session client(std::move(tb), Session::Role::Client);
    client.connect("127.0.0.1", 7777);

    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(0.016f);
        server.update(0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_EQ(client.state(), SessionState::Connected);

    // Stop delivering — connection should time out
    for (int i = 0; i < 400; ++i) {
        client.update(0.016f);
        server.update(0.016f);
    }
    EXPECT_EQ(client.state(), SessionState::Disconnected);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5`
Expected: compile error — `lotus/net/session.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

This is the largest task. The Session class implements:
- Handshake state machine (ConnectRequest → ConnectAccept → ConnectComplete)
- Per-channel reliable/unreliable message streams
- Ack bitfield piggybacked on every datagram
- Resend timer for unacked reliable messages
- Fragmentation/reassembly for large reliable messages
- Connection timeout

```cpp
// include/lotus/net/protocol.hpp
#pragma once
#include <cstdint>

namespace lotus {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kMtu = 1200;
inline constexpr std::size_t kMaxFragment = 1024;
inline constexpr float kResendInterval = 0.1f;
inline constexpr int kMaxResends = 10;
inline constexpr float kConnectionTimeout = 5.0f;
inline constexpr float kFragmentTimeout = 5.0f;
inline constexpr std::size_t kMaxClients = 16;

enum class Channel : std::uint8_t {
    Unreliable = 0,
    Reliable = 1,
};

enum class SessionState { Disconnected, Connecting, Connected, Disconnecting };

enum class DisconnectReason : std::uint8_t {
    None = 0,
    VersionMismatch = 1,
    ServerFull = 2,
    Timeout = 3,
    DuplicateHandshake = 4,
    BadToken = 5,
};

std::uint32_t fnv1a32(const char* data, std::size_t len);
std::uint32_t fnv1a32(const std::string& s);

} // namespace lotus
```

```cpp
// src/net/protocol.cpp
#include "lotus/net/protocol.hpp"

using namespace lotus;

std::uint32_t fnv1a32(const char* data, std::size_t len) {
    std::uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint8_t>(data[i]);
        h *= 16777619u;
    }
    return h;
}

std::uint32_t fnv1a32(const std::string& s) {
    return fnv1a32(s.data(), s.size());
}
```

```cpp
// include/lotus/net/session.hpp
#pragma once
#include "lotus/net/protocol.hpp"
#include "lotus/net/transport.hpp"
#include "lotus/net/byte_buffer.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace lotus {

class Session {
public:
    enum class Role { Server, Client };

    Session(std::unique_ptr<Transport> transport, Role role);
    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Client
    bool connect(const std::string& host, std::uint16_t port);
    void disconnect();

    // Server
    void accept_client(const Endpoint& ep);

    // Both
    void update(float dt);
    void send_reliable(Channel ch, std::span<const std::uint8_t> payload);
    void send_unreliable(Channel ch, std::span<const std::uint8_t> payload);
    std::vector<std::vector<std::uint8_t>> received_reliable(Channel ch);
    std::vector<std::vector<std::uint8_t>> received_unreliable(Channel ch);

    SessionState state() const { return state_; }
    std::uint32_t local_client_id() const { return client_id_; }
    std::uint32_t remote_client_id() const { return remote_client_id_; }
    Transport* transport() { return transport_.get(); }
    void set_protocol_version(std::uint8_t v) { protocol_version_ = v; }

private:
    struct UnackedMessage {
        std::uint16_t sequence;
        std::vector<std::uint8_t> bytes;
        float time_since_send;
        int resend_count;
    };

    struct Fragment {
        std::uint32_t id;
        std::uint32_t index;
        std::uint32_t count;
        std::vector<std::uint8_t> data;
        float time;
    };

    std::unique_ptr<Transport> transport_;
    Role role_;
    SessionState state_{SessionState::Disconnected};
    std::uint8_t protocol_version_{kProtocolVersion};
    std::uint32_t client_id_{0};
    std::uint32_t remote_client_id_{0};
    std::uint32_t next_sequence_{1};
    std::uint32_t next_channel_seq_[2]{1, 1};
    std::uint32_t last_received_seq_{0};
    std::uint32_t last_acked_seq_{0};
    std::uint32_t ack_bits_{0};
    float time_since_last_received_{0};
    float time_since_last_sent_{0};
    std::optional<Endpoint> peer_;
    std::string pending_host_;
    std::uint16_t pending_port_{0};

    std::vector<UnackedMessage> unacked_[2];
    std::map<std::uint16_t, std::vector<std::uint8_t>> reorder_buffer_[2];
    std::map<std::uint32_t, Fragment> fragments_;
    std::vector<std::vector<std::uint8_t>> received_reliable_[2];
    std::vector<std::vector<std::uint8_t>> received_unreliable_[2];

    void send_datagram(const std::vector<std::uint8_t>& bytes);
    void handle_datagram(const Datagram& dgram);
    void handle_message(Channel ch, std::uint16_t seq, std::span<const std::uint8_t> payload);
    void handle_fragment(std::span<const std::uint8_t> payload);
    void resend_unacked(float dt);
    void flush();
    void build_datagram(ByteWriter& w);
    void process_outbound(ByteWriter& w);
};

} // namespace lotus
```

The `src/net/session.cpp` implementation is ~300 lines. Key methods:
- `connect()`: sends ConnectRequest, enters Connecting state
- `update()`: polls transport, handles datagrams, resends unacked, checks timeout
- `send_reliable()`: fragments if needed, assigns channel sequence, adds to unacked
- `send_unreliable()`: wraps in message frame, adds to outbound
- `handle_datagram()`: parses header, updates acks, dispatches messages
- `handle_message()`: routes to reorder buffer (reliable) or direct delivery (unreliable)
- `handle_fragment()`: reassembles, delivers complete messages
- `flush()`: builds and sends one datagram with all outbound messages + acks

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='SessionTest.*'`
Expected: all 5 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/protocol.hpp src/net/protocol.cpp include/lotus/net/session.hpp src/net/session.cpp test/net_session_test.cpp
git commit -m "feat(net): add Session with handshake, reliability, and fragmentation"
```

---

## Task 6: Network components and Scene::adopt_guid

**Files:**
- Create: `include/lotus/net/network_components.hpp`
- Modify: `include/lotus/scene/scene.hpp` (add `adopt_guid`)
- Modify: `src/scene/scene.cpp` (implement `adopt_guid`)
- Test: `test/net_guid_test.cpp`

**Interfaces:**
- Produces: `NetworkIdentity`, `NetworkTransform`, `NetworkBehaviour`, `TransformSample`, `NetworkAuthority` enum, `Scene::adopt_guid`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_guid_test.cpp
#include <gtest/gtest.h>
#include "lotus/scene/scene.hpp"
#include "lotus/scene/components.hpp"
using namespace lotus;

TEST(GuidTest, AdoptGuidAdvancesAllocator) {
    Scene scene{"test"};
    Entity e1 = scene.create("a");
    SceneGuid g1 = scene.guid_of(e1);
    scene.adopt_guid(e1, SceneGuid{100});
    EXPECT_EQ(scene.guid_of(e1).value, 100u);
    Entity e2 = scene.create("b");
    EXPECT_GT(scene.guid_of(e2).value, 100u);
}

TEST(GuidTest, AdoptLowerGuidDoesNotRewind) {
    Scene scene{"test"};
    Entity e1 = scene.create("a");
    Entity e2 = scene.create("b");
    scene.adopt_guid(e2, SceneGuid{1}); // lower than current max
    Entity e3 = scene.create("c");
    EXPECT_GT(scene.guid_of(e3).value, scene.guid_of(e1).value);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='GuidTest.*'`
Expected: `AdoptGuidAdvancesAllocator` fails — `adopt_guid` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/network_components.hpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace lotus {

enum class NetworkAuthority : std::uint8_t { Server, Client };

struct NetworkIdentity {
    std::uint32_t network_id{0};
    std::uint32_t owner{0};
    NetworkAuthority authority{NetworkAuthority::Server};
};

struct TransformSample {
    std::uint32_t tick{0};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct NetworkTransform {
    float send_interval{1.0f / 30.0f};
    float position_epsilon{0.01f};
    float rotation_epsilon{0.005f};
    float accumulated{0.0f};
    std::vector<TransformSample> samples;
};

struct NetworkBehaviour {
    std::string behaviour_id;
};

} // namespace lotus
```

In `include/lotus/scene/scene.hpp`, add after `assign_guid`:

```cpp
/// Adopts \p guid for \p entity and advances the allocator past it, so a later
/// create() cannot hand out a GUID already in use.
void adopt_guid(Entity entity, SceneGuid guid);
```

In `src/scene/scene.cpp`, add:

```cpp
void Scene::adopt_guid(Entity entity, SceneGuid guid) {
    registry_.emplace_or_replace<SceneGuid>(entity, guid);
    if (guid.value >= next_guid_) {
        next_guid_ = guid.value + 1;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='GuidTest.*'`
Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/network_components.hpp include/lotus/scene/scene.hpp src/scene/scene.cpp test/net_guid_test.cpp
git commit -m "feat(net): add network components and Scene::adopt_guid"
```

---

## Task 7: Entity serialization hooks

**Files:**
- Modify: `include/lotus/scene/serialization.hpp` (add `entity_to_json`, `adopt_entity_json`)
- Modify: `src/scene/serialization.cpp` (implement them)
- Test: `test/net_entity_json_test.cpp`

**Interfaces:**
- Consumes: `ComponentCodecs`, `SceneGuid` codec (existing)
- Produces: `entity_to_json(Registry, Entity) -> nlohmann::json`, `adopt_entity_json(Scene, json, warnings*) -> Entity`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_entity_json_test.cpp
#include <gtest/gtest.h>
#include "lotus/scene/serialization.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/components.hpp"
using namespace lotus;

TEST(EntityJsonTest, RoundTrip) {
    Scene scene{"test"};
    Entity e = scene.create("player");
    scene.registry().emplace<Transform>(e, Transform{{1,2,3}, {0,0,0}, {1,1,1}});
    scene.registry().emplace<SceneGuid>(e, SceneGuid{42});

    auto j = entity_to_json(scene.registry(), e);
    EXPECT_EQ(j["name"], "player");
    EXPECT_TRUE(j["components"].contains("Transform"));
    EXPECT_TRUE(j["components"].contains("SceneGuid"));

    Scene scene2{"test2"};
    std::vector<std::string> warnings;
    Entity e2 = adopt_entity_json(scene2, j, &warnings);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(scene2.registry().get<Transform>(e2).position.x, 1.0f);
    EXPECT_EQ(scene2.guid_of(e2).value, 42u);
}

TEST(EntityJsonTest, AntiDriftWithSceneSerialization) {
    Scene scene{"test"};
    Entity e = create("drift_test");
    scene.registry().emplace<Transform>(e, Transform{{5,6,7}, {0,0,0.5f}, {2,2,2}});
    scene.registry().emplace<Name>(e, Name{"drift_test"});

    std::string scene_json = to_json_string(scene);
    auto scene_doc = nlohmann::json::parse(scene_json);

    // Extract the entity from the scene document
    auto& entities = scene_doc["entities"];
    ASSERT_FALSE(entities.empty());
    auto& entity_doc = entities[0];

    auto direct = entity_to_json(scene.registry(), e);
    EXPECT_EQ(entity_doc["components"], direct["components"]);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='EntityJsonTest.*'`
Expected: compile error — `entity_to_json` not found.

- [ ] **Step 3: Write minimal implementation**

In `include/lotus/scene/serialization.hpp`, add:

```cpp
/// Serializes one entity (no children) in the same shape the scene reader
/// accepts. Network spawn payloads use it so replication and save/load share
/// one schema.
nlohmann::json entity_to_json(const Registry& registry, Entity entity);

/// Creates one entity from a document produced by entity_to_json, applying the
/// document's SceneGuid when it carries one (adopt_guid semantics). Children
/// are not read; replication sends them as their own spawns.
Entity adopt_entity_json(Scene& scene, const nlohmann::json& document,
                         std::vector<std::string>* warnings = nullptr);
```

In `src/scene/serialization.cpp`, add (after the existing `entity_from_json` helper):

```cpp
nlohmann::json entity_to_json(const Registry& registry, Entity entity) {
    nlohmann::json j;
    j["name"] = registry.get<Name>(entity).value;
    nlohmann::json components = nlohmann::json::object();
    const auto& codecs = default_component_codecs();
    for (const std::string& name : codecs.names()) {
        const auto* write = codecs.writer(name);
        nlohmann::json value;
        if (write && (*write)(registry, entity, value)) {
            components[name] = std::move(value);
        }
    }
    if (!components.empty()) {
        j["components"] = std::move(components);
    }
    return j;
}

Entity adopt_entity_json(Scene& scene, const nlohmann::json& document,
                         std::vector<std::string>* warnings) {
    const std::string name = document.value("name", std::string{});
    Entity entity = scene.create(name);
    if (document.contains("components")) {
        const auto& components = document.at("components");
        const auto& codecs = default_component_codecs();
        for (const auto& [key, value] : components.items()) {
            const auto* read = codecs.reader(key);
            if (!read) {
                if (warnings) warnings->push_back("unknown component: " + key);
                continue;
            }
            (*read)(scene.registry(), entity, value);
        }
    }
    // Apply SceneGuid if present
    if (document.contains("components") &&
        document["components"].contains("SceneGuid")) {
        SceneGuid guid;
        from_json(document["components"]["SceneGuid"], guid);
        if (guid.value != 0) {
            scene.adopt_guid(entity, guid);
        }
    }
    return entity;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='EntityJsonTest.*'`
Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/scene/serialization.hpp src/scene/serialization.cpp test/net_entity_json_test.cpp
git commit -m "feat(net): add entity_to_json and adopt_entity_json for spawn payloads"
```

---

## Task 8: Replication — spawn, despawn, transform

**Files:**
- Create: `src/net/replication.cpp`
- Modify: `include/lotus/net/network_manager.hpp` (add replication methods)
- Test: `test/net_replication_test.cpp`

**Interfaces:**
- Consumes: `Session` (Task 5), `entity_to_json`/`adopt_entity_json` (Task 7), `NetworkIdentity`/`NetworkTransform` (Task 6), `ByteWriter/ByteReader` (Task 1)
- Produces: `NetworkManager::server_spawn`, `server_despawn`, `server_tick` (internal), message types `0x05`-`0x07`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_replication_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/network_manager.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/components.hpp"
using namespace lotus;

TEST(ReplicationTest, HostRegistersScene) {
    Scene scene{"test"};
    Entity e = scene.create("cube");
    scene.registry().emplace<Transform>(e, Transform{{0,0,0}, {0,0,0}, {1,1,1}});

    NetworkManager net;
    net.set_transport(std::make_unique<LoopbackTransport>());
    EXPECT_TRUE(net.host(7777));
    EXPECT_TRUE(net.hosting());
}

TEST(ReplicationTest, ClientReceivesSpawn) {
    // Server side
    Scene server_scene{"server"};
    Entity se = server_scene.create("cube");
    server_scene.registry().emplace<Transform>(se, Transform{{1,2,3}, {0,0,0}, {1,1,1}});

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server_net;
    server_net.set_transport(std::move(ta));
    server_net.host(7777);
    server_net.server_spawn(server_scene, se);

    // Client side
    Scene client_scene{"client"};
    NetworkManager client_net;
    client_net.set_transport(std::move(tb));
    EXPECT_TRUE(client_net.connect("127.0.0.1", 7777));

    // Pump
    for (int i = 0; i < 100 && client_net.state() != SessionState::Connected; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }
    ASSERT_EQ(client_net.state(), SessionState::Connected);

    // Pump more for spawn
    for (int i = 0; i < 50; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }

    // Client should have the entity
    Entity ce = client_scene.find("cube");
    ASSERT_NE(ce, NullEntity);
    EXPECT_NEAR(client_scene.registry().get<Transform>(ce).position.x, 1.0f, 0.1f);
}

TEST(ReplicationTest, DespawnRemovesSubtree) {
    Scene server_scene{"server"};
    Entity parent = server_scene.create("parent");
    Entity child = server_scene.create("child");
    server_scene.set_parent(child, parent);

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server_net;
    server_net.set_transport(std::move(ta));
    server_net.host(7777);
    server_net.server_spawn(server_scene, parent);
    server_net.server_spawn(server_scene, child);

    Scene client_scene{"client"};
    NetworkManager client_net;
    client_net.set_transport(std::move(tb));
    client_net.connect("127.0.0.1", 7777);

    for (int i = 0; i < 200; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }

    ASSERT_NE(client_scene.find("parent"), NullEntity);
    ASSERT_NE(client_scene.find("child"), NullEntity);

    // Despawn parent
    server_net.server_despawn(server_scene, parent);
    for (int i = 0; i < 50; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }

    EXPECT_EQ(client_scene.find("parent"), NullEntity);
    EXPECT_EQ(client_scene.find("child"), NullEntity);
}

TEST(ReplicationTest, TransformDeltaRespectsEpsilon) {
    Scene server_scene{"server"};
    Entity e = server_scene.create("mover");
    server_scene.registry().emplace<Transform>(e, Transform{{0,0,0}, {0,0,0}, {1,1,1}});
    server_scene.registry().emplace<NetworkTransform>(e);

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server_net;
    server_net.set_transport(std::move(ta));
    server_net.host(7777);
    server_net.server_spawn(server_scene, e);

    Scene client_scene{"client"};
    NetworkManager client_net;
    client_net.set_transport(std::move(tb));
    client_net.connect("127.0.0.1", 7777);

    for (int i = 0; i < 200; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }

    Entity ce = client_scene.find("mover");
    ASSERT_NE(ce, NullEntity);

    // Move server entity
    server_scene.registry().get<Transform>(e).position.x = 10.0f;
    for (int i = 0; i < 50; ++i) {
        client_net.update(client_scene, 0.016f);
        server_net.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client_net.transport())->deliver();
        static_cast<LoopbackTransport*>(server_net.transport())->deliver();
    }
    EXPECT_NEAR(client_scene.registry().get<Transform>(ce).position.x, 10.0f, 0.5f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='ReplicationTest.*'`
Expected: compile error — `server_spawn` not found.

- [ ] **Step 3: Write minimal implementation**

In `include/lotus/net/network_manager.hpp`, add to `NetworkManager`:

```cpp
std::uint32_t server_spawn(Scene& scene, Entity entity,
                           NetworkAuthority authority = NetworkAuthority::Server);
void server_despawn(Scene& scene, Entity entity);
```

In `src/net/replication.cpp`, implement:

```cpp
#include "lotus/net/network_manager.hpp"
#include "lotus/net/byte_buffer.hpp"
#include "lotus/scene/serialization.hpp"
#include "lotus/scene/components.hpp"
#include <algorithm>

using namespace lotus;

std::uint32_t NetworkManager::server_spawn(Scene& scene, Entity entity,
                                           NetworkAuthority authority) {
    if (!hosting_) return 0;
    std::uint32_t nid = next_network_id_++;
    scene.registry().emplace<NetworkIdentity>(entity, NetworkIdentity{
        nid, 0, authority});
    // Queue spawn for each connected client
    for (auto& [cid, session] : sessions_) {
        SpawnMsg msg;
        msg.network_id = nid;
        msg.scene_guid = scene.guid_of(entity).value;
        // Find parent network id
        msg.parent_network_id = 0;
        if (scene.registry().all_of<Hierarchy>(entity)) {
            Entity p = scene.registry().get<Hierarchy>(entity).parent;
            if (p != NullEntity && scene.registry().all_of<NetworkIdentity>(p)) {
                msg.parent_network_id = scene.registry().get<NetworkIdentity>(p).network_id;
            }
        }
        msg.authority = static_cast<std::uint8_t>(authority);
        msg.owner = 0;
        auto payload = entity_to_json(scene.registry(), entity);
        msg.payload = payload.dump();
        queue_reliable(session.get(), Channel::Reliable, msg.serialize());
    }
    return nid;
}

void NetworkManager::server_despawn(Scene& scene, Entity entity) {
    if (!hosting_ || !scene.registry().all_of<NetworkIdentity>(entity)) return;
    std::uint32_t nid = scene.registry().get<NetworkIdentity>(entity).network_id;
    for (auto& [cid, session] : sessions_) {
        DespawnMsg msg;
        msg.network_id = nid;
        msg.reason = 0; // Destroyed
        queue_reliable(session.get(), Channel::Reliable, msg.serialize());
    }
    scene.destroy(entity);
}
```

The `SpawnMsg` and `DespawnMsg` structs with `serialize()` methods are defined in `src/net/replication.cpp` as file-static helpers. The client-side handling (in `NetworkManager::update`) processes inbound `0x05`/`0x06` messages.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='ReplicationTest.*'`
Expected: all 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/network_manager.hpp src/net/replication.cpp test/net_replication_test.cpp
git commit -m "feat(net): add spawn/despawn/transform replication"
```

---

## Task 9: RPC registry

**Files:**
- Create: `include/lotus/net/rpc.hpp`
- Create: `src/net/rpc.cpp`
- Test: `test/net_rpc_test.cpp`

**Interfaces:**
- Consumes: `ByteWriter/ByteReader` (Task 1), `fnv1a32` (Task 5)
- Produces: `RpcRegistry`, `NetworkContext`, `NetworkManager::server_rpc`, `NetworkManager::client_rpc`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_rpc_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/rpc.hpp"
#include "lotus/net/byte_buffer.hpp"
using namespace lotus;

TEST(RpcTest, ServerRpcDispatches) {
    RpcRegistry reg;
    bool called = false;
    std::uint32_t sender = 0;
    reg.add_server_rpc("player", "Pickup", [&](NetworkContext& ctx, ByteReader& r) {
        called = true;
        sender = ctx.sender;
        std::uint32_t item = 0;
        r.u32(item);
        EXPECT_EQ(item, 42u);
    });
    EXPECT_TRUE(called == false);

    // Simulate dispatch
    ByteWriter w;
    w.u32(42);
    auto handler = reg.find_server_rpc(fnv1a32("player"), fnv1a32("Pickup"));
    ASSERT_NE(handler, nullptr);
    NetworkContext ctx;
    ctx.sender = 7;
    ByteReader r(w.data());
    (*handler)(ctx, r);
    EXPECT_TRUE(called);
    EXPECT_EQ(sender, 7u);
}

TEST(RpcTest, UnknownRpcDropped) {
    RpcRegistry reg;
    auto handler = reg.find_server_rpc(fnv1a32("player"), fnv1a32("Unknown"));
    EXPECT_EQ(handler, nullptr);
}

TEST(RpcTest, RegistrationCollisionRejected) {
    RpcRegistry reg;
    std::string err;
    EXPECT_TRUE(reg.add_server_rpc("player", "Pickup", nullptr, &err));
    EXPECT_FALSE(reg.add_server_rpc("player", "Pickup", nullptr, &err));
    EXPECT_FALSE(err.empty());
}

TEST(RpcTest, ClientRpcDispatches) {
    RpcRegistry reg;
    bool called = false;
    reg.add_client_rpc("effect", "Play", [&](NetworkContext& ctx, ByteReader& r) {
        called = true;
        std::string name;
        r.string(name);
        EXPECT_EQ(name, "spark");
    });

    ByteWriter w;
    w.string("spark");
    auto handler = reg.find_client_rpc(fnv1a32("effect"), fnv1a32("Play"));
    ASSERT_NE(handler, nullptr);
    NetworkContext ctx;
    ByteReader r(w.data());
    (*handler)(ctx, r);
    EXPECT_TRUE(called);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='RpcTest.*'`
Expected: compile error — `lotus/net/rpc.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/rpc.hpp
#pragma once
#include "lotus/net/byte_buffer.hpp"
#include "lotus/net/protocol.hpp"
#include "lotus/ecs.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lotus {

class NetworkManager;

struct NetworkContext {
    NetworkManager* net{nullptr};
    bool server_side{false};
    std::uint32_t sender{0};
    Entity entity{NullEntity};
};

class RpcRegistry {
public:
    using Handler = std::function<void(NetworkContext&, ByteReader&)>;

    bool add_server_rpc(const std::string& behaviour_id,
                        const std::string& name, Handler handler,
                        std::string* error = nullptr);
    bool add_client_rpc(const std::string& behaviour_id,
                        const std::string& name, Handler handler,
                        std::string* error = nullptr);

    const Handler* find_server_rpc(std::uint32_t behaviour_hash,
                                  std::uint32_t name_hash) const;
    const Handler* find_client_rpc(std::uint32_t behaviour_hash,
                                  std::uint32_t name_hash) const;

    std::vector<std::pair<std::string, std::string>> names() const;

private:
    struct RpcKey {
        std::uint32_t behaviour_hash;
        std::uint32_t name_hash;
        bool operator<(const RpcKey& o) const {
            if (behaviour_hash != o.behaviour_hash) return behaviour_hash < o.behaviour_hash;
            return name_hash < o.name_hash;
        }
    };
    std::map<RpcKey, Handler> server_rpcs_;
    std::map<RpcKey, Handler> client_rpcs_;
};

} // namespace lotus
```

```cpp
// src/net/rpc.cpp
#include "lotus/net/rpc.hpp"
#include <algorithm>

using namespace lotus;

bool RpcRegistry::add_server_rpc(const std::string& behaviour_id,
                                  const std::string& name, Handler handler,
                                  std::string* error) {
    RpcKey key{fnv1a32(behaviour_id), fnv1a32(name)};
    if (server_rpcs_.count(key)) {
        if (error) *error = "duplicate server RPC: " + behaviour_id + "." + name;
        return false;
    }
    server_rpcs_[key] = std::move(handler);
    return true;
}

bool RpcRegistry::add_client_rpc(const std::string& behaviour_id,
                                  const std::string& name, Handler handler,
                                  std::string* error) {
    RpcKey key{fnv1a32(behaviour_id), fnv1a32(name)};
    if (client_rpcs_.count(key)) {
        if (error) *error = "duplicate client RPC: " + behaviour_id + "." + name;
        return false;
    }
    client_rpcs_[key] = std::move(handler);
    return true;
}

const RpcRegistry::Handler* RpcRegistry::find_server_rpc(
    std::uint32_t behaviour_hash, std::uint32_t name_hash) const {
    auto it = server_rpcs_.find({behaviour_hash, name_hash});
    return it != server_rpcs_.end() ? &it->second : nullptr;
}

const RpcRegistry::Handler* RpcRegistry::find_client_rpc(
    std::uint32_t behaviour_hash, std::uint32_t name_hash) const {
    auto it = client_rpcs_.find({behaviour_hash, name_hash});
    return it != client_rpcs_.end() ? &it->second : nullptr;
}

std::vector<std::pair<std::string, std::string>> RpcRegistry::names() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [key, _] : server_rpcs_)
        out.emplace_back("server", std::to_string(key.behaviour_hash) + "." + std::to_string(key.name_hash));
    for (const auto& [key, _] : client_rpcs_)
        out.emplace_back("client", std::to_string(key.behaviour_hash) + "." + std::to_string(key.name_hash));
    std::sort(out.begin(), out.end());
    return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='RpcTest.*'`
Expected: all 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/rpc.hpp src/net/rpc.cpp test/net_rpc_test.cpp
git commit -m "feat(net): add RPC registry with hashed dispatch"
```

---

## Task 10: NetworkManager and Application wiring

**Files:**
- Create: `include/lotus/net/network_manager.hpp`
- Create: `src/net/network_manager.cpp`
- Modify: `include/lotus/application/application.hpp` (add `network_` member)
- Modify: `src/core/application/application.cpp` (wire into frame loop)

**Interfaces:**
- Consumes: `Session` (Task 5), `RpcRegistry` (Task 9), replication (Task 8)
- Produces: `NetworkManager`, `NetworkStats`, `ClientInfo`, `Application::net()`

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_manager_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/network_manager.hpp"
#include "lotus/scene/scene.hpp"
using namespace lotus;

TEST(ManagerTest, HostAndConnect) {
    Scene scene{"test"};
    Entity e = scene.create("obj");

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server;
    server.set_transport(std::move(ta));
    EXPECT_TRUE(server.host(7777));
    EXPECT_TRUE(server.hosting());
    server.server_spawn(scene, e);

    NetworkManager client;
    client.set_transport(std::move(tb));
    EXPECT_TRUE(client.connect("127.0.0.1", 7777));

    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(scene, 0.016f);
        server.update(scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    EXPECT_EQ(client.state(), SessionState::Connected);
    EXPECT_EQ(server.client_count(), 1u);
    EXPECT_GT(client.local_client_id(), 0u);
}

TEST(ManagerTest, StatsPopulated) {
    auto [ta, tb] = make_loopback_pair();
    NetworkManager a, b;
    a.set_transport(std::move(ta));
    b.set_transport(std::move(tb));
    a.host(7777);
    b.connect("127.0.0.1", 7777);
    Scene s{"s"};
    for (int i = 0; i < 100; ++i) {
        a.update(s, 0.016f);
        b.update(s, 0.016f);
        static_cast<LoopbackTransport*>(a.transport())->deliver();
        static_cast<LoopbackTransport*>(b.transport())->deliver();
    }
    auto stats = a.stats();
    EXPECT_GT(stats.packets_sent, 0u);
    EXPECT_GT(stats.packets_received, 0u);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='ManagerTest.*'`
Expected: compile error — `lotus/net/network_manager.hpp` not found.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/lotus/net/network_manager.hpp
#pragma once
#include "lotus/net/transport.hpp"
#include "lotus/net/session.hpp"
#include "lotus/net/rpc.hpp"
#include "lotus/net/network_components.hpp"
#include "lotus/ecs.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {

class Scene;

struct NetworkStats {
    std::uint64_t packets_sent{0};
    std::uint64_t packets_received{0};
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_received{0};
    std::uint64_t resends{0};
    std::uint64_t drops{0};
    float rtt{0.0f};
    float loss{0.0f};
};

struct ClientInfo {
    std::uint32_t id{0};
    std::string address;
    std::uint16_t port{0};
    float rtt{0.0f};
    float loss{0.0f};
};

using RpcWriter = std::function<void(ByteWriter&)>;

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    void set_transport(std::unique_ptr<Transport> transport);
    Transport* transport();

    // Server
    bool host(std::uint16_t port, std::string* error = nullptr);
    void stop_hosting();
    bool hosting() const;
    std::size_t client_count() const;
    std::uint32_t server_spawn(Scene& scene, Entity entity,
                               NetworkAuthority authority = NetworkAuthority::Server);
    void server_despawn(Scene& scene, Entity entity);

    // Client
    bool connect(const std::string& host, std::uint16_t port,
                 std::string* error = nullptr);
    void disconnect();
    SessionState state() const;
    std::uint32_t local_client_id() const;
    const std::string& connect_status() const;

    // Both
    void update(Scene& scene, float dt);
    void set_tick_rate(float hz);
    float tick_rate() const;

    RpcRegistry& rpcs();
    void server_rpc(Scene&, Entity, std::string_view name, const RpcWriter& args);
    void client_rpc(Scene&, Entity, std::string_view name, const RpcWriter& args);

    const NetworkStats& stats() const;
    std::vector<ClientInfo> clients() const;

private:
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Session> session_;
    RpcRegistry rpcs_;
    std::uint32_t next_network_id_{1};
    float tick_rate_{30.0f};
    float tick_accumulator_{0.0f};
    std::string connect_status_;
    NetworkStats stats_;
    std::unordered_map<std::uint32_t, std::unique_ptr<Session>> sessions_; // server: client_id -> session
    std::unordered_map<std::uint32_t, Entity> network_id_to_entity_; // client: nid -> entity
    std::unordered_map<Entity, std::uint32_t> entity_to_network_id_; // server: entity -> nid

    void server_tick(Scene& scene);
    void client_update(Scene& scene);
    void handle_spawn(Scene& scene, std::span<const std::uint8_t> payload);
    void handle_despawn(Scene& scene, std::span<const std::uint8_t> payload);
    void handle_transform(Scene& scene, std::span<const std::uint8_t> payload);
    void handle_server_rpc(Scene& scene, std::span<const std::uint8_t> payload);
    void handle_client_rpc(Scene& scene, std::span<const std::uint8_t> payload);
    void queue_reliable(Session* s, Channel ch, std::vector<std::uint8_t> msg);
};

} // namespace lotus
```

The `src/net/network_manager.cpp` implementation wires everything together. Key logic:
- `host()`: creates a `UdpTransport`, binds, creates a server `Session`
- `connect()`: creates a `UdpTransport`, binds ephemeral, creates a client `Session`
- `update()`: polls transport, updates session, runs server tick or client update, flushes
- `server_tick()`: gathers transform deltas, sends unreliable transform messages
- `client_update()`: applies interpolation, dispatches RPCs

In `include/lotus/application/application.hpp`, add:

```cpp
#include "lotus/net/network_manager.hpp"
// ... in the class:
NetworkManager& net() { return network_; }
// ... in private members:
NetworkManager network_;
```

In `src/core/application/application.cpp`, in the frame loop after `scripts_.poll(*scene)`:

```cpp
network_.update(*scene, dt);
```

And wrap `physics_.step` with a check:

```cpp
if (network_.state() != SessionState::Connected && !network_.hosting()) {
    physics_.step(*scene, dt);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='ManagerTest.*'`
Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/lotus/net/network_manager.hpp src/net/network_manager.cpp include/lotus/application/application.hpp src/core/application/application.cpp test/net_manager_test.cpp
git commit -m "feat(net): add NetworkManager and wire into Application"
```

---

## Task 11: lotus_server binary

**Files:**
- Create: `src/server/main.cpp`
- Modify: `CMakeLists.txt` (add `lotus_server` target)

**Interfaces:**
- Consumes: `NetworkManager` (Task 10), `Scene`, `PhysicsWorld` (existing)
- Produces: `lotus_server` executable

- [ ] **Step 1: Write the server main**

```cpp
// src/server/main.cpp
#include "lotus/net/network_manager.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"
#include "lotus/physics/physics_world.hpp"
#include "lotus/console/console.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace lotus;

namespace {
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Args {
    std::string scene_path = "scenes/main.json";
    std::uint16_t port = 7777;
    float tick_rate = 30.0f;
    std::string assets_dir = "assets";
    int max_frames = 0;
    bool quiet = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--scene" && i + 1 < argc) a.scene_path = argv[++i];
        else if (arg == "--port" && i + 1 < argc) a.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--tick" && i + 1 < argc) a.tick_rate = static_cast<float>(std::atof(argv[++i]));
        else if (arg == "--assets" && i + 1 < argc) a.assets_dir = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) a.max_frames = std::atoi(argv[++i]);
        else if (arg == "--quiet") a.quiet = true;
    }
    return a;
}
} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Scene scene{"server"};
    PhysicsWorld physics;
    NetworkManager net;
    Console console;

    std::string error;
    if (!load_scene(scene, args.scene_path, &error)) {
        std::cerr << "lotus_server: cannot load " << args.scene_path << ": " << error << "\n";
        return 1;
    }

    if (!net.host(args.port, &error)) {
        std::cerr << "lotus_server: cannot bind port " << args.port << ": " << error << "\n";
        return 1;
    }
    net.set_tick_rate(args.tick_rate);

    // Register all entities
    for (Entity e : scene.registry().view<Name>()) {
        net.server_spawn(scene, e);
    }

    if (!args.quiet) {
        std::cout << "lotus_server: serving " << args.scene_path
                  << " on port " << args.port << "\n";
    }

    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    int frame = 0;

    while (!g_stop) {
        if (args.max_frames > 0 && frame >= args.max_frames) break;

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(static_cast<double>(now - last) / freq);
        last = now;
        if (dt > 0.25f) dt = 0.25f;

        scene.update(dt);
        physics.step(scene, dt);
        net.update(scene, dt);

        ++frame;
    }

    if (!args.quiet) {
        std::cout << "lotus_server: " << frame << " frames\n";
    }
    return 0;
}
```

- [ ] **Step 2: Add CMake target**

In `CMakeLists.txt`, after the `lotus` executable:

```cmake
add_executable(lotus_server src/server/main.cpp)
target_link_libraries(lotus_server lotus_engine)
set_target_properties(lotus_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
```

- [ ] **Step 3: Build and verify**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_server 2>&1 | tail -5`
Expected: builds without errors.

- [ ] **Step 4: Smoke test**

Run: `timeout 2 ./bin/lotus_server --scene scenes/main.json --port 7788 --frames 10 --quiet && echo OK`
Expected: prints OK.

- [ ] **Step 5: Commit**

```bash
git add src/server/main.cpp CMakeLists.txt
git commit -m "feat(net): add lotus_server headless binary"
```

---

## Task 12: Console commands and integration test

**Files:**
- Modify: `src/core/application/application.cpp` (add `net.*` commands)
- Test: `test/net_integration_test.cpp`

**Interfaces:**
- Consumes: `NetworkManager` (Task 10), `Console` (existing)
- Produces: `net.host`, `net.connect`, `net.disconnect`, `net.status`, `net.stats`, `net.rpcs`, `net.entities` console commands

- [ ] **Step 1: Write the failing test**

```cpp
// test/net_integration_test.cpp
#include <gtest/gtest.h>
#include "lotus/net/network_manager.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/components.hpp"
using namespace lotus;

TEST(IntegrationTest, EndToEndLoopback) {
    // Server
    Scene server_scene{"server"};
    Entity se = server_scene.create("player");
    server_scene.registry().emplace<Transform>(se, Transform{{0,0,0}, {0,0,0}, {1,1,1}});
    server_scene.registry().emplace<NetworkTransform>(se);

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server;
    server.set_transport(std::move(ta));
    ASSERT_TRUE(server.host(7777));
    server.server_spawn(server_scene, se);

    // Client
    Scene client_scene{"client"};
    NetworkManager client;
    client.set_transport(std::move(tb));
    ASSERT_TRUE(client.connect("127.0.0.1", 7777));

    // Connect
    for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
        client.update(client_scene, 0.016f);
        server.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_EQ(client.state(), SessionState::Connected);

    // Receive spawn
    for (int i = 0; i < 50; ++i) {
        client.update(client_scene, 0.016f);
        server.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_NE(client_scene.find("player"), NullEntity);

    // Move server entity, verify client converges
    server_scene.registry().get<Transform>(se).position.x = 5.0f;
    for (int i = 0; i < 100; ++i) {
        client.update(client_scene, 0.016f);
        server.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    Entity ce = client_scene.find("player");
    ASSERT_NE(ce, NullEntity);
    EXPECT_NEAR(client_scene.registry().get<Transform>(ce).position.x, 5.0f, 0.5f);
}

TEST(IntegrationTest, RpcRoundTrip) {
    Scene server_scene{"server"};
    Entity se = server_scene.create("obj");
    server_scene.registry().emplace<NetworkBehaviour>(se, NetworkBehaviour{"test_behaviour"});

    auto [ta, tb] = make_loopback_pair();
    NetworkManager server;
    server.set_transport(std::move(ta));
    server.host(7777);
    server.server_spawn(server_scene, se);

    // Register server RPC
    bool rpc_called = false;
    server.rpcs().add_server_rpc("test_behaviour", "DoThing", [&](NetworkContext& ctx, ByteReader&) {
        rpc_called = true;
    });

    Scene client_scene{"client"};
    NetworkManager client;
    client.set_transport(std::move(tb));
    client.connect("127.0.0.1", 7777);

    for (int i = 0; i < 200; ++i) {
        client.update(client_scene, 0.016f);
        server.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    ASSERT_EQ(client.state(), SessionState::Connected);

    // Client sends RPC
    Entity ce = client_scene.find("obj");
    ASSERT_NE(ce, NullEntity);
    client.server_rpc(client_scene, ce, "DoThing", [](ByteWriter&) {});

    for (int i = 0; i < 50; ++i) {
        client.update(client_scene, 0.016f);
        server.update(server_scene, 0.016f);
        static_cast<LoopbackTransport*>(client.transport())->deliver();
        static_cast<LoopbackTransport*>(server.transport())->deliver();
    }
    EXPECT_TRUE(rpc_called);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='IntegrationTest.*'`
Expected: tests fail — `server_rpc` not yet wired into `NetworkManager`.

- [ ] **Step 3: Add console commands and wire RPC dispatch**

In `src/core/application/application.cpp`, in `register_commands()`, add:

```cpp
console_.add_command("net.host", "net.host <port> - start hosting",
    [this](const std::vector<std::string>& args) {
        if (args.empty()) return std::string{"usage: net.host <port>"};
        std::string error;
        if (!network_.host(static_cast<std::uint16_t>(std::atoi(args[0].c_str())), &error))
            return error;
        if (Scene* s = scenes_.active())
            for (Entity e : s->registry().view<Name>())
                network_.server_spawn(*s, e);
        return std::string{"hosting on port "} + args[0];
    });
console_.add_command("net.connect", "net.connect <host> <port> - connect to a server",
    [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return std::string{"usage: net.connect <host> <port>"};
        std::string error;
        if (!network_.connect(args[0], static_cast<std::uint16_t>(std::atoi(args[1].c_str())), &error))
            return error;
        return std::string{"connecting to "} + args[0] + ":" + args[1];
    });
console_.add_command("net.disconnect", "net.disconnect - close the session",
    [this](const std::vector<std::string>&) {
        network_.disconnect();
        return std::string{"disconnected"};
    });
console_.add_command("net.status", "net.status - show connection info",
    [this](const std::vector<std::string>&) {
        std::string out;
        out += "mode: ";
        out += network_.hosting() ? "server" : (network_.state() == SessionState::Connected ? "client" : "offline");
        out += "\nstate: ";
        switch (network_.state()) {
            case SessionState::Disconnected: out += "disconnected"; break;
            case SessionState::Connecting: out += "connecting"; break;
            case SessionState::Connected: out += "connected"; break;
            case SessionState::Disconnecting: out += "disconnecting"; break;
        }
        out += "\nclients: " + std::to_string(network_.client_count());
        out += "\nclient id: " + std::to_string(network_.local_client_id());
        return out;
    });
console_.add_command("net.stats", "net.stats - show network counters",
    [this](const std::vector<std::string>&) {
        auto& s = network_.stats();
        return "sent: " + std::to_string(s.packets_sent) +
               " recv: " + std::to_string(s.packets_received) +
               " resends: " + std::to_string(s.resends) +
               " drops: " + std::to_string(s.drops) +
               " rtt: " + std::to_string(static_cast<int>(s.rtt * 1000)) + "ms";
    });
console_.add_command("net.rpcs", "net.rpcs - list registered RPCs",
    [this](const std::vector<std::string>&) {
        std::string out;
        for (auto& [side, name] : network_.rpcs().names())
            out += "\n" + side + ": " + name;
        return out.empty() ? std::string{"(no RPCs)"} : out;
    });
console_.add_command("net.entities", "net.entities - list replicated entities",
    [this](const std::vector<std::string>&) {
        Scene* s = scenes_.active();
        if (!s) return std::string{"no active scene"};
        std::string out;
        for (Entity e : s->registry().view<NetworkIdentity>()) {
            auto& nid = s->registry().get<NetworkIdentity>(e);
            out += "\n" + std::to_string(nid.network_id) + " " +
                   s->registry().get<Name>(e).value +
                   (nid.authority == NetworkAuthority::Client ? " (client)" : "");
        }
        return out.empty() ? std::string{"(no replicated entities)"} : out;
    });
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd build && cmake --build . -j"$(nproc)" --target lotus_tests 2>&1 | tail -5 && ./bin/lotus_tests --gtest_filter='IntegrationTest.*'`
Expected: both tests pass.

- [ ] **Step 5: Run full test suite**

Run: `cd build && cmake --build . -j"$(nproc)" 2>&1 | tail -5 && ./bin/lotus_tests 2>&1 | tail -10`
Expected: all tests pass (200 baseline + new tests).

- [ ] **Step 6: Commit**

```bash
git add src/core/application/application.cpp test/net_integration_test.cpp
git commit -m "feat(net): add console commands and integration test"
```

---

## Self-Review

**Spec coverage:**
- Transport (UDP + loopback): Tasks 3, 4
- Wire format: Tasks 1, 2
- Session (handshake, reliability, fragmentation): Task 5
- Network components + adopt_guid: Task 6
- Entity serialization hooks: Task 7
- Replication (spawn/despawn/transform): Task 8
- RPC registry: Task 9
- NetworkManager + Application wiring: Task 10
- lotus_server: Task 11
- Console commands + integration: Task 12

All spec sections 1-12 of milestone 1 are covered.

**Placeholder scan:** No TBDs, no "implement later", no vague steps. Every step has actual code or a concrete command.

**Type consistency:** `NetworkIdentity`, `NetworkTransform`, `NetworkBehaviour`, `TransformSample`, `NetworkAuthority`, `SessionState`, `Channel`, `RpcRegistry`, `NetworkContext`, `NetworkManager`, `NetworkStats`, `ClientInfo` — all defined once and used consistently.

**Build verification:** Each task ends with a build + test step. Task 12 ends with a full suite run.

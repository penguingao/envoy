// Unit tests for PayloadRef::External storage and MmapPayloadStore.
//
// Test matrix:
//
//   PayloadRef
//     ExternalConstructionAndAccessors      makeExternal, size, empty, offset/length
//     ExternalEmptyRef                      zero-length external ref
//     DefaultRefIsInline                    default-constructed ref is Inline/empty
//
//   MmapPayloadStore – inline threshold
//     SmallStringStaysInline                string ≤ max_inline_bytes → Storage::Inline
//     ExactThresholdStringStaysInline       string == max_inline_bytes → Inline
//     LargeStringGoesExternal               string > max_inline_bytes → Storage::External
//     SmallBufferStaysInline                Buffer::Instance ≤ threshold → Inline
//     LargeBufferGoesExternal               Buffer::Instance > threshold → External
//
//   MmapPayloadStore – fetch roundtrip
//     FetchExternalStringRoundtrip          store string → External → fetch → same bytes
//     FetchExternalBufferRoundtrip          store Buffer → External → fetch → same bytes
//     FetchMultiSliceBufferRoundtrip        multi-chunk buffer stored correctly
//     FetchInlineRef                        inline ref materialized via toString()
//     FetchBufferedRef                      Buffered ref handled by fallback path
//     FetchEmptyRefReturnsNull              empty PayloadRef → callback(nullptr)
//
//   MmapPayloadStore – multiple payloads
//     MultiplePayloadsDistinctOffsets       offsets are non-overlapping and sequential
//     BytesWrittenTracksExternalOnly        inline writes do not advance write cursor
//
//   MmapPayloadStore – capacity growth
//     ExceedsInitialCapacityPreservesData   force doubling; all prior payloads intact
//
//   MmapPayloadStore – custom threshold
//     CustomInlineThreshold                 threshold=100 inline/external boundary
//
//   InMemoryPayloadStore – regression
//     InMemorySmallStringInline             unchanged: small → Inline
//     InMemoryLargeStringBuffered           unchanged: large → Buffered (not External)
//     InMemoryFetchRoundtrip                fetch returns same bytes

#include "source/extensions/filters/http/ai_protocol_manager/codec/mmap_payload_store.h"

#include "source/common/buffer/buffer_impl.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace AiProtocolManager {
namespace Codec {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// PayloadRef::External construction and accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST(PayloadRefExternalTest, ExternalConstructionAndAccessors) {
  PayloadRef ref = PayloadRef::makeExternal(/*offset=*/1024, /*length=*/2048);
  EXPECT_EQ(PayloadRef::Storage::External, ref.storage());
  EXPECT_EQ(1024u, ref.externalOffset());
  EXPECT_EQ(2048u, ref.externalLength());
  EXPECT_EQ(2048u, ref.size());
  EXPECT_FALSE(ref.empty());
}

TEST(PayloadRefExternalTest, ExternalEmptyRef) {
  PayloadRef ref = PayloadRef::makeExternal(0, 0);
  EXPECT_EQ(PayloadRef::Storage::External, ref.storage());
  EXPECT_EQ(0u, ref.size());
  EXPECT_TRUE(ref.empty());
}

TEST(PayloadRefExternalTest, DefaultRefIsInlineAndEmpty) {
  PayloadRef ref;
  EXPECT_EQ(PayloadRef::Storage::Inline, ref.storage());
  EXPECT_TRUE(ref.empty());
  EXPECT_EQ(0u, ref.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – inline threshold (string overload)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, SmallStringStaysInline) {
  MmapPayloadStore store;
  const std::string payload(100, 'x');
  PayloadRef ref = store.store(payload, PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Inline, ref.storage());
  EXPECT_EQ(0u, store.bytesWritten());
}

TEST(MmapPayloadStoreTest, ExactThresholdStringStaysInline) {
  MmapPayloadStore store; // threshold = 4096
  const std::string payload(MmapPayloadStore::kDefaultMaxInlineBytes, 'x');
  PayloadRef ref = store.store(payload, PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Inline, ref.storage());
  EXPECT_EQ(0u, store.bytesWritten());
}

TEST(MmapPayloadStoreTest, LargeStringGoesExternal) {
  MmapPayloadStore store;
  const std::string payload(MmapPayloadStore::kDefaultMaxInlineBytes + 1, 'A');
  PayloadRef ref = store.store(payload, PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::External, ref.storage());
  EXPECT_EQ(0u, ref.externalOffset());
  EXPECT_EQ(payload.size(), ref.externalLength());
  EXPECT_EQ(payload.size(), store.bytesWritten());
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – inline threshold (Buffer::Instance overload)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, SmallBufferStaysInline) {
  MmapPayloadStore store;
  Buffer::OwnedImpl buf;
  buf.add("small payload");
  PayloadRef ref = store.store(buf, PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Inline, ref.storage());
  EXPECT_EQ(0u, store.bytesWritten());
}

TEST(MmapPayloadStoreTest, LargeBufferGoesExternal) {
  MmapPayloadStore store;
  Buffer::OwnedImpl buf;
  buf.add(std::string(MmapPayloadStore::kDefaultMaxInlineBytes + 1, 'B'));
  PayloadRef ref = store.store(buf, PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::External, ref.storage());
  EXPECT_EQ(MmapPayloadStore::kDefaultMaxInlineBytes + 1, ref.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – fetch roundtrip
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, FetchExternalStringRoundtrip) {
  MmapPayloadStore store;
  const std::string payload(8000, 'Z');
  PayloadRef ref = store.store(payload, PayloadKind::JsonObject);
  ASSERT_EQ(PayloadRef::Storage::External, ref.storage());

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(payload.size(), result->length());
    EXPECT_EQ(payload, result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(MmapPayloadStoreTest, FetchExternalBufferRoundtrip) {
  MmapPayloadStore store;
  const std::string content(6000, 'C');
  Buffer::OwnedImpl buf;
  buf.add(content);
  PayloadRef ref = store.store(buf, PayloadKind::JsonObject);
  ASSERT_EQ(PayloadRef::Storage::External, ref.storage());

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(content, result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(MmapPayloadStoreTest, FetchMultiSliceBufferRoundtrip) {
  // Two add() calls may produce multiple internal slices (each slab is ~4 KB).
  // Verify the slice-walking loop in store(Buffer::Instance&) stitches them
  // into a contiguous mmap region correctly.
  MmapPayloadStore store;
  const std::string chunk1(4097, 'X');
  const std::string chunk2(4097, 'Y');
  Buffer::OwnedImpl buf;
  buf.add(chunk1);
  buf.add(chunk2);

  PayloadRef ref = store.store(buf, PayloadKind::JsonObject);
  ASSERT_EQ(PayloadRef::Storage::External, ref.storage());
  EXPECT_EQ(chunk1.size() + chunk2.size(), ref.size());

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(chunk1 + chunk2, result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(MmapPayloadStoreTest, FetchInlineRef) {
  MmapPayloadStore store;
  PayloadRef ref = store.store(std::string("hello world"), PayloadKind::JsonObject);
  ASSERT_EQ(PayloadRef::Storage::Inline, ref.storage());

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ("hello world", result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(MmapPayloadStoreTest, FetchBufferedRefFallback) {
  // MmapPayloadStore::fetch must handle Buffered refs (e.g. produced by a
  // different store implementation and handed to this one for fetch).
  MmapPayloadStore store;
  const std::string payload(5000, 'P');
  auto owned = std::make_unique<Buffer::OwnedImpl>(payload);
  PayloadRef ref = PayloadRef::makeBuffered(std::move(owned));

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(payload, result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(MmapPayloadStoreTest, FetchEmptyRefReturnsNull) {
  MmapPayloadStore store;
  bool called = false;
  store.fetch(PayloadRef{}, [&](Buffer::InstancePtr result) {
    EXPECT_EQ(nullptr, result);
    called = true;
  });
  EXPECT_TRUE(called);
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – multiple payloads at distinct offsets
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, MultiplePayloadsDistinctOffsets) {
  MmapPayloadStore store;
  const std::string a(5000, 'A');
  const std::string b(7000, 'B');
  const std::string c(6000, 'C');

  PayloadRef ra = store.store(a, PayloadKind::JsonObject);
  PayloadRef rb = store.store(b, PayloadKind::JsonObject);
  PayloadRef rc = store.store(c, PayloadKind::JsonObject);

  ASSERT_EQ(PayloadRef::Storage::External, ra.storage());
  ASSERT_EQ(PayloadRef::Storage::External, rb.storage());
  ASSERT_EQ(PayloadRef::Storage::External, rc.storage());

  EXPECT_EQ(0u,                       ra.externalOffset());
  EXPECT_EQ(a.size(),                 rb.externalOffset());
  EXPECT_EQ(a.size() + b.size(),      rc.externalOffset());
  EXPECT_EQ(a.size() + b.size() + c.size(), store.bytesWritten());

  // Verify no cross-contamination between payloads.
  bool ok_a = false;
  store.fetch(ra, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(a, result->toString());
    ok_a = true;
  });
  bool ok_b = false;
  store.fetch(rb, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(b, result->toString());
    ok_b = true;
  });
  bool ok_c = false;
  store.fetch(rc, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(c, result->toString());
    ok_c = true;
  });
  EXPECT_TRUE(ok_a);
  EXPECT_TRUE(ok_b);
  EXPECT_TRUE(ok_c);
}

TEST(MmapPayloadStoreTest, BytesWrittenTracksExternalOnly) {
  MmapPayloadStore store;
  store.store(std::string(100, 'a'), PayloadKind::JsonObject);  // inline
  store.store(std::string(200, 'b'), PayloadKind::JsonObject);  // inline
  EXPECT_EQ(0u, store.bytesWritten());

  store.store(std::string(5000, 'c'), PayloadKind::JsonObject); // external
  EXPECT_EQ(5000u, store.bytesWritten());

  store.store(std::string(3000, 'd'), PayloadKind::JsonObject); // inline (< 4096)
  EXPECT_EQ(5000u, store.bytesWritten());

  store.store(std::string(5000, 'e'), PayloadKind::JsonObject); // external
  EXPECT_EQ(10000u, store.bytesWritten());
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – capacity growth
//
// Initial capacity is 64 KB. Store 10 × 8 KB = 80 KB total, forcing at least
// one capacity doubling. Verify all payloads round-trip correctly after the
// remap.
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, ExceedsInitialCapacityPreservesData) {
  MmapPayloadStore store;
  constexpr size_t kChunkSize = 8192; // 8 KB
  constexpr int    kChunks    = 10;   // 80 KB total > 64 KB initial

  std::vector<PayloadRef>  refs;
  std::vector<std::string> payloads;
  refs.reserve(kChunks);
  payloads.reserve(kChunks);

  for (int i = 0; i < kChunks; ++i) {
    payloads.push_back(std::string(kChunkSize, static_cast<char>('A' + i)));
    refs.push_back(store.store(payloads.back(), PayloadKind::JsonObject));
    ASSERT_EQ(PayloadRef::Storage::External, refs.back().storage())
        << "chunk " << i << " should be External";
  }

  EXPECT_EQ(kChunkSize * kChunks, store.bytesWritten());

  for (int i = 0; i < kChunks; ++i) {
    bool called = false;
    store.fetch(refs[i], [&, i](Buffer::InstancePtr result) {
      ASSERT_NE(nullptr, result) << "chunk " << i;
      EXPECT_EQ(payloads[i], result->toString()) << "chunk " << i;
      called = true;
    });
    EXPECT_TRUE(called) << "fetch not called for chunk " << i;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// MmapPayloadStore – custom inline threshold
// ─────────────────────────────────────────────────────────────────────────────

TEST(MmapPayloadStoreTest, CustomInlineThreshold) {
  MmapPayloadStore store("/tmp", /*max_inline=*/100);

  PayloadRef small = store.store(std::string(100, 's'), PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Inline, small.storage());
  EXPECT_EQ(0u, store.bytesWritten());

  PayloadRef large = store.store(std::string(101, 'l'), PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::External, large.storage());
  EXPECT_EQ(101u, store.bytesWritten());

  // Roundtrip the external one.
  bool called = false;
  store.fetch(large, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(std::string(101, 'l'), result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

// ─────────────────────────────────────────────────────────────────────────────
// InMemoryPayloadStore – regression: External enum addition must not change
// existing Inline/Buffered behaviour.
// ─────────────────────────────────────────────────────────────────────────────

TEST(InMemoryPayloadStoreTest, SmallStringStillInline) {
  InMemoryPayloadStore store;
  PayloadRef ref = store.store(std::string(100, 'x'), PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Inline, ref.storage());
}

TEST(InMemoryPayloadStoreTest, LargeStringStillBuffered) {
  InMemoryPayloadStore store;
  PayloadRef ref = store.store(std::string(5000, 'y'), PayloadKind::JsonObject);
  EXPECT_EQ(PayloadRef::Storage::Buffered, ref.storage());
}

TEST(InMemoryPayloadStoreTest, FetchRoundtrip) {
  InMemoryPayloadStore store;
  const std::string payload(5000, 'q');
  PayloadRef ref = store.store(payload, PayloadKind::JsonObject);

  bool called = false;
  store.fetch(ref, [&](Buffer::InstancePtr result) {
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(payload, result->toString());
    called = true;
  });
  EXPECT_TRUE(called);
}

TEST(InMemoryPayloadStoreTest, LargeBufferInstanceStillBuffered) {
  InMemoryPayloadStore store;
  Buffer::OwnedImpl buf;
  buf.add(std::string(5000, 'r'));
  PayloadRef ref = store.store(buf, PayloadKind::JsonObject);
  // InMemoryPayloadStore uses move() so the source buffer is drained.
  EXPECT_EQ(PayloadRef::Storage::Buffered, ref.storage());
  EXPECT_EQ(0u, buf.length()); // drained by move()
}

} // namespace
} // namespace Codec
} // namespace AiProtocolManager
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy

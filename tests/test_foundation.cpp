// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Deterministic primitives, identity, generations, checked arithmetic and evidence.

#include "federation_observatory/codec.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/version.hpp"
#include "harness.hpp"

using namespace fo;

FO_TEST(foundation, version_matches_banner) {
  FO_CHECK_EQ(std::string(version_string()), std::string("1.0.0"));
  FO_CHECK(version().major == 1);
  FO_CHECK(build_banner().find("Federation Observatory 1.0.0") == 0);
  FO_CHECK_EQ(protocol_version(), 1u);
  FO_CHECK_EQ(persistence_format_version(), 1u);
}

FO_TEST(foundation, identifier_validation) {
  FO_CHECK(FO_UNWRAP(FederationId::parse("fed-1")).value() == "fed-1");
  FO_CHECK(FO_UNWRAP(ClusterId::parse("a.b_c:d@e/f+g=h")).value() == "a.b_c:d@e/f+g=h");
  FO_CHECK(!FederationId::parse("").ok());
  FO_CHECK(!FederationId::parse("has space").ok());
  FO_CHECK(!FederationId::parse("has\ttab").ok());
  FO_CHECK(!FederationId::parse(std::string(kMaxIdentifierLength + 1, 'a')).ok());
  FO_CHECK(ClusterId{}.empty());
}

FO_TEST(foundation, generation_zero_is_unset_and_never_newer) {
  const FederationGeneration unset;
  const FederationGeneration first = FederationGeneration::first();
  FO_CHECK(unset.is_unset());
  FO_CHECK(!unset.is_set());
  FO_CHECK(first.is_set());
  FO_CHECK(first.newer_than(unset));
  FO_CHECK(!unset.newer_than(first));
  FO_CHECK_EQ(unset.next().value(), 1u);
  const FederationGeneration maximum(UINT64_MAX);
  FO_CHECK_EQ(maximum.next().value(), UINT64_MAX);
}

FO_TEST(foundation, checked_arithmetic) {
  std::uint64_t out = 0;
  FO_CHECK(checked_add(1, 2, out));
  FO_CHECK_EQ(out, 3u);
  FO_CHECK(!checked_add(UINT64_MAX, 1, out));
  FO_CHECK(checked_sub(5, 3, out));
  FO_CHECK_EQ(out, 2u);
  FO_CHECK(!checked_sub(3, 5, out));
  FO_CHECK(checked_mul(4, 5, out));
  FO_CHECK_EQ(out, 20u);
  FO_CHECK(!checked_mul(UINT64_MAX, 2, out));
  FO_CHECK_EQ(saturating_add(UINT64_MAX, 10), UINT64_MAX);
  const std::uint64_t values[] = {1, 2, 3, 4};
  FO_CHECK(checked_sum(values, 4, out));
  FO_CHECK_EQ(out, 10u);
}

FO_TEST(foundation, exact_percentages) {
  FO_CHECK_EQ(percent_string(0, 100), std::string("0.00%"));
  FO_CHECK_EQ(percent_string(50, 100), std::string("50.00%"));
  FO_CHECK_EQ(percent_string(1, 3), std::string("33.33%"));
  FO_CHECK_EQ(percent_string(2, 3), std::string("66.67%"));
  FO_CHECK_EQ(percent_string(1, 0), std::string("0.00%"));
  FO_CHECK_EQ(percent_string(28, 100), std::string("28.00%"));
  FO_CHECK_EQ(percent_string(1, 7), std::string("14.29%"));
  FO_CHECK_EQ(basis_points_of(28, 100), 2800u);
}

FO_TEST(foundation, crc_and_fnv_are_stable) {
  FO_CHECK_EQ(crc32(std::string_view("123456789")), 0xCBF43926u);
  FO_CHECK_EQ(fnv1a64(std::string_view("")), 14695981039346656037ull);
}

FO_TEST(foundation, encoder_decoder_round_trip) {
  Encoder encoder;
  encoder.u8(0x12);
  encoder.u16(0x1234);
  encoder.u32(0x12345678);
  encoder.u64(0x123456789ABCDEF0ull);
  encoder.i64(-42);
  encoder.f64(1.5);
  encoder.boolean(true);
  encoder.str("hello");
  std::vector<std::uint8_t> bytes = encoder.take();
  Decoder decoder(bytes.data(), bytes.size(), 1024);
  FO_CHECK_EQ(FO_UNWRAP(decoder.u8()), 0x12);
  FO_CHECK_EQ(FO_UNWRAP(decoder.u16()), 0x1234);
  FO_CHECK_EQ(FO_UNWRAP(decoder.u32()), 0x12345678u);
  FO_CHECK_EQ(FO_UNWRAP(decoder.u64()), 0x123456789ABCDEF0ull);
  FO_CHECK_EQ(FO_UNWRAP(decoder.i64()), -42);
  FO_CHECK_EQ(FO_UNWRAP(decoder.f64()), 1.5);
  FO_CHECK_EQ(FO_UNWRAP(decoder.boolean()), true);
  FO_CHECK_EQ(FO_UNWRAP(decoder.str()), std::string("hello"));
  FO_CHECK(decoder.exhausted());
}

FO_TEST(foundation, decoder_rejects_truncation_and_oversize) {
  Encoder encoder;
  encoder.u64(7);
  const std::vector<std::uint8_t> bytes = encoder.take();
  {
    Decoder decoder(bytes.data(), 4, 1024);
    FO_CHECK(!decoder.u64().ok());
  }
  {
    Decoder decoder(bytes.data(), bytes.size(), 4);
    FO_CHECK(!decoder.u64().ok());
  }
  {
    Encoder strings;
    strings.str(std::string(kMaxStringFieldBytes + 64, 'x'));
    const std::vector<std::uint8_t> blob = strings.take();
    Decoder decoder(blob.data(), blob.size(), 1024 * 1024);
    FO_CHECK(!decoder.str().ok());
  }
}

FO_TEST(foundation, deterministic_random_is_reproducible) {
  DeterministicRandom first(1234);
  DeterministicRandom second(1234);
  for (int i = 0; i < 64; ++i) {
    FO_CHECK_EQ(first.next_u64(), second.next_u64());
  }
  DeterministicRandom third(4321);
  FO_CHECK(first.next_u64() != third.next_u64());
}

FO_TEST(foundation, tri_algebra_preserves_unknown) {
  FO_CHECK(tri_and(Tri::Yes, Tri::Unknown) == Tri::Unknown);
  FO_CHECK(tri_and(Tri::No, Tri::Unknown) == Tri::No);
  FO_CHECK(tri_or(Tri::Yes, Tri::Unknown) == Tri::Yes);
  FO_CHECK(tri_or(Tri::No, Tri::Unknown) == Tri::Unknown);
  FO_CHECK(tri_not(Tri::Unknown) == Tri::Unknown);
}

FO_TEST(foundation, precision_and_evidence_class_combination_is_pessimistic) {
  FO_CHECK(weakest(Precision::Exact, Precision::Inferred) == Precision::Inferred);
  FO_CHECK(weakest(Precision::Unknown, Precision::Exact) == Precision::Unknown);
  FO_CHECK(strongest(Precision::Exact, Precision::Ambiguous) == Precision::Exact);
  FO_CHECK(weaker(EvidenceClass::Real, EvidenceClass::Synthetic) == EvidenceClass::Synthetic);
  FO_CHECK(weaker(EvidenceClass::Real, EvidenceClass::Unsupported) == EvidenceClass::Unsupported);
  FO_CHECK(weaker(EvidenceClass::Synthetic, EvidenceClass::Unknown) == EvidenceClass::Unknown);
  Precision parsed = Precision::Exact;
  FO_CHECK(parse_precision("AMBIGUOUS", parsed));
  FO_CHECK(parsed == Precision::Ambiguous);
  EvidenceClass parsed_class = EvidenceClass::Real;
  FO_CHECK(parse_evidence_class("SYNTHETIC", parsed_class));
  FO_CHECK(parsed_class == EvidenceClass::Synthetic);
}

FO_TEST(foundation, evidence_list_deduplicates_and_bounds) {
  EvidenceList list(2);
  FO_CHECK(list.add(Provenance::Scheduler, EvidenceClass::Real, "scheduler", Precision::Exact,
                    "first")
               .ok());
  FO_CHECK(list.add(Provenance::Scheduler, EvidenceClass::Real, "scheduler", Precision::Exact,
                    "first")
               .ok());
  FO_CHECK_EQ(list.size(), 1u);
  FO_CHECK(list.add(Provenance::Scheduler, EvidenceClass::Real, "scheduler", Precision::Exact,
                    "second")
              .ok());
  FO_CHECK_EQ(list.size(), 2u);
  const Status overflow = list.add(Provenance::Scheduler, EvidenceClass::Real, "scheduler",
                                   Precision::Exact, "third");
  FO_CHECK(!overflow.ok());
  FO_CHECK(overflow.code() == ErrorCode::BoundExceeded);
  FO_CHECK(list.combined_precision() == Precision::Exact);
  FO_CHECK(list.all_real());
  FO_CHECK(!list.any_synthetic());
  FO_CHECK(!evidence_digest(list).empty());
}

FO_TEST(foundation, bounds_validation_rejects_impossible_configuration) {
  Bounds bounds;
  FO_CHECK(bounds.validate().ok());
  bounds.max_frame_bytes = 1;
  FO_CHECK(!bounds.validate().ok());
  bounds = Bounds{};
  bounds.max_clusters = 0;
  FO_CHECK(!bounds.validate().ok());
  bounds = Bounds{};
  bounds.max_evidence_per_finding = 0;
  FO_CHECK(!bounds.validate().ok());
}

FO_TEST(foundation, rendering_helpers_are_deterministic) {
  const std::string table = render_table({"a", "bb"}, {{"1", "2"}, {"333", "4"}}, "  ");
  FO_CHECK_EQ(table, render_table({"a", "bb"}, {{"1", "2"}, {"333", "4"}}, "  "));
  FO_CHECK(table.find("333") != std::string::npos);
  FO_CHECK_EQ(hex_digest64(0), std::string("0000000000000000"));
  FO_CHECK_EQ(hex_digest64(255), std::string("00000000000000ff"));
  FO_CHECK_EQ(digest_text("x"), digest_text("x"));
  FO_CHECK(digest_text("x") != digest_text("y"));
  FO_CHECK_EQ(truncate_with_marker("abcdef", 4), std::string("a..."));
  FO_CHECK_EQ(truncate_with_marker("abc", 10), std::string("abc"));
  FO_CHECK_EQ(join_strings({"a", "b"}, ","), std::string("a,b"));
}

FO_TEST(foundation, timestamps_render_deterministically) {
  FO_CHECK_EQ(format_timestamp(0), std::string("unset"));
  FO_CHECK_EQ(format_timestamp(1767225600000000000LL), std::string("2026-01-01T00:00:00.000Z"));
  FO_CHECK_EQ(format_timestamp(1767225600123456789LL), std::string("2026-01-01T00:00:00.123Z"));
}

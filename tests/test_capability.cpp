// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Structured capability keys, typed values, and tri-state requirement evaluation.

#include "federation_observatory/capability.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

CapabilityEntry entry(CapabilityKey key, CapabilityValue value) {
  CapabilityEntry result;
  result.key.key = key;
  result.value = std::move(value);
  result.precision = Precision::Exact;
  result.evidence_class = EvidenceClass::Real;
  return result;
}

CapabilityRequirement requirement(CapabilityKey key, CapabilityComparator comparator,
                                  CapabilityValue value = CapabilityValue{}) {
  CapabilityRequirement result;
  result.key.key = key;
  result.comparator = comparator;
  result.value = std::move(value);
  return result;
}

}  // namespace

FO_TEST(capability, keys_are_typed_and_round_trip) {
  CapabilityKey key = CapabilityKey::Unknown;
  FO_CHECK(parse_capability_key("precision.fp8_e4m3", key));
  FO_CHECK(key == CapabilityKey::PrecisionFp8E4M3);
  FO_CHECK(category_of(key) == CapabilityCategory::PrecisionSupport);
  FO_CHECK(parse_capability_key("memory.bytes", key));
  FO_CHECK(category_of(key) == CapabilityCategory::MemorySize);
  FO_CHECK(!parse_capability_key("not.a.real.key", key));
  CapabilityRef custom;
  custom.key = CapabilityKey::Custom;
  custom.custom = CapabilityKeyId::unchecked("vendor.secret");
  FO_CHECK(custom.valid());
  FO_CHECK_EQ(custom.to_string(), std::string("custom:vendor.secret"));
  CapabilityRef invalid;
  invalid.key = CapabilityKey::Custom;
  FO_CHECK(!invalid.valid());
}

FO_TEST(capability, values_are_typed) {
  FO_CHECK(CapabilityValue::boolean(true).is_boolean());
  FO_CHECK(CapabilityValue::integer(-3).is_integral());
  FO_CHECK(CapabilityValue::unsigned_integer(7).is_integral());
  FO_CHECK(CapabilityValue::number(1.5).is_number());
  FO_CHECK(CapabilityValue::text("x").is_text());
  FO_CHECK(!CapabilityValue{}.known());
  FO_CHECK_EQ(CapabilityValue::integer(-3).as_integer(0), -3);
  FO_CHECK_EQ(CapabilityValue::unsigned_integer(7).as_unsigned(0), 7u);
  const CapabilityValue text_value = CapabilityValue::text("x");
  FO_CHECK_EQ(text_value.as_text(), std::string_view("x"));
  const Result<CapabilityValue> set = CapabilityValue::text_set({"b", "a", "a"});
  FO_CHECK(set.ok());
  FO_CHECK(set.value().is_text_set());
  FO_CHECK_EQ(set.value().as_text_set().size(), 2u);
  FO_CHECK_EQ(set.value().to_string(), std::string("[a, b]"));
}

FO_TEST(capability, set_is_ordered_deduplicated_and_bounded) {
  CapabilitySet set(2);
  FO_CHECK(set.put(entry(CapabilityKey::RuntimeVersion, CapabilityValue::text("1.0"))).ok());
  FO_CHECK(set.put(entry(CapabilityKey::DriverVersion, CapabilityValue::text("2.0"))).ok());
  FO_CHECK_EQ(set.size(), 2u);
  const Status overflow =
      set.put(entry(CapabilityKey::MemoryBytes, CapabilityValue::unsigned_integer(1)));
  FO_CHECK(!overflow.ok());
  FO_CHECK(overflow.code() == ErrorCode::BoundExceeded);
  FO_CHECK(set.put(entry(CapabilityKey::RuntimeVersion, CapabilityValue::text("1.1"))).ok());
  FO_CHECK_EQ(set.size(), 2u);
  CapabilityRef probe;
  probe.key = CapabilityKey::RuntimeVersion;
  FO_CHECK(set.find(probe) != nullptr);
  FO_CHECK_EQ(set.find(probe)->value.to_string(), std::string("1.1"));
}

FO_TEST(capability, absent_evidence_is_never_treated_as_compatible) {
  CapabilitySet published;
  FO_CHECK(
      published.put(entry(CapabilityKey::PrecisionFp8E4M3, CapabilityValue::boolean(true))).ok());

  const CapabilityCheck present =
      evaluate_requirement(requirement(CapabilityKey::PrecisionBf16, CapabilityComparator::Present),
                           published);
  FO_CHECK(present.satisfied == Tri::No);

  const CapabilityCheck at_least = evaluate_requirement(
      requirement(CapabilityKey::MemoryBytes, CapabilityComparator::AtLeast,
                  CapabilityValue::unsigned_integer(1)),
      published);
  FO_CHECK(at_least.satisfied == Tri::Unknown);

  const CapabilityCheck absent =
      evaluate_requirement(requirement(CapabilityKey::CxlCapability, CapabilityComparator::Absent),
                           published);
  FO_CHECK(absent.satisfied == Tri::Yes);
}

FO_TEST(capability, comparators_behave) {
  CapabilitySet published;
  FO_CHECK(published.put(entry(CapabilityKey::MemoryBytes, CapabilityValue::unsigned_integer(1024)))
               .ok());
  FO_CHECK(
      published.put(entry(CapabilityKey::RuntimeVersion, CapabilityValue::text("12.4.1"))).ok());
  FO_CHECK(published.put(entry(CapabilityKey::ArtifactFormats,
                               FO_UNWRAP(CapabilityValue::text_set({"cubin", "elf"}))))
               .ok());
  FO_CHECK(
      published.put(entry(CapabilityKey::PartitionSupport, CapabilityValue::boolean(false))).ok());

  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::MemoryBytes, CapabilityComparator::AtLeast,
                                            CapabilityValue::unsigned_integer(512)),
                                published)
               .satisfied == Tri::Yes);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::MemoryBytes, CapabilityComparator::AtLeast,
                                            CapabilityValue::unsigned_integer(2048)),
                                published)
               .satisfied == Tri::No);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::MemoryBytes, CapabilityComparator::AtMost,
                                            CapabilityValue::unsigned_integer(2048)),
                                published)
               .satisfied == Tri::Yes);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::RuntimeVersion,
                                            CapabilityComparator::AtLeast,
                                            CapabilityValue::text("12.4")),
                                published)
               .satisfied == Tri::Yes);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::RuntimeVersion,
                                            CapabilityComparator::AtLeast,
                                            CapabilityValue::text("13.0")),
                                published)
               .satisfied == Tri::No);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::ArtifactFormats,
                                            CapabilityComparator::AnyOf,
                                            FO_UNWRAP(CapabilityValue::text_set({"cubin"}))),
                                published)
               .satisfied == Tri::Yes);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::ArtifactFormats,
                                            CapabilityComparator::AllOf,
                                            FO_UNWRAP(CapabilityValue::text_set({"cubin", "ptx"}))),
                                published)
               .satisfied == Tri::No);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::PartitionSupport,
                                            CapabilityComparator::Present),
                                published)
               .satisfied == Tri::No);
  FO_CHECK(evaluate_requirement(requirement(CapabilityKey::PartitionSupport,
                                            CapabilityComparator::Absent),
                                published)
               .satisfied == Tri::Yes);
}

FO_TEST(capability, version_ordering) {
  FO_CHECK(compare_version_strings("1.2", "1.2") == 0);
  FO_CHECK(compare_version_strings("1.2", "1.10") < 0);
  FO_CHECK(compare_version_strings("12.9", "12.10") < 0);
  FO_CHECK(compare_version_strings("2.0", "1.9") > 0);
  FO_CHECK(compare_version_strings("1.2", "1.2.1") < 0);
  FO_CHECK(compare_version_strings("1.2.0", "1.2") > 0);
  FO_CHECK(compare_version_strings("12.4.1", "12.4") > 0);
  FO_CHECK_EQ(FO_UNWRAP(parse_version("12.4.1")).size(), 3u);
  FO_CHECK(!parse_version("").ok());
  FO_CHECK(!parse_version("12.x").ok());
}

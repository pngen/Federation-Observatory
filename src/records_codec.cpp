// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Canonical binary encoding for every record the runtime publishes or persists.
// Encoding is deterministic: equal records always produce identical bytes, and every
// collection is written in sorted order regardless of the order the caller supplied.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/records_codec.hpp"

namespace fo::codec {
namespace {

constexpr std::uint32_t kCollectionLimit = 1u << 22;

template <class Tag>
void enc_id(Encoder& out, const StrongId<Tag>& id) {
  out.str(id.value());
}

template <class Tag>
Result<StrongId<Tag>> dec_id(Decoder& in) {
  Result<std::string> text = in.str();
  if (!text.ok()) {
    return text.error();
  }
  if (text.value().empty()) {
    return StrongId<Tag>{};
  }
  if (!is_valid_identifier(text.value())) {
    return Error(ErrorCode::ProtocolViolation, "encoded identifier is not valid",
                 std::string(Tag::name));
  }
  return StrongId<Tag>::unchecked(text.value());
}

template <class Tag>
void enc_gen(Encoder& out, const Generation<Tag>& generation) {
  out.u64(generation.value());
}

template <class Tag>
Result<Generation<Tag>> dec_gen(Decoder& in) {
  Result<std::uint64_t> value = in.u64();
  if (!value.ok()) {
    return value.error();
  }
  return Generation<Tag>(value.value());
}

void enc_sequence(Encoder& out, Sequence sequence) { out.u64(sequence.value()); }

Result<Sequence> dec_sequence(Decoder& in) {
  Result<std::uint64_t> value = in.u64();
  if (!value.ok()) {
    return value.error();
  }
  return Sequence(value.value());
}

template <class E>
void enc_enum(Encoder& out, E value) {
  out.u8(static_cast<std::uint8_t>(value));
}

template <class E>
Result<E> dec_enum(Decoder& in, std::uint8_t max_value, const char* what) {
  Result<std::uint8_t> value = in.u8();
  if (!value.ok()) {
    return value.error();
  }
  if (value.value() > max_value) {
    return Error(ErrorCode::ProtocolViolation, std::string("encoded ") + what + " is out of range");
  }
  return static_cast<E>(value.value());
}

void enc_text(Encoder& out, const std::string& value) { out.str(value); }

void enc_text_vector(Encoder& out, std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  out.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) {
    out.str(value);
  }
}

Result<std::vector<std::string>> dec_text_vector(Decoder& in, std::uint32_t limit) {
  Result<std::uint32_t> count = in.u32();
  if (!count.ok()) {
    return count.error();
  }
  if (count.value() > limit) {
    return Error(ErrorCode::BoundExceeded, "encoded string collection exceeds the permitted size");
  }
  std::vector<std::string> values;
  values.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    Result<std::string> value = in.str();
    if (!value.ok()) {
      return value.error();
    }
    values.push_back(value.value());
  }
  std::sort(values.begin(), values.end());
  if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
    return Error(ErrorCode::ProtocolViolation, "encoded string collection contains duplicates");
  }
  return values;
}

template <class T, class Enc>
void enc_vector(Encoder& out, std::vector<T> values, Enc enc) {
  std::sort(values.begin(), values.end());
  out.u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) {
    enc(out, value);
  }
}

template <class T, class Dec>
Result<std::vector<T>> dec_vector(Decoder& in, std::uint32_t limit, Dec dec) {
  Result<std::uint32_t> count = in.u32();
  if (!count.ok()) {
    return count.error();
  }
  if (count.value() > limit) {
    return Error(ErrorCode::BoundExceeded, "encoded collection exceeds the permitted size");
  }
  std::vector<T> values;
  values.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    Result<T> value = dec(in);
    if (!value.ok()) {
      return value.error();
    }
    values.push_back(value.take());
  }
  return values;
}

constexpr std::uint8_t enum_max(Currentness) { return static_cast<std::uint8_t>(Currentness::Retired); }
constexpr std::uint8_t enum_max(Readiness) { return static_cast<std::uint8_t>(Readiness::Retired); }
constexpr std::uint8_t enum_max(RuntimeKind) { return static_cast<std::uint8_t>(RuntimeKind::Custom); }
constexpr std::uint8_t enum_max(ArtifactKind) { return static_cast<std::uint8_t>(ArtifactKind::Custom); }
constexpr std::uint8_t enum_max(DomainKind) { return static_cast<std::uint8_t>(DomainKind::Federation); }
constexpr std::uint8_t enum_max(ResourceKind) { return static_cast<std::uint8_t>(ResourceKind::Custom); }
constexpr std::uint8_t enum_max(Tri) { return static_cast<std::uint8_t>(Tri::Yes); }
constexpr std::uint8_t enum_max(Precision) { return static_cast<std::uint8_t>(Precision::Unknown); }
constexpr std::uint8_t enum_max(ReasonBasis) { return static_cast<std::uint8_t>(ReasonBasis::Unattributed); }
constexpr std::uint8_t enum_max(EvidenceClass) { return static_cast<std::uint8_t>(EvidenceClass::Unknown); }
constexpr std::uint8_t enum_max(Provenance) { return static_cast<std::uint8_t>(Provenance::Unknown); }
constexpr std::uint8_t enum_max(CapabilityComparator) {
  return static_cast<std::uint8_t>(CapabilityComparator::AllOf);
}
constexpr std::uint8_t enum_max(PortabilityDimension) {
  return static_cast<std::uint8_t>(PortabilityDimension::Operational);
}
constexpr std::uint8_t enum_max(PortabilityOutcome) {
  return static_cast<std::uint8_t>(PortabilityOutcome::Unsupported);
}
constexpr std::uint8_t enum_max(MigrationStage) {
  return static_cast<std::uint8_t>(MigrationStage::OutcomeUnknown);
}
constexpr std::uint8_t enum_max(MigrationOutcome) {
  return static_cast<std::uint8_t>(MigrationOutcome::Unknown);
}
constexpr std::uint8_t enum_max(CandidateStatus) {
  return static_cast<std::uint8_t>(CandidateStatus::Unattributed);
}
constexpr std::uint8_t enum_max(CandidateSetCompleteness) {
  return static_cast<std::uint8_t>(CandidateSetCompleteness::Complete);
}

[[nodiscard]] bool valid_capability_key(std::uint16_t raw) noexcept {
  if (raw == 0 || raw == static_cast<std::uint16_t>(CapabilityKey::Custom)) {
    return raw == static_cast<std::uint16_t>(CapabilityKey::Custom);
  }
  CapabilityRef ref;
  ref.key = static_cast<CapabilityKey>(raw);
  return !ref.to_string().empty() && ref.to_string() != "unknown";
}

}  // namespace

void encode_evidence(Encoder& out, const EvidenceRef& value) {
  enc_enum(out, value.source);
  enc_enum(out, value.evidence_class);
  out.str(value.source_id);
  enc_gen(out, value.generation);
  enc_enum(out, value.precision);
  out.str(value.detail);
}

Result<EvidenceRef> decode_evidence(Decoder& in) {
  EvidenceRef value;
  Result<Provenance> provenance = dec_enum<Provenance>(in, enum_max(Provenance{}), "provenance");
  if (!provenance.ok()) return provenance.error();
  value.source = provenance.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<std::string> source_id = in.str();
  if (!source_id.ok()) return source_id.error();
  value.source_id = source_id.take();
  Result<EvidenceGeneration> generation = dec_gen<EvidenceGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<std::string> detail = in.str();
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  return value;
}

void encode_evidence_list(Encoder& out, const EvidenceList& value) {
  out.u32(static_cast<std::uint32_t>(value.items().size()));
  for (const EvidenceRef& ref : value.items()) {
    encode_evidence(out, ref);
  }
}

Result<EvidenceList> decode_evidence_list(Decoder& in, std::size_t limit) {
  Result<std::uint32_t> count = in.u32();
  if (!count.ok()) return count.error();
  if (count.value() > kMaxEvidencePerFinding) {
    return Error(ErrorCode::BoundExceeded, "encoded evidence list exceeds the permitted size");
  }
  EvidenceList list(limit);
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    Result<EvidenceRef> ref = decode_evidence(in);
    if (!ref.ok()) return ref.error();
    const Status s = list.add(ref.take());
    if (!s.ok()) return s.error();
  }
  return list;
}

void encode_capability_ref(Encoder& out, const CapabilityRef& value) {
  out.u16(static_cast<std::uint16_t>(value.key));
  enc_id(out, value.custom);
}

Result<CapabilityRef> decode_capability_ref(Decoder& in) {
  Result<std::uint16_t> raw = in.u16();
  if (!raw.ok()) return raw.error();
  if (!valid_capability_key(raw.value())) {
    return Error(ErrorCode::ProtocolViolation, "encoded capability key is not recognised");
  }
  CapabilityRef value;
  value.key = static_cast<CapabilityKey>(raw.value());
  Result<CapabilityKeyId> custom = dec_id<CapabilityKeyIdTag>(in);
  if (!custom.ok()) return custom.error();
  value.custom = custom.take();
  if (!value.valid()) {
    return Error(ErrorCode::ProtocolViolation, "encoded capability key is inconsistent");
  }
  return value;
}

void encode_capability_value(Encoder& out, const CapabilityValue& value) {
  if (value.is_boolean()) {
    out.u8(1);
    out.boolean(value.as_boolean(false));
  } else if (value.is_integral()) {
    out.u8(2);
    out.i64(value.as_integer(0));
  } else if (value.is_number()) {
    out.u8(3);
    out.f64(value.as_number(0.0));
  } else if (value.is_text()) {
    out.u8(4);
    out.str(std::string(value.as_text()));
  } else if (value.is_text_set()) {
    out.u8(5);
    enc_text_vector(out, value.as_text_set());
  } else {
    out.u8(0);
  }
}

Result<CapabilityValue> decode_capability_value(Decoder& in) {
  Result<std::uint8_t> kind = in.u8();
  if (!kind.ok()) return kind.error();
  switch (kind.value()) {
    case 0:
      return CapabilityValue{};
    case 1: {
      Result<bool> value = in.boolean();
      if (!value.ok()) return value.error();
      return CapabilityValue::boolean(value.value());
    }
    case 2: {
      Result<std::int64_t> value = in.i64();
      if (!value.ok()) return value.error();
      return CapabilityValue::integer(value.value());
    }
    case 3: {
      Result<double> value = in.f64();
      if (!value.ok()) return value.error();
      return CapabilityValue::number(value.value());
    }
    case 4: {
      Result<std::string> value = in.str();
      if (!value.ok()) return value.error();
      return CapabilityValue::text(value.take());
    }
    case 5: {
      Result<std::vector<std::string>> values = dec_text_vector(in, kMaxValueSetEntries);
      if (!values.ok()) return values.error();
      return CapabilityValue::text_set(values.take());
    }
    default:
      return Error(ErrorCode::ProtocolViolation, "encoded capability value kind is out of range");
  }
}

void encode_capability_entry(Encoder& out, const CapabilityEntry& value) {
  encode_capability_ref(out, value.key);
  encode_capability_value(out, value.value);
  enc_enum(out, value.precision);
  enc_enum(out, value.evidence_class);
  enc_gen(out, value.generation);
  out.str(value.note);
}

Result<CapabilityEntry> decode_capability_entry(Decoder& in) {
  CapabilityEntry value;
  Result<CapabilityRef> key = decode_capability_ref(in);
  if (!key.ok()) return key.error();
  value.key = key.take();
  Result<CapabilityValue> cap_value = decode_capability_value(in);
  if (!cap_value.ok()) return cap_value.error();
  value.value = cap_value.take();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<EvidenceGeneration> generation = dec_gen<EvidenceGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<std::string> note = in.str();
  if (!note.ok()) return note.error();
  value.note = note.take();
  return value;
}

void encode_capability_set(Encoder& out, const CapabilitySet& value) {
  out.u32(static_cast<std::uint32_t>(value.entries().size()));
  for (const CapabilityEntry& entry : value.entries()) {
    encode_capability_entry(out, entry);
  }
}

Result<CapabilitySet> decode_capability_set(Decoder& in, std::size_t limit) {
  Result<std::uint32_t> count = in.u32();
  if (!count.ok()) return count.error();
  if (count.value() > kMaxCapabilityEntriesPerPublication) {
    return Error(ErrorCode::BoundExceeded, "encoded capability set exceeds the permitted size");
  }
  CapabilitySet set(limit);
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    Result<CapabilityEntry> entry = decode_capability_entry(in);
    if (!entry.ok()) return entry.error();
    const Status s = set.put(entry.take());
    if (!s.ok()) return s.error();
  }
  return set;
}

void encode_capability_requirement(Encoder& out, const CapabilityRequirement& value) {
  encode_capability_ref(out, value.key);
  enc_enum(out, value.comparator);
  encode_capability_value(out, value.value);
  out.boolean(value.optional);
  out.str(value.rationale);
}

Result<CapabilityRequirement> decode_capability_requirement(Decoder& in) {
  CapabilityRequirement value;
  Result<CapabilityRef> key = decode_capability_ref(in);
  if (!key.ok()) return key.error();
  value.key = key.take();
  Result<CapabilityComparator> comparator =
      dec_enum<CapabilityComparator>(in, enum_max(CapabilityComparator{}), "capability comparator");
  if (!comparator.ok()) return comparator.error();
  value.comparator = comparator.value();
  Result<CapabilityValue> cap_value = decode_capability_value(in);
  if (!cap_value.ok()) return cap_value.error();
  value.value = cap_value.take();
  Result<bool> optional = in.boolean();
  if (!optional.ok()) return optional.error();
  value.optional = optional.value();
  Result<std::string> rationale = in.str();
  if (!rationale.ok()) return rationale.error();
  value.rationale = rationale.take();
  return value;
}

void encode_stamp(Encoder& out, const ObservationStamp& value) {
  enc_id(out, value.publisher);
  enc_gen(out, value.publisher_boot);
  enc_gen(out, value.coordinator_epoch);
  enc_gen(out, value.evidence_generation);
  enc_sequence(out, value.sequence);
  out.i64(value.observed_at);
  enc_enum(out, value.precision);
  enc_enum(out, value.evidence_class);
  enc_enum(out, value.provenance);
}

Result<ObservationStamp> decode_stamp(Decoder& in) {
  ObservationStamp value;
  Result<PublisherId> publisher = dec_id<PublisherIdTag>(in);
  if (!publisher.ok()) return publisher.error();
  value.publisher = publisher.take();
  Result<BootGeneration> boot = dec_gen<BootGenerationTag>(in);
  if (!boot.ok()) return boot.error();
  value.publisher_boot = boot.value();
  Result<CoordinatorEpoch> epoch = dec_gen<CoordinatorEpochTag>(in);
  if (!epoch.ok()) return epoch.error();
  value.coordinator_epoch = epoch.value();
  Result<EvidenceGeneration> evidence = dec_gen<EvidenceGenerationTag>(in);
  if (!evidence.ok()) return evidence.error();
  value.evidence_generation = evidence.value();
  Result<Sequence> sequence = dec_sequence(in);
  if (!sequence.ok()) return sequence.error();
  value.sequence = sequence.value();
  Result<std::int64_t> observed_at = in.i64();
  if (!observed_at.ok()) return observed_at.error();
  value.observed_at = observed_at.value();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<Provenance> provenance = dec_enum<Provenance>(in, enum_max(Provenance{}), "provenance");
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.value();
  return value;
}

void encode_ledger(Encoder& out, const CapacityLedger& value) {
  out.u64(value.nominal);
  out.u64(value.offline);
  out.u64(value.allocated);
  out.u64(value.reserved);
  out.u64(value.draining);
  out.u64(value.unusable);
  out.u64(value.idle);
}

Result<CapacityLedger> decode_ledger(Decoder& in) {
  CapacityLedger value;
  const std::uint64_t* fields[] = {&value.nominal, &value.offline, &value.allocated,
                                   &value.reserved, &value.draining, &value.unusable, &value.idle};
  for (const std::uint64_t* field : fields) {
    Result<std::uint64_t> raw = in.u64();
    if (!raw.ok()) return raw.error();
    *const_cast<std::uint64_t*>(field) = raw.value();
  }
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded capacity ledger does not close",
                 s.error().message());
  }
  return value;
}

void encode_capacity_pool(Encoder& out, const CapacityPool& value) {
  enc_id(out, value.pool_id);
  enc_enum(out, value.kind);
  enc_id(out, value.accelerator_class);
  out.str(value.custom_name);
  encode_ledger(out, value.ledger);
  enc_gen(out, value.generation);
  enc_enum(out, value.precision);
  enc_enum(out, value.evidence_class);
  encode_evidence_list(out, value.evidence);
}

Result<CapacityPool> decode_capacity_pool(Decoder& in) {
  CapacityPool value;
  Result<ResourcePoolId> pool_id = dec_id<ResourcePoolIdTag>(in);
  if (!pool_id.ok()) return pool_id.error();
  value.pool_id = pool_id.take();
  Result<ResourceKind> kind = dec_enum<ResourceKind>(in, enum_max(ResourceKind{}), "resource kind");
  if (!kind.ok()) return kind.error();
  value.kind = kind.value();
  Result<AcceleratorClassId> accelerator_class = dec_id<AcceleratorClassIdTag>(in);
  if (!accelerator_class.ok()) return accelerator_class.error();
  value.accelerator_class = accelerator_class.take();
  Result<std::string> custom_name = in.str();
  if (!custom_name.ok()) return custom_name.error();
  value.custom_name = custom_name.take();
  Result<CapacityLedger> ledger = decode_ledger(in);
  if (!ledger.ok()) return ledger.error();
  value.ledger = ledger.value();
  Result<CapacityGeneration> generation = dec_gen<CapacityGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  return value;
}

// --- Records ---------------------------------------------------------------

void encode_federation(Encoder& out, const FederationRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  out.str(value.display_name);
  enc_gen(out, value.coordinator_epoch);
  enc_vector<SiteId>(out, value.sites, [](Encoder& e, const SiteId& id) { enc_id(e, id); });
  enc_vector<ClusterId>(out, value.clusters, [](Encoder& e, const ClusterId& id) { enc_id(e, id); });
  enc_vector<AcceleratorClassId>(out, value.accelerator_classes,
                                 [](Encoder& e, const AcceleratorClassId& id) { enc_id(e, id); });
  enc_vector<RuntimeId>(out, value.runtimes, [](Encoder& e, const RuntimeId& id) { enc_id(e, id); });
  enc_vector<WorkloadClassId>(out, value.workload_classes,
                              [](Encoder& e, const WorkloadClassId& id) { enc_id(e, id); });
  enc_id(out, value.policy);
  enc_gen(out, value.policy_generation);
  enc_gen(out, value.compatibility_generation);
  enc_gen(out, value.capacity_generation);
  enc_gen(out, value.topology_generation);
  enc_gen(out, value.evidence_generation);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<FederationRecord> decode_federation(Decoder& in) {
  FederationRecord value;
  Result<FederationId> id = dec_id<FederationIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<FederationGeneration> generation = dec_gen<FederationGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<std::string> display_name = in.str();
  if (!display_name.ok()) return display_name.error();
  value.display_name = display_name.take();
  Result<CoordinatorEpoch> epoch = dec_gen<CoordinatorEpochTag>(in);
  if (!epoch.ok()) return epoch.error();
  value.coordinator_epoch = epoch.value();
  Result<std::vector<SiteId>> sites =
      dec_vector<SiteId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<SiteIdTag>(d); });
  if (!sites.ok()) return sites.error();
  value.sites = sites.take();
  Result<std::vector<ClusterId>> clusters =
      dec_vector<ClusterId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<ClusterIdTag>(d); });
  if (!clusters.ok()) return clusters.error();
  value.clusters = clusters.take();
  Result<std::vector<AcceleratorClassId>> classes = dec_vector<AcceleratorClassId>(
      in, kCollectionLimit, [](Decoder& d) { return dec_id<AcceleratorClassIdTag>(d); });
  if (!classes.ok()) return classes.error();
  value.accelerator_classes = classes.take();
  Result<std::vector<RuntimeId>> runtimes =
      dec_vector<RuntimeId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<RuntimeIdTag>(d); });
  if (!runtimes.ok()) return runtimes.error();
  value.runtimes = runtimes.take();
  Result<std::vector<WorkloadClassId>> workload_classes = dec_vector<WorkloadClassId>(
      in, kCollectionLimit, [](Decoder& d) { return dec_id<WorkloadClassIdTag>(d); });
  if (!workload_classes.ok()) return workload_classes.error();
  value.workload_classes = workload_classes.take();
  Result<PolicyId> policy = dec_id<PolicyIdTag>(in);
  if (!policy.ok()) return policy.error();
  value.policy = policy.take();
  Result<PolicyGeneration> policy_generation = dec_gen<PolicyGenerationTag>(in);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  Result<CompatibilityGeneration> compatibility = dec_gen<CompatibilityGenerationTag>(in);
  if (!compatibility.ok()) return compatibility.error();
  value.compatibility_generation = compatibility.value();
  Result<CapacityGeneration> capacity = dec_gen<CapacityGenerationTag>(in);
  if (!capacity.ok()) return capacity.error();
  value.capacity_generation = capacity.value();
  Result<TopologyGeneration> topology = dec_gen<TopologyGenerationTag>(in);
  if (!topology.ok()) return topology.error();
  value.topology_generation = topology.value();
  Result<EvidenceGeneration> evidence_generation = dec_gen<EvidenceGenerationTag>(in);
  if (!evidence_generation.ok()) return evidence_generation.error();
  value.evidence_generation = evidence_generation.value();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded federation is not valid", s.error().message());
  }
  return value;
}

void encode_site(Encoder& out, const SiteRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.federation);
  out.str(value.region);
  out.str(value.zone);
  out.str(value.failure_domain);
  enc_vector<ClusterId>(out, value.clusters, [](Encoder& e, const ClusterId& id) { enc_id(e, id); });
  enc_vector<PolicyId>(out, value.policies, [](Encoder& e, const PolicyId& id) { enc_id(e, id); });
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<SiteRecord> decode_site(Decoder& in) {
  SiteRecord value;
  Result<SiteId> id = dec_id<SiteIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<SiteGeneration> generation = dec_gen<SiteGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<std::string> region = in.str();
  if (!region.ok()) return region.error();
  value.region = region.take();
  Result<std::string> zone = in.str();
  if (!zone.ok()) return zone.error();
  value.zone = zone.take();
  Result<std::string> failure_domain = in.str();
  if (!failure_domain.ok()) return failure_domain.error();
  value.failure_domain = failure_domain.take();
  Result<std::vector<ClusterId>> clusters =
      dec_vector<ClusterId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<ClusterIdTag>(d); });
  if (!clusters.ok()) return clusters.error();
  value.clusters = clusters.take();
  Result<std::vector<PolicyId>> policies =
      dec_vector<PolicyId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<PolicyIdTag>(d); });
  if (!policies.ok()) return policies.error();
  value.policies = policies.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded site is not valid", s.error().message());
  }
  return value;
}

void encode_accelerator_class(Encoder& out, const AcceleratorClassRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.capability_generation);
  out.str(value.vendor);
  out.str(value.family);
  out.str(value.model);
  out.str(value.architecture);
  out.str(value.compute_capability);
  out.u64(value.memory_bytes_per_device);
  out.u32(value.memory_bandwidth_gbps);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<AcceleratorClassRecord> decode_accelerator_class(Decoder& in) {
  AcceleratorClassRecord value;
  Result<AcceleratorClassId> id = dec_id<AcceleratorClassIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<AcceleratorCapabilityGeneration> generation = dec_gen<AcceleratorCapabilityGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.capability_generation = generation.value();
  const std::string* texts[] = {&value.vendor, &value.family, &value.model, &value.architecture,
                                &value.compute_capability};
  for (const std::string* text : texts) {
    Result<std::string> read = in.str();
    if (!read.ok()) return read.error();
    *const_cast<std::string*>(text) = read.take();
  }
  Result<std::uint64_t> memory = in.u64();
  if (!memory.ok()) return memory.error();
  value.memory_bytes_per_device = memory.value();
  Result<std::uint32_t> bandwidth = in.u32();
  if (!bandwidth.ok()) return bandwidth.error();
  value.memory_bandwidth_gbps = bandwidth.value();
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded accelerator class is not valid",
                 s.error().message());
  }
  return value;
}

void encode_runtime(Encoder& out, const RuntimeRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.backend);
  enc_gen(out, value.backend_generation);
  enc_enum(out, value.kind);
  out.str(value.version);
  out.str(value.abi);
  out.str(value.driver_version);
  out.str(value.driver_abi);
  out.str(value.compiler_version);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<RuntimeRecord> decode_runtime(Decoder& in) {
  RuntimeRecord value;
  Result<RuntimeId> id = dec_id<RuntimeIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<RuntimeGeneration> generation = dec_gen<RuntimeGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<BackendId> backend = dec_id<BackendIdTag>(in);
  if (!backend.ok()) return backend.error();
  value.backend = backend.take();
  Result<BackendGeneration> backend_generation = dec_gen<BackendGenerationTag>(in);
  if (!backend_generation.ok()) return backend_generation.error();
  value.backend_generation = backend_generation.value();
  Result<RuntimeKind> kind = dec_enum<RuntimeKind>(in, enum_max(RuntimeKind{}), "runtime kind");
  if (!kind.ok()) return kind.error();
  value.kind = kind.value();
  const std::string* texts[] = {&value.version, &value.abi, &value.driver_version,
                                &value.driver_abi, &value.compiler_version};
  for (const std::string* text : texts) {
    Result<std::string> read = in.str();
    if (!read.ok()) return read.error();
    *const_cast<std::string*>(text) = read.take();
  }
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded runtime is not valid", s.error().message());
  }
  return value;
}

void encode_backend(Encoder& out, const BackendRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.runtime);
  out.str(value.name);
  out.str(value.version);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<BackendRecord> decode_backend(Decoder& in) {
  BackendRecord value;
  Result<BackendId> id = dec_id<BackendIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<BackendGeneration> generation = dec_gen<BackendGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<RuntimeId> runtime = dec_id<RuntimeIdTag>(in);
  if (!runtime.ok()) return runtime.error();
  value.runtime = runtime.take();
  const std::string* texts[] = {&value.name, &value.version};
  for (const std::string* text : texts) {
    Result<std::string> read = in.str();
    if (!read.ok()) return read.error();
    *const_cast<std::string*>(text) = read.take();
  }
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded backend is not valid", s.error().message());
  }
  return value;
}

void encode_domain(Encoder& out, const DomainRecord& value) {
  enc_id(out, value.id);
  enc_enum(out, value.kind);
  enc_id(out, value.federation);
  enc_id(out, value.site);
  enc_vector<ClusterId>(out, value.clusters, [](Encoder& e, const ClusterId& id) { enc_id(e, id); });
  enc_gen(out, value.topology_generation);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<DomainRecord> decode_domain(Decoder& in) {
  DomainRecord value;
  Result<DomainId> id = dec_id<DomainIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<DomainKind> kind = dec_enum<DomainKind>(in, enum_max(DomainKind{}), "domain kind");
  if (!kind.ok()) return kind.error();
  value.kind = kind.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<SiteId> site = dec_id<SiteIdTag>(in);
  if (!site.ok()) return site.error();
  value.site = site.take();
  Result<std::vector<ClusterId>> clusters =
      dec_vector<ClusterId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<ClusterIdTag>(d); });
  if (!clusters.ok()) return clusters.error();
  value.clusters = clusters.take();
  Result<TopologyGeneration> topology = dec_gen<TopologyGenerationTag>(in);
  if (!topology.ok()) return topology.error();
  value.topology_generation = topology.value();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded domain is not valid", s.error().message());
  }
  return value;
}

void encode_cluster(Encoder& out, const ClusterRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_gen(out, value.epoch);
  enc_id(out, value.federation);
  enc_id(out, value.site);
  enc_id(out, value.domain);
  out.str(value.failure_domain);
  out.str(value.zone);
  enc_vector<AcceleratorClassId>(out, value.accelerator_classes,
                                 [](Encoder& e, const AcceleratorClassId& id) { enc_id(e, id); });
  enc_vector<RuntimeId>(out, value.runtimes, [](Encoder& e, const RuntimeId& id) { enc_id(e, id); });
  enc_vector<BackendId>(out, value.backends, [](Encoder& e, const BackendId& id) { enc_id(e, id); });
  enc_vector<CapacityPool>(out, value.capacity_pools,
                           [](Encoder& e, const CapacityPool& pool) { encode_capacity_pool(e, pool); });
  enc_gen(out, value.topology_generation);
  enc_gen(out, value.capability_generation);
  enc_gen(out, value.capacity_generation);
  enc_gen(out, value.runtime_generation);
  enc_gen(out, value.compatibility_generation);
  enc_gen(out, value.evidence_generation);
  enc_enum(out, value.readiness);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<ClusterRecord> decode_cluster(Decoder& in) {
  ClusterRecord value;
  Result<ClusterId> id = dec_id<ClusterIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<ClusterGeneration> generation = dec_gen<ClusterGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<ClusterEpoch> epoch = dec_gen<ClusterEpochTag>(in);
  if (!epoch.ok()) return epoch.error();
  value.epoch = epoch.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<SiteId> site = dec_id<SiteIdTag>(in);
  if (!site.ok()) return site.error();
  value.site = site.take();
  Result<DomainId> domain = dec_id<DomainIdTag>(in);
  if (!domain.ok()) return domain.error();
  value.domain = domain.take();
  Result<std::string> failure_domain = in.str();
  if (!failure_domain.ok()) return failure_domain.error();
  value.failure_domain = failure_domain.take();
  Result<std::string> zone = in.str();
  if (!zone.ok()) return zone.error();
  value.zone = zone.take();
  Result<std::vector<AcceleratorClassId>> classes = dec_vector<AcceleratorClassId>(
      in, kCollectionLimit, [](Decoder& d) { return dec_id<AcceleratorClassIdTag>(d); });
  if (!classes.ok()) return classes.error();
  value.accelerator_classes = classes.take();
  Result<std::vector<RuntimeId>> runtimes =
      dec_vector<RuntimeId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<RuntimeIdTag>(d); });
  if (!runtimes.ok()) return runtimes.error();
  value.runtimes = runtimes.take();
  Result<std::vector<BackendId>> backends =
      dec_vector<BackendId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<BackendIdTag>(d); });
  if (!backends.ok()) return backends.error();
  value.backends = backends.take();
  Result<std::vector<CapacityPool>> pools = dec_vector<CapacityPool>(
      in, static_cast<std::uint32_t>(bounds::kMaxPoolsPerCluster),
      [](Decoder& d) { return decode_capacity_pool(d); });
  if (!pools.ok()) return pools.error();
  value.capacity_pools = pools.take();
  Result<TopologyGeneration> topology = dec_gen<TopologyGenerationTag>(in);
  if (!topology.ok()) return topology.error();
  value.topology_generation = topology.value();
  Result<AcceleratorCapabilityGeneration> capability = dec_gen<AcceleratorCapabilityGenerationTag>(in);
  if (!capability.ok()) return capability.error();
  value.capability_generation = capability.value();
  Result<CapacityGeneration> capacity = dec_gen<CapacityGenerationTag>(in);
  if (!capacity.ok()) return capacity.error();
  value.capacity_generation = capacity.value();
  Result<RuntimeGeneration> runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!runtime_generation.ok()) return runtime_generation.error();
  value.runtime_generation = runtime_generation.value();
  Result<CompatibilityGeneration> compatibility = dec_gen<CompatibilityGenerationTag>(in);
  if (!compatibility.ok()) return compatibility.error();
  value.compatibility_generation = compatibility.value();
  Result<EvidenceGeneration> evidence_generation = dec_gen<EvidenceGenerationTag>(in);
  if (!evidence_generation.ok()) return evidence_generation.error();
  value.evidence_generation = evidence_generation.value();
  Result<Readiness> readiness = dec_enum<Readiness>(in, enum_max(Readiness{}), "readiness");
  if (!readiness.ok()) return readiness.error();
  value.readiness = readiness.value();
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded cluster is not valid", s.error().message());
  }
  return value;
}

void encode_policy(Encoder& out, const PolicyRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  out.str(value.name);
  enc_vector<SiteId>(out, value.allowed_sites, [](Encoder& e, const SiteId& id) { enc_id(e, id); });
  enc_vector<SiteId>(out, value.denied_sites, [](Encoder& e, const SiteId& id) { enc_id(e, id); });
  enc_vector<ClusterId>(out, value.allowed_clusters,
                        [](Encoder& e, const ClusterId& id) { enc_id(e, id); });
  enc_vector<ClusterId>(out, value.denied_clusters,
                        [](Encoder& e, const ClusterId& id) { enc_id(e, id); });
  enc_enum(out, value.allow_cross_site);
  out.str(value.data_residency);
  out.str(value.isolation_requirement);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<PolicyRecord> decode_policy(Decoder& in) {
  PolicyRecord value;
  Result<PolicyId> id = dec_id<PolicyIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<PolicyGeneration> generation = dec_gen<PolicyGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<std::string> name = in.str();
  if (!name.ok()) return name.error();
  value.name = name.take();
  Result<std::vector<SiteId>> allowed_sites =
      dec_vector<SiteId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<SiteIdTag>(d); });
  if (!allowed_sites.ok()) return allowed_sites.error();
  value.allowed_sites = allowed_sites.take();
  Result<std::vector<SiteId>> denied_sites =
      dec_vector<SiteId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<SiteIdTag>(d); });
  if (!denied_sites.ok()) return denied_sites.error();
  value.denied_sites = denied_sites.take();
  Result<std::vector<ClusterId>> allowed_clusters =
      dec_vector<ClusterId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<ClusterIdTag>(d); });
  if (!allowed_clusters.ok()) return allowed_clusters.error();
  value.allowed_clusters = allowed_clusters.take();
  Result<std::vector<ClusterId>> denied_clusters =
      dec_vector<ClusterId>(in, kCollectionLimit, [](Decoder& d) { return dec_id<ClusterIdTag>(d); });
  if (!denied_clusters.ok()) return denied_clusters.error();
  value.denied_clusters = denied_clusters.take();
  Result<Tri> cross_site = dec_enum<Tri>(in, enum_max(Tri{}), "tri-state");
  if (!cross_site.ok()) return cross_site.error();
  value.allow_cross_site = cross_site.value();
  Result<std::string> residency = in.str();
  if (!residency.ok()) return residency.error();
  value.data_residency = residency.take();
  Result<std::string> isolation = in.str();
  if (!isolation.ok()) return isolation.error();
  value.isolation_requirement = isolation.take();
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded policy is not valid", s.error().message());
  }
  return value;
}

void encode_artifact(Encoder& out, const ArtifactRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_enum(out, value.kind);
  out.str(value.format);
  out.str(value.kernel_format);
  enc_text_vector(out, value.target_architectures);
  out.str(value.minimum_compute_capability);
  out.str(value.runtime_abi);
  out.str(value.driver_abi);
  out.str(value.compiler_target);
  out.str(value.state_format);
  // Scalars follow the string block so that the encoder and the decoder traverse the
  // record in exactly the same order. A codec round-trip test guards this.
  out.u64(value.size_bytes);
  out.u64(value.required_memory_bytes);
  encode_capability_set(out, value.capabilities);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<ArtifactRecord> decode_artifact(Decoder& in) {
  ArtifactRecord value;
  Result<ArtifactId> id = dec_id<ArtifactIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<ArtifactGeneration> generation = dec_gen<ArtifactGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<ArtifactKind> kind = dec_enum<ArtifactKind>(in, enum_max(ArtifactKind{}), "artifact kind");
  if (!kind.ok()) return kind.error();
  value.kind = kind.value();
  const std::string* texts[] = {&value.format, &value.kernel_format};
  for (const std::string* text : texts) {
    Result<std::string> read = in.str();
    if (!read.ok()) return read.error();
    *const_cast<std::string*>(text) = read.take();
  }
  Result<std::vector<std::string>> architectures = dec_text_vector(in, 64);
  if (!architectures.ok()) return architectures.error();
  value.target_architectures = architectures.take();
  const std::string* more_texts[] = {&value.minimum_compute_capability, &value.runtime_abi,
                                     &value.driver_abi, &value.compiler_target, &value.state_format};
  for (const std::string* text : more_texts) {
    Result<std::string> read = in.str();
    if (!read.ok()) return read.error();
    *const_cast<std::string*>(text) = read.take();
  }
  Result<std::uint64_t> size = in.u64();
  if (!size.ok()) return size.error();
  value.size_bytes = size.value();
  Result<std::uint64_t> required_memory = in.u64();
  if (!required_memory.ok()) return required_memory.error();
  value.required_memory_bytes = required_memory.value();
  Result<CapabilitySet> capabilities = decode_capability_set(in, kMaxCapabilityEntriesPerPublication);
  if (!capabilities.ok()) return capabilities.error();
  value.capabilities = capabilities.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded artifact is not valid", s.error().message());
  }
  return value;
}

void encode_workload_class(Encoder& out, const WorkloadClassRecord& value) {
  enc_id(out, value.id);
  out.str(value.display_name);
  out.boolean(value.prefers_accelerators);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<WorkloadClassRecord> decode_workload_class(Decoder& in) {
  WorkloadClassRecord value;
  Result<WorkloadClassId> id = dec_id<WorkloadClassIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<std::string> display_name = in.str();
  if (!display_name.ok()) return display_name.error();
  value.display_name = display_name.take();
  Result<bool> prefers = in.boolean();
  if (!prefers.ok()) return prefers.error();
  value.prefers_accelerators = prefers.value();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded workload class is not valid",
                 s.error().message());
  }
  return value;
}

void encode_workload(Encoder& out, const WorkloadRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.workload_class);
  enc_id(out, value.artifact);
  enc_gen(out, value.artifact_generation);
  out.u32(value.required_accelerators);
  out.u64(value.required_memory_bytes_per_accelerator);
  enc_vector<AcceleratorClassId>(out, value.acceptable_accelerator_classes,
                                 [](Encoder& e, const AcceleratorClassId& id) { enc_id(e, id); });
  out.u32(static_cast<std::uint32_t>(value.requirements.size()));
  for (const CapabilityRequirement& requirement : value.requirements) {
    encode_capability_requirement(out, requirement);
  }
  enc_id(out, value.policy);
  enc_gen(out, value.policy_generation);
  enc_id(out, value.required_domain);
  enc_enum(out, value.required_domain_kind);
  out.boolean(value.require_single_failure_domain);
  out.str(value.isolation_requirement);
  out.str(value.portability_class);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<WorkloadRecord> decode_workload(Decoder& in) {
  WorkloadRecord value;
  Result<WorkloadId> id = dec_id<WorkloadIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<WorkloadGeneration> generation = dec_gen<WorkloadGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<WorkloadClassId> workload_class = dec_id<WorkloadClassIdTag>(in);
  if (!workload_class.ok()) return workload_class.error();
  value.workload_class = workload_class.take();
  Result<ArtifactId> artifact = dec_id<ArtifactIdTag>(in);
  if (!artifact.ok()) return artifact.error();
  value.artifact = artifact.take();
  Result<ArtifactGeneration> artifact_generation = dec_gen<ArtifactGenerationTag>(in);
  if (!artifact_generation.ok()) return artifact_generation.error();
  value.artifact_generation = artifact_generation.value();
  Result<std::uint32_t> accelerators = in.u32();
  if (!accelerators.ok()) return accelerators.error();
  value.required_accelerators = accelerators.value();
  Result<std::uint64_t> memory = in.u64();
  if (!memory.ok()) return memory.error();
  value.required_memory_bytes_per_accelerator = memory.value();
  Result<std::vector<AcceleratorClassId>> classes = dec_vector<AcceleratorClassId>(
      in, kCollectionLimit, [](Decoder& d) { return dec_id<AcceleratorClassIdTag>(d); });
  if (!classes.ok()) return classes.error();
  value.acceptable_accelerator_classes = classes.take();
  Result<std::uint32_t> requirement_count = in.u32();
  if (!requirement_count.ok()) return requirement_count.error();
  if (requirement_count.value() > kMaxCapabilityEntriesPerPublication) {
    return Error(ErrorCode::BoundExceeded, "encoded requirement list exceeds the permitted size");
  }
  for (std::uint32_t i = 0; i < requirement_count.value(); ++i) {
    Result<CapabilityRequirement> requirement = decode_capability_requirement(in);
    if (!requirement.ok()) return requirement.error();
    value.requirements.push_back(requirement.take());
  }
  Result<PolicyId> policy = dec_id<PolicyIdTag>(in);
  if (!policy.ok()) return policy.error();
  value.policy = policy.take();
  Result<PolicyGeneration> policy_generation = dec_gen<PolicyGenerationTag>(in);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  Result<DomainId> domain = dec_id<DomainIdTag>(in);
  if (!domain.ok()) return domain.error();
  value.required_domain = domain.take();
  Result<DomainKind> domain_kind = dec_enum<DomainKind>(in, enum_max(DomainKind{}), "domain kind");
  if (!domain_kind.ok()) return domain_kind.error();
  value.required_domain_kind = domain_kind.value();
  Result<bool> single_failure_domain = in.boolean();
  if (!single_failure_domain.ok()) return single_failure_domain.error();
  value.require_single_failure_domain = single_failure_domain.value();
  Result<std::string> isolation = in.str();
  if (!isolation.ok()) return isolation.error();
  value.isolation_requirement = isolation.take();
  Result<std::string> portability_class = in.str();
  if (!portability_class.ok()) return portability_class.error();
  value.portability_class = portability_class.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded workload is not valid", s.error().message());
  }
  return value;
}

void encode_candidate(Encoder& out, const CandidateObservation& value) {
  enc_id(out, value.cluster);
  enc_gen(out, value.cluster_generation);
  enc_gen(out, value.cluster_epoch);
  enc_id(out, value.accelerator_class);
  enc_enum(out, value.status);
  out.u32(value.reasons);
  enc_enum(out, value.basis);
  enc_enum(out, value.precision);
  enc_enum(out, value.evidence_class);
  out.str(value.detail);
  encode_evidence_list(out, value.evidence);
}

Result<CandidateObservation> decode_candidate(Decoder& in) {
  CandidateObservation value;
  Result<ClusterId> cluster = dec_id<ClusterIdTag>(in);
  if (!cluster.ok()) return cluster.error();
  value.cluster = cluster.take();
  Result<ClusterGeneration> generation = dec_gen<ClusterGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.cluster_generation = generation.value();
  Result<ClusterEpoch> epoch = dec_gen<ClusterEpochTag>(in);
  if (!epoch.ok()) return epoch.error();
  value.cluster_epoch = epoch.value();
  Result<AcceleratorClassId> accelerator_class = dec_id<AcceleratorClassIdTag>(in);
  if (!accelerator_class.ok()) return accelerator_class.error();
  value.accelerator_class = accelerator_class.take();
  Result<CandidateStatus> status =
      dec_enum<CandidateStatus>(in, enum_max(CandidateStatus{}), "candidate status");
  if (!status.ok()) return status.error();
  value.status = status.value();
  Result<std::uint32_t> reasons = in.u32();
  if (!reasons.ok()) return reasons.error();
  const RejectionMask valid_mask =
      (kRejectionReasonCount >= 32) ? 0xFFFFFFFFu : ((1u << kRejectionReasonCount) - 1u);
  if ((reasons.value() & ~valid_mask) != 0) {
    return Error(ErrorCode::ProtocolViolation, "encoded rejection mask has unknown bits set");
  }
  value.reasons = reasons.value();
  Result<ReasonBasis> basis = dec_enum<ReasonBasis>(in, enum_max(ReasonBasis{}), "reason basis");
  if (!basis.ok()) return basis.error();
  value.basis = basis.value();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<std::string> detail = in.str();
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  return value;
}

void encode_placement(Encoder& out, const PlacementRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.workload);
  enc_gen(out, value.workload_generation);
  enc_id(out, value.federation);
  enc_gen(out, value.federation_generation);
  enc_id(out, value.workload_class);
  enc_id(out, value.selected);
  enc_gen(out, value.selected_generation);
  enc_gen(out, value.selected_epoch);
  enc_id(out, value.selected_accelerator_class);
  out.u32(value.selected_accelerator_count);
  out.u64(value.selected_memory_bytes);
  enc_vector<CandidateObservation>(out, value.candidates,
                                   [](Encoder& e, const CandidateObservation& c) {
                                     encode_candidate(e, c);
                                   });
  enc_enum(out, value.candidate_completeness);
  enc_gen(out, value.policy_generation);
  enc_gen(out, value.capacity_generation);
  enc_gen(out, value.topology_generation);
  enc_gen(out, value.compatibility_generation);
  enc_gen(out, value.runtime_generation);
  out.boolean(value.capacity_available);
  enc_enum(out, value.compatibility_constrained);
  enc_enum(out, value.topology_constrained);
  enc_enum(out, value.policy_constrained);
  enc_enum(out, value.capability_constrained);
  enc_enum(out, value.affinity_constrained);
  out.boolean(value.fallback_required);
  out.str(value.fallback_detail);
  out.boolean(value.cost_known);
  out.f64(value.cost_estimate);
  out.str(value.slo_class);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<PlacementRecord> decode_placement(Decoder& in) {
  PlacementRecord value;
  Result<PlacementId> id = dec_id<PlacementIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<PlacementGeneration> generation = dec_gen<PlacementGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<WorkloadId> workload = dec_id<WorkloadIdTag>(in);
  if (!workload.ok()) return workload.error();
  value.workload = workload.take();
  Result<WorkloadGeneration> workload_generation = dec_gen<WorkloadGenerationTag>(in);
  if (!workload_generation.ok()) return workload_generation.error();
  value.workload_generation = workload_generation.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<FederationGeneration> federation_generation = dec_gen<FederationGenerationTag>(in);
  if (!federation_generation.ok()) return federation_generation.error();
  value.federation_generation = federation_generation.value();
  Result<WorkloadClassId> workload_class = dec_id<WorkloadClassIdTag>(in);
  if (!workload_class.ok()) return workload_class.error();
  value.workload_class = workload_class.take();
  Result<ClusterId> selected = dec_id<ClusterIdTag>(in);
  if (!selected.ok()) return selected.error();
  value.selected = selected.take();
  Result<ClusterGeneration> selected_generation = dec_gen<ClusterGenerationTag>(in);
  if (!selected_generation.ok()) return selected_generation.error();
  value.selected_generation = selected_generation.value();
  Result<ClusterEpoch> selected_epoch = dec_gen<ClusterEpochTag>(in);
  if (!selected_epoch.ok()) return selected_epoch.error();
  value.selected_epoch = selected_epoch.value();
  Result<AcceleratorClassId> selected_class = dec_id<AcceleratorClassIdTag>(in);
  if (!selected_class.ok()) return selected_class.error();
  value.selected_accelerator_class = selected_class.take();
  Result<std::uint32_t> count = in.u32();
  if (!count.ok()) return count.error();
  value.selected_accelerator_count = count.value();
  Result<std::uint64_t> memory = in.u64();
  if (!memory.ok()) return memory.error();
  value.selected_memory_bytes = memory.value();
  Result<std::vector<CandidateObservation>> candidates = dec_vector<CandidateObservation>(
      in, static_cast<std::uint32_t>(kMaxCandidatesPerPlacement),
      [](Decoder& d) { return decode_candidate(d); });
  if (!candidates.ok()) return candidates.error();
  value.candidates = candidates.take();
  Result<CandidateSetCompleteness> completeness =
      dec_enum<CandidateSetCompleteness>(in, enum_max(CandidateSetCompleteness{}),
                                         "candidate set completeness");
  if (!completeness.ok()) return completeness.error();
  value.candidate_completeness = completeness.value();
  Result<PolicyGeneration> policy_generation = dec_gen<PolicyGenerationTag>(in);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  Result<CapacityGeneration> capacity_generation = dec_gen<CapacityGenerationTag>(in);
  if (!capacity_generation.ok()) return capacity_generation.error();
  value.capacity_generation = capacity_generation.value();
  Result<TopologyGeneration> topology_generation = dec_gen<TopologyGenerationTag>(in);
  if (!topology_generation.ok()) return topology_generation.error();
  value.topology_generation = topology_generation.value();
  Result<CompatibilityGeneration> compatibility_generation = dec_gen<CompatibilityGenerationTag>(in);
  if (!compatibility_generation.ok()) return compatibility_generation.error();
  value.compatibility_generation = compatibility_generation.value();
  Result<RuntimeGeneration> runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!runtime_generation.ok()) return runtime_generation.error();
  value.runtime_generation = runtime_generation.value();
  Result<bool> capacity_available = in.boolean();
  if (!capacity_available.ok()) return capacity_available.error();
  value.capacity_available = capacity_available.value();
  Tri* tri_fields[] = {&value.compatibility_constrained, &value.topology_constrained,
                       &value.policy_constrained, &value.capability_constrained,
                       &value.affinity_constrained};
  for (Tri* field : tri_fields) {
    Result<Tri> tri = dec_enum<Tri>(in, enum_max(Tri{}), "tri-state");
    if (!tri.ok()) return tri.error();
    *field = tri.value();
  }
  Result<bool> fallback_required = in.boolean();
  if (!fallback_required.ok()) return fallback_required.error();
  value.fallback_required = fallback_required.value();
  Result<std::string> fallback_detail = in.str();
  if (!fallback_detail.ok()) return fallback_detail.error();
  value.fallback_detail = fallback_detail.take();
  Result<bool> cost_known = in.boolean();
  if (!cost_known.ok()) return cost_known.error();
  value.cost_known = cost_known.value();
  Result<double> cost = in.f64();
  if (!cost.ok()) return cost.error();
  value.cost_estimate = cost.value();
  Result<std::string> slo_class = in.str();
  if (!slo_class.ok()) return slo_class.error();
  value.slo_class = slo_class.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded placement is not valid", s.error().message());
  }
  return value;
}

void encode_migration(Encoder& out, const MigrationRecord& value) {
  enc_id(out, value.id);
  enc_gen(out, value.generation);
  enc_id(out, value.workload);
  enc_gen(out, value.workload_generation);
  enc_id(out, value.federation);
  enc_gen(out, value.federation_generation);
  enc_id(out, value.workload_class);
  enc_id(out, value.supersedes);
  enc_gen(out, value.supersedes_generation);
  enc_id(out, value.source);
  enc_gen(out, value.source_generation);
  enc_gen(out, value.source_epoch);
  enc_id(out, value.destination);
  enc_gen(out, value.destination_generation);
  enc_gen(out, value.destination_epoch);
  enc_id(out, value.artifact);
  enc_gen(out, value.artifact_generation);
  enc_id(out, value.source_runtime);
  enc_gen(out, value.source_runtime_generation);
  enc_id(out, value.destination_runtime);
  enc_gen(out, value.destination_runtime_generation);
  enc_gen(out, value.compatibility_generation);
  enc_gen(out, value.policy_generation);
  enc_gen(out, value.capacity_generation);
  enc_gen(out, value.topology_generation);
  enc_enum(out, value.stage);
  enc_enum(out, value.outcome);
  out.u32(static_cast<std::uint32_t>(value.stage_events.size()));
  for (const MigrationStageEvent& event : value.stage_events) {
    enc_gen(out, event.generation);
    enc_enum(out, event.stage);
    enc_sequence(out, event.sequence);
    out.i64(event.observed_at);
    enc_enum(out, event.precision);
    enc_enum(out, event.evidence_class);
    out.str(event.detail);
  }
  out.str(value.reason);
  out.boolean(value.reason_observed);
  out.boolean(value.state_transfer_known);
  out.u64(value.state_transfer_bytes);
  out.boolean(value.downtime_known);
  out.u64(value.downtime_micros);
  out.boolean(value.requires_rebuild);
  out.boolean(value.requires_recompile);
  out.boolean(value.requires_conversion);
  out.boolean(value.requires_state_translation);
  out.boolean(value.revalidation_pending);
  out.boolean(value.destination_generation_changed);
  out.boolean(value.changed_effective_capability);
  out.boolean(value.changed_slo);
  out.str(value.fallback_detail);
  enc_enum(out, value.currentness);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<MigrationRecord> decode_migration(Decoder& in) {
  MigrationRecord value;
  Result<MigrationId> id = dec_id<MigrationIdTag>(in);
  if (!id.ok()) return id.error();
  value.id = id.take();
  Result<MigrationGeneration> generation = dec_gen<MigrationGenerationTag>(in);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  Result<WorkloadId> workload = dec_id<WorkloadIdTag>(in);
  if (!workload.ok()) return workload.error();
  value.workload = workload.take();
  Result<WorkloadGeneration> workload_generation = dec_gen<WorkloadGenerationTag>(in);
  if (!workload_generation.ok()) return workload_generation.error();
  value.workload_generation = workload_generation.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<FederationGeneration> federation_generation = dec_gen<FederationGenerationTag>(in);
  if (!federation_generation.ok()) return federation_generation.error();
  value.federation_generation = federation_generation.value();
  Result<WorkloadClassId> workload_class = dec_id<WorkloadClassIdTag>(in);
  if (!workload_class.ok()) return workload_class.error();
  value.workload_class = workload_class.take();
  Result<MigrationId> supersedes = dec_id<MigrationIdTag>(in);
  if (!supersedes.ok()) return supersedes.error();
  value.supersedes = supersedes.take();
  Result<MigrationGeneration> supersedes_generation = dec_gen<MigrationGenerationTag>(in);
  if (!supersedes_generation.ok()) return supersedes_generation.error();
  value.supersedes_generation = supersedes_generation.value();
  Result<ClusterId> source = dec_id<ClusterIdTag>(in);
  if (!source.ok()) return source.error();
  value.source = source.take();
  Result<ClusterGeneration> source_generation = dec_gen<ClusterGenerationTag>(in);
  if (!source_generation.ok()) return source_generation.error();
  value.source_generation = source_generation.value();
  Result<ClusterEpoch> source_epoch = dec_gen<ClusterEpochTag>(in);
  if (!source_epoch.ok()) return source_epoch.error();
  value.source_epoch = source_epoch.value();
  Result<ClusterId> destination = dec_id<ClusterIdTag>(in);
  if (!destination.ok()) return destination.error();
  value.destination = destination.take();
  Result<ClusterGeneration> destination_generation = dec_gen<ClusterGenerationTag>(in);
  if (!destination_generation.ok()) return destination_generation.error();
  value.destination_generation = destination_generation.value();
  Result<ClusterEpoch> destination_epoch = dec_gen<ClusterEpochTag>(in);
  if (!destination_epoch.ok()) return destination_epoch.error();
  value.destination_epoch = destination_epoch.value();
  Result<ArtifactId> artifact = dec_id<ArtifactIdTag>(in);
  if (!artifact.ok()) return artifact.error();
  value.artifact = artifact.take();
  Result<ArtifactGeneration> artifact_generation = dec_gen<ArtifactGenerationTag>(in);
  if (!artifact_generation.ok()) return artifact_generation.error();
  value.artifact_generation = artifact_generation.value();
  Result<RuntimeId> source_runtime = dec_id<RuntimeIdTag>(in);
  if (!source_runtime.ok()) return source_runtime.error();
  value.source_runtime = source_runtime.take();
  Result<RuntimeGeneration> source_runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!source_runtime_generation.ok()) return source_runtime_generation.error();
  value.source_runtime_generation = source_runtime_generation.value();
  Result<RuntimeId> destination_runtime = dec_id<RuntimeIdTag>(in);
  if (!destination_runtime.ok()) return destination_runtime.error();
  value.destination_runtime = destination_runtime.take();
  Result<RuntimeGeneration> destination_runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!destination_runtime_generation.ok()) return destination_runtime_generation.error();
  value.destination_runtime_generation = destination_runtime_generation.value();
  Result<CompatibilityGeneration> compatibility_generation = dec_gen<CompatibilityGenerationTag>(in);
  if (!compatibility_generation.ok()) return compatibility_generation.error();
  value.compatibility_generation = compatibility_generation.value();
  Result<PolicyGeneration> policy_generation = dec_gen<PolicyGenerationTag>(in);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  Result<CapacityGeneration> capacity_generation = dec_gen<CapacityGenerationTag>(in);
  if (!capacity_generation.ok()) return capacity_generation.error();
  value.capacity_generation = capacity_generation.value();
  Result<TopologyGeneration> topology_generation = dec_gen<TopologyGenerationTag>(in);
  if (!topology_generation.ok()) return topology_generation.error();
  value.topology_generation = topology_generation.value();
  Result<MigrationStage> stage =
      dec_enum<MigrationStage>(in, enum_max(MigrationStage{}), "migration stage");
  if (!stage.ok()) return stage.error();
  value.stage = stage.value();
  Result<MigrationOutcome> outcome =
      dec_enum<MigrationOutcome>(in, enum_max(MigrationOutcome{}), "migration outcome");
  if (!outcome.ok()) return outcome.error();
  value.outcome = outcome.value();
  Result<std::uint32_t> event_count = in.u32();
  if (!event_count.ok()) return event_count.error();
  if (event_count.value() > kMaxStageEventsPerMigration) {
    return Error(ErrorCode::BoundExceeded, "encoded migration has too many stage events");
  }
  for (std::uint32_t i = 0; i < event_count.value(); ++i) {
    MigrationStageEvent event;
    Result<MigrationGeneration> event_generation = dec_gen<MigrationGenerationTag>(in);
    if (!event_generation.ok()) return event_generation.error();
    event.generation = event_generation.value();
    Result<MigrationStage> event_stage =
        dec_enum<MigrationStage>(in, enum_max(MigrationStage{}), "migration stage");
    if (!event_stage.ok()) return event_stage.error();
    event.stage = event_stage.value();
    Result<Sequence> sequence = dec_sequence(in);
    if (!sequence.ok()) return sequence.error();
    event.sequence = sequence.value();
    Result<std::int64_t> observed_at = in.i64();
    if (!observed_at.ok()) return observed_at.error();
    event.observed_at = observed_at.value();
    Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
    if (!precision.ok()) return precision.error();
    event.precision = precision.value();
    Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
    if (!cls.ok()) return cls.error();
    event.evidence_class = cls.value();
    Result<std::string> detail = in.str();
    if (!detail.ok()) return detail.error();
    event.detail = detail.take();
    value.stage_events.push_back(std::move(event));
  }
  Result<std::string> reason = in.str();
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  Result<bool> reason_observed = in.boolean();
  if (!reason_observed.ok()) return reason_observed.error();
  value.reason_observed = reason_observed.value();
  Result<bool> transfer_known = in.boolean();
  if (!transfer_known.ok()) return transfer_known.error();
  value.state_transfer_known = transfer_known.value();
  Result<std::uint64_t> transfer_bytes = in.u64();
  if (!transfer_bytes.ok()) return transfer_bytes.error();
  value.state_transfer_bytes = transfer_bytes.value();
  Result<bool> downtime_known = in.boolean();
  if (!downtime_known.ok()) return downtime_known.error();
  value.downtime_known = downtime_known.value();
  Result<std::uint64_t> downtime = in.u64();
  if (!downtime.ok()) return downtime.error();
  value.downtime_micros = downtime.value();
  bool* flags[] = {&value.requires_rebuild,     &value.requires_recompile,
                   &value.requires_conversion,  &value.requires_state_translation,
                   &value.revalidation_pending, &value.destination_generation_changed,
                   &value.changed_effective_capability, &value.changed_slo};
  for (bool* flag : flags) {
    Result<bool> read = in.boolean();
    if (!read.ok()) return read.error();
    *flag = read.value();
  }
  Result<std::string> fallback_detail = in.str();
  if (!fallback_detail.ok()) return fallback_detail.error();
  value.fallback_detail = fallback_detail.take();
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded migration is not valid", s.error().message());
  }
  return value;
}

void encode_portability(Encoder& out, const PortabilityAssessment& value) {
  enc_id(out, value.workload);
  enc_gen(out, value.workload_generation);
  enc_id(out, value.artifact);
  enc_gen(out, value.artifact_generation);
  enc_id(out, value.workload_class);
  enc_id(out, value.source);
  enc_gen(out, value.source_generation);
  enc_gen(out, value.source_epoch);
  enc_id(out, value.destination);
  enc_gen(out, value.destination_generation);
  enc_gen(out, value.destination_epoch);
  enc_id(out, value.source_runtime);
  enc_gen(out, value.source_runtime_generation);
  enc_id(out, value.destination_runtime);
  enc_gen(out, value.destination_runtime_generation);
  enc_id(out, value.federation);
  enc_gen(out, value.federation_generation);
  enc_gen(out, value.compatibility_generation);
  enc_gen(out, value.policy_generation);
  out.u32(static_cast<std::uint32_t>(value.dimensions.size()));
  for (const PortabilityDimensionResult& result : value.dimensions) {
    enc_enum(out, result.dimension);
    enc_enum(out, result.outcome);
    out.str(result.detail);
    enc_enum(out, result.basis);
    enc_enum(out, result.precision);
    enc_enum(out, result.evidence_class);
    out.boolean(result.constrains_overall);
    encode_evidence_list(out, result.evidence);
  }
  enc_enum(out, value.overall);
  out.boolean(value.requires_rebuild);
  out.boolean(value.requires_recompile);
  out.boolean(value.requires_conversion);
  out.boolean(value.requires_state_translation);
  out.boolean(value.requires_fallback);
  out.boolean(value.technically_blocked);
  out.boolean(value.policy_blocked);
  enc_enum(out, value.currentness);
  enc_enum(out, value.precision);
  enc_enum(out, value.evidence_class);
  encode_stamp(out, value.stamp);
  encode_evidence_list(out, value.evidence);
}

Result<PortabilityAssessment> decode_portability(Decoder& in) {
  PortabilityAssessment value;
  Result<WorkloadId> workload = dec_id<WorkloadIdTag>(in);
  if (!workload.ok()) return workload.error();
  value.workload = workload.take();
  Result<WorkloadGeneration> workload_generation = dec_gen<WorkloadGenerationTag>(in);
  if (!workload_generation.ok()) return workload_generation.error();
  value.workload_generation = workload_generation.value();
  Result<ArtifactId> artifact = dec_id<ArtifactIdTag>(in);
  if (!artifact.ok()) return artifact.error();
  value.artifact = artifact.take();
  Result<ArtifactGeneration> artifact_generation = dec_gen<ArtifactGenerationTag>(in);
  if (!artifact_generation.ok()) return artifact_generation.error();
  value.artifact_generation = artifact_generation.value();
  Result<WorkloadClassId> workload_class = dec_id<WorkloadClassIdTag>(in);
  if (!workload_class.ok()) return workload_class.error();
  value.workload_class = workload_class.take();
  Result<ClusterId> source = dec_id<ClusterIdTag>(in);
  if (!source.ok()) return source.error();
  value.source = source.take();
  Result<ClusterGeneration> source_generation = dec_gen<ClusterGenerationTag>(in);
  if (!source_generation.ok()) return source_generation.error();
  value.source_generation = source_generation.value();
  Result<ClusterEpoch> source_epoch = dec_gen<ClusterEpochTag>(in);
  if (!source_epoch.ok()) return source_epoch.error();
  value.source_epoch = source_epoch.value();
  Result<ClusterId> destination = dec_id<ClusterIdTag>(in);
  if (!destination.ok()) return destination.error();
  value.destination = destination.take();
  Result<ClusterGeneration> destination_generation = dec_gen<ClusterGenerationTag>(in);
  if (!destination_generation.ok()) return destination_generation.error();
  value.destination_generation = destination_generation.value();
  Result<ClusterEpoch> destination_epoch = dec_gen<ClusterEpochTag>(in);
  if (!destination_epoch.ok()) return destination_epoch.error();
  value.destination_epoch = destination_epoch.value();
  Result<RuntimeId> source_runtime = dec_id<RuntimeIdTag>(in);
  if (!source_runtime.ok()) return source_runtime.error();
  value.source_runtime = source_runtime.take();
  Result<RuntimeGeneration> source_runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!source_runtime_generation.ok()) return source_runtime_generation.error();
  value.source_runtime_generation = source_runtime_generation.value();
  Result<RuntimeId> destination_runtime = dec_id<RuntimeIdTag>(in);
  if (!destination_runtime.ok()) return destination_runtime.error();
  value.destination_runtime = destination_runtime.take();
  Result<RuntimeGeneration> destination_runtime_generation = dec_gen<RuntimeGenerationTag>(in);
  if (!destination_runtime_generation.ok()) return destination_runtime_generation.error();
  value.destination_runtime_generation = destination_runtime_generation.value();
  Result<FederationId> federation = dec_id<FederationIdTag>(in);
  if (!federation.ok()) return federation.error();
  value.federation = federation.take();
  Result<FederationGeneration> federation_generation = dec_gen<FederationGenerationTag>(in);
  if (!federation_generation.ok()) return federation_generation.error();
  value.federation_generation = federation_generation.value();
  Result<CompatibilityGeneration> compatibility_generation = dec_gen<CompatibilityGenerationTag>(in);
  if (!compatibility_generation.ok()) return compatibility_generation.error();
  value.compatibility_generation = compatibility_generation.value();
  Result<PolicyGeneration> policy_generation = dec_gen<PolicyGenerationTag>(in);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  Result<std::uint32_t> dimension_count = in.u32();
  if (!dimension_count.ok()) return dimension_count.error();
  if (dimension_count.value() > kPortabilityDimensionCount) {
    return Error(ErrorCode::BoundExceeded, "encoded portability has too many dimensions");
  }
  for (std::uint32_t i = 0; i < dimension_count.value(); ++i) {
    PortabilityDimensionResult result;
    Result<PortabilityDimension> dimension = dec_enum<PortabilityDimension>(
        in, enum_max(PortabilityDimension{}), "portability dimension");
    if (!dimension.ok()) return dimension.error();
    result.dimension = dimension.value();
    Result<PortabilityOutcome> outcome =
        dec_enum<PortabilityOutcome>(in, enum_max(PortabilityOutcome{}), "portability outcome");
    if (!outcome.ok()) return outcome.error();
    result.outcome = outcome.value();
    Result<std::string> detail = in.str();
    if (!detail.ok()) return detail.error();
    result.detail = detail.take();
    Result<ReasonBasis> basis = dec_enum<ReasonBasis>(in, enum_max(ReasonBasis{}), "reason basis");
    if (!basis.ok()) return basis.error();
    result.basis = basis.value();
    Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
    if (!precision.ok()) return precision.error();
    result.precision = precision.value();
    Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
    if (!cls.ok()) return cls.error();
    result.evidence_class = cls.value();
    Result<bool> constrains = in.boolean();
    if (!constrains.ok()) return constrains.error();
    result.constrains_overall = constrains.value();
    Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
    if (!evidence.ok()) return evidence.error();
    result.evidence = evidence.take();
    value.dimensions.push_back(std::move(result));
  }
  Result<PortabilityOutcome> overall =
      dec_enum<PortabilityOutcome>(in, enum_max(PortabilityOutcome{}), "portability outcome");
  if (!overall.ok()) return overall.error();
  value.overall = overall.value();
  bool* flags[] = {&value.requires_rebuild,     &value.requires_recompile,
                   &value.requires_conversion,  &value.requires_state_translation,
                   &value.requires_fallback,    &value.technically_blocked,
                   &value.policy_blocked};
  for (bool* flag : flags) {
    Result<bool> read = in.boolean();
    if (!read.ok()) return read.error();
    *flag = read.value();
  }
  Result<Currentness> currentness =
      dec_enum<Currentness>(in, enum_max(Currentness{}), "currentness");
  if (!currentness.ok()) return currentness.error();
  value.currentness = currentness.value();
  Result<Precision> precision = dec_enum<Precision>(in, enum_max(Precision{}), "precision");
  if (!precision.ok()) return precision.error();
  value.precision = precision.value();
  Result<EvidenceClass> cls = dec_enum<EvidenceClass>(in, enum_max(EvidenceClass{}), "evidence class");
  if (!cls.ok()) return cls.error();
  value.evidence_class = cls.value();
  Result<ObservationStamp> stamp = decode_stamp(in);
  if (!stamp.ok()) return stamp.error();
  value.stamp = stamp.take();
  Result<EvidenceList> evidence = decode_evidence_list(in, kDefaultEvidencePerFinding);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.take();
  const Status s = value.validate();
  if (!s.ok()) {
    return Error(ErrorCode::ProtocolViolation, "encoded portability assessment is not valid",
                 s.error().message());
  }
  return value;
}

// --- Digests ---------------------------------------------------------------

namespace {

template <class T, class Enc>
std::string digest_of(const T& value, Enc enc) {
  Encoder encoder;
  enc(encoder, value);
  if (encoder.failed()) {
    return std::string("encode-failed");
  }
  const std::vector<std::uint8_t>& bytes = encoder.bytes();
  return hex_digest64(fnv1a64(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size())));
}

}  // namespace

std::string digest_record(const FederationRecord& value) {
  return digest_of(value, [](Encoder& e, const FederationRecord& v) { encode_federation(e, v); });
}
std::string digest_record(const SiteRecord& value) {
  return digest_of(value, [](Encoder& e, const SiteRecord& v) { encode_site(e, v); });
}
std::string digest_record(const ClusterRecord& value) {
  return digest_of(value, [](Encoder& e, const ClusterRecord& v) { encode_cluster(e, v); });
}
std::string digest_record(const AcceleratorClassRecord& value) {
  return digest_of(value,
                   [](Encoder& e, const AcceleratorClassRecord& v) { encode_accelerator_class(e, v); });
}
std::string digest_record(const RuntimeRecord& value) {
  return digest_of(value, [](Encoder& e, const RuntimeRecord& v) { encode_runtime(e, v); });
}
std::string digest_record(const BackendRecord& value) {
  return digest_of(value, [](Encoder& e, const BackendRecord& v) { encode_backend(e, v); });
}
std::string digest_record(const DomainRecord& value) {
  return digest_of(value, [](Encoder& e, const DomainRecord& v) { encode_domain(e, v); });
}
std::string digest_record(const PolicyRecord& value) {
  return digest_of(value, [](Encoder& e, const PolicyRecord& v) { encode_policy(e, v); });
}
std::string digest_record(const ArtifactRecord& value) {
  return digest_of(value, [](Encoder& e, const ArtifactRecord& v) { encode_artifact(e, v); });
}
std::string digest_record(const WorkloadClassRecord& value) {
  return digest_of(value,
                   [](Encoder& e, const WorkloadClassRecord& v) { encode_workload_class(e, v); });
}
std::string digest_record(const WorkloadRecord& value) {
  return digest_of(value, [](Encoder& e, const WorkloadRecord& v) { encode_workload(e, v); });
}
std::string digest_record(const PlacementRecord& value) {
  return digest_of(value, [](Encoder& e, const PlacementRecord& v) { encode_placement(e, v); });
}
std::string digest_record(const MigrationRecord& value) {
  return digest_of(value, [](Encoder& e, const MigrationRecord& v) { encode_migration(e, v); });
}
std::string digest_record(const PortabilityAssessment& value) {
  return digest_of(value, [](Encoder& e, const PortabilityAssessment& v) { encode_portability(e, v); });
}

std::string hex_digest(std::uint64_t value) { return hex_digest64(value); }

}  // namespace fo::codec

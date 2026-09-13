// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

// Canonical binary encoding of every published record. Shared by the wire protocol and
// the persistence format so that a record can never be encoded one way on the wire and
// another way on disk.
//
// Conventions:
//  * every multi-byte integer is little-endian;
//  * every string is length-prefixed with a u32 and bounded before allocation;
//  * every collection is length-prefixed with a u32 and bounded before allocation;
//  * collections are stored in canonical (sorted, deduplicated) order so that two
//    equal records always produce identical bytes;
//  * decoding validates every bound and every semantic invariant; a decoded record is
//    never returned in a state that validate() would reject.

#include "federation_observatory/capability.hpp"
#include "federation_observatory/capacity.hpp"
#include "federation_observatory/codec.hpp"
#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/ids.hpp"
#include "federation_observatory/migration.hpp"
#include "federation_observatory/model.hpp"
#include "federation_observatory/placement.hpp"
#include "federation_observatory/portability.hpp"

namespace fo::codec {

FO_API void encode_evidence(Encoder& out, const EvidenceRef& value);
FO_API Result<EvidenceRef> decode_evidence(Decoder& in);
FO_API void encode_evidence_list(Encoder& out, const EvidenceList& value);
FO_API Result<EvidenceList> decode_evidence_list(Decoder& in, std::size_t limit);

FO_API void encode_capability_ref(Encoder& out, const CapabilityRef& value);
FO_API Result<CapabilityRef> decode_capability_ref(Decoder& in);
FO_API void encode_capability_value(Encoder& out, const CapabilityValue& value);
FO_API Result<CapabilityValue> decode_capability_value(Decoder& in);
FO_API void encode_capability_entry(Encoder& out, const CapabilityEntry& value);
FO_API Result<CapabilityEntry> decode_capability_entry(Decoder& in);
FO_API void encode_capability_set(Encoder& out, const CapabilitySet& value);
FO_API Result<CapabilitySet> decode_capability_set(Decoder& in, std::size_t limit);
FO_API void encode_capability_requirement(Encoder& out, const CapabilityRequirement& value);
FO_API Result<CapabilityRequirement> decode_capability_requirement(Decoder& in);

FO_API void encode_stamp(Encoder& out, const ObservationStamp& value);
FO_API Result<ObservationStamp> decode_stamp(Decoder& in);

FO_API void encode_ledger(Encoder& out, const CapacityLedger& value);
FO_API Result<CapacityLedger> decode_ledger(Decoder& in);
FO_API void encode_capacity_pool(Encoder& out, const CapacityPool& value);
FO_API Result<CapacityPool> decode_capacity_pool(Decoder& in);

FO_API void encode_federation(Encoder& out, const FederationRecord& value);
FO_API Result<FederationRecord> decode_federation(Decoder& in);
FO_API void encode_site(Encoder& out, const SiteRecord& value);
FO_API Result<SiteRecord> decode_site(Decoder& in);
FO_API void encode_accelerator_class(Encoder& out, const AcceleratorClassRecord& value);
FO_API Result<AcceleratorClassRecord> decode_accelerator_class(Decoder& in);
FO_API void encode_runtime(Encoder& out, const RuntimeRecord& value);
FO_API Result<RuntimeRecord> decode_runtime(Decoder& in);
FO_API void encode_backend(Encoder& out, const BackendRecord& value);
FO_API Result<BackendRecord> decode_backend(Decoder& in);
FO_API void encode_domain(Encoder& out, const DomainRecord& value);
FO_API Result<DomainRecord> decode_domain(Decoder& in);
FO_API void encode_cluster(Encoder& out, const ClusterRecord& value);
FO_API Result<ClusterRecord> decode_cluster(Decoder& in);
FO_API void encode_policy(Encoder& out, const PolicyRecord& value);
FO_API Result<PolicyRecord> decode_policy(Decoder& in);
FO_API void encode_artifact(Encoder& out, const ArtifactRecord& value);
FO_API Result<ArtifactRecord> decode_artifact(Decoder& in);
FO_API void encode_workload_class(Encoder& out, const WorkloadClassRecord& value);
FO_API Result<WorkloadClassRecord> decode_workload_class(Decoder& in);
FO_API void encode_workload(Encoder& out, const WorkloadRecord& value);
FO_API Result<WorkloadRecord> decode_workload(Decoder& in);

FO_API void encode_candidate(Encoder& out, const CandidateObservation& value);
FO_API Result<CandidateObservation> decode_candidate(Decoder& in);
FO_API void encode_placement(Encoder& out, const PlacementRecord& value);
FO_API Result<PlacementRecord> decode_placement(Decoder& in);
FO_API void encode_migration(Encoder& out, const MigrationRecord& value);
FO_API Result<MigrationRecord> decode_migration(Decoder& in);
FO_API void encode_portability(Encoder& out, const PortabilityAssessment& value);
FO_API Result<PortabilityAssessment> decode_portability(Decoder& in);

/// Canonical digest of any encoded record. Used to detect duplicate publications.
FO_API std::string digest_record(const FederationRecord& value);
FO_API std::string digest_record(const SiteRecord& value);
FO_API std::string digest_record(const ClusterRecord& value);
FO_API std::string digest_record(const AcceleratorClassRecord& value);
FO_API std::string digest_record(const RuntimeRecord& value);
FO_API std::string digest_record(const BackendRecord& value);
FO_API std::string digest_record(const DomainRecord& value);
FO_API std::string digest_record(const PolicyRecord& value);
FO_API std::string digest_record(const ArtifactRecord& value);
FO_API std::string digest_record(const WorkloadClassRecord& value);
FO_API std::string digest_record(const WorkloadRecord& value);
FO_API std::string digest_record(const PlacementRecord& value);
FO_API std::string digest_record(const MigrationRecord& value);
FO_API std::string digest_record(const PortabilityAssessment& value);

/// Lowercase hex rendering of a 64-bit hash, used for all digests.
FO_API std::string hex_digest(std::uint64_t value);

}  // namespace fo::codec

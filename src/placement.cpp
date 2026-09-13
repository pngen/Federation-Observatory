// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Placement observation and attribution. The runtime reports what the upstream
// scheduler exposed and marks everything it computed itself. It never claims to know
// the candidate set a scheduler did not publish.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/placement.hpp"

namespace fo {
namespace {

const char* rejection_name(RejectionReason reason) {
  switch (reason) {
    case RejectionReason::Unknown: return "UNKNOWN";
    case RejectionReason::InsufficientAccelerators: return "INSUFFICIENT_ACCELERATORS";
    case RejectionReason::InsufficientMemory: return "INSUFFICIENT_MEMORY";
    case RejectionReason::CapabilityMissing: return "CAPABILITY_MISSING";
    case RejectionReason::ArtifactIncompatible: return "ARTIFACT_INCOMPATIBLE";
    case RejectionReason::RuntimeIncompatible: return "RUNTIME_INCOMPATIBLE";
    case RejectionReason::DriverIncompatible: return "DRIVER_INCOMPATIBLE";
    case RejectionReason::TopologyIncompatible: return "TOPOLOGY_INCOMPATIBLE";
    case RejectionReason::IsolationIncompatible: return "ISOLATION_INCOMPATIBLE";
    case RejectionReason::AbiIncompatible: return "ABI_INCOMPATIBLE";
    case RejectionReason::PolicyRejected: return "POLICY_REJECTED";
    case RejectionReason::ClusterDraining: return "CLUSTER_DRAINING";
    case RejectionReason::ClusterNotReady: return "CLUSTER_NOT_READY";
    case RejectionReason::StaleClusterEvidence: return "STALE_CLUSTER_EVIDENCE";
    case RejectionReason::CapacityReserved: return "CAPACITY_RESERVED";
    case RejectionReason::PortabilityNotProven: return "PORTABILITY_NOT_PROVEN";
    case RejectionReason::SiteRestriction: return "SITE_RESTRICTION";
    case RejectionReason::LocalityConstraint: return "LOCALITY_CONSTRAINT";
    case RejectionReason::FailureDomainConstraint: return "FAILURE_DOMAIN_CONSTRAINT";
  }
  return "UNKNOWN";
}

}  // namespace

std::string_view to_string(RejectionReason reason) noexcept { return rejection_name(reason); }

bool parse_rejection_reason(std::string_view text, RejectionReason& out) noexcept {
  for (std::size_t i = 0; i < kRejectionReasonCount; ++i) {
    const auto candidate = static_cast<RejectionReason>(i);
    if (rejection_name(candidate) == text) {
      out = candidate;
      return true;
    }
  }
  return false;
}

std::size_t rejection_reason_precedence(RejectionReason reason) noexcept {
  switch (reason) {
    case RejectionReason::Unknown: return 0;
    case RejectionReason::StaleClusterEvidence: return 1;
    case RejectionReason::ClusterDraining: return 2;
    case RejectionReason::ClusterNotReady: return 3;
    case RejectionReason::CapacityReserved: return 4;
    case RejectionReason::InsufficientAccelerators: return 5;
    case RejectionReason::InsufficientMemory: return 6;
    case RejectionReason::CapabilityMissing: return 7;
    case RejectionReason::ArtifactIncompatible: return 8;
    case RejectionReason::AbiIncompatible: return 9;
    case RejectionReason::RuntimeIncompatible: return 10;
    case RejectionReason::DriverIncompatible: return 11;
    case RejectionReason::TopologyIncompatible: return 12;
    case RejectionReason::IsolationIncompatible: return 13;
    case RejectionReason::LocalityConstraint: return 14;
    case RejectionReason::FailureDomainConstraint: return 15;
    case RejectionReason::PortabilityNotProven: return 16;
    case RejectionReason::SiteRestriction: return 17;
    case RejectionReason::PolicyRejected: return 18;
  }
  return 0;
}

RejectionReason reject_reason_for_stranding(StrandingReason reason) noexcept {
  switch (reason) {
    case StrandingReason::UnsupportedAcceleratorCapability: return RejectionReason::CapabilityMissing;
    case StrandingReason::UnsupportedPrecision: return RejectionReason::CapabilityMissing;
    case StrandingReason::MissingOffloadCapability: return RejectionReason::CapabilityMissing;
    case StrandingReason::MissingInterconnectFeature: return RejectionReason::TopologyIncompatible;
    case StrandingReason::RuntimeMismatch: return RejectionReason::RuntimeIncompatible;
    case StrandingReason::DriverMismatch: return RejectionReason::DriverIncompatible;
    case StrandingReason::AbiMismatch: return RejectionReason::AbiIncompatible;
    case StrandingReason::ArtifactIncompatibility: return RejectionReason::ArtifactIncompatible;
    case StrandingReason::MissingCompilerTarget: return RejectionReason::ArtifactIncompatible;
    case StrandingReason::PartitionGeometry: return RejectionReason::TopologyIncompatible;
    case StrandingReason::TopologyConstraint: return RejectionReason::TopologyIncompatible;
    case StrandingReason::IsolationRequirement: return RejectionReason::IsolationIncompatible;
    case StrandingReason::MissingLocalStorageState: return RejectionReason::CapabilityMissing;
    case StrandingReason::MemoryInsufficient: return RejectionReason::InsufficientMemory;
    case StrandingReason::SiteRestriction: return RejectionReason::SiteRestriction;
    case StrandingReason::PolicyRestriction: return RejectionReason::PolicyRejected;
    case StrandingReason::Unknown: return RejectionReason::Unknown;
    case StrandingReason::None: return RejectionReason::Unknown;
  }
  return RejectionReason::Unknown;
}

std::string_view to_string(CandidateStatus status) noexcept {
  switch (status) {
    case CandidateStatus::Selected: return "SELECTED";
    case CandidateStatus::Rejected: return "REJECTED";
    case CandidateStatus::NotConsidered: return "NOT_CONSIDERED";
    case CandidateStatus::Unattributed: return "UNATTRIBUTED";
  }
  return "UNATTRIBUTED";
}

std::string_view to_string(CandidateSetCompleteness completeness) noexcept {
  switch (completeness) {
    case CandidateSetCompleteness::Unknown: return "UNKNOWN";
    case CandidateSetCompleteness::SelectedOnly: return "SELECTED_ONLY";
    case CandidateSetCompleteness::Partial: return "PARTIAL";
    case CandidateSetCompleteness::Complete: return "COMPLETE";
  }
  return "UNKNOWN";
}

bool allows_rejection_attribution(CandidateSetCompleteness completeness) noexcept {
  return completeness == CandidateSetCompleteness::Partial ||
         completeness == CandidateSetCompleteness::Complete;
}

std::string_view to_string(AttributionAspect aspect) noexcept {
  switch (aspect) {
    case AttributionAspect::Selection: return "SELECTION";
    case AttributionAspect::Eligibility: return "ELIGIBILITY";
    case AttributionAspect::Capability: return "CAPABILITY";
    case AttributionAspect::Compatibility: return "COMPATIBILITY";
    case AttributionAspect::Capacity: return "CAPACITY";
    case AttributionAspect::Topology: return "TOPOLOGY";
    case AttributionAspect::Policy: return "POLICY";
    case AttributionAspect::Affinity: return "AFFINITY";
    case AttributionAspect::Fallback: return "FALLBACK";
    case AttributionAspect::Rejection: return "REJECTION";
    case AttributionAspect::CandidateCoverage: return "CANDIDATE_COVERAGE";
    case AttributionAspect::Currentness: return "CURRENTNESS";
  }
  return "SELECTION";
}

std::vector<RejectionReason> CandidateObservation::reason_list() const {
  std::vector<RejectionReason> out;
  for (std::size_t i = 0; i < kRejectionReasonCount; ++i) {
    const auto reason = static_cast<RejectionReason>(i);
    if (has_reason(reason)) {
      out.push_back(reason);
    }
  }
  std::sort(out.begin(), out.end(), [](RejectionReason a, RejectionReason b) {
    return rejection_reason_precedence(a) < rejection_reason_precedence(b);
  });
  return out;
}

std::string CandidateObservation::render(std::string_view indent) const {
  std::vector<std::string> names;
  for (const RejectionReason reason : reason_list()) {
    names.emplace_back(rejection_name(reason));
  }
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"cluster", cluster.value()});
  rows.push_back({"cluster_generation",
                  cluster_generation.is_set() ? cluster_generation.to_string() : "-"});
  rows.push_back({"cluster_epoch", cluster_epoch.is_set() ? cluster_epoch.to_string() : "-"});
  rows.push_back({"accelerator_class", render_id(accelerator_class.value())});
  rows.push_back({"status", std::string(fo::to_string(status))});
  rows.push_back({"reasons", names.empty() ? "-" : join_strings(names, ",")});
  rows.push_back({"basis", std::string(fo::to_string(basis))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"detail", detail.empty() ? "-" : detail});
  return std::string(indent) + "candidate:\n" +
         render_table({"field", "value"}, rows, std::string(indent) + "  ");
}

bool operator<(const CandidateObservation& a, const CandidateObservation& b) noexcept {
  if (a.cluster != b.cluster) return a.cluster < b.cluster;
  if (a.status != b.status) return a.status < b.status;
  return a.accelerator_class < b.accelerator_class;
}

Status PlacementRecord::validate() const {
  if (id.empty()) {
    return fail(ErrorCode::InvalidArgument, "placement id is empty");
  }
  if (!generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "placement generation is unset", id.value());
  }
  if (workload.empty()) {
    return fail(ErrorCode::InvalidArgument, "placement has no workload", id.value());
  }
  if (!workload_generation.is_set()) {
    return fail(ErrorCode::InvalidArgument, "placement workload generation is unset", id.value());
  }
  if (federation.empty()) {
    return fail(ErrorCode::InvalidArgument, "placement has no federation", id.value());
  }
  if (selected.empty()) {
    return fail(ErrorCode::InvalidArgument, "placement has no selected cluster", id.value());
  }
  if (candidates.size() > kMaxCandidatesPerPlacement) {
    return fail(ErrorCode::BoundExceeded, "too many placement candidates", id.value());
  }
  std::vector<ClusterId> seen;
  seen.reserve(candidates.size());
  std::size_t selected_rows = 0;
  for (const CandidateObservation& candidate : candidates) {
    if (candidate.cluster.empty()) {
      return fail(ErrorCode::InvalidArgument, "candidate has no cluster", id.value());
    }
    if (candidate.status == CandidateStatus::Selected) {
      ++selected_rows;
      if (candidate.cluster != selected) {
        return fail(ErrorCode::InvalidArgument, "selected candidate is not the selected cluster",
                    id.value() + " " + candidate.cluster.value());
      }
    }
    if (candidate.reasons == 0 && candidate.status == CandidateStatus::Rejected) {
      return fail(ErrorCode::InvalidArgument, "rejected candidate carries no reason",
                  id.value() + " " + candidate.cluster.value());
    }
    seen.push_back(candidate.cluster);
  }
  if (selected_rows > 1) {
    return fail(ErrorCode::InvalidArgument, "placement has more than one selected candidate",
                id.value());
  }
  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
    return fail(ErrorCode::InvalidArgument, "duplicate candidate cluster", id.value());
  }
  if (candidate_completeness == CandidateSetCompleteness::SelectedOnly && candidates.size() > 1) {
    return fail(ErrorCode::InvalidArgument,
                "candidate set is marked SELECTED_ONLY but carries more than one candidate",
                id.value());
  }
  if (candidate_completeness == CandidateSetCompleteness::SelectedOnly) {
    for (const CandidateObservation& candidate : candidates) {
      if (candidate.status == CandidateStatus::Rejected) {
        return fail(ErrorCode::InvalidArgument,
                    "candidate set is marked SELECTED_ONLY but carries a rejection", id.value());
      }
    }
  }
  return Status::success();
}

const CandidateObservation* PlacementRecord::find_candidate(const ClusterId& cluster) const {
  for (const CandidateObservation& candidate : candidates) {
    if (candidate.cluster == cluster) {
      return &candidate;
    }
  }
  return nullptr;
}

std::size_t PlacementRecord::rejected_count() const {
  return static_cast<std::size_t>(
      std::count_if(candidates.begin(), candidates.end(), [](const CandidateObservation& candidate) {
        return candidate.status == CandidateStatus::Rejected ||
               candidate.status == CandidateStatus::NotConsidered;
      }));
}

std::string PlacementRecord::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"id", id.value()});
  rows.push_back({"generation", generation.to_string()});
  rows.push_back({"workload", workload.value()});
  rows.push_back({"workload_generation", workload_generation.to_string()});
  rows.push_back({"federation", federation.value()});
  rows.push_back({"federation_generation",
                  federation_generation.is_set() ? federation_generation.to_string() : "-"});
  rows.push_back({"selected", selected.value()});
  rows.push_back({"selected_generation",
                  selected_generation.is_set() ? selected_generation.to_string() : "-"});
  rows.push_back({"selected_epoch", selected_epoch.is_set() ? selected_epoch.to_string() : "-"});
  rows.push_back({"selected_accelerator_class", render_id(selected_accelerator_class.value())});
  rows.push_back({"selected_accelerator_count", std::to_string(selected_accelerator_count)});
  rows.push_back({"selected_memory_bytes", std::to_string(selected_memory_bytes)});
  rows.push_back({"candidate_completeness", std::string(fo::to_string(candidate_completeness))});
  rows.push_back({"candidates", std::to_string(candidates.size())});
  rows.push_back({"rejected_candidates", std::to_string(rejected_count())});
  rows.push_back({"policy_generation", policy_generation.is_set() ? policy_generation.to_string() : "-"});
  rows.push_back({"capacity_generation",
                  capacity_generation.is_set() ? capacity_generation.to_string() : "-"});
  rows.push_back({"topology_generation",
                  topology_generation.is_set() ? topology_generation.to_string() : "-"});
  rows.push_back({"compatibility_generation",
                  compatibility_generation.is_set() ? compatibility_generation.to_string() : "-"});
  rows.push_back({"capacity_available", capacity_available ? "yes" : "no"});
  rows.push_back({"compatibility_constrained", std::string(fo::to_string(compatibility_constrained))});
  rows.push_back({"topology_constrained", std::string(fo::to_string(topology_constrained))});
  rows.push_back({"policy_constrained", std::string(fo::to_string(policy_constrained))});
  rows.push_back({"capability_constrained", std::string(fo::to_string(capability_constrained))});
  rows.push_back({"affinity_constrained", std::string(fo::to_string(affinity_constrained))});
  rows.push_back({"fallback_required", fallback_required ? "yes" : "no"});
  rows.push_back({"fallback_detail", fallback_detail.empty() ? "-" : fallback_detail});
  rows.push_back({"cost", cost_known ? std::to_string(cost_estimate) : "UNKNOWN"});
  rows.push_back({"slo_class", slo_class.empty() ? "-" : slo_class});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  std::string out = std::string(indent) + "placement:\n" +
                    render_table({"field", "value"}, rows, std::string(indent) + "  ");
  return out;
}

bool operator<(const AttributionFinding& a, const AttributionFinding& b) noexcept {
  if (a.aspect != b.aspect) return a.aspect < b.aspect;
  if (a.subject != b.subject) return a.subject < b.subject;
  return a.statement < b.statement;
}

std::string AttributionFinding::render(std::string_view indent) const {
  std::string out(indent);
  out += "[";
  out += fo::to_string(aspect);
  out += "] ";
  if (!subject.empty()) {
    out += subject;
    out += ": ";
  }
  out += statement;
  out += "  <";
  out += fo::to_string(basis);
  out += ", ";
  out += fo::to_string(precision);
  out += ", ";
  out += fo::to_string(evidence_class);
  out += ">";
  return out;
}

const CompatibilityAssessment* PlacementExplanationInputs::find(const ClusterId& cluster) const {
  for (const CompatibilityAssessment& assessment : assessments) {
    if (assessment.target_cluster == cluster) {
      return &assessment;
    }
  }
  return nullptr;
}

namespace {

AttributionFinding make_finding(AttributionAspect aspect, std::string subject, std::string statement,
                                ReasonBasis basis, Precision precision, EvidenceClass evidence_class) {
  AttributionFinding finding;
  finding.aspect = aspect;
  finding.subject = std::move(subject);
  finding.statement = std::move(statement);
  finding.basis = basis;
  finding.precision = precision;
  finding.evidence_class = evidence_class;
  return finding;
}

}  // namespace

PlacementExplanation explain_placement(const PlacementExplanationInputs& inputs) {
  PlacementExplanation explanation;
  explanation.snapshot_generation = inputs.snapshot_generation;
  if (inputs.placement == nullptr) {
    explanation.findings.push_back(make_finding(
        AttributionAspect::CandidateCoverage, "", "no placement record was supplied",
        ReasonBasis::Unattributed, Precision::Unknown, EvidenceClass::Unknown));
    explanation.precision = Precision::Unknown;
    explanation.evidence_class = EvidenceClass::Unknown;
    return explanation;
  }

  const PlacementRecord& placement = *inputs.placement;
  explanation.placement = placement.id;
  explanation.placement_generation = placement.generation;
  explanation.workload = placement.workload;
  explanation.workload_generation = placement.workload_generation;
  explanation.selected = placement.selected;
  explanation.candidate_completeness = placement.candidate_completeness;
  explanation.currentness = placement.currentness;

  const Precision placement_precision = placement.stamp.precision;
  const EvidenceClass placement_class = placement.stamp.evidence_class;

  // Candidate coverage: what the source exposed, stated before anything else.
  {
    std::string statement;
    ReasonBasis basis = ReasonBasis::Observed;
    Precision precision = placement_precision;
    switch (placement.candidate_completeness) {
      case CandidateSetCompleteness::Complete:
        statement = "the source exposed the complete evaluated candidate set (" +
                    std::to_string(placement.candidates.size()) + " candidates)";
        break;
      case CandidateSetCompleteness::Partial:
        statement = "the source exposed a partial candidate set (" +
                    std::to_string(placement.candidates.size()) +
                    " candidates); unlisted candidates are unknown, not eligible";
        break;
      case CandidateSetCompleteness::SelectedOnly:
        statement =
            "only the selected cluster was exposed; rejection attribution for alternatives "
            "is unavailable";
        precision = weakest(precision, Precision::Ambiguous);
        break;
      case CandidateSetCompleteness::Unknown:
        statement = "the completeness of the exposed candidate set is unknown";
        precision = Precision::Unknown;
        basis = ReasonBasis::Unattributed;
        break;
    }
    AttributionFinding finding = make_finding(AttributionAspect::CandidateCoverage, "", statement,
                                              basis, precision, placement_class);
    const Status s = finding.evidence.add(Provenance::Scheduler, placement_class,
                                          placement.stamp.publisher.value(), placement_precision,
                                          "candidate set published by the upstream scheduler");
    (void)s;
    explanation.findings.push_back(std::move(finding));
  }

  // Selection: the observed fact.
  {
    AttributionFinding finding = make_finding(
        AttributionAspect::Selection, placement.selected.value(),
        "workload " + placement.workload.value() + " generation " +
            placement.workload_generation.to_string() + " was placed on cluster " +
            placement.selected.value() + " generation " +
            (placement.selected_generation.is_set() ? placement.selected_generation.to_string()
                                                    : std::string("UNSET")) +
            " under federation generation " +
            (placement.federation_generation.is_set() ? placement.federation_generation.to_string()
                                                      : std::string("UNSET")),
        ReasonBasis::Observed, placement_precision, placement_class);
    for (const EvidenceRef& ref : placement.evidence.items()) {
      const Status s = finding.evidence.add(ref);
      (void)s;
    }
    explanation.findings.push_back(std::move(finding));
  }

  // Constraints asserted by the source.
  const struct {
    AttributionAspect aspect;
    Tri value;
    const char* label;
  } constraints[] = {
      {AttributionAspect::Compatibility, placement.compatibility_constrained,
       "compatibility constrained the placement"},
      {AttributionAspect::Topology, placement.topology_constrained,
       "topology or locality constrained the placement"},
      {AttributionAspect::Policy, placement.policy_constrained, "policy constrained the placement"},
      {AttributionAspect::Capability, placement.capability_constrained,
       "capability mismatch constrained the placement"},
      {AttributionAspect::Affinity, placement.affinity_constrained,
       "affinity or anti-affinity constrained the placement"},
  };
  for (const auto& constraint : constraints) {
    std::string statement;
    Precision precision = placement_precision;
    ReasonBasis basis = ReasonBasis::Observed;
    switch (constraint.value) {
      case Tri::Yes:
        statement = std::string(constraint.label);
        break;
      case Tri::No:
        statement = std::string(constraint.label) + ": no";
        break;
      case Tri::Unknown:
        statement = std::string(constraint.label) + ": not exposed by the source";
        precision = Precision::Unknown;
        basis = ReasonBasis::Unattributed;
        break;
    }
    explanation.findings.push_back(make_finding(constraint.aspect, placement.selected.value(),
                                                statement, basis, precision, placement_class));
  }

  // Capacity.
  {
    AttributionFinding finding = make_finding(
        AttributionAspect::Capacity, placement.selected.value(),
        placement.capacity_available
            ? std::string("the source reported capacity available on the selected cluster")
            : std::string("the source did not report capacity availability on the selected cluster; "
                          "capacity is not asserted"),
        placement.capacity_available ? ReasonBasis::Observed : ReasonBasis::Unattributed,
        placement.capacity_available ? placement_precision : Precision::Unknown,
        placement_class);
    explanation.findings.push_back(std::move(finding));
  }

  // Fallback.
  if (placement.fallback_required) {
    explanation.findings.push_back(make_finding(
        AttributionAspect::Fallback, placement.selected.value(),
        placement.fallback_detail.empty()
            ? std::string("the placement required a fallback; the source did not state which one")
            : "the placement required a fallback: " + placement.fallback_detail,
        placement.fallback_detail.empty() ? ReasonBasis::Unattributed : ReasonBasis::Observed,
        placement.fallback_detail.empty() ? Precision::Unknown : placement_precision,
        placement_class));
  }

  // Eligibility and compatibility for the selected target, when an assessment exists.
  if (const CompatibilityAssessment* assessment = inputs.find(placement.selected)) {
    AttributionFinding finding = make_finding(
        AttributionAspect::Compatibility, placement.selected.value(),
        std::string("compatibility with the selected cluster is ") +
            std::string(fo::to_string(assessment->overall)),
        ReasonBasis::Derived, assessment->precision, assessment->evidence_class);
    for (const EvidenceRef& ref : assessment->evidence.items()) {
      const Status s = finding.evidence.add(ref);
      (void)s;
    }
    explanation.findings.push_back(std::move(finding));

    for (const CompatibilityCheck& check : assessment->checks) {
      const std::string statement = std::string(fo::to_string(check.outcome)) + " for " +
                                    std::string(fo::to_string(check.subject)) + ": " + check.detail;
      explanation.findings.push_back(make_finding(AttributionAspect::Eligibility,
                                                  placement.selected.value(), statement,
                                                  check.basis, check.precision,
                                                  check.evidence_class));
    }
    if (assessment->policy_admissible == Tri::No) {
      explanation.findings.push_back(make_finding(AttributionAspect::Policy,
                                                  placement.selected.value(),
                                                  "policy admissibility: " + assessment->policy_detail,
                                                  ReasonBasis::Derived, assessment->precision,
                                                  assessment->evidence_class));
    }
  }

  // Rejections.
  for (const CandidateObservation& candidate : placement.candidates) {
    if (candidate.status != CandidateStatus::Rejected &&
        candidate.status != CandidateStatus::NotConsidered) {
      continue;
    }
    std::vector<std::string> names;
    for (const RejectionReason reason : candidate.reason_list()) {
      names.emplace_back(rejection_name(reason));
    }
    std::string statement;
    if (candidate.status == CandidateStatus::NotConsidered) {
      statement = "was not considered: ";
    } else {
      statement = "was rejected: ";
    }
    statement += names.empty() ? "no reason stated" : join_strings(names, ",");
    if (!candidate.detail.empty()) {
      statement += " - " + candidate.detail;
    }
    if (!allows_rejection_attribution(placement.candidate_completeness)) {
      statement += " (candidate set not fully exposed; this row is what the source published)";
    }
    AttributionFinding finding = make_finding(AttributionAspect::Rejection, candidate.cluster.value(),
                                              statement, candidate.basis, candidate.precision,
                                              candidate.evidence_class);
    for (const EvidenceRef& ref : candidate.evidence.items()) {
      const Status s = finding.evidence.add(ref);
      (void)s;
    }
    explanation.findings.push_back(std::move(finding));
  }

  // Currentness of the evidence behind the whole explanation.
  {
    ReasonBasis basis = ReasonBasis::Observed;
    std::string statement;
    switch (placement.currentness) {
      case Currentness::Current:
        statement = "the placement evidence is current";
        break;
      case Currentness::Stale:
        statement = "the placement evidence is stale; this explanation is historical";
        basis = ReasonBasis::Observed;
        break;
      case Currentness::RevalidationRequired:
        statement = "the placement evidence requires revalidation";
        break;
      case Currentness::Retired:
        statement = "the placement evidence belongs to a retired cluster";
        break;
      case Currentness::Unknown:
        statement = "the currentness of the placement evidence is unknown";
        basis = ReasonBasis::Unattributed;
        break;
    }
    explanation.findings.push_back(make_finding(AttributionAspect::Currentness, "", statement, basis,
                                                placement.currentness == Currentness::Current
                                                    ? placement_precision
                                                    : Precision::Unknown,
                                                placement_class));
  }

  std::sort(explanation.findings.begin(), explanation.findings.end());

  Precision precision = Precision::Exact;
  EvidenceClass evidence_class = EvidenceClass::Real;
  bool first = true;
  for (const AttributionFinding& finding : explanation.findings) {
    if (first) {
      precision = finding.precision;
      evidence_class = finding.evidence_class;
      first = false;
      continue;
    }
    precision = weakest(precision, finding.precision);
    evidence_class = weaker(evidence_class, finding.evidence_class);
  }
  if (first) {
    precision = Precision::Unknown;
    evidence_class = EvidenceClass::Unknown;
  }
  explanation.precision = precision;
  explanation.evidence_class = evidence_class;
  return explanation;
}

bool PlacementExplanation::has_rejection_attribution() const noexcept {
  return allows_rejection_attribution(candidate_completeness);
}

std::string PlacementExplanation::digest() const { return digest_text(render()); }

std::string PlacementExplanation::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"placement", render_id(placement.value())});
  rows.push_back({"placement_generation",
                  placement_generation.is_set() ? placement_generation.to_string() : "-"});
  rows.push_back({"workload", render_id(workload.value())});
  rows.push_back({"workload_generation",
                  workload_generation.is_set() ? workload_generation.to_string() : "-"});
  rows.push_back({"selected", render_id(selected.value())});
  rows.push_back({"candidate_completeness", std::string(fo::to_string(candidate_completeness))});
  rows.push_back({"rejection_attribution_available", has_rejection_attribution() ? "yes" : "no"});
  rows.push_back({"currentness", std::string(fo::to_string(currentness))});
  rows.push_back({"snapshot_generation",
                  snapshot_generation.is_set() ? snapshot_generation.to_string() : "-"});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = "placement explanation:\n";
  out += render_table({"field", "value"}, rows, "  ");
  out += "\n  findings:";
  for (const AttributionFinding& finding : findings) {
    out += '\n';
    out += finding.render("    ");
  }
  return out;
}

RejectionExplanation build_rejection_explanation(const PlacementRecord& placement,
                                                const ClusterId& candidate) {
  RejectionExplanation explanation;
  explanation.placement = placement.id;
  explanation.placement_generation = placement.generation;
  explanation.candidate = candidate;
  explanation.attribution_available = allows_rejection_attribution(placement.candidate_completeness);

  const CandidateObservation* row = placement.find_candidate(candidate);
  if (row == nullptr) {
    explanation.status = CandidateStatus::Unattributed;
    explanation.basis = ReasonBasis::Unattributed;
    explanation.precision = Precision::Unknown;
    explanation.evidence_class = placement.stamp.evidence_class;
    explanation.summary =
        "cluster " + candidate.value() +
        " does not appear in the exposed candidate set; its outcome is unknown rather than rejected";
    const Status s = explanation.evidence.add(
        Provenance::Scheduler, placement.stamp.evidence_class, placement.stamp.publisher.value(),
        Precision::Unknown, "candidate absent from the exposed set");
    (void)s;
    return explanation;
  }

  explanation.status = row->status;
  explanation.candidate_generation = row->cluster_generation;
  explanation.reasons = row->reason_list();
  explanation.basis = row->basis;
  explanation.precision = row->precision;
  explanation.evidence_class = row->evidence_class;
  for (const EvidenceRef& ref : row->evidence.items()) {
    const Status s = explanation.evidence.add(ref);
    (void)s;
  }

  if (row->status == CandidateStatus::Selected) {
    explanation.summary = "cluster " + candidate.value() + " received the placement";
    return explanation;
  }
  if (!explanation.attribution_available) {
    explanation.summary =
        "the upstream scheduler exposed only the selected cluster; rejection attribution for " +
        candidate.value() + " is unavailable";
    explanation.basis = ReasonBasis::Unattributed;
    explanation.precision = Precision::Unknown;
    return explanation;
  }
  std::vector<std::string> names;
  for (const RejectionReason reason : explanation.reasons) {
    names.emplace_back(rejection_name(reason));
  }
  explanation.summary = "cluster " + candidate.value() + " was " +
                        (row->status == CandidateStatus::NotConsidered ? "not considered" : "rejected") +
                        ": " + (names.empty() ? "no reason stated" : join_strings(names, ","));
  return explanation;
}

std::string RejectionExplanation::digest() const { return digest_text(render()); }

std::string RejectionExplanation::render() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"placement", render_id(placement.value())});
  rows.push_back({"candidate", render_id(candidate.value())});
  rows.push_back({"candidate_generation",
                  candidate_generation.is_set() ? candidate_generation.to_string() : "-"});
  rows.push_back({"status", std::string(fo::to_string(status))});
  std::vector<std::string> names;
  for (const RejectionReason reason : reasons) {
    names.emplace_back(rejection_name(reason));
  }
  rows.push_back({"reasons", names.empty() ? "-" : join_strings(names, ",")});
  rows.push_back({"attribution_available", attribution_available ? "yes" : "no"});
  rows.push_back({"basis", std::string(fo::to_string(basis))});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  rows.push_back({"summary", summary});
  std::string out = "rejection explanation:\n";
  out += render_table({"field", "value"}, rows, "  ");
  return out;
}

}  // namespace fo

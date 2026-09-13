// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Placement observation, rejection attribution, and explanation determinism.

#include "federation_observatory/placement.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

PlacementRecord make_placement(CandidateSetCompleteness completeness) {
  PlacementRecord placement;
  placement.id = PlacementId::unchecked("placement-1");
  placement.generation = PlacementGeneration{1};
  placement.workload = WorkloadId::unchecked("wl-1");
  placement.workload_generation = WorkloadGeneration{1};
  placement.federation = FederationId::unchecked("fed-1");
  placement.federation_generation = FederationGeneration{1};
  placement.workload_class = WorkloadClassId::unchecked("wc-1");
  placement.selected = ClusterId::unchecked("cluster-a");
  placement.selected_generation = ClusterGeneration{1};
  placement.selected_epoch = ClusterEpoch{1};
  placement.candidate_completeness = completeness;
  placement.stamp.precision = Precision::Exact;
  placement.stamp.evidence_class = EvidenceClass::Synthetic;
  placement.stamp.publisher = PublisherId::unchecked("scheduler");
  placement.currentness = Currentness::Current;

  CandidateObservation selected;
  selected.cluster = ClusterId::unchecked("cluster-a");
  selected.status = CandidateStatus::Selected;
  selected.basis = ReasonBasis::Observed;
  selected.precision = Precision::Exact;
  selected.evidence_class = EvidenceClass::Synthetic;
  placement.candidates.push_back(selected);
  return placement;
}

}  // namespace

FO_TEST(placement, validation_rejects_incoherent_records) {
  {
    PlacementRecord record = make_placement(CandidateSetCompleteness::Complete);
    FO_CHECK(record.validate().ok());
  }
  {
    PlacementRecord record = make_placement(CandidateSetCompleteness::Complete);
    record.selected = ClusterId::unchecked("cluster-b");
    FO_CHECK(!record.validate().ok());
  }
  {
    PlacementRecord record = make_placement(CandidateSetCompleteness::Complete);
    CandidateObservation rejected;
    rejected.cluster = ClusterId::unchecked("cluster-b");
    rejected.status = CandidateStatus::Rejected;
    rejected.reasons = 0;
    record.candidates.push_back(rejected);
    FO_CHECK(!record.validate().ok());
    FO_CHECK(record.validate().code() == ErrorCode::InvalidArgument);
  }
  {
    PlacementRecord record = make_placement(CandidateSetCompleteness::SelectedOnly);
    CandidateObservation rejected;
    rejected.cluster = ClusterId::unchecked("cluster-b");
    rejected.status = CandidateStatus::Rejected;
    rejected.reasons = rejection_bit(RejectionReason::CapabilityMissing);
    record.candidates.push_back(rejected);
    FO_CHECK(!record.validate().ok());
  }
  {
    PlacementRecord record = make_placement(CandidateSetCompleteness::Complete);
    record.generation = PlacementGeneration{};
    FO_CHECK(!record.validate().ok());
  }
}

FO_TEST(placement, rejection_reasons_keep_their_identity) {
  FO_CHECK(std::string(fo::to_string(RejectionReason::ClusterDraining)) == "CLUSTER_DRAINING");
  FO_CHECK(std::string(fo::to_string(RejectionReason::StaleClusterEvidence)) ==
           "STALE_CLUSTER_EVIDENCE");
  RejectionReason parsed = RejectionReason::Unknown;
  FO_CHECK(parse_rejection_reason("POLICY_REJECTED", parsed));
  FO_CHECK(parsed == RejectionReason::PolicyRejected);
  FO_CHECK(rejection_reason_precedence(RejectionReason::StaleClusterEvidence) <
           rejection_reason_precedence(RejectionReason::PolicyRejected));
  FO_CHECK(reject_reason_for_stranding(StrandingReason::AbiMismatch) ==
           RejectionReason::AbiIncompatible);
  FO_CHECK(reject_reason_for_stranding(StrandingReason::PolicyRestriction) ==
           RejectionReason::PolicyRejected);
}

FO_TEST(placement, explanation_is_deterministic_and_reports_the_selected_target) {
  const PlacementRecord record = make_placement(CandidateSetCompleteness::Partial);
  CandidateObservation rejected;
  rejected.cluster = ClusterId::unchecked("cluster-b");
  rejected.status = CandidateStatus::Rejected;
  rejected.reasons = rejection_bit(RejectionReason::ArtifactIncompatible) |
                     rejection_bit(RejectionReason::CapabilityMissing);
  rejected.basis = ReasonBasis::Observed;
  rejected.precision = Precision::Exact;
  rejected.evidence_class = EvidenceClass::Synthetic;
  PlacementRecord with_rejection = record;
  with_rejection.candidates.push_back(rejected);

  PlacementExplanationInputs inputs;
  inputs.placement = &with_rejection;
  inputs.snapshot_generation = SnapshotGeneration{7};
  const PlacementExplanation first = explain_placement(inputs);
  const PlacementExplanation second = explain_placement(inputs);
  FO_CHECK_EQ(first.digest(), second.digest());
  FO_CHECK_EQ(first.render(), second.render());
  FO_CHECK(first.has_rejection_attribution());
  FO_CHECK(first.candidate_completeness == CandidateSetCompleteness::Partial);
  FO_CHECK(first.render().find("ARTIFACT_INCOMPATIBLE") != std::string::npos);
  FO_CHECK(first.render().find("CAPABILITY_MISSING") != std::string::npos);
}

FO_TEST(placement, selected_only_candidate_set_reports_attribution_unavailable) {
  const PlacementRecord record = make_placement(CandidateSetCompleteness::SelectedOnly);
  PlacementExplanationInputs inputs;
  inputs.placement = &record;
  const PlacementExplanation explanation = explain_placement(inputs);
  FO_CHECK(!explanation.has_rejection_attribution());
  FO_CHECK(explanation.render().find("only the selected cluster was exposed") != std::string::npos);
  FO_CHECK(explanation.currentness == Currentness::Current);
}

FO_TEST(placement, rejection_explanation_refuses_to_invent_a_reason) {
  const PlacementRecord record = make_placement(CandidateSetCompleteness::SelectedOnly);
  const RejectionExplanation explanation =
      build_rejection_explanation(record, ClusterId::unchecked("cluster-z"));
  FO_CHECK(!explanation.attribution_available);
  FO_CHECK(explanation.status == CandidateStatus::Unattributed);
  FO_CHECK(explanation.basis == ReasonBasis::Unattributed);
  FO_CHECK(explanation.summary.find("does not appear in the exposed candidate set") !=
           std::string::npos);
  FO_CHECK(explanation.reasons.empty());
}

FO_TEST(placement, rejection_explanation_for_an_exposed_candidate) {
  PlacementRecord record = make_placement(CandidateSetCompleteness::Complete);
  CandidateObservation rejected;
  rejected.cluster = ClusterId::unchecked("cluster-b");
  rejected.status = CandidateStatus::Rejected;
  rejected.reasons = rejection_bit(RejectionReason::InsufficientAccelerators) |
                     rejection_bit(RejectionReason::ClusterDraining);
  rejected.basis = ReasonBasis::Observed;
  rejected.precision = Precision::Exact;
  rejected.evidence_class = EvidenceClass::Synthetic;
  record.candidates.push_back(rejected);

  const RejectionExplanation explanation =
      build_rejection_explanation(record, ClusterId::unchecked("cluster-b"));
  FO_CHECK(explanation.attribution_available);
  FO_CHECK(explanation.status == CandidateStatus::Rejected);
  FO_CHECK_EQ(explanation.reasons.size(), 2u);
  FO_CHECK(explanation.reasons.front() == RejectionReason::ClusterDraining);
  FO_CHECK(!explanation.digest().empty());
  FO_CHECK_EQ(explanation.digest(), build_rejection_explanation(record, ClusterId::unchecked("cluster-b")).digest());
}

FO_TEST(placement, candidate_set_completeness_gates_attribution) {
  FO_CHECK(allows_rejection_attribution(CandidateSetCompleteness::Complete));
  FO_CHECK(allows_rejection_attribution(CandidateSetCompleteness::Partial));
  FO_CHECK(!allows_rejection_attribution(CandidateSetCompleteness::SelectedOnly));
  FO_CHECK(!allows_rejection_attribution(CandidateSetCompleteness::Unknown));
}

// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Drift: intended versus observed membership and capability, and behavioral change in
// placement between two observation windows. Neither is invented when the inputs are
// absent.

#include "federation_observatory/analysis.hpp"
#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

/// Publish a placement for `workload` at `observed_at` selecting `selected`.
void publish_placement(fixture::World& world, const std::string& id, const std::string& workload,
                       const std::string& selected, std::int64_t observed_at) {
  fo::PlacementRecord placement;
  placement.id = fo::PlacementId::unchecked(id);
  placement.generation = fo::PlacementGeneration{1};
  placement.workload = fo::WorkloadId::unchecked(workload);
  placement.workload_generation = fo::WorkloadGeneration{1};
  placement.federation = world.publisher().federation;
  placement.federation_generation = fo::FederationGeneration{1};
  placement.workload_class = fo::WorkloadClassId::unchecked("wc-1");
  placement.selected = fo::ClusterId::unchecked(selected);
  placement.selected_generation = fo::ClusterGeneration{1};
  placement.selected_epoch = fo::ClusterEpoch{1};
  placement.candidate_completeness = fo::CandidateSetCompleteness::Complete;
  placement.capacity_available = true;
  placement.currentness = fo::Currentness::Current;
  fo::CandidateObservation chosen;
  chosen.cluster = placement.selected;
  chosen.status = fo::CandidateStatus::Selected;
  chosen.basis = fo::ReasonBasis::Observed;
  chosen.precision = fo::Precision::Exact;
  chosen.evidence_class = fo::EvidenceClass::Synthetic;
  placement.candidates.push_back(chosen);
  fo::PublicationContext context = world.publisher().next();
  context.observed_at = observed_at;
  FO_CHECK(world.observatory().publish_placement(context, placement).ok());
}

}  // namespace

FO_TEST(drift, no_intended_state_is_reported_as_not_evaluated) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());
  fo::DriftRequest request;
  request.federation = world.publisher().federation;
  const fo::DriftReport report = FO_UNWRAP(observatory.drift(request));
  FO_CHECK(!report.intended_state_supplied);
  FO_CHECK(!report.behavior_window_supplied);
  FO_CHECK(report.findings.empty());
  FO_CHECK(report.render().find("NOT evaluated") != std::string::npos);
  FO_CHECK(report.render().find("not a clean bill of health") != std::string::npos);
}

FO_TEST(drift, intended_membership_differences_are_reported) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());

  fo::IntendedFederationState intended;
  intended.federation = world.publisher().federation;
  intended.generation = fo::FederationGeneration{1};
  intended.expected_clusters = {fo::ClusterId::unchecked("cluster-0"),
                                fo::ClusterId::unchecked("cluster-missing")};
  intended.expected_sites = {fo::SiteId::unchecked("site-0"), fo::SiteId::unchecked("site-gone")};
  intended.precision = fo::Precision::Exact;
  intended.evidence_class = fo::EvidenceClass::Synthetic;
  FO_CHECK(intended.validate().ok());

  fo::DriftRequest request;
  request.federation = world.publisher().federation;
  request.intended = &intended;
  const fo::DriftReport report = FO_UNWRAP(observatory.drift(request));
  FO_CHECK(report.intended_state_supplied);
  FO_CHECK(report.count_of(fo::DriftKind::ClusterMissing) == 1);
  FO_CHECK(report.count_of(fo::DriftKind::SiteMissing) == 1);
  FO_CHECK(report.count_of(fo::DriftKind::UnexpectedCluster) == 1);
  bool has_before_after = false;
  for (const fo::DriftFinding& finding : report.findings) {
    if (finding.kind == fo::DriftKind::ClusterMissing) {
      FO_CHECK(!finding.intended.empty());
      FO_CHECK(!finding.observed.empty());
      has_before_after = true;
    }
  }
  FO_CHECK(has_before_after);
  FO_CHECK(!report.digest().empty());
  FO_CHECK_EQ(report.digest(), FO_UNWRAP(observatory.drift(request)).digest());
}

FO_TEST(drift, capability_and_runtime_divergence) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());

  fo::IntendedFederationState intended;
  intended.federation = world.publisher().federation;
  intended.generation = fo::FederationGeneration{1};
  intended.expected_clusters = world.clusters();
  fo::IntendedFederationState::ExpectedCapability expected;
  expected.cluster = world.clusters().front();
  expected.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  expected.comparator = fo::CapabilityComparator::Present;
  intended.expected_capabilities.push_back(expected);
  intended.expected_runtime_versions.emplace_back(world.clusters().front(),
                                                  std::string("99.0.0"));
  intended.precision = fo::Precision::Exact;
  intended.evidence_class = fo::EvidenceClass::Synthetic;

  fo::DriftRequest request;
  request.federation = world.publisher().federation;
  request.intended = &intended;
  const fo::DriftReport report = FO_UNWRAP(observatory.drift(request));
  FO_CHECK(report.count_of(fo::DriftKind::CapabilityMismatch) == 0);
  FO_CHECK(report.count_of(fo::DriftKind::RuntimeVersionDivergence) == 1);

  // Now ask for a capability the cluster does not publish.
  fo::IntendedFederationState missing;
  missing.federation = world.publisher().federation;
  missing.generation = fo::FederationGeneration{1};
  fo::IntendedFederationState::ExpectedCapability absent;
  absent.cluster = world.clusters().front();
  absent.key.key = fo::CapabilityKey::CxlCapability;
  absent.comparator = fo::CapabilityComparator::Present;
  missing.expected_capabilities.push_back(absent);
  missing.precision = fo::Precision::Exact;
  missing.evidence_class = fo::EvidenceClass::Synthetic;
  fo::DriftRequest missing_request;
  missing_request.federation = world.publisher().federation;
  missing_request.intended = &missing;
  const fo::DriftReport missing_report = FO_UNWRAP(observatory.drift(missing_request));
  FO_CHECK(missing_report.count_of(fo::DriftKind::CapabilityMismatch) == 1);
}

FO_TEST(drift, stale_cluster_is_drift_against_an_intended_state) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());
  FO_CHECK(observatory.fence_publisher(world.publisher().id, world.publisher().boot, "test").ok());

  fo::IntendedFederationState intended;
  intended.federation = world.publisher().federation;
  intended.generation = fo::FederationGeneration{1};
  intended.expected_clusters = world.clusters();
  intended.precision = fo::Precision::Exact;
  intended.evidence_class = fo::EvidenceClass::Synthetic;
  fo::DriftRequest request;
  request.federation = world.publisher().federation;
  request.intended = &intended;
  const fo::DriftReport report = FO_UNWRAP(observatory.drift(request));
  FO_CHECK(report.count_of(fo::DriftKind::StaleCluster) == 2);
}

FO_TEST(drift, behavioral_drift_between_two_windows) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_CHECK(world.build(2, 1).ok());

  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-1");
  FO_CHECK(observatory.register_workload_class(world.publisher().next(), workload_class).ok());
  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-1");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.required_accelerators = 1;
  FO_CHECK(observatory.register_workload(world.publisher().next(), workload).ok());

  publish_placement(world, "placement-before", "wl-1", world.clusters()[0].value(), 1000);
  publish_placement(world, "placement-after", "wl-1", world.clusters()[1].value(), 5000);

  fo::DriftRequest request;
  request.federation = world.publisher().federation;
  request.behavior_window_supplied = true;
  request.before_window.from = 0;
  request.before_window.to = 2000;
  request.after_window.from = 2000;
  request.after_window.to = 0;
  const fo::DriftReport report = FO_UNWRAP(observatory.drift(request));
  FO_CHECK(report.behavior_window_supplied);
  FO_CHECK(report.count_of(fo::DriftKind::PlacementTargetChanged) == 1);
  for (const fo::DriftFinding& finding : report.findings) {
    if (finding.kind == fo::DriftKind::PlacementTargetChanged) {
      FO_CHECK(finding.intended.find("cluster-0") != std::string::npos);
      FO_CHECK(finding.observed.find("cluster-1") != std::string::npos);
      FO_CHECK(finding.basis == fo::ReasonBasis::Derived);
    }
  }
}

FO_TEST(drift, kinds_are_classified) {
  FO_CHECK(is_behavioral_drift(fo::DriftKind::PlacementTargetChanged));
  FO_CHECK(!is_behavioral_drift(fo::DriftKind::ClusterMissing));
  FO_CHECK(requires_intended_state(fo::DriftKind::ClusterMissing));
  FO_CHECK(!requires_intended_state(fo::DriftKind::PlacementRejectionIntroduced));
}

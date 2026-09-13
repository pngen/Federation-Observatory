// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Publisher authority, sequence handling, generation fencing, immutable snapshots and
// recovery semantics.

#include "fixture.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

fo::FederationRecord basic_federation(const fo::FederationId& id) {
  fo::FederationRecord federation;
  federation.id = id;
  federation.generation = fo::FederationGeneration{1};
  federation.coordinator_epoch = fo::CoordinatorEpoch{1};
  return federation;
}

fo::SiteRecord basic_site(const fo::SiteId& id, const fo::FederationId& federation) {
  fo::SiteRecord site;
  site.id = id;
  site.generation = fo::SiteGeneration{1};
  site.federation = federation;
  return site;
}

fo::ClusterRecord basic_cluster(const fo::ClusterId& id, const fo::SiteId& site,
                                const fo::FederationId& federation) {
  fo::ClusterRecord cluster;
  cluster.id = id;
  cluster.generation = fo::ClusterGeneration{1};
  cluster.epoch = fo::ClusterEpoch{1};
  cluster.federation = federation;
  cluster.site = site;
  return cluster;
}

}  // namespace

FO_TEST(observatory, unregistered_publisher_boot_identity_is_refused) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  const fo::Result<fo::IngestResult> result =
      observatory.register_federation(publisher.next(), basic_federation(publisher.federation));
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::StaleBoot);
}

FO_TEST(observatory, context_validation_rejects_malformed_publications) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());

  fo::PublicationContext no_publisher = publisher.next();
  no_publisher.publisher = fo::PublisherId{};
  FO_CHECK(!observatory.register_federation(no_publisher, basic_federation(publisher.federation)).ok());

  fo::PublicationContext no_boot = publisher.next();
  no_boot.boot = fo::BootGeneration{};
  FO_CHECK(!observatory.register_federation(no_boot, basic_federation(publisher.federation)).ok());

  fo::PublicationContext zero_sequence = publisher.next();
  zero_sequence.sequence = fo::Sequence{};
  FO_CHECK(!observatory.register_federation(zero_sequence, basic_federation(publisher.federation)).ok());
}

FO_TEST(observatory, duplicate_publication_is_suppressed_and_conflicting_duplicate_is_refused) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  fo::FederationRecord federation = basic_federation(publisher.federation);
  const fo::Result<fo::IngestResult> first =
      observatory.register_federation(publisher.next(), federation);
  FO_REQUIRE(first.ok());
  FO_CHECK(first.value().disposition == fo::IngestDisposition::Applied);

  const fo::Result<fo::IngestResult> duplicate =
      observatory.register_federation(publisher.repeat(), federation);
  FO_REQUIRE(duplicate.ok());
  FO_CHECK(duplicate.value().disposition == fo::IngestDisposition::Duplicate);

  // Re-using an accepted sequence number for different content is a protocol violation:
  // the observation cannot be ordered, so it is refused rather than applied.
  fo::FederationRecord changed = federation;
  changed.display_name = "changed without a generation bump";
  const fo::Result<fo::IngestResult> conflict =
      observatory.register_federation(publisher.repeat(), changed);
  FO_CHECK(!conflict.ok());
  FO_CHECK(conflict.error().code() == fo::ErrorCode::Conflict);
}

FO_TEST(observatory, sequence_regression_is_refused_and_gaps_are_reported) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.register_federation(publisher.next(), basic_federation(publisher.federation)).ok());

  // Jump forward: the gap must be reported rather than hidden.
  fo::PublicationContext jumped = publisher.next();
  jumped.sequence = fo::Sequence{jumped.sequence.value() + 4};
  publisher.sequence = jumped.sequence;
  const fo::Result<fo::IngestResult> gapped =
      observatory.register_site(jumped, basic_site(fo::SiteId::unchecked("site-1"), publisher.federation));
  FO_REQUIRE(gapped.ok());
  FO_CHECK_EQ(gapped.value().sequence_gap, 4u);
  FO_CHECK_EQ(observatory.stats().sequence_gaps_observed, 1u);

  fo::PublicationContext old = publisher.next();
  old.sequence = fo::Sequence{1};
  const fo::Result<fo::IngestResult> regressed =
      observatory.register_site(old, basic_site(fo::SiteId::unchecked("site-2"), publisher.federation));
  FO_CHECK(!regressed.ok());
  FO_CHECK(regressed.error().code() == fo::ErrorCode::SequenceRegression);
}

FO_TEST(observatory, stale_epoch_traffic_is_refused) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  fo::PublicationContext stale = publisher.next();
  stale.coordinator_epoch = fo::CoordinatorEpoch{99};
  const fo::Result<fo::IngestResult> result =
      observatory.register_federation(stale, basic_federation(publisher.federation));
  FO_CHECK(!result.ok());
  FO_CHECK(result.error().code() == fo::ErrorCode::StaleEpoch);
}

FO_TEST(observatory, fenced_publisher_can_never_publish_again) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.register_federation(publisher.next(), basic_federation(publisher.federation)).ok());
  FO_REQUIRE(observatory.fence_publisher(publisher.id, publisher.boot, "test fence").ok());

  const fo::Result<fo::IngestResult> refused =
      observatory.register_site(publisher.next(),
                                basic_site(fo::SiteId::unchecked("site-1"), publisher.federation));
  FO_CHECK(!refused.ok());
  FO_CHECK(refused.error().code() == fo::ErrorCode::FencedPublisher);
  FO_CHECK(!observatory.publisher_is_live(publisher.id, publisher.boot));

  // Re-registering the same boot identity is refused as well.
  fo::PublicationContext again = publisher.next();
  const fo::Result<fo::IngestResult> reregister = observatory.register_publisher(again);
  FO_CHECK(!reregister.ok());
  FO_CHECK(reregister.error().code() == fo::ErrorCode::FencedPublisher);
}

FO_TEST(observatory, fresh_boot_identity_is_accepted_after_a_fence) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.fence_publisher(publisher.id, publisher.boot, "first boot died").ok());

  fixture::Publisher replacement;
  replacement.id = publisher.id;
  replacement.boot = fo::BootGeneration{2};
  replacement.federation = publisher.federation;
  FO_REQUIRE(observatory.register_publisher(replacement.next()).ok());
  FO_CHECK(observatory.publisher_is_live(replacement.id, replacement.boot));
  FO_REQUIRE(observatory.register_federation(replacement.next(),
                                             basic_federation(replacement.federation))
                 .ok());
}

FO_TEST(observatory, stale_cluster_generation_cannot_receive_current_evidence) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.register_federation(publisher.next(), basic_federation(publisher.federation)).ok());
  const fo::SiteId site = fo::SiteId::unchecked("site-1");
  FO_REQUIRE(observatory.register_site(publisher.next(), basic_site(site, publisher.federation)).ok());
  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = fo::AcceleratorClassId::unchecked("accel-1");
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  FO_REQUIRE(observatory.register_accelerator_class(publisher.next(), accelerator_class).ok());
  const fo::ClusterId cluster = fo::ClusterId::unchecked("cluster-1");
  fo::ClusterRecord record = basic_cluster(cluster, site, publisher.federation);
  record.generation = fo::ClusterGeneration{2};
  FO_REQUIRE(observatory.register_cluster(publisher.next(), record).ok());

  std::vector<fo::CapacityPool> pools;
  fo::CapacityPool pool;
  pool.pool_id = fo::ResourcePoolId::unchecked("pool-1");
  pool.kind = fo::ResourceKind::Accelerator;
  pool.accelerator_class = fo::AcceleratorClassId::unchecked("accel-1");
  pool.ledger = FO_UNWRAP(fo::CapacityLedger::from_components(4, 0, 0, 0, 0, 0));
  pools.push_back(pool);
  const fo::Result<fo::IngestResult> stale = observatory.publish_capacity(
      publisher.next(), cluster, fo::ClusterGeneration{1}, fo::CapacityGeneration{1},
      std::move(pools));
  FO_CHECK(!stale.ok());
  FO_CHECK(stale.error().code() == fo::ErrorCode::StaleGeneration);
}

FO_TEST(observatory, advancing_a_cluster_generation_clears_dynamic_evidence) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.register_federation(publisher.next(), basic_federation(publisher.federation)).ok());
  const fo::SiteId site = fo::SiteId::unchecked("site-1");
  FO_REQUIRE(observatory.register_site(publisher.next(), basic_site(site, publisher.federation)).ok());
  const fo::ClusterId cluster = fo::ClusterId::unchecked("cluster-1");
  fo::AcceleratorClassRecord accelerator_class;
  accelerator_class.id = fo::AcceleratorClassId::unchecked("accel-1");
  accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
  FO_REQUIRE(observatory.register_accelerator_class(publisher.next(), accelerator_class).ok());
  FO_REQUIRE(observatory.register_cluster(publisher.next(),
                                          basic_cluster(cluster, site, publisher.federation))
                 .ok());
  std::vector<fo::CapacityPool> pools;
  fo::CapacityPool pool;
  pool.pool_id = fo::ResourcePoolId::unchecked("pool-1");
  pool.kind = fo::ResourceKind::Accelerator;
  pool.accelerator_class = fo::AcceleratorClassId::unchecked("accel-1");
  pool.ledger = FO_UNWRAP(fo::CapacityLedger::from_components(4, 0, 0, 0, 0, 0));
  pools.push_back(pool);
  FO_REQUIRE(observatory.publish_capacity(publisher.next(), cluster, fo::ClusterGeneration{1},
                                          fo::CapacityGeneration{1}, std::move(pools))
                 .ok());
  // The snapshot handle must outlive every pointer taken from it; letting the temporary
  // shared_ptr die here would leave a dangling ClusterRecord.
  const fo::SnapshotHandle before_advance = observatory.snapshot();
  FO_REQUIRE(before_advance->find_cluster(cluster) != nullptr);
  FO_CHECK(before_advance->find_cluster(cluster)->capacity_pools.size() == 1);

  fo::ClusterRecord advanced = basic_cluster(cluster, site, publisher.federation);
  advanced.generation = fo::ClusterGeneration{2};
  advanced.epoch = fo::ClusterEpoch{2};
  FO_REQUIRE(observatory.register_cluster(publisher.next(), advanced).ok());
  const fo::SnapshotHandle after_advance = observatory.snapshot();
  const fo::ClusterRecord* stored = after_advance->find_cluster(cluster);
  FO_REQUIRE(stored != nullptr);
  FO_CHECK_EQ(stored->generation.value(), 2u);
  FO_CHECK(stored->capacity_pools.empty());
  FO_CHECK(stored->readiness == fo::Readiness::Unknown);
}

FO_TEST(observatory, capability_change_requires_a_new_generation) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(1, 1).ok());
  const fo::ClusterId cluster = world.clusters().front();

  fo::CapabilitySet first;
  fo::CapabilityEntry entry;
  entry.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  entry.value = fo::CapabilityValue::boolean(true);
  FO_REQUIRE(first.put(std::move(entry)).ok());
  FO_REQUIRE(observatory.publish_capability(world.publisher().next(), cluster,
                                            fo::ClusterGeneration{1},
                                            fo::AcceleratorCapabilityGeneration{2}, first)
                 .ok());

  fo::CapabilitySet changed;
  fo::CapabilityEntry other;
  other.key.key = fo::CapabilityKey::PrecisionFp8E4M3;
  other.value = fo::CapabilityValue::boolean(false);
  FO_REQUIRE(changed.put(std::move(other)).ok());
  const fo::Result<fo::IngestResult> same_generation = observatory.publish_capability(
      world.publisher().next(), cluster, fo::ClusterGeneration{1},
      fo::AcceleratorCapabilityGeneration{2}, changed);
  FO_CHECK(!same_generation.ok());
  FO_CHECK(same_generation.error().code() == fo::ErrorCode::Conflict);

  FO_REQUIRE(observatory.publish_capability(world.publisher().next(), cluster,
                                            fo::ClusterGeneration{1},
                                            fo::AcceleratorCapabilityGeneration{3}, changed)
                 .ok());

  fo::CapabilitySet stale;
  FO_REQUIRE(stale.put(std::move(entry)).ok());
  const fo::Result<fo::IngestResult> older = observatory.publish_capability(
      world.publisher().next(), cluster, fo::ClusterGeneration{1},
      fo::AcceleratorCapabilityGeneration{2}, stale);
  FO_CHECK(!older.ok());
  FO_CHECK(older.error().code() == fo::ErrorCode::StaleGeneration);
}

FO_TEST(observatory, placement_supersession_keeps_history_but_not_currentness) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(2, 1).ok());
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-1");
  FO_REQUIRE(observatory.register_workload_class(world.publisher().next(), workload_class).ok());
  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-1");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.required_accelerators = 1;
  FO_REQUIRE(observatory.register_workload(world.publisher().next(), workload).ok());

  const auto publish = [&](const char* id, const char* cluster, std::int64_t at) {
    fo::PlacementRecord placement;
    placement.id = fo::PlacementId::unchecked(id);
    placement.generation = fo::PlacementGeneration{1};
    placement.workload = workload.id;
    placement.workload_generation = fo::WorkloadGeneration{1};
    placement.federation = world.publisher().federation;
    placement.federation_generation = fo::FederationGeneration{1};
    placement.workload_class = workload_class.id;
    placement.selected = fo::ClusterId::unchecked(cluster);
    placement.selected_generation = fo::ClusterGeneration{1};
    placement.selected_epoch = fo::ClusterEpoch{1};
    placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
    fo::CandidateObservation observation;
    observation.cluster = placement.selected;
    observation.status = fo::CandidateStatus::Selected;
    observation.basis = fo::ReasonBasis::Observed;
    placement.candidates.push_back(observation);
    fo::PublicationContext context = world.publisher().next();
    context.observed_at = at;
    return observatory.publish_placement(context, placement);
  };

  FO_REQUIRE(publish("placement-new", "cluster-1", 5000).ok());
  const fo::Result<fo::IngestResult> older = publish("placement-old", "cluster-0", 1000);
  FO_REQUIRE(older.ok());
  FO_CHECK(older.value().disposition == fo::IngestDisposition::Superseded);

  const fo::SnapshotHandle snapshot = observatory.snapshot();
  FO_CHECK_EQ(snapshot->placements.size(), 2u);
  const fo::PlacementRecord* newer = snapshot->find_placement(fo::PlacementId::unchecked("placement-new"));
  const fo::PlacementRecord* older_record =
      snapshot->find_placement(fo::PlacementId::unchecked("placement-old"));
  FO_REQUIRE(newer != nullptr);
  FO_REQUIRE(older_record != nullptr);
  FO_CHECK(newer->currentness == fo::Currentness::Current);
  FO_CHECK(older_record->currentness == fo::Currentness::Stale);

  const fo::Result<fo::PlacementExplanation> latest =
      observatory.explain_latest_placement(workload.id);
  FO_REQUIRE(latest.ok());
  FO_CHECK_EQ(latest.value().selected.value(), std::string("cluster-1"));
}

FO_TEST(observatory, snapshot_is_immutable_while_publications_continue) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(2, 1).ok());
  const fo::SnapshotHandle before = observatory.snapshot();
  const std::size_t clusters_before = before->clusters.size();
  const fo::SnapshotGeneration generation_before = before->snapshot_generation;

  fo::SiteRecord extra;
  extra.id = fo::SiteId::unchecked("site-extra");
  extra.generation = fo::SiteGeneration{1};
  extra.federation = world.publisher().federation;
  FO_REQUIRE(observatory.register_site(world.publisher().next(), extra).ok());

  FO_CHECK_EQ(before->clusters.size(), clusters_before);
  FO_CHECK(before->snapshot_generation == generation_before);
  const fo::SnapshotHandle after = observatory.snapshot();
  FO_CHECK(after->snapshot_generation != generation_before);
  FO_CHECK(after->find_site(fo::SiteId::unchecked("site-extra")) != nullptr);
}

FO_TEST(observatory, health_census_tracks_currentness_and_publishers) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(2, 1).ok());
  {
    const fo::SnapshotHandle snapshot = observatory.snapshot();
    FO_CHECK_EQ(snapshot->health.clusters_total, 2u);
    FO_CHECK_EQ(snapshot->health.clusters_current, 2u);
    FO_CHECK_EQ(snapshot->health.publishers_live, 1u);
    FO_CHECK(!snapshot->health.degraded);
  }
  FO_REQUIRE(observatory.fence_publisher(world.publisher().id, world.publisher().boot, "fence").ok());
  {
    const fo::SnapshotHandle snapshot = observatory.snapshot();
    FO_CHECK_EQ(snapshot->health.clusters_current, 0u);
    FO_CHECK_EQ(snapshot->health.clusters_stale, 2u);
    FO_CHECK_EQ(snapshot->health.publishers_fenced, 1u);
    FO_CHECK(snapshot->health.degraded);
  }
}

FO_TEST(observatory, history_bounds_are_enforced_rather_than_silently_dropped) {
  fo::ObservatoryConfig config;
  config.bounds.max_placement_history = 1;
  fo::FederationObservatory observatory(config);
  fixture::World world(observatory);
  FO_REQUIRE(world.build(1, 1).ok());
  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-1");
  FO_REQUIRE(observatory.register_workload_class(world.publisher().next(), workload_class).ok());
  fo::WorkloadRecord workload;
  workload.id = fo::WorkloadId::unchecked("wl-1");
  workload.generation = fo::WorkloadGeneration{1};
  workload.workload_class = workload_class.id;
  workload.required_accelerators = 1;
  FO_REQUIRE(observatory.register_workload(world.publisher().next(), workload).ok());

  const auto publish = [&](const char* id) {
    fo::PlacementRecord placement;
    placement.id = fo::PlacementId::unchecked(id);
    placement.generation = fo::PlacementGeneration{1};
    placement.workload = workload.id;
    placement.workload_generation = fo::WorkloadGeneration{1};
    placement.federation = world.publisher().federation;
    placement.federation_generation = fo::FederationGeneration{1};
    placement.workload_class = workload_class.id;
    placement.selected = world.clusters().front();
    placement.selected_generation = fo::ClusterGeneration{1};
    placement.selected_epoch = fo::ClusterEpoch{1};
    placement.candidate_completeness = fo::CandidateSetCompleteness::SelectedOnly;
    fo::CandidateObservation observation;
    observation.cluster = placement.selected;
    observation.status = fo::CandidateStatus::Selected;
    observation.basis = fo::ReasonBasis::Observed;
    placement.candidates.push_back(observation);
    return observatory.publish_placement(world.publisher().next(), placement);
  };
  FO_REQUIRE(publish("placement-1").ok());
  const fo::Result<fo::IngestResult> second = publish("placement-2");
  FO_CHECK(!second.ok());
  FO_CHECK(second.error().code() == fo::ErrorCode::BoundExceeded);
  FO_CHECK_EQ(observatory.stats().bounds_rejections, 1u);
}

FO_TEST(observatory, invalidate_dynamic_evidence_marks_everything_for_revalidation) {
  fo::FederationObservatory observatory;
  fixture::World world(observatory);
  FO_REQUIRE(world.build(2, 1).ok());
  FO_REQUIRE(observatory.invalidate_dynamic_evidence("operator request").ok());
  const fo::SnapshotHandle snapshot = observatory.snapshot();
  FO_CHECK_EQ(snapshot->health.clusters_current, 0u);
  FO_CHECK_EQ(snapshot->health.clusters_revalidation_required, 2u);
  FO_CHECK(snapshot->health.revalidation_pending);
  FO_CHECK(snapshot->clusters.front().capacity_pools.empty());
}

FO_TEST(observatory, stats_account_for_every_disposition) {
  fo::FederationObservatory observatory;
  fixture::Publisher publisher;
  FO_REQUIRE(observatory.register_publisher(publisher.next()).ok());
  FO_REQUIRE(observatory.register_federation(publisher.next(), basic_federation(publisher.federation)).ok());
  FO_REQUIRE(observatory.register_federation(publisher.repeat(), basic_federation(publisher.federation)).ok());
  FO_REQUIRE(observatory.fence_publisher(publisher.id, publisher.boot, "fence").ok());
  const fo::Result<fo::IngestResult> refused =
      observatory.register_site(publisher.next(),
                                basic_site(fo::SiteId::unchecked("site-1"), publisher.federation));
  FO_CHECK(!refused.ok());
  const fo::ObservatoryStats stats = observatory.stats();
  FO_CHECK(stats.publications_applied >= 2);
  FO_CHECK(stats.publications_duplicate >= 1);
  FO_CHECK(stats.publications_fenced >= 1);
  FO_CHECK(stats.publisher_registrations == 1);
  FO_CHECK(stats.publisher_fences == 1);
  FO_CHECK(!stats.render().empty());
}

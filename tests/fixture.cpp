// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.

#include "fixture.hpp"

namespace fixture {

fo::CapabilitySet family_capabilities(std::size_t family) {
  const bool nvidia = family == 0;
  fo::CapabilitySet set;
  const auto put = [&set](fo::CapabilityKey key, fo::CapabilityValue value) {
    fo::CapabilityEntry entry;
    entry.key.key = key;
    entry.value = std::move(value);
    entry.precision = fo::Precision::Exact;
    entry.evidence_class = fo::EvidenceClass::Synthetic;
    const fo::Status status = set.put(std::move(entry));
    FO_REQUIRE(status.ok());
  };
  put(fo::CapabilityKey::AcceleratorArchitecture,
      fo::CapabilityValue::text(nvidia ? "sm_90" : "gfx942"));
  put(fo::CapabilityKey::ComputeCapability, fo::CapabilityValue::text(nvidia ? "9.0" : ""));
  put(fo::CapabilityKey::MemoryBytes,
      fo::CapabilityValue::unsigned_integer(nvidia ? 80 * kGiB : 192 * kGiB));
  put(fo::CapabilityKey::ArtifactFormats,
      FO_UNWRAP(fo::CapabilityValue::text_set(nvidia ? std::vector<std::string>{"cubin", "fatbin", "elf"}
                                                     : std::vector<std::string>{"hsaco", "elf"})));
  put(fo::CapabilityKey::KernelFormat, fo::CapabilityValue::text(nvidia ? "cubin" : "hsaco"));
  put(fo::CapabilityKey::CompilerTarget, fo::CapabilityValue::text(nvidia ? "sm_90" : "gfx942"));
  put(fo::CapabilityKey::RuntimeApi, fo::CapabilityValue::text(nvidia ? "CUDA" : "ROCM"));
  put(fo::CapabilityKey::OperatingSystem, fo::CapabilityValue::text("linux"));
  if (nvidia) {
    put(fo::CapabilityKey::PrecisionFp8E4M3, fo::CapabilityValue::boolean(true));
  }
  put(fo::CapabilityKey::PrecisionFp32, fo::CapabilityValue::boolean(true));
  return set;
}

fo::Status World::build(std::size_t cluster_count, std::size_t family_count) {
  require_applied(observatory_->register_publisher(publisher_.next()), "publisher registration");

  fo::FederationRecord federation;
  federation.id = publisher_.federation;
  federation.generation = fo::FederationGeneration{1};
  federation.display_name = "fixture-federation";
  federation.coordinator_epoch = fo::CoordinatorEpoch{1};
  require_applied(observatory_->register_federation(publisher_.next(), federation), "federation");

  const char* family_names[] = {"family-a", "family-b"};
  const char* family_architectures[] = {"sm_90", "gfx942"};
  const char* family_compute_capabilities[] = {"9.0", ""};
  const bool family_fp8[] = {true, false};

  for (std::size_t f = 0; f < family_count && f < 2; ++f) {
    fo::AcceleratorClassRecord accelerator_class;
    accelerator_class.id = fo::AcceleratorClassId::unchecked(std::string("accel-") +
                                                             family_names[f]);
    accelerator_class.capability_generation = fo::AcceleratorCapabilityGeneration{1};
    accelerator_class.vendor = f == 0 ? "nvidia" : "amd";
    accelerator_class.model = f == 0 ? "h100" : "mi300x";
    accelerator_class.architecture = family_architectures[f];
    accelerator_class.compute_capability = family_compute_capabilities[f];
    accelerator_class.memory_bytes_per_device = f == 0 ? 80 * kGiB : 192 * kGiB;
    accelerator_class.capabilities = family_capabilities(f);
    require_applied(observatory_->register_accelerator_class(publisher_.next(), accelerator_class),
                    "accelerator class");
    classes_.push_back(accelerator_class.id);

    fo::RuntimeRecord runtime;
    runtime.id = fo::RuntimeId::unchecked(std::string("runtime-") + family_names[f]);
    runtime.generation = fo::RuntimeGeneration{1};
    runtime.kind = f == 0 ? fo::RuntimeKind::Cuda : fo::RuntimeKind::Rocm;
    runtime.version = f == 0 ? "12.4.1" : "6.2.0";
    runtime.abi = f == 0 ? "cuda-12.4" : "rocm-6.2";
    runtime.driver_abi = f == 0 ? "550.54" : "6.2";
    require_applied(observatory_->register_runtime(publisher_.next(), runtime), "runtime");
    runtimes_.push_back(runtime.id);
  }

  const std::size_t site_count = cluster_count == 0 ? 1 : (cluster_count + 1) / 2;
  for (std::size_t s = 0; s < site_count; ++s) {
    fo::SiteRecord site;
    site.id = fo::SiteId::unchecked("site-" + std::to_string(s));
    site.generation = fo::SiteGeneration{1};
    site.federation = publisher_.federation;
    site.region = "region-" + std::to_string(s);
    require_applied(observatory_->register_site(publisher_.next(), site), "site");
    sites_.push_back(site.id);
  }

  for (std::size_t i = 0; i < cluster_count; ++i) {
    fo::ClusterRecord cluster;
    cluster.id = fo::ClusterId::unchecked("cluster-" + std::to_string(i));
    cluster.generation = fo::ClusterGeneration{1};
    cluster.epoch = fo::ClusterEpoch{1};
    cluster.federation = publisher_.federation;
    cluster.site = sites_[i % sites_.size()];
    cluster.failure_domain = "fd-" + std::to_string(i % site_count);
    cluster.accelerator_classes = {classes_[i % classes_.size()]};
    cluster.runtimes = {runtimes_[i % runtimes_.size()]};
    cluster.topology_generation = fo::TopologyGeneration{1};
    cluster.capability_generation = fo::AcceleratorCapabilityGeneration{1};
    cluster.capacity_generation = fo::CapacityGeneration{1};
    cluster.runtime_generation = fo::RuntimeGeneration{1};
    cluster.compatibility_generation = fo::CompatibilityGeneration{1};
    cluster.evidence_generation = fo::EvidenceGeneration{1};
    cluster.readiness = fo::Readiness::Ready;
    cluster.capabilities = family_capabilities(i % classes_.size());
    require_applied(observatory_->register_cluster(publisher_.next(), cluster), "cluster");
    clusters_.push_back(cluster.id);

    std::vector<fo::CapacityPool> pools;
    fo::CapacityPool pool;
    pool.pool_id = fo::ResourcePoolId::unchecked("pool-" + std::to_string(i));
    pool.kind = fo::ResourceKind::Accelerator;
    pool.accelerator_class = classes_[i % classes_.size()];
    pool.generation = fo::CapacityGeneration{1};
    const fo::Result<fo::CapacityLedger> ledger = fo::CapacityLedger::from_components(4, 0, 0, 0, 0, 0);
    FO_REQUIRE(ledger.ok());
    pool.ledger = ledger.value();
    pool.precision = fo::Precision::Exact;
    pool.evidence_class = fo::EvidenceClass::Synthetic;
    pools.push_back(std::move(pool));
    require_applied(observatory_->publish_capacity(publisher_.next(), cluster.id,
                                                    fo::ClusterGeneration{1},
                                                    fo::CapacityGeneration{1}, std::move(pools)),
                    "capacity");
  }

  (void)family_fp8;
  return fo::Status::success();
}

}  // namespace fixture

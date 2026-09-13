// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// REAL hardware validation and controlled real artifact compatibility proof. Everything
// asserted here was observed on this host; when the host cannot demonstrate something,
// the test says so instead of inventing a fact.

#include <cstdio>
#include <filesystem>
#include <string>

#include "federation_observatory/hardware.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/process.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

std::string temporary_directory() { return std::filesystem::temp_directory_path().string(); }

struct TemporaryFile {
  std::string path;
  explicit TemporaryFile(const char* name)
      : path(join_path(temporary_directory(), std::string("fo-hw-") + name + "-" + unique_token())) {}
  ~TemporaryFile() {
    const Status removed = remove_file_if_present(path);
    (void)removed;
  }
  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile& operator=(const TemporaryFile&) = delete;
};

}  // namespace

FO_TEST(hardware, real_host_facts_are_discovered) {
  const fo::HostInventory inventory = FO_UNWRAP(fo::probe_host_inventory());
  FO_CHECK(!inventory.host.os_name.empty());
  FO_CHECK(!inventory.host.cpu_architecture.empty());
  FO_CHECK(inventory.host.logical_cores > 0);
  FO_CHECK(inventory.host.total_memory_bytes > 0);
  FO_CHECK(inventory.host.evidence_class == fo::EvidenceClass::Real);
  FO_CHECK(inventory.host.precision == fo::Precision::Exact);

  // The runtime must never claim a physical federation from one machine.
  bool mentions_single_machine = false;
  bool mentions_heterogeneous = false;
  for (const std::string& claim : inventory.unsupported_claims) {
    if (claim.find("single machine") != std::string::npos) {
      mentions_single_machine = true;
    }
    if (claim.find("heterogeneous physical accelerator federation") != std::string::npos) {
      mentions_heterogeneous = true;
    }
  }
  FO_CHECK(mentions_single_machine);
  FO_CHECK(mentions_heterogeneous);
  FO_CHECK(!inventory.render().empty());
}

FO_TEST(hardware, real_accelerator_facts_when_a_device_is_present) {
  const fo::HostInventory inventory = FO_UNWRAP(fo::probe_host_inventory());
  if (inventory.accelerators.empty()) {
    // Nothing to assert about an accelerator that is not present. The inventory says so.
    FO_CHECK(!inventory.unsupported_claims.empty());
    return;
  }
  for (const fo::AcceleratorFacts& accelerator : inventory.accelerators) {
    FO_CHECK(!accelerator.model.empty());
    FO_CHECK(accelerator.memory_bytes > 0);
    FO_CHECK(!accelerator.compute_capability.empty());
    FO_CHECK(!accelerator.architecture.empty());
    FO_CHECK(accelerator.architecture.rfind("sm_", 0) == 0);
    FO_CHECK(accelerator.evidence_class == fo::EvidenceClass::Real);
    FO_CHECK(accelerator.precision == fo::Precision::Exact);
    FO_CHECK(!accelerator.driver_version.empty());
  }

  const std::vector<fo::AcceleratorClassRecord> classes = inventory.as_accelerator_classes();
  FO_CHECK(!classes.empty());
  for (const fo::AcceleratorClassRecord& accelerator_class : classes) {
    FO_CHECK(!accelerator_class.architecture.empty());
    FO_CHECK(accelerator_class.memory_bytes_per_device > 0);
    FO_CHECK(accelerator_class.stamp.evidence_class == fo::EvidenceClass::Real);
    fo::CapabilityRef architecture;
    architecture.key = fo::CapabilityKey::AcceleratorArchitecture;
    FO_CHECK(accelerator_class.capabilities.contains(architecture));
  }

  const fo::ClusterRecord cluster = inventory.as_cluster(
      fo::ClusterId::unchecked("host-cluster"), fo::SiteId::unchecked("host-site"),
      fo::FederationId::unchecked("host-federation"));
  FO_CHECK(!cluster.capacity_pools.empty());
  const fo::Result<fo::CapacityLedger> ledger = cluster.total_ledger(fo::ResourceKind::Accelerator);
  FO_REQUIRE(ledger.ok());
  FO_CHECK_EQ(ledger.value().nominal,
              static_cast<std::uint64_t>(inventory.accelerators.size()));
  FO_CHECK(cluster.stamp.evidence_class == fo::EvidenceClass::Real);
}

FO_TEST(hardware, real_toolchain_facts_when_a_toolchain_is_present) {
  const fo::HostInventory inventory = FO_UNWRAP(fo::probe_host_inventory());
  if (!inventory.toolchain.nvcc_present) {
    return;
  }
  FO_CHECK(!inventory.toolchain.nvcc_path.empty());
  FO_CHECK(!inventory.toolchain.nvcc_version.empty());
  FO_CHECK(!inventory.toolchain.supported_architectures.empty());
  FO_CHECK(inventory.toolchain.evidence_class == fo::EvidenceClass::Real);
  FO_CHECK(inventory.toolchain.supports_architecture("50"));
}

FO_TEST(hardware, real_cuda_artifact_compatibility_proof) {
  const fo::HostInventory inventory = FO_UNWRAP(fo::probe_host_inventory());
  if (inventory.accelerators.empty() || !inventory.toolchain.nvcc_present) {
    // The environment cannot provide real device images. Report that rather than
    // substituting a synthetic claim for a real one.
    const std::string note =
        "SKIPPED: no local accelerator or no CUDA toolchain is present on this host";
    FO_CHECK(!note.empty());
    return;
  }
  // nvcc needs a host C++ compiler on PATH. Without one the real device-image proof cannot
  // run at all, and the runtime says so instead of substituting a synthetic claim.
  const Result<std::string> host_compiler = fo::run_tool_capture({"where", "cl"});
  if (!host_compiler.ok()) {
    std::printf(
        "SKIPPED: nvcc is present but no host C++ compiler (cl.exe) is on PATH, so the REAL "
        "device-image proof cannot run here. Run the suite from a developer command prompt "
        "to execute it.\n");
    return;
  }
  const std::string device_architecture = inventory.accelerators.front().architecture;
  const std::string device_capability = inventory.accelerators.front().compute_capability;
  FO_REQUIRE(!device_architecture.empty());
  FO_REQUIRE(inventory.toolchain.supports_architecture(device_architecture.substr(3)));

  TemporaryFile matching("cubin-matching");
  TemporaryFile incompatible("cubin-incompatible");
  const std::string source =
      "extern \"C\" __global__ void federation_observatory_probe(float* out) {\n"
      "  out[threadIdx.x] = 1.0f;\n"
      "}\n";

  const Status compiled_matching =
      fo::compile_cuda_object(source, device_architecture, matching.path);
  if (!compiled_matching.ok()) {
    ::fotest::fail(__FILE__, __LINE__,
                   std::string("nvcc could not build a real device image for ") +
                       device_architecture + ": " + compiled_matching.error().to_string());
  }

  // A deliberately incompatible target: an architecture this device is not.
  const std::string other_architecture = device_architecture == "sm_75" ? "sm_80" : "sm_75";
  const Status compiled_other =
      fo::compile_cuda_object(source, other_architecture, incompatible.path);
  FO_REQUIRE(compiled_other.ok());

  const fo::ArtifactInspection matching_inspection = FO_UNWRAP(fo::inspect_artifact(matching.path));
  const fo::ArtifactInspection other_inspection = FO_UNWRAP(fo::inspect_artifact(incompatible.path));
  FO_CHECK(matching_inspection.is_elf);
  FO_CHECK(matching_inspection.is_cuda_elf);
  FO_CHECK_EQ(matching_inspection.elf_machine, 190);
  FO_CHECK(matching_inspection.precision == fo::Precision::Exact);
  FO_CHECK(matching_inspection.targets(device_architecture));
  FO_CHECK(!other_inspection.targets(device_architecture));
  FO_CHECK(other_inspection.targets(other_architecture));
  FO_CHECK(matching_inspection.content_digest != other_inspection.content_digest);
  FO_CHECK(matching_inspection.evidence_class == fo::EvidenceClass::Real);

  // Feed the two REAL artifacts through the real analysis pipeline.
  fo::FederationObservatory observatory;
  fo::PublicationContext context;
  context.publisher = fo::PublisherId::unchecked("host-probe");
  context.boot = fo::BootGeneration{1};
  context.coordinator_epoch = observatory.coordinator_epoch();
  context.federation = fo::FederationId::unchecked("host-federation");
  context.federation_generation = fo::FederationGeneration{1};
  context.observed_at = fo::now_unix_nanos();
  context.precision = fo::Precision::Exact;
  context.evidence_class = fo::EvidenceClass::Real;
  context.provenance = fo::Provenance::ArtifactInspection;
  context.sequence = fo::Sequence{1};
  FO_REQUIRE(observatory.register_publisher(context).ok());

  fo::FederationRecord federation;
  federation.id = context.federation;
  federation.generation = fo::FederationGeneration{1};
  federation.coordinator_epoch = fo::CoordinatorEpoch{1};
  context.sequence = fo::Sequence{2};
  FO_REQUIRE(observatory.register_federation(context, federation).ok());

  fo::ClusterRecord cluster = inventory.as_cluster(fo::ClusterId::unchecked("host-cluster"),
                                                   fo::SiteId::unchecked("host-site"),
                                                   context.federation);
  context.sequence = fo::Sequence{3};
  FO_REQUIRE(observatory.register_accelerator_class(context, inventory.as_accelerator_classes().front()).ok());
  fo::SiteRecord site;
  site.id = fo::SiteId::unchecked("host-site");
  site.generation = fo::SiteGeneration{1};
  site.federation = context.federation;
  context.sequence = fo::Sequence{4};
  FO_REQUIRE(observatory.register_site(context, site).ok());
  context.sequence = fo::Sequence{5};
  FO_REQUIRE(observatory.register_cluster(context, cluster).ok());

  fo::WorkloadClassRecord workload_class;
  workload_class.id = fo::WorkloadClassId::unchecked("wc-real");
  context.sequence = fo::Sequence{6};
  FO_REQUIRE(observatory.register_workload_class(context, workload_class).ok());

  const auto register_artifact = [&](const char* id, const fo::ArtifactInspection& inspection,
                                     std::uint64_t sequence) {
    fo::ArtifactRecord artifact;
    artifact.id = fo::ArtifactId::unchecked(id);
    artifact.generation = fo::ArtifactGeneration{1};
    artifact.kind = fo::ArtifactKind::DeviceImage;
    artifact.format = inspection.format;
    artifact.kernel_format = inspection.kernel_format;
    artifact.target_architectures = inspection.target_architectures;
    artifact.size_bytes = inspection.size_bytes;
    artifact.stamp.precision = inspection.precision;
    artifact.stamp.evidence_class = fo::EvidenceClass::Real;
    artifact.stamp.provenance = fo::Provenance::ArtifactInspection;
    artifact.evidence = inspection.evidence;
    context.sequence = fo::Sequence{sequence};
    return observatory.register_artifact(context, artifact);
  };
  FO_REQUIRE(register_artifact("artifact-real-matching", matching_inspection, 7).ok());
  FO_REQUIRE(register_artifact("artifact-real-incompatible", other_inspection, 8).ok());

  fo::WorkloadRecord matching_workload;
  matching_workload.id = fo::WorkloadId::unchecked("wl-real-matching");
  matching_workload.generation = fo::WorkloadGeneration{1};
  matching_workload.workload_class = workload_class.id;
  matching_workload.artifact = fo::ArtifactId::unchecked("artifact-real-matching");
  matching_workload.artifact_generation = fo::ArtifactGeneration{1};
  matching_workload.required_accelerators = 1;
  context.sequence = fo::Sequence{9};
  FO_REQUIRE(observatory.register_workload(context, matching_workload).ok());

  fo::WorkloadRecord other_workload = matching_workload;
  other_workload.id = fo::WorkloadId::unchecked("wl-real-incompatible");
  other_workload.artifact = fo::ArtifactId::unchecked("artifact-real-incompatible");
  context.sequence = fo::Sequence{10};
  FO_REQUIRE(observatory.register_workload(context, other_workload).ok());

  const fo::CompatibilityAssessment matching_assessment = FO_UNWRAP(
      observatory.compatibility(fo::ClusterId::unchecked("host-cluster"), matching_workload.id));
  FO_CHECK(matching_assessment.overall == fo::CompatibilityOutcome::Compatible);
  FO_CHECK(matching_assessment.evidence_class == fo::EvidenceClass::Real);
  FO_CHECK(matching_assessment.admits_placement());

  const fo::CompatibilityAssessment other_assessment = FO_UNWRAP(
      observatory.compatibility(fo::ClusterId::unchecked("host-cluster"), other_workload.id));
  FO_CHECK(other_assessment.overall == fo::CompatibilityOutcome::IncompatibleArchitecture);
  FO_CHECK(other_assessment.evidence_class == fo::EvidenceClass::Real);
  FO_CHECK(!other_assessment.admits_placement());

  std::printf(
      "REAL artifact proof: device=%s (%s); matching artifact targets %s; incompatible artifact "
      "targets %s; verdicts %s / %s\n",
      device_architecture.c_str(), device_capability.c_str(),
      matching_inspection.target_architectures.empty()
          ? "?"
          : matching_inspection.target_architectures.front().c_str(),
      other_inspection.target_architectures.empty()
          ? "?"
          : other_inspection.target_architectures.front().c_str(),
      std::string(fo::to_string(matching_assessment.overall)).c_str(),
      std::string(fo::to_string(other_assessment.overall)).c_str());
}

FO_TEST(hardware, non_elf_artifact_reports_unknown_architecture) {
  TemporaryFile plain("plain");
  FO_REQUIRE(write_text_file(plain.path, "this is not an object file\n").ok());
  const fo::ArtifactInspection inspection = FO_UNWRAP(fo::inspect_artifact(plain.path));
  FO_CHECK(inspection.exists);
  FO_CHECK(inspection.readable);
  FO_CHECK(!inspection.is_elf);
  FO_CHECK(!inspection.is_cuda_elf);
  FO_CHECK(inspection.target_architectures.empty());
  FO_CHECK(inspection.precision == fo::Precision::Unknown);
  FO_CHECK_EQ(inspection.format, std::string("unknown"));
}

FO_TEST(hardware, missing_artifact_is_reported) {
  const fo::Result<fo::ArtifactInspection> inspection =
      fo::inspect_artifact(join_path(temporary_directory(), "definitely-not-here-" + unique_token()));
  FO_CHECK(!inspection.ok());
  FO_CHECK(inspection.error().code() == fo::ErrorCode::NotFound);
}

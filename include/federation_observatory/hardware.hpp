// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "federation_observatory/errors.hpp"
#include "federation_observatory/evidence.hpp"
#include "federation_observatory/export.hpp"
#include "federation_observatory/model.hpp"

namespace fo {

/// Real facts about the machine this runtime is executing on. Only facts that were
/// actually observed are populated; everything else stays empty and is rendered as
/// UNKNOWN.
struct FO_API HostFacts {
  std::string os_name;
  std::string os_version;
  std::string hostname;
  std::string cpu_architecture;
  std::string cpu_model;
  std::uint32_t physical_cores = 0;
  std::uint32_t logical_cores = 0;
  std::uint32_t numa_nodes = 0;
  std::uint64_t total_memory_bytes = 0;
  std::vector<std::string> network_interfaces;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceList evidence;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Real facts about one locally present accelerator.
struct FO_API AcceleratorFacts {
  std::string vendor;
  std::string model;
  std::string uuid;
  std::uint64_t memory_bytes = 0;
  std::string compute_capability;
  /// Vendor architecture token derived from the compute capability, e.g. "sm_120".
  std::string architecture;
  std::string driver_version;
  std::string driver_model;
  std::string runtime_version;
  std::string pcie_generation;
  std::string pcie_width;
  bool ecc_enabled = false;
  bool ecc_known = false;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceList evidence;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// Real facts about the locally installed accelerator toolchain.
struct FO_API ToolchainFacts {
  bool nvcc_present = false;
  std::string nvcc_path;
  std::string nvcc_version;
  std::vector<std::string> supported_architectures;
  bool cuobjdump_present = false;
  std::string cuobjdump_path;
  bool cuda_toolkit_present = false;
  std::string cuda_toolkit_version;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceList evidence;

  [[nodiscard]] bool supports_architecture(std::string_view architecture) const;
  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
};

/// The complete real inventory of this machine.
struct FO_API HostInventory {
  HostFacts host;
  std::vector<AcceleratorFacts> accelerators;
  ToolchainFacts toolchain;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceList evidence;
  /// Explicit statement of what this machine cannot demonstrate.
  std::vector<std::string> unsupported_claims;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;

  /// Build a REAL single-cluster record from these facts. The result never claims a
  /// heterogeneous physical federation: every accelerator present on this host belongs
  /// to one cluster record, and unsupported claims are enumerated.
  [[nodiscard]] ClusterRecord as_cluster(const ClusterId& cluster, const SiteId& site,
                                         const FederationId& federation) const;

  /// Build REAL accelerator-class records for the accelerators present.
  [[nodiscard]] std::vector<AcceleratorClassRecord> as_accelerator_classes() const;
};

/// Probe the local machine. Returns an inventory containing only observed facts.
[[nodiscard]] FO_API Result<HostInventory> probe_host_inventory();

/// Real inspection of one artifact file.
struct FO_API ArtifactInspection {
  std::string path;
  std::uint64_t size_bytes = 0;
  std::string content_digest;
  bool exists = false;
  bool readable = false;
  bool is_elf = false;
  std::uint16_t elf_machine = 0;
  /// True when the ELF machine type is EM_CUDA (190).
  bool is_cuda_elf = false;
  /// Architectures that the artifact was built for, read from the artifact itself or
  /// from the toolchain that produced it. Empty means "could not be determined".
  std::vector<std::string> target_architectures;
  std::string format;
  std::string kernel_format;
  Precision precision = Precision::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceList evidence;
  /// Tool output used to determine the facts, retained for auditability.
  std::string tool_output;

  [[nodiscard]] std::string render(std::string_view indent = "  ") const;
  [[nodiscard]] bool targets(std::string_view architecture) const;
};

/// Inspect a real artifact file on disk. Never fabricates architecture facts: when the
/// architecture cannot be read, `target_architectures` is empty and precision is
/// Unknown.
[[nodiscard]] FO_API Result<ArtifactInspection> inspect_artifact(const std::string& path);

/// Compile \p source_text with the local nvcc for \p architecture and write the device
/// image to \p output_path. Returns UnsupportedOperation when no toolchain is present.
[[nodiscard]] FO_API Status compile_cuda_object(const std::string& source_text,
                                                const std::string& architecture,
                                                const std::string& output_path);

/// Run \p argv and capture its standard output. Returns UnsupportedOperation when the
/// program is absent. Used for real toolchain interrogation only.
[[nodiscard]] FO_API Result<std::string> run_tool_capture(const std::vector<std::string>& argv);

}  // namespace fo

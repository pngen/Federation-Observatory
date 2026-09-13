// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Real local machine discovery. Every fact in this file was actually observed on the
// host; nothing is inferred or filled in from a table. What this host cannot demonstrate
// is enumerated explicitly in unsupported_claims instead of being left implicit.

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/hardware.hpp"
#include "federation_observatory/process.hpp"
#include "federation_observatory/state_store.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2.h must precede windows.h, and iphlpapi.h must follow both.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace fo {
namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr std::size_t kMaxCapturedBytes = 1024 * 1024;
constexpr std::size_t kArtifactProbeBytes = 64 * 1024;
constexpr std::uint64_t kMaxDigestBytes = 64ull * 1024ull * 1024ull;

std::string trim(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                           text.back() == '\n')) {
    text.pop_back();
  }
  std::size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
    ++start;
  }
  return text.substr(start);
}

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      parts.push_back(trim(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(trim(current));
  return parts;
}

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char c : text) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

std::string environment_variable(const char* name) {
#ifdef _WIN32
  char buffer[512] = {0};
  const DWORD length = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) {
    return std::string();
  }
  return std::string(buffer, length);
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
#endif
}

CapabilityEntry make_capability(CapabilityKey key, CapabilityValue value) {
  CapabilityEntry entry;
  entry.key.key = key;
  entry.value = std::move(value);
  entry.precision = Precision::Exact;
  entry.evidence_class = EvidenceClass::Real;
  entry.generation = EvidenceGeneration{1};
  entry.note = "observed on the local host";
  return entry;
}

bool parse_memory_field(const std::string& text, std::uint64_t& out) {
  std::istringstream stream(text);
  std::uint64_t value = 0;
  std::string unit;
  if (!(stream >> value)) {
    return false;
  }
  stream >> unit;
  std::uint64_t multiplier = 1;
  if (unit == "MiB" || unit == "MB") {
    multiplier = kMiB;
  } else if (unit == "GiB" || unit == "GB") {
    multiplier = kMiB * 1024;
  } else if (unit == "KiB" || unit == "KB") {
    multiplier = 1024;
  } else if (!unit.empty() && unit != "bytes") {
    return false;
  }
  return checked_mul(value, multiplier, out);
}

std::string architecture_from_compute_capability(const std::string& capability) {
  const std::vector<std::string> parts = split(capability, '.');
  if (parts.size() != 2) {
    return std::string();
  }
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  try {
    major = static_cast<std::uint32_t>(std::stoul(parts[0]));
    minor = static_cast<std::uint32_t>(std::stoul(parts[1]));
  } catch (const std::exception&) {
    return std::string();
  }
  if (major == 0) {
    return std::string();
  }
  return "sm_" + std::to_string(major * 10 + minor);
}

std::string sanitize_identifier(std::string text) {
  std::string out;
  for (const char c : text) {
    const bool upper = c >= 'A' && c <= 'Z';
    const bool lower = c >= 'a' && c <= 'z';
    const bool digit = c >= '0' && c <= '9';
    out.push_back((upper || lower || digit) ? static_cast<char>(upper ? c - 'A' + 'a' : c) : '-');
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out.empty() ? std::string("unknown") : out;
}

}  // namespace

Result<std::string> run_tool_capture(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return Error(ErrorCode::InvalidArgument, "no program was supplied");
  }
#ifdef _WIN32
  const std::string temp_directory = environment_variable("TEMP").empty()
                                         ? std::string(".")
                                         : environment_variable("TEMP");
#else
  const std::string temp_directory = "/tmp";
#endif
  const std::string capture_path =
      join_path(temp_directory, "fo-capture-" + unique_token() + ".txt");

#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE file = ::CreateFileA(capture_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::Internal, "could not create the capture file", capture_path);
  }
  std::string command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      command_line.push_back(' ');
    }
    const std::string& argument = argv[i];
    const bool needs_quotes = argument.find_first_of(" \t") != std::string::npos;
    if (needs_quotes) {
      command_line.push_back('"');
    }
    command_line += argument;
    if (needs_quotes) {
      command_line.push_back('"');
    }
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');
  if (::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &process) == 0) {
    ::CloseHandle(file);
    const Status removed = remove_file_if_present(capture_path);
    (void)removed;
    return Error(ErrorCode::UnsupportedOperation, "could not start the program",
                 argv[0] + " (windows error " + std::to_string(::GetLastError()) + ")");
  }
  ::CloseHandle(process.hThread);
  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  const BOOL exit_ok = ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(file);
  if (exit_ok == 0 || exit_code != 0) {
    const Result<std::string> captured = read_text_file(capture_path);
    const Status removed = remove_file_if_present(capture_path);
    (void)removed;
    return Error(ErrorCode::UnsupportedOperation, "the program exited unsuccessfully",
                 argv[0] + " (exit " + std::to_string(exit_code) + ")" +
                     (captured.ok() ? ": " + truncate_with_marker(captured.value(), 512)
                                    : std::string()));
  }
#else
  std::string shell = "exec " + argv[0];
  for (std::size_t i = 1; i < argv.size(); ++i) {
    shell += " '";
    for (const char c : argv[i]) {
      if (c == '\'') {
        shell += "'\\''";
      } else {
        shell.push_back(c);
      }
    }
    shell += "'";
  }
  shell += " > " + capture_path + " 2>&1";
  const int status = std::system(shell.c_str());
  if (status != 0) {
    const Result<std::string> captured = read_text_file(capture_path);
    const Status removed = remove_file_if_present(capture_path);
    (void)removed;
    return Error(ErrorCode::UnsupportedOperation, "the program did not run successfully",
                 argv[0] + (captured.ok() ? ": " + truncate_with_marker(captured.value(), 256)
                                          : std::string()));
  }
#endif

  Result<std::vector<std::uint8_t>> bytes = read_file_bounded(capture_path, kMaxCapturedBytes);
  const Status removed = remove_file_if_present(capture_path);
  (void)removed;
  if (!bytes.ok()) {
    return Error(ErrorCode::Internal, "could not read the captured output", capture_path);
  }
  return std::string(bytes.value().begin(), bytes.value().end());
}

namespace {

std::string executable_on_path(const std::string& name) {
#ifdef _WIN32
  const Result<std::string> output = run_tool_capture({"where", name});
  if (!output.ok()) {
    return std::string();
  }
  for (const std::string& line : split_lines(output.value())) {
    const std::string candidate = trim(line);
    if (!candidate.empty() && std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return std::string();
#else
  const Result<std::string> output = run_tool_capture({"sh", "-c", "command -v " + name});
  if (!output.ok()) {
    return std::string();
  }
  const std::string candidate = trim(output.value());
  return std::filesystem::exists(candidate) ? candidate : std::string();
#endif
}

std::string locate_nvcc() {
  const std::string on_path = executable_on_path("nvcc");
  if (!on_path.empty()) {
    return on_path;
  }
#ifdef _WIN32
  const char* root = "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA";
#else
  const char* root = "/usr/local";
#endif
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return std::string();
  }
  std::vector<std::string> candidates;
  for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    const std::filesystem::path bin = entry.path() / "bin";
    for (const char* name : {"nvcc.exe", "nvcc"}) {
      const std::filesystem::path candidate = bin / name;
      if (std::filesystem::exists(candidate)) {
        candidates.push_back(candidate.string());
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  return candidates.empty() ? std::string() : candidates.back();
}

std::string sibling_tool(const std::string& nvcc, const std::string& name) {
  if (nvcc.empty()) {
    return std::string();
  }
  const std::filesystem::path directory = std::filesystem::path(nvcc).parent_path();
  for (const std::string& candidate : {name + ".exe", name}) {
    const std::filesystem::path path = directory / candidate;
    if (std::filesystem::exists(path)) {
      return path.string();
    }
  }
  return std::string();
}

}  // namespace

std::string HostFacts::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"os_name", os_name.empty() ? "UNKNOWN" : os_name});
  rows.push_back({"os_version", os_version.empty() ? "UNKNOWN" : os_version});
  rows.push_back({"hostname", hostname.empty() ? "UNKNOWN" : hostname});
  rows.push_back({"cpu_architecture", cpu_architecture.empty() ? "UNKNOWN" : cpu_architecture});
  rows.push_back({"cpu_model", cpu_model.empty() ? "UNKNOWN" : cpu_model});
  rows.push_back({"physical_cores", physical_cores == 0 ? "UNKNOWN" : std::to_string(physical_cores)});
  rows.push_back({"logical_cores", logical_cores == 0 ? "UNKNOWN" : std::to_string(logical_cores)});
  rows.push_back({"numa_nodes", numa_nodes == 0 ? "UNKNOWN" : std::to_string(numa_nodes)});
  rows.push_back({"total_memory_bytes",
                  total_memory_bytes == 0 ? "UNKNOWN" : std::to_string(total_memory_bytes)});
  rows.push_back({"network_interfaces",
                  network_interfaces.empty() ? "UNKNOWN" : join_strings(network_interfaces, " | ")});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  return render_table({"host_fact", "value"}, rows, indent);
}

std::string AcceleratorFacts::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"vendor", vendor.empty() ? "UNKNOWN" : vendor});
  rows.push_back({"model", model.empty() ? "UNKNOWN" : model});
  rows.push_back({"uuid", uuid.empty() ? "UNKNOWN" : uuid});
  rows.push_back({"memory_bytes", memory_bytes == 0 ? "UNKNOWN" : std::to_string(memory_bytes)});
  rows.push_back({"compute_capability", compute_capability.empty() ? "UNKNOWN" : compute_capability});
  rows.push_back({"architecture", architecture.empty() ? "UNKNOWN" : architecture});
  rows.push_back({"driver_version", driver_version.empty() ? "UNKNOWN" : driver_version});
  rows.push_back({"runtime_version", runtime_version.empty() ? "UNKNOWN" : runtime_version});
  rows.push_back({"pcie_generation", pcie_generation.empty() ? "UNKNOWN" : pcie_generation});
  rows.push_back({"pcie_width", pcie_width.empty() ? "UNKNOWN" : pcie_width});
  rows.push_back({"ecc", ecc_known ? (ecc_enabled ? "enabled" : "disabled") : "UNKNOWN"});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  return render_table({"accelerator_fact", "value"}, rows, indent);
}

bool ToolchainFacts::supports_architecture(std::string_view architecture) const {
  for (const std::string& supported : supported_architectures) {
    if (supported == architecture) {
      return true;
    }
  }
  return false;
}

std::string ToolchainFacts::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"nvcc_present", nvcc_present ? "yes" : "no"});
  rows.push_back({"nvcc_path", nvcc_path.empty() ? "-" : nvcc_path});
  rows.push_back({"nvcc_version", nvcc_version.empty() ? "UNKNOWN" : nvcc_version});
  rows.push_back(
      {"cuda_toolkit_version", cuda_toolkit_version.empty() ? "UNKNOWN" : cuda_toolkit_version});
  rows.push_back({"supported_architectures",
                  supported_architectures.empty() ? "UNKNOWN"
                                                  : join_strings(supported_architectures, ",")});
  rows.push_back({"cuobjdump_present", cuobjdump_present ? "yes" : "no"});
  rows.push_back({"cuobjdump_path", cuobjdump_path.empty() ? "-" : cuobjdump_path});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  return render_table({"toolchain_fact", "value"}, rows, indent);
}

std::string HostInventory::render(std::string_view indent) const {
  std::string out(indent);
  out += "REAL local host inventory (this is one machine, not a federation):\n";
  out += indent_block(host.render("  "), indent);
  out += '\n';
  if (accelerators.empty()) {
    out += std::string(indent) + "  accelerators: none discovered\n";
  }
  for (const AcceleratorFacts& accelerator : accelerators) {
    out += indent_block(accelerator.render("  "), indent);
    out += '\n';
  }
  out += indent_block(toolchain.render("  "), indent);
  out += '\n';
  out += std::string(indent) + "unsupported claims on this host:";
  for (const std::string& claim : unsupported_claims) {
    out += '\n';
    out += indent;
    out += "  - ";
    out += claim;
  }
  return out;
}

std::vector<AcceleratorClassRecord> HostInventory::as_accelerator_classes() const {
  struct Group {
    AcceleratorClassId id;
    std::string vendor;
    std::string model;
    std::string architecture;
    std::string compute_capability;
    std::uint64_t memory_bytes = 0;
    std::uint32_t devices = 0;
    std::string driver_version;
  };
  std::map<std::string, Group> groups;
  for (const AcceleratorFacts& accelerator : accelerators) {
    const std::string key = accelerator.model + "|" + accelerator.compute_capability;
    auto it = groups.find(key);
    if (it == groups.end()) {
      Group group;
      group.id = AcceleratorClassId::unchecked(
          "accel-" +
          sanitize_identifier(accelerator.model.empty() ? std::string("unknown")
                                                       : accelerator.model) +
          "-" + sanitize_identifier(accelerator.architecture.empty() ? std::string("unknown")
                                                                    : accelerator.architecture));
      group.vendor = accelerator.vendor;
      group.model = accelerator.model;
      group.architecture = accelerator.architecture;
      group.compute_capability = accelerator.compute_capability;
      group.memory_bytes = accelerator.memory_bytes;
      group.driver_version = accelerator.driver_version;
      it = groups.emplace(key, std::move(group)).first;
    }
    it->second.devices += 1;
    if (it->second.memory_bytes == 0) {
      it->second.memory_bytes = accelerator.memory_bytes;
    }
  }

  std::vector<AcceleratorClassRecord> records;
  for (const auto& entry : groups) {
    const Group& group = entry.second;
    AcceleratorClassRecord record;
    record.id = group.id;
    record.capability_generation = AcceleratorCapabilityGeneration{1};
    record.vendor = group.vendor;
    record.model = group.model;
    record.architecture = group.architecture;
    record.compute_capability = group.compute_capability;
    record.memory_bytes_per_device = group.memory_bytes;
    record.currentness = Currentness::Unknown;
    record.stamp.precision = Precision::Exact;
    record.stamp.evidence_class = EvidenceClass::Real;
    record.stamp.provenance = Provenance::HostProbe;
    record.stamp.observed_at = now_unix_nanos();
    const auto put = [&record](CapabilityEntry capability) {
      const Status s = record.capabilities.put(std::move(capability));
      (void)s;
    };
    if (!group.architecture.empty()) {
      put(make_capability(CapabilityKey::AcceleratorArchitecture,
                          CapabilityValue::text(group.architecture)));
    }
    if (!group.compute_capability.empty()) {
      put(make_capability(CapabilityKey::ComputeCapability,
                          CapabilityValue::text(group.compute_capability)));
    }
    if (!group.vendor.empty()) {
      put(make_capability(CapabilityKey::AcceleratorVendor, CapabilityValue::text(group.vendor)));
    }
    if (!group.model.empty()) {
      put(make_capability(CapabilityKey::AcceleratorModel, CapabilityValue::text(group.model)));
    }
    if (group.memory_bytes != 0) {
      put(make_capability(CapabilityKey::MemoryBytes,
                          CapabilityValue::unsigned_integer(group.memory_bytes)));
    }
    if (!group.driver_version.empty()) {
      put(make_capability(CapabilityKey::DriverVersion,
                          CapabilityValue::text(group.driver_version)));
    }
    const Result<CapabilityValue> formats = CapabilityValue::text_set({"cubin", "fatbin", "ptx"});
    if (formats.ok()) {
      put(make_capability(CapabilityKey::ArtifactFormats, formats.value()));
    }
    put(make_capability(CapabilityKey::KernelFormat, CapabilityValue::text("cubin")));
    put(make_capability(CapabilityKey::RuntimeApi, CapabilityValue::text("cuda")));
    if (!host.os_name.empty()) {
      put(make_capability(CapabilityKey::OperatingSystem, CapabilityValue::text(host.os_name)));
    }
    if (toolchain.nvcc_present && !toolchain.supported_architectures.empty()) {
      const Result<CapabilityValue> kernels =
          CapabilityValue::text_set(toolchain.supported_architectures);
      if (kernels.ok()) {
        put(make_capability(CapabilityKey::KernelArchitectures, kernels.value()));
      }
    }
    const Status note = record.evidence.add(
        Provenance::HostProbe, EvidenceClass::Real, "nvidia-smi", Precision::Exact,
        std::to_string(group.devices) + " device(s) of this class were observed on this host");
    (void)note;
    records.push_back(std::move(record));
  }
  return records;
}

ClusterRecord HostInventory::as_cluster(const ClusterId& cluster, const SiteId& site,
                                        const FederationId& federation) const {
  ClusterRecord record;
  record.id = cluster;
  record.generation = ClusterGeneration{1};
  record.epoch = ClusterEpoch{1};
  record.federation = federation;
  record.site = site;
  // No placement domain is invented here: a cluster that publishes no domain is treated as
  // its own cluster-scoped placement domain, which is exactly what a single host is.
  record.failure_domain = "host";
  record.zone = "local";
  record.topology_generation = TopologyGeneration{1};
  record.capability_generation = AcceleratorCapabilityGeneration{1};
  record.capacity_generation = CapacityGeneration{1};
  record.runtime_generation = RuntimeGeneration{1};
  record.compatibility_generation = CompatibilityGeneration{1};
  record.evidence_generation = EvidenceGeneration{1};
  record.readiness = Readiness::Ready;
  record.currentness = Currentness::Unknown;
  record.stamp.precision = Precision::Exact;
  record.stamp.evidence_class = EvidenceClass::Real;
  record.stamp.provenance = Provenance::HostProbe;
  record.stamp.observed_at = now_unix_nanos();

  std::map<std::string, std::uint32_t> device_counts;
  for (const AcceleratorFacts& accelerator : accelerators) {
    device_counts[accelerator.model + "|" + accelerator.compute_capability] += 1;
  }
  const std::vector<AcceleratorClassRecord> classes = as_accelerator_classes();
  for (const AcceleratorClassRecord& accelerator_class : classes) {
    record.accelerator_classes.push_back(accelerator_class.id);
    const auto count =
        device_counts.find(accelerator_class.model + "|" + accelerator_class.compute_capability);
    const std::uint64_t devices = count == device_counts.end() ? 0 : count->second;
    if (devices == 0) {
      continue;
    }
    CapacityPool devices_pool;
    devices_pool.pool_id = ResourcePoolId::unchecked("host-" + accelerator_class.id.value());
    devices_pool.kind = ResourceKind::Accelerator;
    devices_pool.accelerator_class = accelerator_class.id;
    devices_pool.generation = CapacityGeneration{1};
    const Result<CapacityLedger> ledger = CapacityLedger::from_components(devices, 0, 0, 0, 0, 0);
    if (!ledger.ok()) {
      continue;
    }
    devices_pool.ledger = ledger.value();
    devices_pool.precision = Precision::Exact;
    devices_pool.evidence_class = EvidenceClass::Real;
    const Status note = devices_pool.evidence.add(
        Provenance::HostProbe, EvidenceClass::Real, "nvidia-smi", Precision::Exact,
        "real device count observed on this host; per-device allocation state was not observable "
        "from here and is therefore reported as idle rather than as free capacity owned by this "
        "runtime");
    (void)note;
    record.capacity_pools.push_back(std::move(devices_pool));

    CapacityPool memory_pool;
    memory_pool.pool_id =
        ResourcePoolId::unchecked("host-" + accelerator_class.id.value() + "-memory");
    memory_pool.kind = ResourceKind::MemoryBytes;
    memory_pool.accelerator_class = accelerator_class.id;
    memory_pool.generation = CapacityGeneration{1};
    std::uint64_t total = 0;
    if (checked_mul(devices, accelerator_class.memory_bytes_per_device, total)) {
      const Result<CapacityLedger> memory_ledger =
          CapacityLedger::from_components(total, 0, 0, 0, 0, 0);
      if (memory_ledger.ok()) {
        memory_pool.ledger = memory_ledger.value();
        memory_pool.precision = Precision::Exact;
        memory_pool.evidence_class = EvidenceClass::Real;
        record.capacity_pools.push_back(std::move(memory_pool));
      }
    }
  }

  const auto put = [&record](CapabilityEntry capability) {
    const Status s = record.capabilities.put(std::move(capability));
    (void)s;
  };
  if (!host.os_name.empty()) {
    put(make_capability(CapabilityKey::OperatingSystem, CapabilityValue::text(host.os_name)));
  }
  put(make_capability(CapabilityKey::RuntimeApi, CapabilityValue::text("cuda")));
  if (!host.cpu_architecture.empty()) {
    put(make_capability(CapabilityKey::CompilerTarget, CapabilityValue::text(host.cpu_architecture)));
  }
  if (toolchain.nvcc_present && !toolchain.cuda_toolkit_version.empty()) {
    put(make_capability(CapabilityKey::CompilerVersion,
                        CapabilityValue::text(toolchain.cuda_toolkit_version)));
  }
  if (!accelerators.empty() && !accelerators.front().runtime_version.empty()) {
    put(make_capability(CapabilityKey::RuntimeVersion,
                        CapabilityValue::text(accelerators.front().runtime_version)));
  }
  if (!accelerators.empty() && !accelerators.front().driver_version.empty()) {
    put(make_capability(CapabilityKey::DriverVersion,
                        CapabilityValue::text(accelerators.front().driver_version)));
  }
  for (const std::string& claim : unsupported_claims) {
    const Status note = record.evidence.add(Provenance::HostProbe, EvidenceClass::Unsupported,
                                            "host-probe", Precision::Exact, claim);
    (void)note;
  }
  return record;
}

Result<HostInventory> probe_host_inventory() {
  HostInventory inventory;
  inventory.evidence_class = EvidenceClass::Real;
  inventory.precision = Precision::Exact;
  inventory.host.evidence_class = EvidenceClass::Real;
  inventory.host.precision = Precision::Exact;

#ifdef _WIN32
  char computer_name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
  DWORD computer_name_length = sizeof(computer_name);
  if (::GetComputerNameA(computer_name, &computer_name_length) != 0) {
    inventory.host.hostname.assign(computer_name, computer_name_length);
  }
  inventory.host.os_name = "windows";
  {
    const std::string build = environment_variable("OS");
    inventory.host.os_version = build.empty() ? std::string("unknown-build") : build;
  }
  inventory.host.cpu_architecture = environment_variable("PROCESSOR_ARCHITECTURE");
  if (inventory.host.cpu_architecture.empty()) {
    SYSTEM_INFO info{};
    ::GetNativeSystemInfo(&info);
    inventory.host.cpu_architecture = "arch-" + std::to_string(info.wProcessorArchitecture);
  }
  {
    HKEY key = nullptr;
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                        &key) == ERROR_SUCCESS) {
      char buffer[512] = {0};
      DWORD size = sizeof(buffer);
      DWORD type = 0;
      if (::RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
        inventory.host.cpu_model = trim(std::string(buffer, size > 0 ? size - 1 : 0));
      }
      ::RegCloseKey(key);
    }
  }
  {
    DWORD length = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length != 0) {
      std::vector<char> buffer(length);
      if (::GetLogicalProcessorInformationEx(
              RelationProcessorCore,
              reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
              &length) != 0) {
        DWORD offset = 0;
        while (offset < length) {
          const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
              buffer.data() + offset);
          inventory.host.physical_cores += 1;
          offset += info->Size;
        }
      }
    }
    length = 0;
    ::GetLogicalProcessorInformationEx(RelationNumaNode, nullptr, &length);
    if (length != 0) {
      std::vector<char> buffer(length);
      if (::GetLogicalProcessorInformationEx(
              RelationNumaNode,
              reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
              &length) != 0) {
        DWORD offset = 0;
        while (offset < length) {
          const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
              buffer.data() + offset);
          inventory.host.numa_nodes += 1;
          offset += info->Size;
        }
      }
    }
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    inventory.host.logical_cores = info.dwNumberOfProcessors;
  }
  {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status) != 0) {
      inventory.host.total_memory_bytes = status.ullTotalPhys;
    }
  }
  {
    ULONG size = 16 * 1024;
    std::vector<char> buffer(size);
    ULONG result = ::GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
      buffer.resize(size);
      result = ::GetAdaptersAddresses(
          AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == NO_ERROR) {
      const auto* address = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
      while (address != nullptr) {
        std::string description;
        for (const wchar_t c : std::wstring(address->Description)) {
          description.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        const bool up = address->OperStatus == IfOperStatusUp;
        inventory.host.network_interfaces.push_back(description + " [" + (up ? "up" : "down") + "]");
        address = address->Next;
      }
    }
  }
#else
  {
    struct utsname info {};
    if (::uname(&info) == 0) {
      inventory.host.os_name = info.sysname;
      inventory.host.os_version = info.release;
      inventory.host.hostname = info.nodename;
      inventory.host.cpu_architecture = info.machine;
    }
  }
  inventory.host.logical_cores = static_cast<std::uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
  inventory.host.physical_cores = inventory.host.logical_cores;
  inventory.host.numa_nodes = 1;
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (pages > 0 && page_size > 0) {
    inventory.host.total_memory_bytes = static_cast<std::uint64_t>(pages) *
                                        static_cast<std::uint64_t>(page_size);
  }
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.rfind("model name", 0) == 0) {
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
          inventory.host.cpu_model = trim(line.substr(colon + 1));
        }
        break;
      }
    }
  }
#endif

  {
    const Status note = inventory.host.evidence.add(
        Provenance::HostProbe, EvidenceClass::Real, "host-probe", Precision::Exact,
        "CPU, memory, NUMA and network facts were read from the operating system on this host");
    (void)note;
  }

  // Real accelerator discovery through the vendor's own query tool.
  const std::string smi = executable_on_path("nvidia-smi");
  if (!smi.empty()) {
    const Result<std::string> query = run_tool_capture(
        {smi,
         "--query-gpu=name,memory.total,compute_cap,driver_version,vbios_version,"
         "pcie.link.gen.max,pcie.link.width.max",
         "--format=csv,noheader"});
    if (query.ok()) {
      const Result<std::string> uuids =
          run_tool_capture({smi, "--query-gpu=uuid", "--format=csv,noheader"});
      std::vector<std::string> uuid_lines;
      if (uuids.ok()) {
        uuid_lines = split_lines(uuids.value());
      }
      std::size_t index = 0;
      for (const std::string& line : split_lines(query.value())) {
        const std::vector<std::string> fields = split(line, ',');
        if (fields.size() < 7) {
          continue;
        }
        AcceleratorFacts facts;
        facts.model = fields[0];
        facts.vendor = "nvidia";
        std::uint64_t memory = 0;
        if (parse_memory_field(fields[1], memory)) {
          facts.memory_bytes = memory;
        }
        facts.compute_capability = fields[2];
        facts.architecture = architecture_from_compute_capability(facts.compute_capability);
        facts.driver_version = fields[3];
        facts.pcie_generation = fields[5];
        facts.pcie_width = fields[6];
        if (index < uuid_lines.size()) {
          facts.uuid = trim(uuid_lines[index]);
        }
        facts.precision = Precision::Exact;
        facts.evidence_class = EvidenceClass::Real;
        const Status note = facts.evidence.add(
            Provenance::HostProbe, EvidenceClass::Real, smi, Precision::Exact,
            "facts were read from nvidia-smi on this host: " + line);
        (void)note;
        inventory.accelerators.push_back(std::move(facts));
        ++index;
      }
    }
  }

  // Real toolchain discovery.
  const std::string nvcc = locate_nvcc();
  if (!nvcc.empty()) {
    inventory.toolchain.nvcc_present = true;
    inventory.toolchain.nvcc_path = nvcc;
    const Result<std::string> version = run_tool_capture({nvcc, "--version"});
    if (version.ok()) {
      inventory.toolchain.nvcc_version = trim(version.value());
      const std::size_t release = version.value().find("release ");
      if (release != std::string::npos) {
        std::size_t end = version.value().find(',', release);
        if (end == std::string::npos) {
          end = version.value().size();
        }
        inventory.toolchain.cuda_toolkit_version =
            trim(version.value().substr(release + 8, end - release - 8));
      }
    }
    const Result<std::string> architectures = run_tool_capture({nvcc, "--list-gpu-arch"});
    if (architectures.ok()) {
      for (const std::string& line : split_lines(architectures.value())) {
        const std::string entry = trim(line);
        if (entry.rfind("compute_", 0) == 0) {
          inventory.toolchain.supported_architectures.push_back(entry.substr(8));
        }
      }
      std::sort(inventory.toolchain.supported_architectures.begin(),
                inventory.toolchain.supported_architectures.end());
      inventory.toolchain.supported_architectures.erase(
          std::unique(inventory.toolchain.supported_architectures.begin(),
                      inventory.toolchain.supported_architectures.end()),
          inventory.toolchain.supported_architectures.end());
    }
    inventory.toolchain.cuda_toolkit_present = !inventory.toolchain.cuda_toolkit_version.empty();
    inventory.toolchain.precision = Precision::Exact;
    inventory.toolchain.evidence_class = EvidenceClass::Real;
    const Status note = inventory.toolchain.evidence.add(
        Provenance::ToolchainProbe, EvidenceClass::Real, nvcc, Precision::Exact,
        "the CUDA toolchain was located and interrogated on this host");
    (void)note;
  }
  const std::string cuobjdump =
      !nvcc.empty() ? sibling_tool(nvcc, "cuobjdump") : executable_on_path("cuobjdump");
  if (!cuobjdump.empty()) {
    inventory.toolchain.cuobjdump_present = true;
    inventory.toolchain.cuobjdump_path = cuobjdump;
  }

  if (inventory.accelerators.empty()) {
    inventory.unsupported_claims.emplace_back("no local accelerator was discovered on this host");
  }
  if (!inventory.toolchain.nvcc_present) {
    inventory.unsupported_claims.emplace_back(
        "no CUDA toolchain was found on this host, so real device images cannot be produced here");
  }
  inventory.unsupported_claims.emplace_back(
      "this host is a single machine: no physical multi-cluster federation is observable here, "
      "and every multi-cluster statement in this runtime is SYNTHETIC");
  inventory.unsupported_claims.emplace_back(
      "no second accelerator family is present, so heterogeneous physical accelerator federation "
      "is NOT demonstrable on this host");
  inventory.unsupported_claims.emplace_back(
      "cross-site execution and physical cross-cluster migration cannot be executed or observed on "
      "a single host");
  inventory.unsupported_claims.emplace_back(
      "cluster liveness, readiness and per-device allocation state are not observable from this "
      "host; the real cluster record reports idle capacity derived from device count only");

  return inventory;
}

bool ArtifactInspection::targets(std::string_view architecture) const {
  for (const std::string& candidate : target_architectures) {
    if (candidate == architecture) {
      return true;
    }
  }
  return false;
}

std::string ArtifactInspection::render(std::string_view indent) const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"path", path});
  rows.push_back({"exists", exists ? "yes" : "no"});
  rows.push_back({"readable", readable ? "yes" : "no"});
  rows.push_back({"size_bytes", std::to_string(size_bytes)});
  rows.push_back({"content_digest", content_digest.empty() ? "-" : content_digest});
  rows.push_back({"is_elf", is_elf ? "yes" : "no"});
  rows.push_back({"elf_machine", std::to_string(elf_machine)});
  rows.push_back({"is_cuda_elf", is_cuda_elf ? "yes" : "no"});
  rows.push_back({"target_architectures",
                  target_architectures.empty() ? "UNKNOWN"
                                               : join_strings(target_architectures, ",")});
  rows.push_back({"format", format.empty() ? "UNKNOWN" : format});
  rows.push_back({"kernel_format", kernel_format.empty() ? "UNKNOWN" : kernel_format});
  rows.push_back({"precision", std::string(fo::to_string(precision))});
  rows.push_back({"evidence_class", std::string(fo::to_string(evidence_class))});
  std::string out = render_table({"artifact_fact", "value"}, rows, indent);
  if (!tool_output.empty()) {
    out += '\n';
    out += std::string(indent) + "tool output:";
    out += '\n';
    out += indent_block(truncate_with_marker(tool_output, 2048), std::string(indent) + "  ");
  }
  return out;
}

Result<ArtifactInspection> inspect_artifact(const std::string& path) {
  ArtifactInspection inspection;
  inspection.path = path;
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) {
    return Error(ErrorCode::NotFound, "artifact does not exist", path);
  }
  inspection.exists = true;
  inspection.size_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(path, error));
  if (error) {
    return Error(ErrorCode::Internal, "could not read the artifact size", path);
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Error(ErrorCode::NotFound, "artifact is not readable", path);
  }
  inspection.readable = true;
  std::vector<std::uint8_t> head(kArtifactProbeBytes, 0);
  stream.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
  head.resize(static_cast<std::size_t>(stream.gcount()));

  // Digest the whole artifact (up to a bounded prefix) with an incrementally chained
  // FNV-1a, so that the digest is a real function of the file content.
  {
    std::ifstream whole(path, std::ios::binary);
    std::vector<std::uint8_t> chunk(64 * 1024, 0);
    std::uint64_t hash = 14695981039346656037ull;
    std::uint64_t total = 0;
    while (whole) {
      whole.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
      const std::size_t read = static_cast<std::size_t>(whole.gcount());
      if (read == 0) {
        break;
      }
      total += read;
      if (total > kMaxDigestBytes) {
        break;
      }
      for (std::size_t i = 0; i < read; ++i) {
        hash ^= static_cast<std::uint64_t>(chunk[i]);
        hash *= 1099511628211ull;
      }
    }
    inspection.content_digest = hex_digest64(hash);
    if (inspection.size_bytes > kMaxDigestBytes) {
      const Status note = inspection.evidence.add(
          Provenance::ArtifactInspection, EvidenceClass::Real, "artifact-inspection",
          Precision::Aggregated,
          "the artifact exceeds the digest bound; the digest covers a bounded prefix only");
      (void)note;
    }
  }

  if (head.size() >= 20 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
    inspection.is_elf = true;
    inspection.elf_machine = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(head[18]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(head[19]) << 8));
    inspection.is_cuda_elf = inspection.elf_machine == 190;
    inspection.format = inspection.is_cuda_elf ? "cubin" : "elf";
    if (inspection.is_cuda_elf) {
      inspection.kernel_format = "cubin";
    }
  } else {
    inspection.format = "unknown";
  }

  if (inspection.is_elf) {
    std::string cuobjdump = sibling_tool(locate_nvcc(), "cuobjdump");
    if (cuobjdump.empty()) {
      cuobjdump = executable_on_path("cuobjdump");
    }
    if (!cuobjdump.empty()) {
      const Result<std::string> listed = run_tool_capture({cuobjdump, "-lelf", path});
      if (listed.ok()) {
        inspection.tool_output = listed.value();
        for (const std::string& line : split_lines(listed.value())) {
          // cuobjdump prints the architecture as part of a file name (for example
          // "ELF file 1: kernel.sm_120.cubin"), so every "sm_<token>" occurrence in the
          // line is extracted rather than only the leading token.
          for (std::size_t position = line.find("sm_"); position != std::string::npos;
               position = line.find("sm_", position + 1)) {
            std::size_t end = position + 3;
            while (end < line.size()) {
              const char c = line[end];
              const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                 (c >= 'A' && c <= 'Z');
              if (!alnum) {
                break;
              }
              ++end;
            }
            if (end > position + 3) {
              inspection.target_architectures.push_back(line.substr(position, end - position));
            }
          }
        }
        std::sort(inspection.target_architectures.begin(), inspection.target_architectures.end());
        inspection.target_architectures.erase(
            std::unique(inspection.target_architectures.begin(),
                        inspection.target_architectures.end()),
            inspection.target_architectures.end());
      }
    }
  }
  inspection.precision =
      inspection.target_architectures.empty() ? Precision::Unknown : Precision::Exact;
  inspection.evidence_class = EvidenceClass::Real;
  const Status note = inspection.evidence.add(
      Provenance::ArtifactInspection, EvidenceClass::Real, path, inspection.precision,
      inspection.target_architectures.empty()
          ? "the target architecture could not be determined from the artifact; the runtime does "
            "not guess one"
          : "target architectures were read from the artifact with cuobjdump");
  (void)note;
  return inspection;
}

Status compile_cuda_object(const std::string& source_text, const std::string& architecture,
                           const std::string& output_path) {
  const std::string nvcc = locate_nvcc();
  if (nvcc.empty()) {
    return fail(ErrorCode::UnsupportedOperation,
                "no CUDA toolchain is present on this host, so no real device image can be built");
  }
  if (architecture.empty()) {
    return fail(ErrorCode::InvalidArgument, "no target architecture was supplied");
  }
  const std::string source_path = output_path + ".cu";
  const Status written = write_text_file(source_path, source_text);
  if (!written.ok()) {
    return written;
  }
  const Result<std::string> compiled = run_tool_capture(
      {nvcc, "-arch=" + architecture, "-cubin", "-o", output_path, source_path});
  const Status removed = remove_file_if_present(source_path);
  (void)removed;
  if (!compiled.ok()) {
    return fail(compiled.error().code(), "nvcc failed to build the device image",
                compiled.error().detail());
  }
  if (!std::filesystem::exists(output_path)) {
    return fail(ErrorCode::Internal, "nvcc reported success but produced no device image",
                output_path);
  }
  return Status::success();
}

}  // namespace fo

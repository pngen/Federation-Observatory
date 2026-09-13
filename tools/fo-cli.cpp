// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// fo-cli - read-only inspection of a Federation Observatory deployment.
//
// Two data sources: an in-process deterministic synthetic federation published through
// fo::ObservatorySink + fo::SyntheticFederation, or a live coordinator reached with
// fo::FederationClient over framed TCP. The tool is strictly observational: it never
// publishes, registers, retires, fences or decides a placement, and every conclusion it
// prints is attributed to the upstream scheduler that owns those decisions.
//
// --json-free is accepted and ignored on purpose: the runtime's deterministic text
// rendering is the contract. A second serialization of the same observations would be a
// second source of truth for the same facts, and two sources of truth is precisely what
// this runtime exists to avoid.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "federation_observatory/analysis.hpp"
#include "federation_observatory/bounds.hpp"
#include "federation_observatory/client.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/hardware.hpp"
#include "federation_observatory/observatory.hpp"
#include "federation_observatory/synthetic.hpp"

namespace {

// Exit codes: 0 success, 2 usage error, 3 not found / no data, 4 connection or protocol
// failure, 5 internal error. Every failure is printed through fo::Error::to_string().
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNoData = 3;
constexpr int kExitConnection = 4;
constexpr int kExitInternal = 5;

int exit_code_for(fo::ErrorCode code) {
  switch (code) {
    case fo::ErrorCode::Ok: return kExitOk;
    case fo::ErrorCode::InvalidArgument: return kExitUsage;
    case fo::ErrorCode::NotFound:
    case fo::ErrorCode::UnsupportedOperation: return kExitNoData;
    case fo::ErrorCode::ConnectionClosed:
    case fo::ErrorCode::ProtocolViolation:
    case fo::ErrorCode::OversizedPayload:
    case fo::ErrorCode::IntegrityFailure:
    case fo::ErrorCode::UnsupportedVersion:
    case fo::ErrorCode::NotReady:
    case fo::ErrorCode::Backpressure:
    case fo::ErrorCode::ShuttingDown: return kExitConnection;
    default: return kExitInternal;
  }
}

int report_error(const fo::Error& error) {
  std::printf("fo-cli: %s\n", error.to_string().c_str());
  return exit_code_for(error.code());
}

int usage_error(const std::string& message) {
  std::printf("fo-cli: %s\n", message.c_str());
  std::printf("fo-cli: run 'fo-cli --help' for the supported surface\n");
  return kExitUsage;
}

// --- deterministic output helpers -------------------------------------------

void print_line(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
}

void print_block(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  if (!text.empty() && text.back() != '\n') std::fputc('\n', stdout);
}

std::string yes_no(bool value) { return value ? "yes" : "no"; }

std::string or_dash(std::string_view text) {
  return text.empty() ? std::string("-") : std::string(text);
}

template <class GenerationType>
std::string generation_text(const GenerationType& generation) {
  return generation.is_set() ? generation.to_string() : std::string("-");
}

std::string bytes_text(std::uint64_t bytes) {
  constexpr std::uint64_t kKiB = 1024;
  constexpr std::uint64_t kMiB = 1024 * kKiB;
  constexpr std::uint64_t kGiB = 1024 * kMiB;
  if (bytes == 0) return "0 B";
  if (bytes % kGiB == 0) return std::to_string(bytes / kGiB) + " GiB";
  if (bytes % kMiB == 0) return std::to_string(bytes / kMiB) + " MiB";
  if (bytes % kKiB == 0) return std::to_string(bytes / kKiB) + " KiB";
  return std::to_string(bytes) + " B";
}

using Row = std::vector<std::string>;
using Table = std::vector<Row>;
using Args = std::vector<std::string>;

void print_table(std::string_view title, const std::vector<std::string>& headers, const Table& rows) {
  print_line(title);
  if (rows.empty()) {
    print_line("  <none: this observation contains no such records>");
    return;
  }
  print_block(fo::render_table(headers, rows, "  "));
}

/// Observation-only footer: the runtime explains decisions, it never makes them.
void print_observation_note() {
  print_line("  note: observation only - the upstream scheduler owns every placement, policy and");
  print_line("        migration decision; this CLI explains those decisions and never makes them.");
}

/// An analysis command prints its observation-only footer only when it produced a result.
int analysis_result(int result) {
  if (result == kExitOk) print_observation_note();
  return result;
}

// --- data source banner ------------------------------------------------------

enum class Source { Synthetic, Real, Remote };

struct Context {
  Source source = Source::Synthetic;
  std::uint64_t seed = 20260101;
  std::string host;
  std::uint16_t port = 0;
  bool json_free = false;
};

const char* source_name(Source source) {
  switch (source) {
    case Source::Synthetic: return "SYNTHETIC";
    case Source::Real: return "REAL";
    case Source::Remote: return "REMOTE";
  }
  return "UNKNOWN";
}

void print_banner(const Context& context, std::string_view command) {
  std::printf("SOURCE: %s  command=%s\n", source_name(context.source), std::string(command).c_str());
  switch (context.source) {
    case Source::Synthetic:
      std::printf("  in-process fo::SyntheticFederation published through fo::ObservatorySink; "
                  "seed=%llu; evidence class SYNTHETIC (no physical federation is involved)\n",
                  static_cast<unsigned long long>(context.seed));
      break;
    case Source::Real:
      print_line("  local host probe via fo::probe_host_inventory(); evidence class REAL");
      break;
    case Source::Remote:
      std::printf("  live coordinator %s:%u read through fo::FederationClient; the evidence class "
                  "of each record is whatever the coordinator reports\n",
                  context.host.c_str(), static_cast<unsigned>(context.port));
      break;
  }
  if (context.json_free) {
    print_line("  --json-free: accepted and ignored; the deterministic text rendering is the contract");
  }
}

// --- data sources ------------------------------------------------------------

/// In-process synthetic federation: the same generator the tests drive.
class InProcessFederation {
 public:
  explicit InProcessFederation(std::uint64_t seed) {
    fo::SyntheticConfig config;
    config.seed = seed;
    observatory_ = std::make_unique<fo::FederationObservatory>(fo::ObservatoryConfig{});
    sink_ = std::make_unique<fo::ObservatorySink>(*observatory_, config.publisher, config.boot,
                                                  config.federation);
    scenario_ = std::make_unique<fo::SyntheticFederation>(config, *sink_);
    const fo::Status ran = scenario_->run_all();
    if (!ran.ok()) error_ = ran.error().to_string();
  }

  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] const fo::FederationObservatory& observatory() const noexcept { return *observatory_; }
  [[nodiscard]] const fo::SyntheticFederation& scenario() const noexcept { return *scenario_; }

 private:
  std::unique_ptr<fo::FederationObservatory> observatory_;
  std::unique_ptr<fo::ObservatorySink> sink_;
  std::unique_ptr<fo::SyntheticFederation> scenario_;
  std::string error_;
};

struct Environment {
  Context context;
  const InProcessFederation* local = nullptr;
  fo::FederationClient* remote = nullptr;

  [[nodiscard]] bool is_remote() const noexcept { return remote != nullptr; }
  [[nodiscard]] fo::FederationId federation() const { return local->scenario().federation_id(); }
};

// --- wire helpers: the coordinator's rendering is printed verbatim and never re-parsed;
// --- structured rows are used exactly as delivered.

int print_remote_reply(const fo::protocol::ReplyPayload& payload) {
  if (payload.code != fo::ErrorCode::Ok) {
    return report_error(fo::Error(payload.code, payload.message, payload.detail));
  }
  if (!payload.message.empty()) print_line("  " + payload.message);
  if (!payload.digest.empty()) print_line("  digest: " + payload.digest);
  if (payload.text.empty()) {
    print_line("  <the coordinator returned no rendered payload>");
    return kExitOk;
  }
  print_block(payload.text);
  return kExitOk;
}

int print_remote_summary(const fo::protocol::ReplyPayload& payload, std::string_view title) {
  if (payload.code != fo::ErrorCode::Ok) {
    return report_error(fo::Error(payload.code, payload.message, payload.detail));
  }
  Table rows;
  for (const fo::protocol::WireRow& row : payload.rows) {
    for (const fo::protocol::WireField& field : row.fields) rows.push_back({field.name, field.value});
  }
  if (rows.empty()) return print_remote_reply(payload);
  print_line("  " + payload.message);
  print_table(title, {"field", "value"}, rows);
  return kExitOk;
}

int remote_reply(const fo::Result<fo::protocol::ReplyPayload>& reply) {
  if (!reply.ok()) return report_error(reply.error());
  return print_remote_reply(reply.value());
}

/// Local analysis results are rendered by the runtime itself, so a local read and a remote
/// read of the same evidence cannot disagree in wording or in numbers.
template <class AnalysisType>
int local_reply(const fo::Result<AnalysisType>& result) {
  if (!result.ok()) return report_error(result.error());
  print_block(result.value().render());
  return kExitOk;
}

/// Structural commands with no dedicated query are answered by QUERY_SNAPSHOT: the
/// coordinator's own rendering of the structural snapshot is authoritative for them.
int remote_snapshot(const Environment& env, std::string_view command) {
  fo::protocol::SnapshotQuery query;
  query.federation = fo::FederationId{};
  query.max_records = 4096;
  query.include_history = false;
  const fo::Result<fo::protocol::ReplyPayload> reply = env.remote->query_snapshot(query);
  if (!reply.ok()) return report_error(reply.error());
  std::printf("  remote surface: QUERY_SNAPSHOT is the coordinator's structural answer for %s;\n",
              std::string(command).c_str());
  print_line("  the rendering below comes from the coordinator and is not re-parsed by this CLI.");
  return print_remote_reply(reply.value());
}

// --- in-process record tables -------------------------------------------------

Table cluster_rows(const fo::FederationSnapshot& snapshot) {
  Table rows;
  for (const fo::ClusterRecord& cluster : snapshot.clusters) {
    const fo::Result<fo::CapacityLedger> ledger = cluster.total_ledger(fo::ResourceKind::Accelerator);
    rows.push_back({fo::render_id(cluster.id.view()), fo::render_id(cluster.site.view()),
                    generation_text(cluster.generation), generation_text(cluster.epoch),
                    std::string(fo::to_string(cluster.readiness)),
                    std::string(fo::to_string(cluster.currentness)),
                    ledger.ok() ? std::to_string(ledger.value().nominal) : std::string("UNKNOWN"),
                    ledger.ok() ? std::to_string(ledger.value().idle) : std::string("UNKNOWN"),
                    std::string(fo::to_string(cluster.stamp.evidence_class))});
  }
  return rows;
}

Table publisher_rows(const fo::FederationSnapshot& snapshot) {
  Table rows;
  for (const fo::PublisherStatus& publisher : snapshot.publishers) {
    rows.push_back({fo::render_id(publisher.id.view()), generation_text(publisher.boot),
                    yes_no(publisher.live), yes_no(publisher.fenced),
                    publisher.watermark.to_string(), std::to_string(publisher.publications_accepted),
                    std::to_string(publisher.publications_rejected),
                    std::to_string(publisher.duplicates_suppressed),
                    std::to_string(publisher.sequence_gaps),
                    generation_text(publisher.coordinator_epoch),
                    std::string(fo::to_string(publisher.evidence_class))});
  }
  return rows;
}

const std::vector<std::string> kClusterHeaders = {"cluster",       "site",
                                                  "generation",    "epoch",
                                                  "readiness",     "currentness",
                                                  "nominal_accel", "idle_accel",
                                                  "evidence_class"};
const std::vector<std::string> kPublisherHeaders = {
    "publisher", "boot", "live", "fenced", "watermark", "accepted", "rejected", "duplicates",
    "sequence_gaps", "epoch", "evidence_class"};

// --- commands -----------------------------------------------------------------

int cmd_bounds(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("bounds takes no arguments");
  print_banner(env.context, "bounds");
  if (env.is_remote()) return remote_reply(env.remote->query_bounds());
  print_block(fo::render_bounds(env.local->observatory().bounds()));
  return kExitOk;
}

int cmd_hardware(const Context& context, const Args& args) {
  if (!args.empty()) return usage_error("hardware takes no arguments");
  print_banner(context, "hardware");
  print_line("  --host/--port do not apply: this inventory is always the local machine.");
  const fo::Result<fo::HostInventory> inventory = fo::probe_host_inventory();
  if (!inventory.ok()) return report_error(inventory.error());
  print_block(inventory.value().render());
  return kExitOk;
}

int cmd_snapshot(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("snapshot takes no arguments");
  print_banner(env.context, "snapshot");
  if (env.is_remote()) return remote_snapshot(env, "snapshot");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table federations;
  for (const fo::FederationRecord& record : snapshot->federations) {
    federations.push_back({fo::render_id(record.id.view()), generation_text(record.generation),
                           or_dash(record.display_name), generation_text(record.coordinator_epoch),
                           std::to_string(record.sites.size()), std::to_string(record.clusters.size()),
                           std::to_string(record.accelerator_classes.size()),
                           std::to_string(record.runtimes.size()),
                           std::to_string(record.workload_classes.size()),
                           std::string(fo::to_string(record.currentness))});
  }
  print_table("federation membership:",
              {"federation", "generation", "display_name", "coordinator_epoch", "sites", "clusters",
               "accel_classes", "runtimes", "workload_classes", "currentness"},
              federations);
  const fo::SnapshotHealth& health = snapshot->health;
  print_table("freshness census:", {"counter", "value"},
              {{"clusters_total", std::to_string(health.clusters_total)},
               {"clusters_current", std::to_string(health.clusters_current)},
               {"clusters_stale", std::to_string(health.clusters_stale)},
               {"clusters_revalidation_required", std::to_string(health.clusters_revalidation_required)},
               {"clusters_retired", std::to_string(health.clusters_retired)},
               {"clusters_unknown", std::to_string(health.clusters_unknown)},
               {"sites_total", std::to_string(health.sites_total)},
               {"sites_current", std::to_string(health.sites_current)},
               {"publishers_total", std::to_string(health.publishers_total)},
               {"publishers_live", std::to_string(health.publishers_live)},
               {"publishers_fenced", std::to_string(health.publishers_fenced)},
               {"revalidation_pending", yes_no(health.revalidation_pending)},
               {"degraded", yes_no(health.degraded)}});
  print_table("clusters:", kClusterHeaders, cluster_rows(*snapshot));
  print_table("publishers:", kPublisherHeaders, publisher_rows(*snapshot));
  return snapshot->clusters.empty() ? kExitNoData : kExitOk;
}

int cmd_clusters(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("clusters takes no arguments");
  print_banner(env.context, "clusters");
  if (env.is_remote()) return remote_snapshot(env, "clusters");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  print_table("clusters:", kClusterHeaders, cluster_rows(*snapshot));
  return snapshot->clusters.empty() ? kExitNoData : kExitOk;
}

int cmd_sites(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("sites takes no arguments");
  print_banner(env.context, "sites");
  if (env.is_remote()) return remote_snapshot(env, "sites");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  for (const fo::SiteRecord& site : snapshot->sites) {
    rows.push_back({fo::render_id(site.id.view()), fo::render_id(site.federation.view()),
                    generation_text(site.generation), or_dash(site.region), or_dash(site.zone),
                    or_dash(site.failure_domain), std::to_string(site.clusters.size()),
                    std::string(fo::to_string(site.currentness)),
                    std::string(fo::to_string(site.stamp.evidence_class))});
  }
  print_table("sites:",
              {"site", "federation", "generation", "region", "zone", "failure_domain", "clusters",
               "currentness", "evidence_class"},
              rows);
  return rows.empty() ? kExitNoData : kExitOk;
}

int cmd_accelerator_classes(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("accelerator-classes takes no arguments");
  print_banner(env.context, "accelerator-classes");
  if (env.is_remote()) return remote_snapshot(env, "accelerator-classes");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  for (const fo::AcceleratorClassRecord& item : snapshot->accelerator_classes) {
    rows.push_back({fo::render_id(item.id.view()), or_dash(item.vendor), or_dash(item.family),
                    or_dash(item.model), or_dash(item.architecture), or_dash(item.compute_capability),
                    bytes_text(item.memory_bytes_per_device),
                    generation_text(item.capability_generation),
                    std::to_string(item.capabilities.size()),
                    std::string(fo::to_string(item.currentness))});
  }
  print_table("accelerator classes:",
              {"accelerator_class", "vendor", "family", "model", "architecture", "compute_capability",
               "memory_per_device", "capability_generation", "capabilities", "currentness"},
              rows);
  return rows.empty() ? kExitNoData : kExitOk;
}

int cmd_runtimes(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("runtimes takes no arguments");
  print_banner(env.context, "runtimes");
  if (env.is_remote()) return remote_snapshot(env, "runtimes");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  for (const fo::RuntimeRecord& runtime : snapshot->runtimes) {
    rows.push_back({fo::render_id(runtime.id.view()), std::string(fo::to_string(runtime.kind)),
                    or_dash(runtime.version), or_dash(runtime.abi), or_dash(runtime.driver_version),
                    or_dash(runtime.driver_abi), fo::render_id(runtime.backend.view()),
                    generation_text(runtime.backend_generation), generation_text(runtime.generation),
                    std::string(fo::to_string(runtime.currentness))});
  }
  print_table("runtime and backend generations:",
              {"runtime", "kind", "version", "abi", "driver_version", "driver_abi", "backend",
               "backend_generation", "generation", "currentness"},
              rows);
  return rows.empty() ? kExitNoData : kExitOk;
}

int cmd_capabilities(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("capabilities takes no arguments");
  print_banner(env.context, "capabilities");
  if (env.is_remote()) return remote_snapshot(env, "capabilities");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  print_line("capability publications per cluster:");
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    std::printf("  cluster %s  capability_generation=%s  currentness=%s  entries=%llu\n",
                fo::render_id(cluster.id.view()).c_str(),
                generation_text(cluster.capability_generation).c_str(),
                std::string(fo::to_string(cluster.currentness)).c_str(),
                static_cast<unsigned long long>(cluster.capabilities.size()));
    if (cluster.capabilities.empty()) {
      print_line("    <no capability entries were published for this cluster>");
      continue;
    }
    print_block(fo::indent_block(cluster.capabilities.render("  "), "    "));
  }
  return snapshot->clusters.empty() ? kExitNoData : kExitOk;
}

int cmd_capacity(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("capacity takes no arguments");
  print_banner(env.context, "capacity");
  if (env.is_remote()) return remote_snapshot(env, "capacity");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  fo::CapacityLedger total;
  bool all_close = true;
  std::vector<std::string> unreadable;
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    const fo::Result<fo::CapacityLedger> ledger = cluster.total_ledger(fo::ResourceKind::Accelerator);
    if (!ledger.ok()) {
      unreadable.push_back(fo::render_id(cluster.id.view()) + ": " + ledger.error().to_string());
      all_close = false;
      continue;
    }
    const fo::CapacityLedger& value = ledger.value();
    const bool closes = value.validate().ok();
    all_close = all_close && closes;
    total.nominal += value.nominal;
    total.offline += value.offline;
    total.allocated += value.allocated;
    total.reserved += value.reserved;
    total.draining += value.draining;
    total.unusable += value.unusable;
    total.idle += value.idle;
    rows.push_back({fo::render_id(cluster.id.view()), std::to_string(value.nominal),
                    std::to_string(value.offline), std::to_string(value.allocated),
                    std::to_string(value.reserved), std::to_string(value.draining),
                    std::to_string(value.unusable), std::to_string(value.idle),
                    closes ? "ok" : "INCONSISTENT"});
  }
  if (!rows.empty()) {
    rows.push_back({"TOTAL", std::to_string(total.nominal), std::to_string(total.offline),
                    std::to_string(total.allocated), std::to_string(total.reserved),
                    std::to_string(total.draining), std::to_string(total.unusable),
                    std::to_string(total.idle), total.validate().ok() ? "ok" : "INCONSISTENT"});
  }
  print_table("capacity ledger (resource kind accelerator; observed, never owned):",
              {"cluster", "nominal", "offline", "allocated", "reserved", "draining", "unusable",
               "idle", "identity"},
              rows);
  for (const std::string& detail : unreadable) print_line("  ledger unavailable for " + detail);
  print_line("  identity: nominal = offline + allocated + reserved + draining + unusable + idle");
  std::printf("  the accounting identity holds for %llu of %llu cluster(s): %s\n",
              static_cast<unsigned long long>(snapshot->clusters.size() - unreadable.size()),
              static_cast<unsigned long long>(snapshot->clusters.size()), all_close ? "yes" : "NO");
  if (snapshot->clusters.empty()) return kExitNoData;
  return all_close ? kExitOk : kExitInternal;
}

/// "stranded" reports the whole analysis; "usable" reports the summary only.
int cmd_stranded(const Environment& env, const Args& args, bool summary_only) {
  const char* command = summary_only ? "usable" : "stranded";
  if (args.size() != 1) {
    return usage_error(std::string(command) + " requires exactly one <workload-class>");
  }
  print_banner(env.context, command);
  const fo::Result<fo::WorkloadClassId> parsed = fo::WorkloadClassId::parse(args[0]);
  if (!parsed.ok()) return report_error(parsed.error());
  fo::StrandedCapacityRequest request;
  request.workload_class = parsed.value();
  request.kind = fo::ResourceKind::Accelerator;
  request.include_stale = false;
  if (env.is_remote()) {
    fo::protocol::StrandedQuery query;
    query.request = request;
    const fo::Result<fo::protocol::ReplyPayload> reply = env.remote->query_stranded_capacity(query);
    if (!reply.ok()) return report_error(reply.error());
    if (summary_only) return print_remote_summary(reply.value(), "usable / stranded / unknown");
    return analysis_result(print_remote_reply(reply.value()));
  }
  request.federation = env.federation();
  const fo::Result<fo::StrandedCapacityReport> report =
      env.local->observatory().stranded_capacity(request);
  if (!report.ok()) return report_error(report.error());
  if (!summary_only) {
    print_block(report.value().render());
    return analysis_result(report.value().closes() ? kExitOk : kExitInternal);
  }
  const fo::CapacitySummary& summary = report.value().summary;
  const std::uint64_t accounted = summary.usable + summary.unknown + summary.stranded;
  print_table("usable / stranded / unknown:", {"field", "value"},
              {{"workload_class", fo::render_id(report.value().workload_class.view())},
               {"resource_kind", std::string(fo::to_string(report.value().kind))},
               {"nominal", std::to_string(summary.nominal)},
               {"idle", std::to_string(summary.idle)},
               {"usable", std::to_string(summary.usable)},
               {"stranded", std::to_string(summary.stranded)},
               {"unknown", std::to_string(summary.unknown)},
               {"closes", yes_no(report.value().closes())},
               {"excluded_stale_clusters", std::to_string(report.value().excluded_stale_clusters.size())},
               {"excluded_unknown_clusters", std::to_string(report.value().excluded_unknown_clusters.size())},
               {"precision", std::string(fo::to_string(report.value().precision))},
               {"evidence_class", std::string(fo::to_string(report.value().evidence_class))}});
  std::printf("  identity: idle = usable + unknown + stranded  ->  %llu = %llu  (%s)\n",
              static_cast<unsigned long long>(summary.idle),
              static_cast<unsigned long long>(accounted),
              summary.idle == accounted ? "holds" : "VIOLATED");
  print_line("  excluded clusters are stated explicitly: their capacity is NOT counted above.");
  return analysis_result(report.value().closes() ? kExitOk : kExitInternal);
}

int cmd_fragmentation(const Environment& env, const Args& args) {
  if (args.size() != 1) return usage_error("fragmentation requires exactly one <workload-class>");
  print_banner(env.context, "fragmentation");
  const fo::Result<fo::WorkloadClassId> parsed = fo::WorkloadClassId::parse(args[0]);
  if (!parsed.ok()) return report_error(parsed.error());
  if (env.is_remote()) {
    fo::protocol::FragmentationQuery query;
    query.workload_class = parsed.value();
    query.kind = fo::ResourceKind::Accelerator;
    return analysis_result(remote_reply(env.remote->query_fragmentation(query)));
  }
  return analysis_result(local_reply(env.local->observatory().fragmentation(
      env.federation(), parsed.value(), fo::ResourceKind::Accelerator)));
}

int cmd_placements(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("placements takes no arguments");
  print_banner(env.context, "placements");
  if (env.is_remote()) return remote_snapshot(env, "placements");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  for (const fo::PlacementRecord& placement : snapshot->placements) {
    rows.push_back({fo::render_id(placement.id.view()), fo::render_id(placement.workload.view()),
                    fo::render_id(placement.workload_class.view()),
                    fo::render_id(placement.selected.view()),
                    std::to_string(placement.selected_accelerator_count),
                    std::to_string(placement.candidates.size()),
                    std::string(fo::to_string(placement.candidate_completeness)),
                    std::to_string(placement.rejected_count()),
                    yes_no(placement.fallback_required),
                    std::string(fo::to_string(placement.currentness)),
                    std::string(fo::to_string(placement.stamp.evidence_class))});
  }
  print_table("placements (observed decisions; the upstream scheduler made them):",
              {"placement", "workload", "workload_class", "selected", "accelerators", "candidates",
               "candidate_completeness", "rejected", "fallback", "currentness", "evidence_class"},
              rows);
  if (!rows.empty()) {
    print_line("  candidate_completeness states how much of the evaluated candidate set the upstream");
    print_line("  scheduler exposed; rejection attribution is unavailable below Partial.");
  }
  return rows.empty() ? kExitNoData : kExitOk;
}

int cmd_placement(const Environment& env, const Args& args) {
  if (args.size() != 1) return usage_error("placement requires exactly one <placement-id>");
  print_banner(env.context, "placement");
  const fo::Result<fo::PlacementId> parsed = fo::PlacementId::parse(args[0]);
  if (!parsed.ok()) return report_error(parsed.error());
  if (env.is_remote()) {
    fo::protocol::PlacementQuery query;
    query.placement = parsed.value();
    return analysis_result(remote_reply(env.remote->query_placement(query)));
  }
  return analysis_result(local_reply(env.local->observatory().explain_placement(parsed.value())));
}

int cmd_rejection(const Environment& env, const Args& args) {
  if (args.size() != 2) return usage_error("rejection requires <placement-id> <cluster-id>");
  print_banner(env.context, "rejection");
  const fo::Result<fo::PlacementId> placement = fo::PlacementId::parse(args[0]);
  if (!placement.ok()) return report_error(placement.error());
  const fo::Result<fo::ClusterId> cluster = fo::ClusterId::parse(args[1]);
  if (!cluster.ok()) return report_error(cluster.error());
  if (env.is_remote()) {
    fo::protocol::RejectionQuery query;
    query.placement = placement.value();
    query.candidate = cluster.value();
    return analysis_result(remote_reply(env.remote->query_rejection(query)));
  }
  return analysis_result(
      local_reply(env.local->observatory().explain_rejection(placement.value(), cluster.value())));
}

int cmd_migrations(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("migrations takes no arguments");
  print_banner(env.context, "migrations");
  if (env.is_remote()) return remote_snapshot(env, "migrations");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  Table rows;
  for (const fo::MigrationRecord& migration : snapshot->migrations) {
    std::vector<std::string> adaptations;
    if (migration.requires_rebuild) adaptations.push_back("rebuild");
    if (migration.requires_recompile) adaptations.push_back("recompile");
    if (migration.requires_conversion) adaptations.push_back("conversion");
    if (migration.requires_state_translation) adaptations.push_back("state_translation");
    rows.push_back({fo::render_id(migration.id.view()), fo::render_id(migration.workload.view()),
                    fo::render_id(migration.source.view()), fo::render_id(migration.destination.view()),
                    std::string(fo::to_string(migration.stage)),
                    std::string(fo::to_string(migration.outcome)),
                    adaptations.empty() ? std::string("none") : fo::join_strings(adaptations, ","),
                    yes_no(migration.revalidation_pending),
                    std::string(fo::to_string(migration.currentness)),
                    std::string(fo::to_string(migration.stamp.evidence_class))});
  }
  print_table("migrations (observed stages; this runtime never drives one):",
              {"migration", "workload", "source", "destination", "stage", "outcome", "adaptations",
               "revalidation_pending", "currentness", "evidence_class"},
              rows);
  return rows.empty() ? kExitNoData : kExitOk;
}

int cmd_migration(const Environment& env, const Args& args) {
  if (args.size() != 1) return usage_error("migration requires exactly one <migration-id>");
  print_banner(env.context, "migration");
  const fo::Result<fo::MigrationId> parsed = fo::MigrationId::parse(args[0]);
  if (!parsed.ok()) return report_error(parsed.error());
  if (env.is_remote()) {
    fo::protocol::MigrationQuery query;
    query.migration = parsed.value();
    return analysis_result(remote_reply(env.remote->query_migration(query)));
  }
  return analysis_result(local_reply(env.local->observatory().migration_analysis(parsed.value())));
}

int cmd_portability(const Environment& env, const Args& args, bool evaluate) {
  if (args.size() != 2) return usage_error("portability requires <workload-id> <cluster-id>");
  print_banner(env.context, "portability");
  const fo::Result<fo::WorkloadId> workload = fo::WorkloadId::parse(args[0]);
  if (!workload.ok()) return report_error(workload.error());
  const fo::Result<fo::ClusterId> cluster = fo::ClusterId::parse(args[1]);
  if (!cluster.ok()) return report_error(cluster.error());
  print_line(evaluate ? "  source: freshly evaluated by the runtime (--evaluate)"
                      : "  source: the last assessment the upstream scheduler published");
  if (env.is_remote()) {
    fo::protocol::PortabilityQuery query;
    query.workload = workload.value();
    query.destination = cluster.value();
    query.evaluate = evaluate;
    return analysis_result(remote_reply(env.remote->query_portability(query)));
  }
  const fo::FederationObservatory& observatory = env.local->observatory();
  return analysis_result(
      evaluate ? local_reply(observatory.evaluate_portability(workload.value(), cluster.value()))
               : local_reply(observatory.portability(workload.value(), cluster.value())));
}

int cmd_compatibility(const Environment& env, const Args& args) {
  if (args.size() != 2) return usage_error("compatibility requires <cluster-id> <workload-id>");
  print_banner(env.context, "compatibility");
  const fo::Result<fo::ClusterId> cluster = fo::ClusterId::parse(args[0]);
  if (!cluster.ok()) return report_error(cluster.error());
  const fo::Result<fo::WorkloadId> workload = fo::WorkloadId::parse(args[1]);
  if (!workload.ok()) return report_error(workload.error());
  if (env.is_remote()) {
    fo::protocol::CompatibilityQuery query;
    query.cluster = cluster.value();
    query.workload = workload.value();
    return analysis_result(remote_reply(env.remote->query_compatibility(query)));
  }
  return analysis_result(
      local_reply(env.local->observatory().compatibility(cluster.value(), workload.value())));
}

int cmd_mismatch(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("mismatch takes no arguments");
  print_banner(env.context, "mismatch");
  fo::MismatchAnalysisRequest request;
  request.kind = fo::ResourceKind::Accelerator;
  request.include_stale = false;
  print_line("  scope: every cluster whose evidence is Current; population counts are printed with");
  print_line("         their denominator, so no percentage is read without one.");
  if (env.is_remote()) {
    fo::protocol::MismatchQuery query;
    query.request = request;
    return analysis_result(remote_reply(env.remote->query_mismatch(query)));
  }
  request.federation = env.federation();
  return analysis_result(local_reply(env.local->observatory().mismatch_analysis(request)));
}

int cmd_drift(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("drift takes no arguments");
  print_banner(env.context, "drift");
  fo::DriftRequest request;
  request.intended = nullptr;
  request.behavior_window_supplied = false;
  print_line("  intended-state drift was NOT evaluated: no intended federation state was supplied,");
  print_line("  and the runtime never invents one. A clean report here is not a clean bill of health.");
  if (env.is_remote()) {
    fo::protocol::DriftQuery query;
    query.have_intended = false;
    query.behavior_window_supplied = false;
    return remote_reply(env.remote->query_drift(query));
  }
  request.federation = env.federation();
  return local_reply(env.local->observatory().drift(request));
}

int cmd_publishers(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("publishers takes no arguments");
  print_banner(env.context, "publishers");
  if (env.is_remote()) return remote_snapshot(env, "publishers");
  const fo::SnapshotHandle snapshot = env.local->observatory().snapshot();
  print_table("publisher authority:", kPublisherHeaders, publisher_rows(*snapshot));
  for (const fo::PublisherStatus& publisher : snapshot->publishers) {
    if (publisher.fenced) {
      print_line("  fenced " + fo::render_id(publisher.id.view()) + ": " +
                 or_dash(publisher.fenced_reason));
    }
  }
  return snapshot->publishers.empty() ? kExitNoData : kExitOk;
}

int cmd_provenance(const Environment& env, const Args& args) {
  if (!args.empty()) return usage_error("provenance takes no arguments");
  print_banner(env.context, "provenance");
  struct Entry {
    const char* family;
    std::string_view term;
    const char* meaning;
  };
  const Entry legend[] = {
      {"precision", fo::to_string(fo::Precision::Exact),
       "stated by the authoritative source for the exact generation"},
      {"precision", fo::to_string(fo::Precision::Aggregated),
       "summed over a stated population; per-member detail is not retained"},
      {"precision", fo::to_string(fo::Precision::Sampled),
       "extrapolated from a stated sample; never presented as a census"},
      {"precision", fo::to_string(fo::Precision::Derived),
       "computed deterministically from exact inputs by this runtime"},
      {"precision", fo::to_string(fo::Precision::Inferred),
       "consistent with the evidence, but stated by no source"},
      {"precision", fo::to_string(fo::Precision::Ambiguous),
       "the evidence supports more than one mutually exclusive reading"},
      {"precision", fo::to_string(fo::Precision::Unknown), "no usable evidence"},
      {"evidence_class", fo::to_string(fo::EvidenceClass::Real),
       "produced by physical hardware, a real runtime or a real process"},
      {"evidence_class", fo::to_string(fo::EvidenceClass::Synthetic),
       "produced by the deterministic synthetic backend"},
      {"evidence_class", fo::to_string(fo::EvidenceClass::Unsupported),
       "the environment cannot produce this evidence at all"},
      {"evidence_class", fo::to_string(fo::EvidenceClass::Unknown),
       "the source did not declare its class"},
      {"reason_basis", fo::to_string(fo::ReasonBasis::Observed),
       "the source stated this reason; the runtime did not invent it"},
      {"reason_basis", fo::to_string(fo::ReasonBasis::Derived),
       "computed from observed facts by a deterministic rule"},
      {"reason_basis", fo::to_string(fo::ReasonBasis::Inferred),
       "consistent with the evidence; no source stated it"},
      {"reason_basis", fo::to_string(fo::ReasonBasis::Unattributed),
       "no reason is supportable - a first-class outcome, never a guess"},
  };
  Table rows;
  for (const Entry& entry : legend) {
    rows.push_back({entry.family, std::string(entry.term), entry.meaning});
  }
  print_table("provenance legend:", {"family", "term", "meaning"}, rows);
  print_line("  The banner above names the class this invocation's data source carries: SYNTHETIC for");
  print_line("  the in-process scenario, REAL for the local hardware probe, coordinator-reported");
  print_line("  classes for a remote read. Combining evidence never upgrades a weaker class.");
  return kExitOk;
}

// --- self-test: cheap, honest invariants over the synthetic scenario -----------

class SelfTest {
 public:
  void expect(bool condition, const std::string& name, const std::string& detail = {}) {
    if (condition) {
      std::printf("  [ PASS ] %s\n", name.c_str());
      ++passed_;
      return;
    }
    ++failed_;
    std::printf("  [ FAIL ] %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
  }
  [[nodiscard]] std::size_t passed() const noexcept { return passed_; }
  [[nodiscard]] std::size_t failed() const noexcept { return failed_; }

 private:
  std::size_t passed_ = 0;
  std::size_t failed_ = 0;
};

int cmd_self_test(const Context& context, const Args& args) {
  if (!args.empty()) return usage_error("self-test takes no arguments");
  print_banner(context, "self-test");
  InProcessFederation local(context.seed);
  if (!local.ok()) {
    print_line("  the synthetic scenario did not run: " + local.error());
    return kExitInternal;
  }
  const fo::FederationObservatory& observatory = local.observatory();
  const fo::SyntheticFederation& scenario = local.scenario();
  const fo::SnapshotHandle snapshot = observatory.snapshot();
  SelfTest test;

  const fo::Status bounds = observatory.bounds().validate();
  test.expect(bounds.ok(), "effective bounds validate",
              bounds.ok() ? std::string() : bounds.error().to_string());
  test.expect(snapshot->health.clusters_total == snapshot->clusters.size(),
              "the freshness census agrees with federation membership",
              std::to_string(snapshot->health.clusters_total) + " vs " +
                  std::to_string(snapshot->clusters.size()));
  test.expect(!snapshot->truncated, "the snapshot was not truncated by a bound");

  std::uint64_t nominal = 0;
  bool ledgers_close = true;
  bool all_synthetic = true;
  std::string ledger_detail;
  for (const fo::ClusterRecord& cluster : snapshot->clusters) {
    const fo::Result<fo::CapacityLedger> ledger = cluster.total_ledger(fo::ResourceKind::Accelerator);
    if (!ledger.ok()) {
      ledgers_close = false;
      ledger_detail = ledger.error().to_string();
      continue;
    }
    nominal += ledger.value().nominal;
    if (!ledger.value().validate().ok()) {
      ledgers_close = false;
      ledger_detail = fo::render_id(cluster.id.view());
    }
    if (cluster.stamp.evidence_class != fo::EvidenceClass::Synthetic) all_synthetic = false;
  }
  test.expect(ledgers_close, "every cluster ledger satisfies the accounting identity", ledger_detail);
  test.expect(nominal == scenario.expectations().total_nominal_accelerators,
              "published capacity matches the generator's expectation",
              std::to_string(nominal) + " vs " +
                  std::to_string(scenario.expectations().total_nominal_accelerators));
  test.expect(all_synthetic, "every cluster observation is classified SYNTHETIC");
  test.expect(snapshot->placements.size() == scenario.expectations().placements_expected,
              "observed placements match the generator's expectation");
  test.expect(snapshot->migrations.size() == scenario.expectations().migrations_expected,
              "observed migrations match the generator's expectation");

  for (const fo::WorkloadClassId& workload_class : scenario.workload_class_ids()) {
    fo::StrandedCapacityRequest request;
    request.federation = scenario.federation_id();
    request.workload_class = workload_class;
    request.kind = fo::ResourceKind::Accelerator;
    const fo::Result<fo::StrandedCapacityReport> first = observatory.stranded_capacity(request);
    const fo::Result<fo::StrandedCapacityReport> second = observatory.stranded_capacity(request);
    const std::string label = fo::render_id(workload_class.view());
    test.expect(first.ok(), "stranded capacity is available for " + label,
                first.ok() ? std::string() : first.error().to_string());
    if (!first.ok()) continue;
    test.expect(first.value().closes(),
                "stranded report closes (idle = usable + unknown + stranded) for " + label);
    test.expect(second.ok() && first.value().digest() == second.value().digest(),
                "stranded capacity is byte-deterministic for " + label);
  }

  fo::MismatchAnalysisRequest mismatch;
  mismatch.federation = scenario.federation_id();
  mismatch.kind = fo::ResourceKind::Accelerator;
  test.expect(observatory.mismatch_analysis(mismatch).ok(),
              "capability mismatch analysis is available over the whole observation");
  fo::DriftRequest drift;
  drift.federation = scenario.federation_id();
  const fo::Result<fo::DriftReport> drift_report = observatory.drift(drift);
  test.expect(drift_report.ok(), "drift analysis is available",
              drift_report.ok() ? std::string() : drift_report.error().to_string());
  if (drift_report.ok()) {
    test.expect(!drift_report.value().intended_state_supplied,
                "drift reports intended-state coverage as not evaluated without an intended state");
  }

  std::printf("  self-test: %llu passed, %llu failed\n",
              static_cast<unsigned long long>(test.passed()),
              static_cast<unsigned long long>(test.failed()));
  return test.failed() == 0 ? kExitOk : kExitInternal;
}

int cmd_scenario(const Context& context, const Args& args) {
  if (!args.empty()) return usage_error("scenario takes no arguments");
  print_banner(context, "scenario");
  InProcessFederation local(context.seed);
  if (!local.ok()) {
    print_line("  the synthetic scenario did not run: " + local.error());
    return kExitInternal;
  }
  const fo::SyntheticFederation& scenario = local.scenario();
  const fo::SnapshotHandle snapshot = local.observatory().snapshot();
  print_line("reproduction record:");
  print_block(fo::indent_block(scenario.reproduction_record(), "  "));
  print_table("scenario summary:", {"field", "value"},
              {{"steps_executed", std::to_string(fo::kSyntheticStepCount)},
               {"federation", fo::render_id(scenario.federation_id().view())},
               {"sites", std::to_string(snapshot->sites.size())},
               {"clusters", std::to_string(snapshot->clusters.size())},
               {"accelerator_classes", std::to_string(snapshot->accelerator_classes.size())},
               {"runtimes", std::to_string(snapshot->runtimes.size())},
               {"workload_classes", std::to_string(snapshot->workload_classes.size())},
               {"workloads", std::to_string(snapshot->workloads.size())},
               {"placements", std::to_string(snapshot->placements.size())},
               {"migrations", std::to_string(snapshot->migrations.size())},
               {"portability_records", std::to_string(snapshot->portability.size())},
               {"publishers", std::to_string(snapshot->publishers.size())},
               {"records_included", std::to_string(snapshot->records_included)},
               {"records_total", std::to_string(snapshot->records_total)},
               {"truncated", yes_no(snapshot->truncated)},
               {"degraded", yes_no(snapshot->health.degraded)},
               {"last_error",
                scenario.last_error().empty() ? std::string("-") : scenario.last_error()}});
  const fo::SyntheticFederation::Expectations& expectations = scenario.expectations();
  print_table("generator expectations (computed by the scenario, not by this CLI):",
              {"expectation", "value"},
              {{"total_nominal_accelerators", std::to_string(expectations.total_nominal_accelerators)},
               {"placements_expected", std::to_string(expectations.placements_expected)},
               {"rejections_expected", std::to_string(expectations.rejections_expected)},
               {"migrations_expected", std::to_string(expectations.migrations_expected)},
               {"portability_failures_expected",
                std::to_string(expectations.portability_failures_expected)},
               {"fragmentation_expected", yes_no(expectations.fragmentation_expected)},
               {"fragmentation_required_group",
                std::to_string(expectations.fragmentation_required_group)}});
  const fo::SnapshotHealth& health = snapshot->health;
  print_table("freshness census:", {"counter", "value"},
              {{"clusters_total", std::to_string(health.clusters_total)},
               {"clusters_current", std::to_string(health.clusters_current)},
               {"clusters_stale", std::to_string(health.clusters_stale)},
               {"clusters_retired", std::to_string(health.clusters_retired)},
               {"sites_total", std::to_string(health.sites_total)},
               {"publishers_total", std::to_string(health.publishers_total)},
               {"publishers_live", std::to_string(health.publishers_live)},
               {"degraded", yes_no(health.degraded)}});
  print_line("  note: the scenario publishes synthetic observations; nothing in it is a physical");
  print_line("        federation and no placement here was chosen by this runtime.");
  return kExitOk;
}

// --- help ----------------------------------------------------------------------

void print_help() {
  print_line(
      "fo-cli: read-only inspection of a Federation Observatory deployment.\n"
      "\n"
      "usage:\n"
      "  fo-cli [--help]\n"
      "  fo-cli bounds\n"
      "  fo-cli hardware\n"
      "  fo-cli self-test\n"
      "  fo-cli [--host H --port P] <command> [options]\n"
      "\n"
      "modes:\n"
      "  in-process  no --host/--port. A deterministic synthetic federation is built with\n"
      "              fo::ObservatorySink + fo::SyntheticFederation and answered from memory.\n"
      "  remote      --host H --port P. Each command is one request/response exchange with a\n"
      "              live coordinator through fo::FederationClient; there is no client cache.\n"
      "\n"
      "commands:\n"
      "  bounds                     effective bounds (in-process, or QUERY_BOUNDS when remote)\n"
      "  hardware                   REAL inventory of this machine (fo::probe_host_inventory)\n"
      "  self-test                  in-process only: scenario and analysis self-checks\n"
      "  snapshot                   membership, freshness census, cluster and publisher tables\n"
      "  clusters                   id, site, generation, epoch, readiness, currentness,\n"
      "                             nominal/idle accelerators, evidence class\n"
      "  sites                      site table\n"
      "  accelerator-classes        id, architecture, compute capability, per-device memory,\n"
      "                             capability generation\n"
      "  runtimes                   runtime/backend generation table\n"
      "  capabilities               capability publications per cluster (CapabilitySet::render)\n"
      "  capacity                   per-cluster capacity ledger and its accounting identity\n"
      "  stranded <workload-class>  fo::analyze_stranded_capacity report\n"
      "  usable <workload-class>    the usable/stranded/unknown summary only\n"
      "  fragmentation <wc>         fo::analyze_fragmentation finding\n"
      "  placements                 placement table with candidate completeness\n"
      "  placement <placement-id>   fo::explain_placement text\n"
      "  rejection <placement-id> <cluster-id>\n"
      "                             fo::explain_rejection text\n"
      "  migrations                 migration table\n"
      "  migration <migration-id>   fo::migration_analysis text\n"
      "  portability <workload-id> <cluster-id>\n"
      "                             published fo::portability assessment (or --evaluate)\n"
      "  compatibility <cluster-id> <workload-id>\n"
      "                             fo::compatibility text\n"
      "  mismatch                   fo::analyze_mismatch over the whole observation\n"
      "  drift                      fo::analyze_drift with no intended state; intended-state\n"
      "                             drift is reported as NOT evaluated\n"
      "  publishers                 publisher authority table (id, boot, live, fenced, watermark)\n"
      "  provenance                 precision / evidence-class / reason-basis legend\n"
      "  scenario                   in-process only: the whole synthetic scenario, its\n"
      "                             reproduction record and a summary\n"
      "\n"
      "options:\n"
      "  --host <h> --port <p>      remote coordinator endpoint\n"
      "  --seed <n>                 synthetic seed (default 20260101)\n"
      "  --evaluate                 portability only: re-evaluate instead of returning the\n"
      "                             published assessment\n"
      "  --json-free                accepted and ignored; this CLI never emits JSON\n"
      "\n"
      "administrative mutation is NOT part of this tool: fo-cli never publishes, registers,\n"
      "retires or fences anything. Use fo-publisher, or the coordinator's administrative\n"
      "channel, for those operations. fo-cli only observes and explains.\n"
      "\n"
      "exit codes: 0 success, 2 usage error, 3 not found / no data, 4 connection or protocol\n"
      "failure, 5 internal error.\n");
}

// --- argument parsing and dispatch ----------------------------------------------

struct Options {
  bool help = false;
  bool evaluate = false;
  bool json_free = false;
  bool has_host = false;
  bool has_port = false;
  std::string host;
  std::uint16_t port = 0;
  std::uint64_t seed = 20260101;
  std::string command;
  Args arguments;
};

bool parse_unsigned(const std::string& text, std::uint64_t limit, std::uint64_t& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || value > limit) return false;
  out = static_cast<std::uint64_t>(value);
  return true;
}

int parse_arguments(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    const auto take_value = [&](std::string& target) -> bool {
      if (i + 1 >= argc) return false;
      target = argv[++i];
      return true;
    };
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      return kExitOk;
    }
    if (argument == "--host") {
      if (!take_value(options.host)) return usage_error("--host requires a value");
      options.has_host = true;
    } else if (argument == "--port") {
      std::string value;
      if (!take_value(value)) return usage_error("--port requires a value");
      std::uint64_t port = 0;
      if (!parse_unsigned(value, 65535, port)) {
        return usage_error("--port must be an integer in 0..65535");
      }
      options.port = static_cast<std::uint16_t>(port);
      options.has_port = true;
    } else if (argument == "--seed") {
      std::string value;
      if (!take_value(value)) return usage_error("--seed requires a value");
      if (!parse_unsigned(value, UINT64_MAX, options.seed)) {
        return usage_error("--seed must be an unsigned integer");
      }
    } else if (argument == "--evaluate") {
      options.evaluate = true;
    } else if (argument == "--json-free") {
      options.json_free = true;
    } else if (!argument.empty() && argument.front() == '-') {
      return usage_error("unrecognised option " + argument);
    } else if (options.command.empty()) {
      options.command = argument;
    } else {
      options.arguments.push_back(argument);
    }
  }
  return kExitOk;
}

/// Commands whose answer does not come from a federation observation.
bool is_source_free_command(const std::string& command) {
  return command == "hardware" || command == "provenance";
}

int dispatch(const Environment& env, const Options& options) {
  const std::string& command = options.command;
  const Args& args = options.arguments;
  if (command == "bounds") return cmd_bounds(env, args);
  if (command == "hardware") return cmd_hardware(env.context, args);
  if (command == "self-test") return cmd_self_test(env.context, args);
  if (command == "scenario") return cmd_scenario(env.context, args);
  if (command == "snapshot") return cmd_snapshot(env, args);
  if (command == "clusters") return cmd_clusters(env, args);
  if (command == "sites") return cmd_sites(env, args);
  if (command == "accelerator-classes") return cmd_accelerator_classes(env, args);
  if (command == "runtimes") return cmd_runtimes(env, args);
  if (command == "capabilities") return cmd_capabilities(env, args);
  if (command == "capacity") return cmd_capacity(env, args);
  if (command == "stranded") return cmd_stranded(env, args, false);
  if (command == "usable") return cmd_stranded(env, args, true);
  if (command == "fragmentation") return cmd_fragmentation(env, args);
  if (command == "placements") return cmd_placements(env, args);
  if (command == "placement") return cmd_placement(env, args);
  if (command == "rejection") return cmd_rejection(env, args);
  if (command == "migrations") return cmd_migrations(env, args);
  if (command == "migration") return cmd_migration(env, args);
  if (command == "portability") return cmd_portability(env, args, options.evaluate);
  if (command == "compatibility") return cmd_compatibility(env, args);
  if (command == "mismatch") return cmd_mismatch(env, args);
  if (command == "drift") return cmd_drift(env, args);
  if (command == "publishers") return cmd_publishers(env, args);
  if (command == "provenance") return cmd_provenance(env, args);
  return usage_error("unknown command '" + command + "'");
}

int run(int argc, char** argv) {
  Options options;
  const int parsed = parse_arguments(argc, argv, options);
  if (parsed != kExitOk) return parsed;
  if (options.help) {
    print_help();
    return kExitOk;
  }
  if (options.command.empty()) {
    print_help();
    return kExitUsage;
  }
  if (options.has_host != options.has_port) {
    return usage_error("remote mode requires both --host and --port");
  }
  if (options.evaluate && options.command != "portability") {
    return usage_error("--evaluate is only valid for the portability command");
  }
  if (options.has_host && (options.command == "self-test" || options.command == "scenario")) {
    return usage_error(options.command +
                       " has no remote form: it runs the in-process synthetic scenario, and this "
                       "tool never publishes");
  }

  Environment env;
  env.context.seed = options.seed;
  env.context.json_free = options.json_free;
  if (is_source_free_command(options.command)) {
    env.context.source = options.command == "hardware" ? Source::Real : Source::Synthetic;
    return dispatch(env, options);
  }

  std::unique_ptr<InProcessFederation> local;
  fo::FederationClient client;
  if (options.has_host) {
    env.context.source = Source::Remote;
    env.context.host = options.host;
    env.context.port = options.port;
    fo::ClientConfig config;
    config.host = options.host;
    config.port = options.port;
    fo::Result<fo::FederationClient> connection = fo::FederationClient::connect(config);
    if (!connection.ok()) {
      // Failing to establish the connection is a connection failure, whatever code the
      // socket layer classified it under.
      std::printf("fo-cli: %s\n", connection.error().to_string().c_str());
      return kExitConnection;
    }
    client = connection.take();
    env.remote = &client;
  } else {
    env.context.source = Source::Synthetic;
    local = std::make_unique<InProcessFederation>(options.seed);
    if (!local->ok()) {
      std::printf("fo-cli: the synthetic scenario did not run: %s\n", local->error().c_str());
      return kExitInternal;
    }
    env.local = local.get();
  }
  const int result = dispatch(env, options);
  if (env.remote != nullptr) {
    const fo::Status closed = client.close();
    if (!closed.ok() && result == kExitOk) return report_error(closed.error());
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) { return run(argc, argv); }

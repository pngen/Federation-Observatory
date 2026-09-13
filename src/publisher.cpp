// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// A publishing agent process. One connection, one monotonically increasing sequence
// counter, one boot identity. It never re-uses a sequence number and never re-uses a
// boot identity, which is what makes publisher death and reincarnation observable.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "federation_observatory/explanation.hpp"
#include "federation_observatory/process.hpp"
#include "federation_observatory/publisher.hpp"
#include "federation_observatory/synthetic.hpp"

namespace fo {

class ObservationPublisher::Impl {
 public:
  PublisherConfig config;
  FederationClient client;
  Sequence sequence;
  CoordinatorEpoch epoch;
  Sequence last_accepted;

  [[nodiscard]] Status expect_accepted(const Result<protocol::PublicationAck>& ack,
                                       const char* what) {
    if (!ack.ok()) {
      return ack.error();
    }
    const protocol::PublicationAck& value = ack.value();
    // Duplicate and Superseded are successful outcomes: the coordinator accepted the
    // observation and did not change state. Only a refusal is a failure, and the
    // coordinator reports refusals as classified errors before this point.
    switch (value.disposition) {
      case IngestDisposition::Applied:
      case IngestDisposition::Duplicate:
      case IngestDisposition::Superseded:
        last_accepted = value.watermark;
        return Status::success();
      case IngestDisposition::Deferred:
        return Status::success();
    }
    return fail(ErrorCode::Internal,
                std::string("coordinator returned an unknown disposition for ") + what);
  }
};

ObservationPublisher::ObservationPublisher() = default;
ObservationPublisher::~ObservationPublisher() = default;
ObservationPublisher::ObservationPublisher(ObservationPublisher&&) noexcept = default;
ObservationPublisher& ObservationPublisher::operator=(ObservationPublisher&&) noexcept = default;

Result<ObservationPublisher> ObservationPublisher::connect(const PublisherConfig& config) {
  ClientConfig client_config;
  client_config.host = config.host;
  client_config.port = config.port;
  client_config.bounds = config.bounds;
  client_config.poll_millis = config.poll_millis;
  Result<FederationClient> client = FederationClient::connect(client_config);
  if (!client.ok()) {
    return client.error();
  }
  ObservationPublisher publisher;
  publisher.impl_ = std::make_unique<Impl>();
  publisher.impl_->config = config;
  publisher.impl_->client = client.take();
  const Result<protocol::HelloReply> handshake = publisher.impl_->client.hello(config.publisher);
  if (!handshake.ok()) {
    return handshake.error();
  }
  publisher.impl_->epoch = handshake.value().epoch;
  return publisher;
}

const PublisherConfig& ObservationPublisher::config() const noexcept {
  static const PublisherConfig kDefault{};
  return impl_ != nullptr ? impl_->config : kDefault;
}

Sequence ObservationPublisher::last_accepted_sequence() const noexcept {
  return impl_ != nullptr ? impl_->last_accepted : Sequence{};
}

Sequence ObservationPublisher::next_sequence() noexcept {
  if (impl_ == nullptr) {
    return Sequence{};
  }
  impl_->sequence = impl_->sequence.next();
  if (impl_->sequence.is_zero()) {
    impl_->sequence = Sequence{1};
  }
  return impl_->sequence;
}

CoordinatorEpoch ObservationPublisher::coordinator_epoch() const noexcept {
  return impl_ != nullptr ? impl_->epoch : CoordinatorEpoch{};
}

PublicationContext ObservationPublisher::make_context(EvidenceGeneration evidence_generation) {
  PublicationContext context;
  if (impl_ == nullptr) {
    return context;
  }
  context.publisher = impl_->config.publisher;
  context.boot = impl_->config.boot;
  context.coordinator_epoch = impl_->epoch;
  context.federation = impl_->config.federation;
  context.federation_generation = impl_->config.federation_generation;
  context.sequence = next_sequence();
  context.observed_at = now_unix_nanos();
  context.precision = impl_->config.precision;
  context.evidence_class = impl_->config.evidence_class;
  context.provenance = impl_->config.provenance;
  context.evidence_generation = evidence_generation;
  return context;
}

Status ObservationPublisher::register_self() {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const PublicationContext context = make_context(EvidenceGeneration{1});
  const Result<protocol::PublicationAck> ack =
      impl_->client.register_publisher(context, impl_->config.role);
  return impl_->expect_accepted(ack, "publisher registration");
}

Status ObservationPublisher::heartbeat() {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const Result<protocol::ReplyPayload> reply = impl_->client.query_health();
  if (!reply.ok()) {
    return reply.error();
  }
  return Status::success();
}

#define FO_PUBLISHER_METHOD(MethodName, RecordType, ClientMethod)                \
  Status ObservationPublisher::MethodName(RecordType record) {                   \
    if (impl_ == nullptr) {                                                      \
      return fail(ErrorCode::ConnectionClosed, "publisher is not connected");    \
    }                                                                            \
    const PublicationContext context = make_context(EvidenceGeneration{1});      \
    const Result<protocol::PublicationAck> ack = impl_->client.ClientMethod(context, std::move(record)); \
    return impl_->expect_accepted(ack, #MethodName);                             \
  }

FO_PUBLISHER_METHOD(publish_federation, FederationRecord, register_federation)
FO_PUBLISHER_METHOD(publish_site, SiteRecord, register_site)
FO_PUBLISHER_METHOD(publish_cluster, ClusterRecord, register_cluster)
FO_PUBLISHER_METHOD(publish_accelerator_class, AcceleratorClassRecord, register_accelerator_class)
FO_PUBLISHER_METHOD(publish_runtime, RuntimeRecord, register_runtime)
FO_PUBLISHER_METHOD(publish_backend, BackendRecord, register_backend)
FO_PUBLISHER_METHOD(publish_domain, DomainRecord, register_domain)
FO_PUBLISHER_METHOD(publish_policy, PolicyRecord, register_policy)
FO_PUBLISHER_METHOD(publish_artifact, ArtifactRecord, register_artifact)
FO_PUBLISHER_METHOD(publish_workload_class, WorkloadClassRecord, register_workload_class)
FO_PUBLISHER_METHOD(publish_workload, WorkloadRecord, register_workload)
FO_PUBLISHER_METHOD(publish_placement, PlacementRecord, publish_placement)
FO_PUBLISHER_METHOD(publish_migration, MigrationRecord, publish_migration)
FO_PUBLISHER_METHOD(publish_portability, PortabilityAssessment, publish_portability)

#undef FO_PUBLISHER_METHOD

Status ObservationPublisher::publish_capability(const ClusterId& cluster,
                                                ClusterGeneration cluster_generation,
                                                AcceleratorCapabilityGeneration capability_generation,
                                                CapabilitySet capabilities) {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const PublicationContext context = make_context(EvidenceGeneration{1});
  const Result<protocol::PublicationAck> ack = impl_->client.publish_capability(
      context, cluster, cluster_generation, capability_generation, std::move(capabilities));
  return impl_->expect_accepted(ack, "capability");
}

Status ObservationPublisher::publish_capacity(const ClusterId& cluster,
                                              ClusterGeneration cluster_generation,
                                              CapacityGeneration capacity_generation,
                                              std::vector<CapacityPool> pools) {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const PublicationContext context = make_context(EvidenceGeneration{1});
  const Result<protocol::PublicationAck> ack = impl_->client.publish_capacity(
      context, cluster, cluster_generation, capacity_generation, std::move(pools));
  return impl_->expect_accepted(ack, "capacity");
}

Status ObservationPublisher::publish_migration_stage(const MigrationId& migration,
                                                     MigrationGeneration generation,
                                                     MigrationStage stage, std::string detail) {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const PublicationContext context = make_context(EvidenceGeneration{1});
  const Result<protocol::PublicationAck> ack = impl_->client.publish_migration_stage(
      context, migration, generation, stage, std::move(detail));
  return impl_->expect_accepted(ack, "migration stage");
}

Status ObservationPublisher::retire_cluster(const ClusterId& cluster,
                                            ClusterGeneration generation, std::string reason) {
  if (impl_ == nullptr) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  const PublicationContext context = make_context(EvidenceGeneration{1});
  const Result<protocol::PublicationAck> ack =
      impl_->client.retire_cluster(context, cluster, generation, std::move(reason));
  return impl_->expect_accepted(ack, "cluster retirement");
}

FederationClient& ObservationPublisher::client() noexcept { return impl_->client; }

Status ObservationPublisher::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  return impl_->client.close();
}

namespace {

struct PublisherOptions {
  PublisherConfig config;
  std::uint64_t seed = 20260101;
  std::string id_prefix;
  bool linger = false;
  bool verbose = false;
  std::string ready_file;
  std::string result_file;
  std::vector<std::string> steps;
};

void print_usage() {
  std::printf(
      "fo-publisher: publish a deterministic synthetic federation over framed TCP.\n"
      "\n"
      "usage: fo-publisher [options]\n"
      "  --host <host>            coordinator host (default 127.0.0.1)\n"
      "  --port <port>            coordinator port (required)\n"
      "  --publisher <id>         publisher identity (default synth-publisher)\n"
      "  --boot <n>               boot generation; must be fresh per process (default 1)\n"
      "  --federation <id>        federation identity (default synth-fed)\n"
      "  --seed <n>               deterministic scenario seed\n"
      "  --namespace <prefix>     prefix for every generated identifier, so that several\n"
      "                           publishers can own disjoint cluster sets under one federation\n"
      "  --steps <a,b,c>          run only these scenario steps, in order\n"
      "  --linger                 keep the connection open after publishing\n"
      "  --ready-file <path>      write this file once publishing has completed\n"
      "  --result-file <path>     write the scenario report to this file\n"
      "  --verbose                print each step to stdout\n");
}

Result<PublisherOptions> parse_args(int argc, char** argv) {
  PublisherOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag(argv[i]);
    const auto next = [&](std::string& target) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      target = argv[++i];
      return true;
    };
    std::string value;
    if (flag == "--help" || flag == "-h") {
      print_usage();
      std::exit(0);
    } else if (flag == "--host") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--host requires a value");
      options.config.host = value;
    } else if (flag == "--port") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--port requires a value");
      const long port = std::strtol(value.c_str(), nullptr, 10);
      if (port <= 0 || port > 65535) {
        return Error(ErrorCode::InvalidArgument, "--port is out of range", value);
      }
      options.config.port = static_cast<std::uint16_t>(port);
    } else if (flag == "--publisher") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--publisher requires a value");
      const Result<PublisherId> id = PublisherId::parse(value);
      if (!id.ok()) return id.error();
      options.config.publisher = id.value();
    } else if (flag == "--boot") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--boot requires a value");
      options.config.boot = BootGeneration{static_cast<std::uint64_t>(std::strtoull(value.c_str(), nullptr, 10))};
    } else if (flag == "--federation") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--federation requires a value");
      const Result<FederationId> id = FederationId::parse(value);
      if (!id.ok()) return id.error();
      options.config.federation = id.value();
    } else if (flag == "--namespace") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--namespace requires a value");
      options.id_prefix = value;
    } else if (flag == "--seed") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--seed requires a value");
      options.seed = std::strtoull(value.c_str(), nullptr, 10);
    } else if (flag == "--steps") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--steps requires a value");
      std::string current;
      for (const char c : value) {
        if (c == ',') {
          if (!current.empty()) options.steps.push_back(current);
          current.clear();
        } else {
          current.push_back(c);
        }
      }
      if (!current.empty()) options.steps.push_back(current);
    } else if (flag == "--linger") {
      options.linger = true;
    } else if (flag == "--verbose") {
      options.verbose = true;
    } else if (flag == "--ready-file") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--ready-file requires a value");
      options.ready_file = value;
    } else if (flag == "--result-file") {
      if (!next(value)) return Error(ErrorCode::InvalidArgument, "--result-file requires a value");
      options.result_file = value;
    } else {
      return Error(ErrorCode::InvalidArgument, "unrecognised option", flag);
    }
  }
  if (options.config.port == 0) {
    return Error(ErrorCode::InvalidArgument, "--port is required");
  }
  return options;
}

Result<SyntheticStep> parse_step(const std::string& name) {
  for (std::size_t i = 0; i < kSyntheticStepCount; ++i) {
    const auto step = static_cast<SyntheticStep>(i);
    if (to_string(step) == name) {
      return step;
    }
  }
  return Error(ErrorCode::InvalidArgument, "unknown synthetic step", name);
}

}  // namespace

int run_publisher_main(int argc, char** argv) {
  const Result<PublisherOptions> parsed = parse_args(argc, argv);
  if (!parsed.ok()) {
    std::printf("fo-publisher: %s\n", parsed.error().to_string().c_str());
    print_usage();
    return 2;
  }
  const PublisherOptions& options = parsed.value();

  Result<ObservationPublisher> publisher = ObservationPublisher::connect(options.config);
  if (!publisher.ok()) {
    std::printf("fo-publisher: could not connect to %s:%u: %s\n", options.config.host.c_str(),
                static_cast<unsigned>(options.config.port),
                publisher.error().to_string().c_str());
    return 3;
  }
  SyntheticConfig config;
  config.seed = options.seed;
  config.federation = options.config.federation;
  config.publisher = options.config.publisher;
  config.boot = options.config.boot;
  config.id_prefix = options.id_prefix;
  config.display_name = options.config.federation.value();
  NetworkSink sink(publisher.value());
  // Register exactly once, through the sink the scenario will publish through. The sink
  // is idempotent, so the registration the scenario performs before it runs is a no-op.
  const Status registered = sink.register_self();
  if (!registered.ok()) {
    std::printf("fo-publisher: registration refused: %s\n", registered.error().to_string().c_str());
    return 4;
  }
  SyntheticFederation scenario(config, sink);

  Status status = Status::success();
  if (options.steps.empty()) {
    status = scenario.run_all();
  } else {
    for (const std::string& name : options.steps) {
      const Result<SyntheticStep> step = parse_step(name);
      if (!step.ok()) {
        std::printf("fo-publisher: %s\n", step.error().to_string().c_str());
        return 2;
      }
      status = scenario.run(step.value());
      if (options.verbose) {
        std::printf("fo-publisher: step %s -> %s\n", name.c_str(), status.to_string().c_str());
      }
      if (!status.ok()) {
        break;
      }
    }
  }
  if (!status.ok()) {
    std::printf("fo-publisher: scenario failed: %s\n", status.error().to_string().c_str());
    return 5;
  }

  const std::string report = scenario.reproduction_record();
  std::printf("%s\n", report.c_str());
  if (!options.result_file.empty()) {
    const Status written = write_text_file(options.result_file, report);
    if (!written.ok()) {
      std::printf("fo-publisher: could not write the result file: %s\n",
                  written.error().to_string().c_str());
      return 6;
    }
  }
  if (!options.ready_file.empty()) {
    const Status written =
        write_text_file(options.ready_file, std::to_string(scenario.expectations().placements_expected));
    if (!written.ok()) {
      std::printf("fo-publisher: could not write the ready file: %s\n",
                  written.error().to_string().c_str());
      return 6;
    }
  }

  if (options.linger) {
    // Stay published and alive until the coordinator or an operator stops this process.
    // This is the state a publisher-death proof kills.
    for (;;) {
      sleep_millis(50);
      const Status alive = publisher.value().heartbeat();
      if (!alive.ok()) {
        std::printf("fo-publisher: connection lost: %s\n", alive.error().to_string().c_str());
        return 0;
      }
    }
  }

  const Status closed = publisher.value().close();
  (void)closed;
  return 0;
}

}  // namespace fo

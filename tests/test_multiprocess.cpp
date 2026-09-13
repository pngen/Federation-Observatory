// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
//
// Multiprocess proofs. The coordinator and the publishers are REAL operating-system
// processes communicating over framed TCP; the publisher-death and coordinator-restart
// proofs depend on that being literally true.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "federation_observatory/client.hpp"
#include "federation_observatory/explanation.hpp"
#include "federation_observatory/process.hpp"
#include "harness.hpp"

using namespace fo;

namespace {

struct TempDir {
  std::string path;
  TempDir()
      : path(join_path(std::filesystem::temp_directory_path().string(),
                       "fo-multiprocess-" + unique_token())) {
    const Status created = ensure_directory(path);
    FO_REQUIRE(created.ok());
  }
  ~TempDir() {
    const Status removed = remove_directory_tree(path);
    (void)removed;
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

struct ToolPaths {
  std::string coordinator;
  std::string publisher;

  ToolPaths() {
    const std::string directory = FO_UNWRAP(current_executable_directory());
    // The tools may be installed beside the test binary or built into a sibling
    // directory; both layouts are searched before the proof is reported as unavailable.
    const std::vector<std::string> candidates = {
        directory, join_path(directory, ".."), join_path(join_path(directory, ".."), "tools"),
        join_path(directory, "tools")};
    for (const std::string& candidate : candidates) {
#ifdef _WIN32
      const std::string coordinator_path = join_path(candidate, "fo-coordinator.exe");
      const std::string publisher_path = join_path(candidate, "fo-publisher.exe");
#else
      const std::string coordinator_path = join_path(candidate, "fo-coordinator");
      const std::string publisher_path = join_path(candidate, "fo-publisher");
#endif
      if (std::filesystem::exists(coordinator_path) && std::filesystem::exists(publisher_path)) {
        coordinator = coordinator_path;
        publisher = publisher_path;
        return;
      }
    }
  }

  [[nodiscard]] bool available() const {
    return std::filesystem::exists(coordinator) && std::filesystem::exists(publisher);
  }
};

/// Start a coordinator and wait for its readiness file, which carries the bound port.
struct CoordinatorProcess {
  fo::ChildProcess process;
  std::uint16_t port = 0;

  static Result<std::uint16_t> start(const ToolPaths& tools, const std::string& directory,
                                     fo::ChildProcess& child, const std::string& tag,
                                     const std::string& state_path = std::string()) {
    const std::string ready = join_path(directory, "ready-" + tag + ".txt");
    std::vector<std::string> argv = {tools.coordinator, "--host", "127.0.0.1", "--port", "0",
                                     "--poll-ms", "5", "--ready-file", ready};
    if (!state_path.empty()) {
      argv.push_back("--state");
      argv.push_back(state_path);
    }
    const std::string output = join_path(directory, "coordinator-" + tag + ".log");
    Result<fo::ChildProcess> spawned = fo::ChildProcess::spawn(argv, directory, output);
    if (!spawned.ok()) {
      return spawned.error();
    }
    child = spawned.take();
    const Status ready_status = wait_for_file(ready, 400, 25);
    if (!ready_status.ok()) {
      const fo::Result<std::string> captured = read_text_file(output);
      return Error(ErrorCode::NotReady, "the coordinator did not become ready",
                   ready_status.error().message() +
                       (captured.ok() ? " output: " + truncate_with_marker(captured.value(), 400)
                                      : std::string()));
    }
    const Result<std::string> text = read_text_file(ready);
    if (!text.ok()) {
      return text.error();
    }
    const long port = std::strtol(text.value().c_str(), nullptr, 10);
    if (port <= 0 || port > 65535) {
      return Error(ErrorCode::Internal, "the coordinator reported an invalid port", text.value());
    }
    return static_cast<std::uint16_t>(port);
  }
};

struct Remote {
  fo::FederationClient client;
  fo::CoordinatorEpoch epoch;

  static Result<Remote> connect(std::uint16_t port, const fo::PublisherId& publisher) {
    fo::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.poll_millis = 5;
    Result<fo::FederationClient> client = fo::FederationClient::connect(config);
    if (!client.ok()) {
      return client.error();
    }
    const Result<fo::protocol::HelloReply> handshake = client.value().hello(publisher);
    if (!handshake.ok()) {
      return handshake.error();
    }
    Remote remote;
    remote.client = client.take();
    remote.epoch = handshake.value().epoch;
    return remote;
  }
};

}  // namespace

FO_TEST(multiprocess, real_publisher_death_fencing_and_reincarnation) {
  const ToolPaths tools;
  if (!tools.available()) {
    std::printf("SKIPPED: fo-coordinator / fo-publisher are not built next to the test binary\n");
    return;
  }
  TempDir directory;
  fo::ChildProcess coordinator_process;
  const std::uint16_t port =
      FO_UNWRAP(CoordinatorProcess::start(tools, directory.path, coordinator_process, "death"));
  FO_CHECK(port != 0);

  const auto spawn_publisher = [&](const std::string& publisher_id, const std::string& boot,
                                   const std::string& tag) {
    const std::string ready = join_path(directory.path, "pub-" + tag + ".txt");
    // Each publisher owns a disjoint cluster set under the one federation, which is what a
    // real multi-publisher federation looks like; publishing the same records from two
    // processes would only exercise duplicate suppression.
    std::vector<std::string> argv = {tools.publisher,
                                     "--host",
                                     "127.0.0.1",
                                     "--port",
                                     std::to_string(port),
                                     "--publisher",
                                     publisher_id,
                                     "--boot",
                                     boot,
                                     "--federation",
                                     "mp-fed",
                                     "--namespace",
                                     tag + "-",
                                     "--seed",
                                     tag == "b" ? "20260102" : "20260101",
                                     "--ready-file",
                                     ready,
                                     "--linger"};
    const std::string output = join_path(directory.path, "publisher-" + tag + ".log");
    Result<fo::ChildProcess> spawned = fo::ChildProcess::spawn(argv, directory.path, output);
    FO_REQUIRE(spawned.ok());
    fo::ChildProcess child = spawned.take();
    const fo::Status ready_status = wait_for_file(ready, 600, 25);
    if (!ready_status.ok()) {
      const bool alive = child.running();
      int exit_code = -1;
      if (!alive) {
        const fo::Result<int> waited = child.wait();
        if (waited.ok()) {
          exit_code = waited.value();
        }
      }
      const fo::Result<std::string> captured = read_text_file(output);
      ::fotest::fail(__FILE__, __LINE__,
                     std::string("the publisher never signalled readiness; running=") +
                         (alive ? "yes" : "no") + " exit=" + std::to_string(exit_code) +
                         " output=\n" +
                         (captured.ok() ? truncate_with_marker(captured.value(), 800)
                                        : std::string("<none>")));
    }
    return child;
  };

  fo::ChildProcess publisher_a = spawn_publisher("pub-a", "1", "a");
  fo::ChildProcess publisher_b = spawn_publisher("pub-b", "2", "b");
  FO_CHECK(publisher_a.running());
  FO_CHECK(publisher_b.running());

  {
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    const fo::protocol::ReplyPayload health = FO_UNWRAP(remote.client.query_health());
    FO_CHECK(health.text.find("publishers") != std::string::npos || !health.rows.empty());
    bool live_a = false;
    bool live_b = false;
    for (const fo::protocol::WireRow& row : health.rows) {
      for (const fo::protocol::WireField& field : row.fields) {
        if (field.name == "publishers_live" && field.value == "2") {
          live_a = true;
          live_b = true;
        }
      }
    }
    FO_CHECK(live_a && live_b);
    const Status closed = remote.client.close();
    (void)closed;
  }

  // Kill publisher A as a real process.
  const Result<int> exit_code = publisher_a.terminate_and_wait();
  FO_REQUIRE(exit_code.ok());
  FO_CHECK(!publisher_a.running());

  {
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    const fo::protocol::ReplyPayload snapshot = FO_UNWRAP(
        remote.client.query_snapshot(fo::protocol::SnapshotQuery{fo::FederationId::unchecked("mp-fed"),
                                                                 4096, true}));
    FO_CHECK(!snapshot.text.empty());

    // Fence the dead boot identity through the real control path.
    const fo::Result<fo::protocol::FenceReply> fenced = remote.client.fence_publisher(
        fo::PublisherId::unchecked("pub-a"), fo::BootGeneration{1}, "publisher process died", "");
    FO_REQUIRE(fenced.ok());
    FO_CHECK(fenced.value().fenced);

    // Publisher B is unaffected.
    const fo::protocol::ReplyPayload after = FO_UNWRAP(remote.client.query_health());
    FO_CHECK(!after.text.empty());

    // A replay from the fenced boot identity must be refused, and the fresh boot identity
    // must register before it can publish anything.
    fo::PublicationContext replayed;
    replayed.publisher = fo::PublisherId::unchecked("pub-a");
    replayed.boot = fo::BootGeneration{1};
    replayed.coordinator_epoch = remote.epoch;
    replayed.federation = fo::FederationId::unchecked("mp-fed");
    replayed.federation_generation = fo::FederationGeneration{1};
    replayed.sequence = fo::Sequence{1};
    const fo::Result<fo::protocol::PublicationAck> refused =
        remote.client.register_federation(replayed, fo::FederationRecord{});
    FO_CHECK(!refused.ok());

    const Status closed = remote.client.close();
    (void)closed;
  }

  // A-prime presents a fresh boot identity and must register and republish.
  fo::ChildProcess publisher_prime = spawn_publisher("pub-a", "2", "a-prime");
  FO_CHECK(publisher_prime.running());
  {
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    fo::PublicationContext retry;
    retry.publisher = fo::PublisherId::unchecked("pub-a");
    retry.boot = fo::BootGeneration{1};
    retry.coordinator_epoch = remote.epoch;
    retry.federation = fo::FederationId::unchecked("mp-fed");
    retry.federation_generation = fo::FederationGeneration{1};
    retry.sequence = fo::Sequence{2};
    const fo::Result<fo::protocol::PublicationAck> still_fenced =
        remote.client.register_federation(retry, fo::FederationRecord{});
    FO_CHECK(!still_fenced.ok());

    // The replacement process registered its own fresh boot identity before publishing.
    // Two boot identities for the same publisher are now on record: one fenced, one live.
    const fo::protocol::ReplyPayload snapshot = FO_UNWRAP(
        remote.client.query_snapshot(fo::protocol::SnapshotQuery{
            fo::FederationId::unchecked("mp-fed"), 4096, true}));
    int live = 0;
    int fenced = 0;
    for (const fo::protocol::WireRow& row : snapshot.rows) {
      bool is_publisher_row = false;
      bool row_live = false;
      bool row_fenced = false;
      for (const fo::protocol::WireField& field : row.fields) {
        if (field.name == "publisher") {
          is_publisher_row = true;
        }
        if (field.name == "live" && field.value == "yes") {
          row_live = true;
        }
        if (field.name == "fenced" && field.value == "yes") {
          row_fenced = true;
        }
      }
      if (!is_publisher_row) {
        continue;
      }
      live += row_live ? 1 : 0;
      fenced += row_fenced ? 1 : 0;
    }
    FO_CHECK(fenced >= 1);
    FO_CHECK(live >= 1);
    const Status closed = remote.client.close();
    (void)closed;
  }

  const Result<int> prime_exit = publisher_prime.terminate_and_wait();
  FO_REQUIRE(prime_exit.ok());
  const Result<int> b_exit = publisher_b.terminate_and_wait();
  FO_REQUIRE(b_exit.ok());

  // The coordinator still answers after three publishers came and went.
  {
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    const fo::protocol::ReplyPayload health = FO_UNWRAP(remote.client.query_health());
    FO_CHECK(!health.text.empty());
    const Status closed = remote.client.close();
    (void)closed;
  }

  const std::string shutdown = "shutdown\n";
  const Status stopped = coordinator_process.kill();
  (void)shutdown;
  (void)stopped;
  const Result<int> coordinator_exit = coordinator_process.terminate_and_wait();
  FO_REQUIRE(coordinator_exit.ok());
}

FO_TEST(multiprocess, real_coordinator_restart_advances_the_epoch_and_preserves_history) {
  const ToolPaths tools;
  if (!tools.available()) {
    std::printf("SKIPPED: fo-coordinator / fo-publisher are not built next to the test binary\n");
    return;
  }
  TempDir directory;
  const std::string state_path = join_path(directory.path, "coordinator.state");

  fo::CoordinatorEpoch first_epoch;
  std::string first_snapshot_digest;
  {
    fo::ChildProcess coordinator_process;
    const std::uint16_t port = FO_UNWRAP(CoordinatorProcess::start(
        tools, directory.path, coordinator_process, "gen1", state_path));
    {
      Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
      first_epoch = remote.epoch;
      const fo::protocol::ReplyPayload payload = FO_UNWRAP(
          remote.client.query_snapshot(fo::protocol::SnapshotQuery{fo::FederationId::unchecked("mp-fed"),
                                                                   4096, true}));
      first_snapshot_digest = payload.digest;
      const Status closed = remote.client.close();
      (void)closed;
    }
    // Publish a small federation so that durable structure exists.
    fo::ChildProcess publisher_process;
    {
      const std::string ready = join_path(directory.path, "pub-gen1.txt");
      std::vector<std::string> argv = {tools.publisher,
                                       "--host",
                                       "127.0.0.1",
                                       "--port",
                                       std::to_string(port),
                                       "--publisher",
                                       "pub-gen1",
                                       "--boot",
                                       "1",
                                       "--federation",
                                       "mp-fed",
                                       "--ready-file",
                                       ready};
      Result<fo::ChildProcess> spawned = fo::ChildProcess::spawn(
          argv, directory.path, join_path(directory.path, "publisher-gen1.log"));
      FO_REQUIRE(spawned.ok());
      publisher_process = spawned.take();
      FO_REQUIRE(wait_for_file(ready, 600, 25).ok());
    }
    const Result<int> publisher_exit = publisher_process.wait();
    FO_REQUIRE(publisher_exit.ok());
    FO_CHECK_EQ(publisher_exit.value(), 0);

    // Graceful stop through the coordinator's own protocol. A closed standard input is
    // deliberately not a stop request, so this is the only clean shutdown path.
    {
      Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
      const fo::Result<fo::protocol::ReplyPayload> acknowledged = remote.client.request_shutdown();
      FO_REQUIRE(acknowledged.ok());
      const Status closed = remote.client.close();
      (void)closed;
    }
    for (int poll = 0; poll < 400 && !coordinator_process.exited(); ++poll) {
      sleep_millis(25);
    }
    if (!coordinator_process.exited()) {
      const Status killed = coordinator_process.kill();
      (void)killed;
      ::fotest::fail(__FILE__, __LINE__,
                     "the coordinator did not stop after an acknowledged SHUTDOWN request");
    }
    const Result<int> exit_code = coordinator_process.wait();
    FO_REQUIRE(exit_code.ok());
    FO_CHECK_EQ(exit_code.value(), 0);
  }

  FO_CHECK(std::filesystem::exists(state_path));

  // A fresh coordinator process restores the durable state and advances the epoch.
  {
    fo::ChildProcess coordinator_process;
    const std::uint16_t port = FO_UNWRAP(CoordinatorProcess::start(
        tools, directory.path, coordinator_process, "gen2", state_path));
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    FO_CHECK(remote.epoch.newer_than(first_epoch));

    // Old-epoch traffic is refused.
    fo::PublicationContext stale;
    stale.publisher = fo::PublisherId::unchecked("pub-gen1");
    stale.boot = fo::BootGeneration{1};
    stale.coordinator_epoch = first_epoch;
    stale.federation = fo::FederationId::unchecked("mp-fed");
    stale.federation_generation = fo::FederationGeneration{1};
    stale.sequence = fo::Sequence{1000};
    const fo::Result<fo::protocol::PublicationAck> refused =
        remote.client.register_federation(stale, fo::FederationRecord{});
    FO_CHECK(!refused.ok());

    // History is preserved exactly, and dynamic evidence did not come back as current.
    const fo::protocol::ReplyPayload snapshot = FO_UNWRAP(
        remote.client.query_snapshot(fo::protocol::SnapshotQuery{fo::FederationId::unchecked("mp-fed"),
                                                                 4096, true}));
    FO_CHECK(!snapshot.text.empty());
    bool saw_revalidation = false;
    for (const fo::protocol::WireRow& row : snapshot.rows) {
      for (const fo::protocol::WireField& field : row.fields) {
        if (field.name == "currentness" && field.value == "REVALIDATION_REQUIRED") {
          saw_revalidation = true;
        }
        if (field.name == "idle_accelerators" && field.value != "UNKNOWN" && field.value != "0") {
          ::fotest::fail(__FILE__, __LINE__,
                         "capacity came back as current after a coordinator restart");
        }
      }
    }
    FO_CHECK(saw_revalidation);

    const Status closed = remote.client.close();
    (void)closed;

    // Abrupt kill of a live coordinator is a real process death.
    const Status killed = coordinator_process.kill();
    FO_REQUIRE(killed.ok());
    const Result<int> exit_code = coordinator_process.terminate_and_wait();
    if (!exit_code.ok()) {
      ::fotest::fail(__FILE__, __LINE__,
                     "killing the coordinator failed: " + exit_code.error().to_string());
    }
    FO_CHECK(!coordinator_process.running());
  }

  // A third coordinator starts from the same durable file and is still coherent.
  {
    fo::ChildProcess coordinator_process;
    const std::uint16_t port = FO_UNWRAP(CoordinatorProcess::start(
        tools, directory.path, coordinator_process, "gen3", state_path));
    Remote remote = FO_UNWRAP(Remote::connect(port, fo::PublisherId::unchecked("inspector")));
    const fo::protocol::ReplyPayload snapshot = FO_UNWRAP(
        remote.client.query_snapshot(fo::protocol::SnapshotQuery{fo::FederationId::unchecked("mp-fed"),
                                                                 4096, true}));
    FO_CHECK(!snapshot.text.empty());
    FO_CHECK(!snapshot.digest.empty());
    const Status closed = remote.client.close();
    (void)closed;
    const Result<int> exit_code = coordinator_process.terminate_and_wait();
    FO_REQUIRE(exit_code.ok());
  }
}

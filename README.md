# Federation Observatory

**Federation Observatory 1.0.0** is a production-grade, open-source, vendor-neutral **C++20**
runtime for observing, attributing, correlating and explaining the behaviour of a
**federation** of clusters, sites, accelerator families, runtime domains and capacity pools.

It is **observational**. It does not schedule, migrate, admit, partition, route or recompile
anything. It explains what those systems did, on the evidence they published, and it says
explicitly when the evidence does not support a conclusion.

## The systems question

> Across a federation of clusters, sites, accelerator families, runtime domains and capacity
> pools, what actually happened to placement, migration, compatibility, portability and usable
> capacity — and why?

The runtime exists to distinguish

> **A workload ran in cluster B.**

from

> **This exact workload generation was placed in cluster B under this exact federation, cluster,
> capability, policy, topology, runtime, artifact and accelerator evidence; cluster A was
> rejected for named compatibility and capacity reasons; 28% of nominal capacity elsewhere was
> stranded by incompatibility; and the explanation remains current under present generations.**

## Exact observational boundary

**Federation Observatory owns:** federation, site, cluster, accelerator-family, runtime and
backend identity; capability publication and comparison; compatibility evidence; placement
observation and outcome attribution; rejection attribution; migration observation and
stage attribution; capacity observation; usable-versus-nominal and stranded-capacity analysis;
federation fragmentation; cross-cluster locality observation; portability-status observation and
failure attribution; artifact/runtime compatibility observation; version and capability mismatch
observation; policy outcome observation; placement and federation drift; deterministic
explanations; immutable snapshots; evidence provenance; precision; current/stale state; durable
structural state; conservative recovery; multiprocess publisher authority; and
REAL/SYNTHETIC/UNSUPPORTED classification.

**It does not own**, and does not absorb the authority of: federation or cluster scheduling,
Heterogeneous Accelerator Federation decisions, Resource Broker decisions, workload execution,
model routing, admission control, accelerator partitioning or virtualization, cluster lifecycle
or failover, data or storage movement, network routing, policy enforcement, artifact conversion,
binary or kernel recompilation, runtime adaptation, migration execution, resource reservation,
capacity creation, or hardware capability truth itself. It consumes evidence from those systems.

## Relationship to Heterogeneous Accelerator Federation

Heterogeneous Accelerator Federation *governs* federated execution across different accelerator
types and environments. Federation Observatory *explains what the federation did*.

| Heterogeneous Accelerator Federation | Federation Observatory |
|---|---|
| "Run workload W on cluster C because it satisfies the current capability and compatibility contract." | "Cluster A was rejected because its runtime ABI generation mismatched the artifact. Cluster B passed capability but failed locality policy. Cluster C was selected and consumed 4 accelerators. Cluster D retained 12 nominal accelerators but 8 were unusable for this workload family because required capability X was absent." |

## Architectural doctrine

The runtime preserves these distinctions and is tested on them:

* Observed is not authoritative control.
* Rejected is not unavailable.
* Unavailable is not incompatible.
* Incompatible is not unsupported.
* Nominal capacity is not usable capacity.
* Unused capacity is not necessarily stranded capacity.
* Migrated is not successfully portable.
* A workload running after migration does not prove byte-for-byte state portability.
* A cluster remaining registered after restart does not make its dynamic evidence current.
* A capability string match does not prove semantic compatibility.
* UNKNOWN, AMBIGUOUS and UNATTRIBUTED remain first-class outcomes.

Nothing in the runtime manufactures precision or causality from incomplete evidence.

## Strong identities and generations

Every identity is a distinct type (`FederationId`, `SiteId`, `ClusterId`,
`AcceleratorClassId`, `RuntimeId`, `BackendId`, `ArtifactId`, `WorkloadId`,
`PlacementId`, `MigrationId`, `PublisherId`, `PolicyId`, `DomainId`, …), so an
identifier cannot be passed where a different kind is expected.

Every generation numbers state that can become stale or change semantics:
`FederationGeneration`, `SiteGeneration`, `ClusterGeneration`, `ClusterEpoch`,
`AcceleratorCapabilityGeneration`, `RuntimeGeneration`, `BackendGeneration`,
`ArtifactGeneration`, `WorkloadGeneration`, `PlacementGeneration`,
`MigrationGeneration`, `PolicyGeneration`, `CompatibilityGeneration`,
`CapacityGeneration`, `TopologyGeneration`, `EvidenceGeneration`,
`CoordinatorEpoch`, `SnapshotGeneration` and `BootGeneration`. Generation 0 is reserved
and always means *unset*; it never compares newer than a real generation.

## Federation model

A snapshot carries federation identity and generation, member sites and clusters with their
generations, accelerator families, runtime and backend classes, capability publications, policy
and compatibility and capacity and topology generations, capacity state, locality domains, current
publisher evidence, per-record currentness, provenance, and REAL/SYNTHETIC/UNSUPPORTED state.
A federation is a graph of related records, not a flat list.

## Cluster model

A cluster record supports a stable identity, its site and domain, cluster generation and epoch,
accelerator classes and per-class capacity pools, runtime/backend stack, driver and runtime and
ABI identifiers, supported artifact and kernel formats, memory capacity, network and storage
capability summaries, health/readiness evidence, allocated/reserved/draining/unusable/idle
capacity, failure-domain metadata, topology/capability/capacity/runtime/compatibility/evidence
generations, and the provenance of every current fact. Not every field must exist: UNKNOWN is
valid, and the runtime reports it rather than guessing.

## Capability model

Capabilities are structured, not free-form strings. A capability key is a typed enumerator
(accelerator architecture, precision support, memory size and bandwidth class, interconnect,
partition support, runtime and driver API, compiler target, kernel format, collective support,
RDMA, GPUDirect, CXL, DPU/SmartNIC, artifact format, quantization, sparse execution, firmware
generation, container runtime, operating system, security isolation, offload) or a validated
vendor extension identifier, and it belongs to a category. Values are typed (boolean, integer,
unsigned, number, text, bounded text set) with a precision and evidence class. Requirements are
compared with `PRESENT`, `ABSENT`, `EQUALS`, `NOT_EQUALS`, `AT_LEAST`, `AT_MOST`,
`ANY_OF` or `ALL_OF`, and every comparison returns a tri-state: **absent evidence is never
treated as evidence of absence**.

Capability precedence when several sources publish the same key: the accelerator-class catalogue
describes the device model, the cluster record describes what the fleet actually publishes now,
and the runtime record is authoritative for runtime and driver keys. A cluster can therefore
withdraw a capability the catalogue advertises — but only explicitly, by publishing it false.

## Compatibility model

Compatibility is explicit and explainable. Every relationship is evaluated in a fixed order and
yields a named outcome: workload↔accelerator, artifact↔architecture, artifact↔format support,
artifact↔runtime, artifact↔driver, runtime↔driver, runtime↔OS, runtime ABIs, compiler ABIs,
kernel↔architecture, model format↔serving backend, collective plan↔topology,
partition↔device capability, isolation↔cluster capability, memory↔device capacity, and
capability requirements. **There is no opaque score anywhere.**

Outcomes: `COMPATIBLE`, `COMPATIBLE_WITH_FALLBACK`, `COMPATIBLE_AFTER_RECONFIGURATION`,
`COMPATIBLE_AFTER_REBUILD`, `CAPABILITY_MISSING`, `INCOMPATIBLE_MEMORY`,
`INCOMPATIBLE_TOPOLOGY`, `INCOMPATIBLE_ISOLATION`, `INCOMPATIBLE_ARTIFACT`,
`INCOMPATIBLE_ABI`, `INCOMPATIBLE_DRIVER`, `INCOMPATIBLE_RUNTIME`,
`INCOMPATIBLE_ARCHITECTURE`, `STALE_EVIDENCE`, `UNKNOWN_COMPATIBILITY`, `UNSUPPORTED`.
Policy admissibility is tracked **outside** the verdict: a target can be perfectly compatible and
still inadmissible, and the two have different owners and different remedies.

## Placement observation and attribution

A placement observation records the workload generation, the candidate set as the scheduler
exposed it, the selected cluster with its generation and epoch, per-candidate structured
rejection reasons, policy/capacity/topology/compatibility generations, fallback, cost/SLO
evidence when supplied, placement generation, sequence, publisher, provenance and precision.

**Candidate-set completeness is stated before anything else.** When the scheduler exposed only
the selected cluster, the runtime reports *"only the selected cluster was exposed; rejection
attribution for alternatives is unavailable"* and never infers the rest.

Rejection reasons keep their identity: `INSUFFICIENT_ACCELERATORS`, `INSUFFICIENT_MEMORY`,
`CAPABILITY_MISSING`, `ARTIFACT_INCOMPATIBLE`, `RUNTIME_INCOMPATIBLE`,
`DRIVER_INCOMPATIBLE`, `TOPOLOGY_INCOMPATIBLE`, `ISOLATION_INCOMPATIBLE`,
`ABI_INCOMPATIBLE`, `POLICY_REJECTED`, `CLUSTER_DRAINING`, `CLUSTER_NOT_READY`,
`STALE_CLUSTER_EVIDENCE`, `CAPACITY_RESERVED`, `PORTABILITY_NOT_PROVEN`,
`SITE_RESTRICTION`, `LOCALITY_CONSTRAINT`, `FAILURE_DOMAIN_CONSTRAINT`, `UNKNOWN`.
They are never collapsed into "unavailable".

Every statement in an explanation carries its own basis (`OBSERVED`, `DERIVED`, `INFERRED`,
`UNATTRIBUTED`), precision and evidence class.

## Capacity model

Capacity accounting is exact and checked:

```
nominal  = offline + allocated + reserved + draining + unusable + idle
online   = nominal - offline = allocated + reserved + draining + unusable + idle
```

The buckets are mutually exclusive and sum exactly to nominal. Every sum is checked arithmetic; an
inconsistent or overflowing ledger is refused, never wrapped. `idle` is the only pool that
eligibility analysis may draw from — **idle is not stranded.**

Eligibility is a *partition of idle* for one workload class:

```
idle = usable + unknown + sum(stranded by primary reason)
```

Each capacity unit is assigned exactly one **primary** reason so that reason amounts sum without
double counting; a separate, explicitly **overlapping** occurrence tally records every reason a
unit exhibits. Fragmentation is deliberately absent from this structure because it describes the
*distribution* of usable capacity, not an amount of it.

## Stranded capacity

Capacity is **stranded** for a workload class when it is physically present, online, healthy and
uncommitted, but cannot legally be consumed because of a named constraint: unsupported
accelerator capability, unsupported precision, missing offload capability, missing interconnect
feature, runtime mismatch, driver mismatch, ABI mismatch, artifact incompatibility, missing
compiler target, partition geometry, topology constraint, isolation requirement, missing local
storage/state, insufficient memory, site restriction or policy restriction. Each finding reports
the amount, the percentage of nominal and of idle (exact, in basis points — no floating point),
the cluster, the resource class, the workload class, the reasons, the evidence, the precision and
the generation bindings. **Merely idle capacity is never labelled stranded.**

## Federation fragmentation

Fragmentation is reported as its own finding, never as a capacity shortage:
`CAPACITY_SHORTAGE`, `PHYSICAL_FRAGMENTATION`, `COMPATIBILITY_FRAGMENTATION`,
`POLICY_FRAGMENTATION`, `TOPOLOGY_FRAGMENTATION`, `SITE_FRAGMENTATION`, `MIXED`, `NONE`,
`UNKNOWN`. The finding states the required group size, aggregate nominal and usable capacity, the
largest legal group in any single placement domain, the number of legal domains, the per-domain
breakdown, and the stranding reasons behind it.

## Migration model

Observed stages: `PLANNED`, `SOURCE_QUIESCING`, `STATE_CAPTURED`, `TRANSFER_STARTED`,
`TRANSFER_COMPLETE`, `DESTINATION_PREPARED`, `RESTORE_STARTED`, `RESTORE_COMPLETE`,
`REVALIDATION_REQUIRED`, `COMMITTED`, `ROLLED_BACK`, `FAILED`, `OUTCOME_UNKNOWN`.
Illegal transitions are refused; a committed migration is recorded once; a superseded migration
stops accepting stage events; a late event from an older migration generation is refused.

A migration record binds the workload generation, source and destination cluster generations and
epochs, artifact generation, runtime generations, compatibility, policy, capacity and topology
generations, a migration generation, and the federation generation. Attribution explains the
reason **if the source stated one** — the runtime never infers causality — plus capability deltas,
state-transfer size and downtime when measured, rebuild/recompile/conversion/state-translation
requirements, fallback behaviour, revalidation, and the final outcome. A committed migration
proves the workload reached the destination; it does **not** prove byte-for-byte state
portability, and the analysis says so.

## Portability model

Portability is evaluated per dimension: `BINARY`, `ARTIFACT`, `MODEL`, `CHECKPOINT`,
`RUNTIME_API`, `KERNEL`, `DATA_FORMAT`, `STATE`, `NUMERICAL_SEMANTIC`, `PERFORMANCE`,
`OPERATIONAL`. Outcomes: `PORTABLE_DIRECT`, `PORTABLE_WITH_FALLBACK`,
`PORTABLE_WITH_REBUILD`, `PORTABLE_WITH_RECOMPILE`, `PORTABLE_WITH_CONVERSION`,
`PORTABLE_WITH_STATE_TRANSLATION`, `NOT_PORTABLE_TOPOLOGY`, `NOT_PORTABLE_POLICY`,
`NOT_PORTABLE_ARTIFACT`, `NOT_PORTABLE_RUNTIME`, `NOT_PORTABLE_STATE`,
`NOT_PORTABLE_ARCHITECTURE`, `UNKNOWN`, `UNSUPPORTED`.

A dimension that nothing in the workload or artifact constrains is reported but marked as not
constraining the verdict; performance portability is never measured here and therefore never
constrains a verdict. Technical incompatibility and policy restriction are distinguished, and a
policy block is carried on the assessment as its own flag rather than folded into an outcome.

## Drift

Intended-state drift (an externally supplied description of what the federation is *supposed* to
be) reports missing and unexpected clusters and sites, capability mismatch, runtime-version
divergence, stale clusters, policy-generation mismatch, artifact-compatibility drift and
capacity-report divergence — each with before/after evidence. **With no intended state supplied,
intended-state drift is reported as NOT evaluated, which is not a clean bill of health.**
Behavioural drift between two observation windows reports changed placement targets, newly
introduced and cleared rejection reasons, introduced fallbacks and changed compatibility
constraints. A change is never labelled a regression.

## Precision and provenance

Precision classes: `EXACT`, `AGGREGATED`, `SAMPLED`, `DERIVED`, `INFERRED`,
`AMBIGUOUS`, `UNKNOWN`. Combination is pessimistic: combining evidence never makes a finding
more precise than its weakest input.

Provenance sources: host probe, toolchain probe, artifact inspection, federation controller,
cluster controller, scheduler, runtime registry, hardware capability registry, artifact registry,
migration runtime, resource broker, external inventory, synthetic backend, imported trace, derived
analysis, publisher report. Every result is traceable to the evidence that supports it.

## Publisher authority

Distributed publishers bind evidence to a publisher identity, a publisher **boot identity**, the
coordinator epoch, the federation and cluster generations, the capability and evidence
generations, and a monotonic sequence number.

* A dead publisher permanently loses authority for that boot identity; a replacement process must
  register under a fresh boot identity.
* A frame stamped with an older coordinator epoch is refused.
* A sequence number older than the accepted watermark is refused as a regression.
* An accepted sequence number re-used for different content is refused as a conflicting duplicate.
* An identical re-publication is suppressed as a duplicate and does not change state.
* Gaps in a publisher's sequence stream are counted and surfaced, never hidden.

## Ordering, snapshots and immutability

Transport arrival order is never trusted. Snapshots are immutable: readers hold a
`shared_ptr<const FederationSnapshot>` and publications produce a new snapshot generation, so a
reader never observes torn state. A snapshot states its freshness census — clusters current,
stale, awaiting revalidation or retired; publishers live or fenced — and whether it was truncated
by a bound.

## Persistence and recovery

The durable format is versioned, magic-prefixed, length-bounded, checksummed and written by atomic
replacement. It stores only what is meaningful across a restart: stable identities, structural
federation membership, workload/artifact mappings, historical placements and migrations,
capability and compatibility schemas, portability classifications, publisher sequence watermarks,
replay-prevention state and historical aggregate findings.

Deliberately excluded: cluster readiness, cluster capacity, publisher liveness and every other
dynamic fact. A corrupted state file is rejected **entirely** — there is no partial apply.

On restart the coordinator epoch advances, durable structure and history are restored, replay
watermarks are preserved, every dynamic record becomes `REVALIDATION_REQUIRED`, cluster capacity
and readiness are cleared, and old-epoch traffic is refused. **Current federation state is never
claimed from persistence alone.**

## Distributed architecture

The coordinator, publishers and inspector are real operating-system processes communicating over
framed TCP. `fo-coordinator` owns the authoritative state; `fo-publisher` publishes; `fo-cli`
inspects. The frame format is bounded and versioned (magic, protocol revision, message type,
flags, reserved bytes, request id, payload length, payload checksum); bad magic, wrong version,
unknown type, oversized payload, truncation, checksum failure, non-zero reserved bytes, stale
epoch, stale boot, stale generation, sequence regression, invalid references and impossible
capacity accounting are all rejected before any state mutation.

## REAL / SYNTHETIC / UNSUPPORTED

Every record and finding carries an evidence class.

**REAL** — produced by this machine or by a real process: local CPU, NUMA and memory facts, the
RTX 5090 and its driver facts via `nvidia-smi`, the installed CUDA toolchain, real compiled device
images inspected with `cuobjdump`, and the real publisher-death and coordinator-restart proofs.

**SYNTHETIC** — produced by the deterministic synthetic federation backend: multi-cluster and
multi-site federations, mixed accelerator families (NVIDIA H100, AMD MI300X, NVIDIA L40S),
mixed runtime generations, cross-site migrations, portability failure matrices, capability
changes, capacity changes, cluster joins and leaves, stale evidence and large-federation
fragmentation. Synthetic evidence travels through **exactly the same** validation, generation
fencing and analysis pipeline as any other publication.

**UNSUPPORTED** — what this host cannot demonstrate, stated by the runtime itself: no physical
multi-cluster federation is observable on a single machine; no second accelerator family is
present, so heterogeneous *physical* accelerator federation is not demonstrable; cross-site
execution and physical cross-cluster migration cannot be executed or observed; cluster liveness,
readiness and per-device allocation state are not observable from here.

**No synthetic multi-cluster scenario is ever presented as physical federation proof.**

## Hardware validation

`fo::probe_host_inventory()` records only what it actually observed. On the development host it
reports: Windows on an AMD Ryzen 7 9800X3D (8 physical / 16 logical cores, 1 NUMA node), ~66 GB
RAM, the Realtek PCIe 5GbE adapter, an **NVIDIA GeForce RTX 5090** (34190917632 bytes, compute
capability 12.0 → architecture `sm_120`, driver 616.92, PCIe gen 5 ×16) and CUDA toolkit 12.9
with the architectures it supports. Anything not observed stays empty and renders as UNKNOWN.

### Controlled real artifact compatibility proof

The test suite compiles a real CUDA device image for the device's own architecture (`sm_120`) and
a second one for a deliberately different architecture (`sm_75`), inspects both with
`cuobjdump`, and feeds the **real** metadata through the production analysis pipeline:

```
REAL artifact proof: device=sm_120 (12.0); matching artifact targets sm_120;
incompatible artifact targets sm_75; verdicts COMPATIBLE / INCOMPATIBLE_ARCHITECTURE
```

That proof requires a host C++ compiler on `PATH` (nvcc needs one). Without it the test reports
that the proof cannot run here rather than substituting a synthetic claim.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Requirements: CMake ≥ 3.20, a C++20 compiler (MSVC 19.3x, GCC 11+, or Clang 14+), and threads.
No third-party dependencies. The library builds with `/W4 /WX` on MSVC and
`-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Werror` elsewhere, with **zero first-party
warnings**.

Options: `FO_BUILD_TESTS`, `FO_BUILD_EXAMPLES`, `FO_BUILD_TOOLS`, `FO_BUILD_BENCHMARKS`,
`FO_SANITIZE`, `FO_WARNINGS_AS_ERRORS`, `FO_LIBRARY_TYPE` (STATIC/SHARED).

## Tests

```
ctest --test-dir build --output-on-failure      # or run the suite directly:
build/tests/fo-tests                            # --verbose, --list, --filter=<substring>
```

No test timeout is configured anywhere: a test that hangs is a defect to diagnose, not something
to bound away. The suite contains 155 tests in 18 files: foundation and codec, capability,
compatibility, placement and rejection attribution, stranded capacity, fragmentation, migration,
portability, drift, observatory semantics, protocol adversarial, persistence adversarial, seeded
property tests, deterministic concurrency, the synthetic backend, real hardware validation and the
multiprocess proofs.

```
155 passed, 0 failed, 155 total
```

Release, Debug and AddressSanitizer configurations all pass the whole suite. The sanitizer
configuration is the strongest available here: MSVC's ASan runtime is shipped with the Visual
Studio **Build Tools** installation, so the sanitizer build uses that toolchain; the regular
builds use whichever MSVC is on `PATH`.

## Examples

```
build/examples/ex_basic_snapshot
build/examples/ex_placement_explanation
build/examples/ex_capability_mismatch
build/examples/ex_stranded_capacity
build/examples/ex_federation_fragmentation
build/examples/ex_migration_analysis
build/examples/ex_portability_failure
build/examples/ex_publisher_fencing
build/examples/ex_installed_consumer
```

Every example exits 0 and prints byte-identical output across runs. Each one exercises the public
API; `ex_installed_consumer` deliberately avoids the synthetic backend entirely.

## CLI

```
build/tools/fo-cli [--help]
build/tools/fo-cli bounds | hardware | snapshot | clusters | sites | accelerator-classes |
                   runtimes | capabilities | capacity | placements | migrations | publishers |
                   mismatch | drift | provenance | scenario
build/tools/fo-cli stranded <workload-class>
build/tools/fo-cli usable <workload-class>
build/tools/fo-cli fragmentation <workload-class>
build/tools/fo-cli placement <placement-id>
build/tools/fo-cli rejection <placement-id> <cluster-id>
build/tools/fo-cli migration <migration-id>
build/tools/fo-cli portability <workload-id> <cluster-id> [--evaluate]
build/tools/fo-cli compatibility <cluster-id> <workload-id>
build/tools/fo-cli --host <h> --port <p> <command>     # remote mode against a coordinator
```

The CLI is read-only: administrative mutation is not part of it. Every command prints a
REAL or SYNTHETIC banner naming its evidence source.

## CMake installation

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build --parallel
cmake --build build --target install
```

This installs the headers, the library, the CMake package configuration and version file, the
exported target `SummonSoftwareLabs::FederationObservatory`, the three tools, and the LICENSE.

## Downstream usage

```cmake
find_package(FederationObservatory CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE SummonSoftwareLabs::FederationObservatory)
```

A complete, self-contained consumer lives in `examples/installed-consumer`. It configures against
an installed prefix only, never referencing this source tree:

```
cmake -S examples/installed-consumer -B consumer-build -DCMAKE_PREFIX_PATH=/your/prefix
cmake --build consumer-build
consumer-build/fo-consumer
```

```
Federation Observatory consumer built against 1.0.0
Federation Observatory 1.0.0 [protocol 1, persistence 1, c++20]
snapshot: 1 cluster(s), 1 placement(s), digest d00bdc1423fcc519
explanation digest 1b98cc09698d381d (15 findings, rejection attribution unavailable)
capacity: nominal 8 idle 5 usable 5 stranded 0 unknown 0 (closes: yes)
persistence: saved and reloaded 1 cluster(s); epoch advanced from 1 to 2
consumer: ok
```

## Benchmarks

```
build/benchmarks/fo-benchmarks [--scale 1|10|100|1000|10000] [--only <substring>]
```

Measured on the development host (Release, MSVC 19.44, Ryzen 7 9800X3D), microseconds per
completed operation:

| operation | 1 cluster | 10 | 100 | 1,000 | 10,000 |
|---|---|---|---|---|---|
| cluster publication | 2.1 | 1.4 | 2.4 | 2.3 | 1.6 |
| capability update | 0.93 | 0.95 | 1.04 | 0.97 | 1.49 |
| capacity update | 0.45 | 0.79 | 0.48 | 0.49 | 0.63 |
| placement ingestion | 1.44 | 2.91 | 1.70 | 1.55 | 2.00 |
| snapshot creation | 2.4 | 6.8 | 52.4 | 596 | 14,289 |
| placement explanation | 29.1 | 36.4 | 251 | 2,388 | 52,770 |
| stranded capacity | 5.3 | 33.6 | 757 | 3,951 | 63,971 |
| fragmentation | 6.4 | 36.9 | 564 | 6,753 | 185,404 |
| capability mismatch | 11.0 | 77.4 | 1,078 | 11,672 | 228,833 |
| compatibility lookup | 8.9 | 12.0 | 63.9 | 667 | 20,472 |
| migration update | – | 2.7 | 1.6 | 1.5 | 1.4 |
| portability analysis | 5.7 | 11.0 | 60.9 | 550 | 20,431 |
| persistence save+load | 8,415 | 16,246 | 8,185 | 14,333 | 792,846 |

Ingest paths are flat with federation size (implied exponent ≈ 0). The analysis paths are
linear (implied exponent 1.14–1.40) because each one takes a snapshot, which copies the
federation; a snapshot is a value, and that copy is the price of readers never seeing torn state.
An earlier revision resolved cluster references with a linear scan per finding, which made
fragmentation quadratic (implied exponent 1.63 at 10,000 clusters, 286 ms per call); snapshot
collections are now identifier-ordered and looked up by binary search, which removed it. Persistence
is dominated by serialization, encoding, checksumming and atomic replacement of the whole state.

## Genuine limitations

* **This is not a physical multi-cluster federation.** Everything multi-cluster, multi-site,
  cross-site-migration, heterogeneous-accelerator and heterogeneous-runtime in this repository is
  **SYNTHETIC**. The only REAL cluster is the single host this runtime runs on.
* The runtime does not choose placements, migrate workloads, own policy or enforce anything; it
  observes and explains. Every placement, migration and policy statement in its output is
  attributed to the system that made the decision.
* Cluster liveness, readiness and per-device allocation state are not observable from a single
  host, so the real cluster record reports device count as idle capacity and says so in its
  evidence.
* Real compatibility evidence exists only where a real artifact and a real device are both present
  on this machine. Alternative accelerator families (ROCm, oneAPI, Metal) are synthetic here.
* Performance portability is never asserted, because this runtime does not measure workload
  performance.
* A federated deployment across genuinely separate machines, with real per-cluster publishers and
  real cross-site migrations, has not been exercised; the multiprocess proofs exercise real
  processes over real TCP on one host.
* Snapshot creation copies the federation; at 10,000 clusters a snapshot costs about 14 ms. Very
  large federations would need an immutable persistent data structure rather than a copy.
* The protocol is revision 1 and carries no authentication or encryption; it is designed for a
  trusted observation network. Fencing can additionally require an administrative token.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

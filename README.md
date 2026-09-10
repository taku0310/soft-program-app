# soft-program-app

A container-native soft PLC with an **IEC 61131-3** core and a pluggable
protocol adapter layer. The first adapter ports **OpENer** (EIPStackGroup) to
serve EtherNet/IP from its own process, so a fault in the fieldbus stack cannot
reach PLC memory.

Both EtherNet/IP roles are implemented, as two independent adapters that can
run together:

| role | stack | what it does | process |
|---|---|---|---|
| **Adapter** (CIP target) | OpENer | presents the PLC as a device an external scanner connects to | `softplc-eip-adapter` |
| **Scanner** (originator) | EIPScanner | drives remote I/O drops and VFDs — the PLC opens the connections | `softplc-eip-scanner` |

They are separate stacks because no single one does both: OpENer has no
originator side at all. See [ADR 0007](docs/adr/0007-opener-as-eip-stack.md)
and [ADR 0008](docs/adr/0008-scanner-aggregates-devices.md).

```
┌── PLC core process ────┐          ┌── stack process ──────────┐
│  IEC 61131-3 runtime   │ 2× SPSC  │  OpENer / CIP             │
│  process image %I %Q %M│◄────────►│  assemblies               │◄─── scanner
│  ProtocolAdapter       │  shm     │  (this one may crash)     │
└────────────────────────┘          └───────────────────────────┘
```

Two processes, always — that split is the crash-containment boundary, and it
does not change with how they are packaged. Whether they are one container or
two is a deployment choice.

Read [`docs/architecture.md`](docs/architecture.md) for the design, and
[`docs/adr/`](docs/adr/) for why each decision went the way it did.

## Build

```sh
git clone --recurse-submodules <this repo>
cd soft-program-app

cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

That builds **without** either protocol stack — both EtherNet/IP adapters fall
back to a mirror backend behind the same interface, so the whole tree compiles
and the full suite passes on a machine with no submodules and no NIC. To link
the real stacks:

```sh
git submodule update --init --recursive
cmake -S . -B build -DSOFTPLC_WITH_OPENER=ON -DSOFTPLC_WITH_EIPSCANNER=ON
cmake --build build -j
```

| option | default | effect |
|---|---|---|
| `SOFTPLC_WITH_EIP` | `ON` | build the EtherNet/IP **Adapter** (target) |
| `SOFTPLC_WITH_OPENER` | `OFF` | link the real OpENer stack into it |
| `SOFTPLC_WITH_EIP_SCANNER` | `ON` | build the EtherNet/IP **Scanner** (originator) |
| `SOFTPLC_WITH_EIPSCANNER` | `OFF` | link the real EIPScanner stack into it |
| `SOFTPLC_BUILD_TESTS` | `ON` | build the CTest suite |
| `SOFTPLC_WERROR` | `OFF` | `-Werror` |

## Run

```sh
./build/softplc --list-adapters

# scan against the in-process loopback adapter
SOFTPLC_ADAPTERS=loopback SOFTPLC_MAX_SCANS=1000 ./build/softplc

# two processes over shared memory
SOFTPLC_INSTANCE=line1 SOFTPLC_ROLE=adapter ./build/softplc &
./build/softplc-eip-adapter line1 eth0

# or let the single-container entrypoint start both, from a config file
SOFTPLC_BIN_DIR=$PWD/build SOFTPLC_CONFIG=examples/config/softplc.conf \
  bash docker/entrypoint.sh

./build/softplc --list-roles
SOFTPLC_CONFIG=examples/config/softplc.conf ./build/softplc --show-config
```

Containers: one image with the role switched by configuration when the PLC is
either an Adapter or a Scanner, three when it is both.
[`docker/README.md`](docker/README.md) has the reasoning and how the processes
share `/dev/shm`.

```sh
# one container: core + the one stack `role` calls for
docker compose -f docker/docker-compose.single.yml up --build

# three containers: both EtherNet/IP roles at once, which needs two
# network namespaces because CIP class 1 uses UDP 2222 at both ends
docker compose -f docker/docker-compose.yml up --build
```

## Configuration

Two ways in, and they compose:

    environment variable   >   config file   >   compiled-in default

Everything below is an environment variable **and** a key in an optional INI
file — `/etc/softplc/softplc.conf`, or wherever `SOFTPLC_CONFIG` points. A key
in section `S` is the variable `SOFTPLC_S_KEY`, and `[core]` adds no prefix, so
`[eip] interface` *is* `SOFTPLC_EIP_INTERFACE`; a key already spelled
`SOFTPLC_...` is taken verbatim in any section. There is no second vocabulary
to learn, and pasting a compose `environment:` block into the file works.

```ini
[core]
role     = adapter        # none | adapter | scanner
instance = line1
cycle_us = 10000

[eip]
interface   = eth0
input_bytes = 32
```

The environment wins, so a deployment that has always used `docker run -e` is
unaffected by the file existing. `softplc --show-config` prints what resolved
to what and flags anything the environment is overriding.
[`examples/config/softplc.conf`](examples/config/softplc.conf) is a commented
example.

### PLC core

| variable | default | meaning |
|---|---|---|
| `SOFTPLC_ROLE` | `none` | `none`, `adapter` or `scanner` — picks the adapter the core binds and the stack process that runs beside it. `softplc --list-roles` |
| `SOFTPLC_INSTANCE` | `default` | namespaces the IPC objects; core and stack process must match |
| `SOFTPLC_ADAPTERS` | from the role | comma-separated protocol names, mapped into `%I`/`%Q` in order. Overrides the role's list; needed only to bind two protocols at once |
| `SOFTPLC_CYCLE_US` | `10000` | task period, µs |
| `SOFTPLC_FAILSAFE` | `hold` | `hold` or `clear` — see [ADR 0005](docs/adr/0005-failsafe-policy.md) |
| `SOFTPLC_MAX_SCANS` | `0` | stop after N scans; `0` runs until signalled |
| `SOFTPLC_LOG_LEVEL` | `info` | `error`, `warn`, `info`, `debug` |
| `SOFTPLC_CONFIG` | `/etc/softplc/softplc.conf` | the config file. Named explicitly, it must exist; left unset, the default path is read if present |

`role` is one value rather than a set because CIP class 1 I/O uses UDP 2222 at
**both** ends: an Adapter and a Scanner in one network namespace receive each
other's traffic. Both roles at once needs two namespaces — see
[ADR 0010](docs/adr/0010-single-container-role-switch.md).

### Single-container supervision

Read by [`docker/entrypoint.sh`](docker/entrypoint.sh), which starts the core
and the role's stack process in one container.

| variable | default | meaning |
|---|---|---|
| `SOFTPLC_STACK_RESTART` | `on` | restart the stack process if it dies. The core keeps scanning on failsafe values either way |
| `SOFTPLC_STACK_RESTART_DELAY_MS` | `1000` | delay before that restart |

### EtherNet/IP Adapter (target role)

| variable | default | meaning |
|---|---|---|
| `SOFTPLC_EIP_INTERFACE` | `eth0` | NIC the stack binds to (adapter process) |
| `SOFTPLC_EIP_INPUT_BYTES` | `32` | consumed assembly size, field → PLC |
| `SOFTPLC_EIP_OUTPUT_BYTES` | `32` | produced assembly size, PLC → field |
| `SOFTPLC_EIP_PRODUCED_ASSEMBLY` | `100` | T→O instance |
| `SOFTPLC_EIP_CONSUMED_ASSEMBLY` | `150` | O→T instance |
| `SOFTPLC_EIP_CONFIG_ASSEMBLY` | `151` | configuration instance |
| `SOFTPLC_EIP_EXCHANGE_TIMEOUT_US` | `5000` | per-exchange budget; also the liveness check |
| `SOFTPLC_EIP_FAILSAFE_TIMEOUT_US` | `100000` | how long without a fresh image before failsafe — measured, see [ADR 0009](docs/adr/0009-timeout-threshold-from-measurement.md) |

### EtherNet/IP Scanner (originator role)

The per-device settings live in a device table rather than environment
variables — see [`examples/config/scanner-devices.conf`](examples/config/scanner-devices.conf).
Both the core and the scanner process read it, and the scanner refuses to start
if the two disagree.

| variable | default | meaning |
|---|---|---|
| `SOFTPLC_SCANNER_DEVICES` | `/etc/softplc/scanner-devices.conf` | path to the device table |
| `SOFTPLC_SCANNER_EXCHANGE_TIMEOUT_US` | `5000` | per-exchange budget for the whole aggregate |
| `SOFTPLC_SCANNER_FAILSAFE_TIMEOUT_US` | `100000` | staleness before the adapter-level failsafe |

The scanner's input image is a **health block followed by the device data**:
one byte per device (`0` offline, `1` online, `2` connection lost and
failsafed), then each device's T→O slice in table order. Per-device failsafe is
applied inside the scanner under that device's own policy, so one drive
dropping does not disturb the others — a POU reads the health byte and decides
what that means for the plant.

## Writing a program

POUs are C functions against the runtime API.
[`examples/demo_program.c`](examples/demo_program.c) is a worked conveyor
interlock: `TON` start delay, `RS` latch, `R_TRIG` part edge, `TP` lamp pulse,
`CTU` count. It touches only the process image, so it does not know or care
whether `%IX0.2` arrives over EtherNet/IP, over Modbus, or from a unit test.

## Adding a protocol

1. Implement `plc_adapter_vtbl_t` from
   [`include/softplc/protocol_adapter.h`](include/softplc/protocol_adapter.h).
2. Export a `plc_adapter_factory_t`.
3. Add an `extern` to `src/adapters/builtins.c` and a source line to CMake.
4. Point `tests/test_adapter_contract.c` at it.

Nothing in `src/core/` changes — it has no link-time dependency on any
protocol, which is what keeps the claim honest. For an out-of-process protocol,
copy the proxy/service-loop split in `src/adapters/eip/`; the crash-containment
behaviour comes with it.

## Status

Working:

* IEC 61131-3 runtime — process image, cyclic scan with deadline pacing and
  overrun/jitter accounting, standard function blocks.
* Protocol adapter interface, registry, and three implementations.
* EtherNet/IP adapter: real OpENer integration (`CipStackInit`, assembly
  create/read/write, connection events, run/idle), shared-memory transport,
  sequence-matched exchange, timeout escalation, both failsafe policies.
* EtherNet/IP Scanner: real EIPScanner integration (ForwardOpen per device,
  Class 1 cyclic I/O, reconnect), four devices aggregated into one adapter,
  per-device failsafe and a per-device health block in the input image.
* Crash containment verified against a real `SIGKILL` of a real adapter
  process, for both `HOLD` and `CLEAR`; per-device loss verified separately.
* Containers both ways: one all-in-one image whose role is a runtime setting,
  and three images sharing one `/dev/shm` namespace for the deployment that
  needs both EtherNet/IP roles at once
  ([ADR 0010](docs/adr/0010-single-container-role-switch.md)). A stack crash is
  contained identically in either form.
* Settings from an INI file underneath the environment, with the role resolved
  by the core binary so the entrypoint and the core cannot disagree about what
  the container is.
* Timeout budget and failsafe trigger set from 100 000 measured exchanges over
  the real two-process path ([ADR 0009](docs/adr/0009-timeout-threshold-from-measurement.md)).
  The measurement also changed the trigger's *shape*: a count of consecutive
  misses turned out to be unfixable — two thresholds picked from an observed
  maximum were both beaten by the next sample — so it is a duration now.
  `tools/bench_exchange.c` is kept so it can be re-measured on target hardware.

Not done, and deliberately so:

* **The failsafe timeout is measured, but on a shared cloud host, not on target
  hardware.** [ADR 0009](docs/adr/0009-timeout-threshold-from-measurement.md)
  has the numbers and the method; re-run `tools/bench_exchange.c` — several
  times, one sample is not enough — on the real machine before trusting it
  there. That host produced ~50 ms of data loss from scheduling alone, which is
  more than CIP's own minimum connection timeout at a 10 ms RPI would tolerate:
  a general-purpose cloud VM is not a platform for hard 10 ms control.
* **`point_overrides` is type-only** — the ABI slot is reserved, no adapter
  implements it ([ADR 0006](docs/adr/0006-point-overrides-reserved.md)).
* **Modbus/TCP and OPC UA adapters are not written.** The EtherNet/IP pair is
  their template.
* **No ST / IL / LD front end.** The runtime model is 61131-3; the language
  front end is a separate phase.
* **No restart supervision and no reconnect state reconciliation**, by decision
  ([ADR 0004](docs/adr/0004-no-restart-no-reconnect-state.md)).

## Licence

This project is Apache-2.0. Vendored OpENer at `third_party/OpENer` is
distributed under its own licence — see
[`third_party/OpENer/license.txt`](third_party/OpENer/license.txt).

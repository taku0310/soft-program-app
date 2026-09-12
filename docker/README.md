# Container deployment

Two ways to deploy, and the choice is one question: **does this PLC use both
EtherNet/IP roles at once?**

| | image(s) | when |
|---|---|---|
| [all-in-one](#all-in-one-one-container-one-role) | `Dockerfile.softplc` | the PLC is *either* an Adapter *or* a Scanner — the usual case |
| [split](#split-three-containers) | `Dockerfile.plc-core` + `Dockerfile.eip-adapter` + `Dockerfile.eip-scanner` | both roles at once |

Either way the protocol stack runs as **its own process** against shared memory
the core owns, which is the whole point of the out-of-process adapter: a stack
fault costs a failsafe transition, not the PLC. One container is a packaging
decision, not a memory-sharing one — see
[ADR 0010](../docs/adr/0010-single-container-role-switch.md).

## Why "both roles" needs two containers

**The two EtherNet/IP roles must not share a network namespace.** CIP class 1
uses a fixed UDP port (2222) at both ends, so an Adapter and a Scanner on one
host fight over it and each receives its own transmissions. This is not a
deduction from the specification: it showed up during interop verification as
`Received data from unknown connection T2O_ID=<our own O2T id>` and was fixed
only by separating the namespaces.

A container is one network namespace. So separate containers get separate
namespaces by default, which is why the split compose file works — do not
collapse them with `network_mode: service:` — and why an all-in-one container
runs exactly one role.

## All-in-one: one container, one role

`Dockerfile.softplc` carries the core and both stacks. `role` picks which stack
runs beside the core; the entrypoint reads it back out of the core binary
(`softplc --role`), so the two halves cannot be configured to disagree.

```sh
git submodule update --init --recursive   # both stacks are submodules
docker compose -f docker/docker-compose.single.yml up --build

# switch role without touching the file - the environment wins
SOFTPLC_ROLE=scanner docker compose -f docker/docker-compose.single.yml up
```

| role | stack process started | listens on |
|---|---|---|
| `none` | none; the core runs alone on the loopback adapter | nothing |
| `adapter` | `softplc-eip-adapter` (OpENer) | TCP/UDP 44818, UDP 2222 |
| `scanner` | `softplc-eip-scanner` (EIPScanner) | nothing; outbound only |

`softplc --list-roles` prints what a given build supports; a build without the
Scanner has no `scanner` role, and asking for one fails at start-up rather than
coming up with nothing connected.

### What the entrypoint supervises

Inside one container `docker/entrypoint.sh` is the supervisor
[ADR 0004](../docs/adr/0004-no-restart-no-reconnect-state.md) defers to, and it
is the only thing in this repository that restarts anything.

* **Stack dies.** Restarted in place; the core never notices beyond its inputs
  going stale. Verified by `SIGKILL`ing the stack mid-run — the core kept
  scanning, applied HOLD after 109 ms, and returned to `ONLINE` when the
  restarted stack reattached. `stack_restart = off` leaves the core running
  without a stack instead.
* **Core dies.** The container goes down with the core's exit status. This is
  deliberate: the core owns the shared memory and the doorbells, so its exit
  invalidates the stack's mappings and nothing inside the container can put
  that back. `restart: unless-stopped` restarts both together.

### Privileges

`cap_drop: ALL`, no `cap_add`, non-root. One container means one uid, so the
`CAP_DAC_OVERRIDE` the split deployment needs — to let a root stack container
read shared memory the core created `0600` as uid 10001 — does not arise. The
OpENer adapter itself needs nothing privileged either: it reads the interface
MAC by `ioctl` and binds 44818 and 2222, all above 1024, and was confirmed to
start and serve as an unprivileged uid with no capabilities at all.

## Split: three containers

Use this when the PLC is an Adapter **and** a Scanner. Three images, one per
failure domain:

| image | contains | may crash |
|---|---|---|
| `Dockerfile.plc-core` | IEC 61131-3 runtime, adapter proxies | no |
| `Dockerfile.eip-adapter` | OpENer, CIP target sockets (Adapter role) | yes, by design |
| `Dockerfile.eip-scanner` | EIPScanner, CIP originator (Scanner role) | yes, by design |

```sh
git submodule update --init --recursive
docker compose -f docker/docker-compose.yml up --build
```

`plc-core` starts, creates the shared memory, and begins scanning immediately -
before the adapter is up, on failsafe inputs.  When `eip-adapter` attaches, the
proxy logs `online` and the images start moving.

## Sharing the IPC namespace

The two processes talk over POSIX shared memory (`shm_open`) and named
semaphores (`sem_open`).  On Linux both live in `/dev/shm`, so what the
containers must share is that **mount**, not Docker's `ipc:` namespace, which
covers System V IPC instead.  The compose file mounts one named tmpfs volume at
`/dev/shm` in both services; anything that achieves the same works, including:

* `--ipc=shareable` plus an explicit `/dev/shm` bind mount;
* a Kubernetes Pod with both containers and an `emptyDir: {medium: Memory}`
  volume mounted at `/dev/shm` in each;
* both processes on the host, no containers at all.

Object names are namespaced by `SOFTPLC_INSTANCE`, so several PLCs can share
one host or one pod:

```
/softplc.<instance>.eip        shared memory
/softplc.<instance>.eip.req    core -> adapter doorbell
/softplc.<instance>.eip.rsp    adapter -> core doorbell
```

Both containers must be given the **same** `SOFTPLC_INSTANCE`, or each will
wait for a peer that is not there.

## Restart behaviour (split deployment)

Neither process restarts the other; that is the orchestrator's job, and the
code is written on that assumption. In the all-in-one image the entrypoint
fills that role for the stack process, as described above.

* **Adapter dies.** The core's `exchange()` starts timing out. Once the input
  image has been stale for `SOFTPLC_EIP_FAILSAFE_TIMEOUT_US` it applies the
  failsafe policy and keeps scanning; until then it holds. `restart:
  unless-stopped` brings the adapter back; it re-attaches and exchanges resume
  with no core intervention.
* **Core dies.** It unlinks the IPC objects on the way out. The adapter's next
  wait fails and it exits non-zero, and is restarted; it then waits for the new
  core to publish a region.

Note that the process image on the far side is *not* reconciled after a
reconnect - the adapter comes back with zeroed assemblies and the first fresh
exchange overwrites the failsafe image. Recovering plant state across a
reconnect is out of scope here and belongs to application logic.

## Configuration

Every setting is an environment variable *and* a key in an optional INI file
mounted at `/etc/softplc/softplc.conf` (or wherever `SOFTPLC_CONFIG` points).
**The environment wins**, so adding a file changes nothing about an existing
deployment — which is also why none of these images bakes a `SOFTPLC_*` default
into `ENV`: that would silently override the mounted file rather than default
it.

```ini
[core]
role     = adapter
instance = line1
cycle_us = 10000

[eip]
interface   = eth0
input_bytes = 32
```

A key in section `S` is the variable `SOFTPLC_S_KEY`, and `[core]` adds no
prefix — so `[eip] interface` *is* `SOFTPLC_EIP_INTERFACE`, and a key already
spelled `SOFTPLC_...` is taken verbatim in any section. There is no second
vocabulary.

`softplc --show-config` prints what a file resolved to and flags anything the
environment is overriding, which is the fast answer to "did my config file take
effect?".

See [`examples/config/softplc.conf`](../examples/config/softplc.conf) for a
commented example, and the table in the top-level
[README](../README.md#configuration) for every key.

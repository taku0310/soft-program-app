# Container deployment

Three images, because the whole point of the out-of-process adapter is that a
protocol stack and the PLC core fail independently:

| image | contains | may crash |
|---|---|---|
| `Dockerfile.plc-core` | IEC 61131-3 runtime, adapter proxies | no |
| `Dockerfile.eip-adapter` | OpENer, CIP target sockets (Adapter role) | yes, by design |
| `Dockerfile.eip-scanner` | EIPScanner, CIP originator (Scanner role) | yes, by design |

**The two EtherNet/IP roles must not share a network namespace.** CIP class 1
uses a fixed UDP port (2222) at both ends, so an Adapter and a Scanner on one
host fight over it and each receives its own transmissions. Separate containers
get separate namespaces by default, which is why the compose file below works —
do not collapse them with `network_mode: service:`.

## Quick start

```sh
git submodule update --init --recursive   # OpENer, needed by the adapter image
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

## Restart behaviour

Neither process restarts the other; that is the orchestrator's job, and the
code is written on that assumption.

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

See the table in the top-level [README](../README.md#configuration).

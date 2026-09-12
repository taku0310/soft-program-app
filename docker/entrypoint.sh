#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Runs the PLC core and, beside it, the one protocol stack the configured role
# calls for.
#
# This exists because of a constraint that is not ours to relax: CIP class 1
# I/O uses UDP 2222 at *both* ends, so an EtherNet/IP Adapter and a Scanner in
# one network namespace receive each other's traffic.  A container is one
# namespace.  So a single container holds the core and *one* role - which is
# all a deployment needs when it is either a device or an originator, never
# both - and running both roles still means two namespaces.
#
# What this does not give up is the crash-containment boundary.  The stack
# still runs as its own process against the core's shared memory (ADR 0002,
# 0003); collapsing them into one binary is what would throw that away, and
# this deliberately does not.  ADR 0004 left process restart to "the
# supervisor"; inside one container, this script is that supervisor, so it is
# also the only place in the project that restarts anything.
#
# Ordering is not coordinated and does not need to be: whichever comes up
# first waits, and the core scans on failsafe values until the stack attaches.

set -uo pipefail

# Where the three binaries live.  Overridable so this script can be exercised
# against a build tree, which is how it is tested outside a container.
BIN_DIR=${SOFTPLC_BIN_DIR:-/usr/local/bin}
CORE="$BIN_DIR/softplc"

log() { printf '%s entrypoint: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }

# The role is resolved by the binary that owns the role table rather than
# re-read here, so the core and this script cannot disagree about it, and an
# unknown role fails before anything is started.
role=$("$CORE" --role) || exit 1

# The one place a role has to be mapped to an executable.  A role the core
# knows and this does not lands in the '*' arm and stops the container, rather
# than leaving a core scanning next to no stack at all.
case "$role" in
    none)    STACK="" ;;
    adapter) STACK="$BIN_DIR/softplc-eip-adapter" ;;
    scanner) STACK="$BIN_DIR/softplc-eip-scanner" ;;
    *)
        log "no stack process is known for role '$role' (see: softplc --list-roles)"
        exit 1
        ;;
esac

if [[ -z $STACK ]]; then
    # No fieldbus: nothing to supervise, so let the core be PID 1 and take
    # signals directly.
    log "role '$role': no protocol stack, running the core alone"
    exec "$CORE"
fi

if [[ ! -x $STACK ]]; then
    log "role '$role' needs $STACK, which this image does not contain"
    exit 1
fi

restart=$("$CORE" --print-config stack_restart)
delay_ms=$("$CORE" --print-config stack_restart_delay_ms)
[[ -z $restart  ]] && restart=on
[[ -z $delay_ms ]] && delay_ms=1000
delay=$(awk "BEGIN { printf \"%.3f\", $delay_ms / 1000 }" 2>/dev/null) || delay=1

STOPPING=0
CORE_PID=""
STACK_PID=""

start_stack() {
    "$STACK" &
    STACK_PID=$!
    log "started $(basename "$STACK") as pid $STACK_PID"
}

on_term() {
    # Ignore further signals: a second SIGTERM while we are shutting down
    # would re-enter this and race the waits below.
    trap '' TERM INT
    STOPPING=1
    log "stopping"
    [[ -n $STACK_PID ]] && kill -TERM "$STACK_PID" 2>/dev/null
    [[ -n $CORE_PID  ]] && kill -TERM "$CORE_PID"  2>/dev/null
    return 0
}
trap on_term TERM INT

"$CORE" &
CORE_PID=$!
log "started softplc as pid $CORE_PID (role '$role')"
start_stack

core_status=0

while :; do
    # `wait -p` unsets the variable before assigning, so every read of it below
    # has to tolerate it being unset - `set -u` would otherwise abort here on
    # the one path that matters, a signal arriving during shutdown.
    wait -n -p reaped
    rc=$?

    if [[ -z ${reaped:-} ]]; then
        # A trap ran (rc > 128) and interrupted the wait, or there is nothing
        # left to reap.  Neither is a child exiting, so decide by STOPPING.
        (( STOPPING == 0 && rc > 128 )) && continue
        break
    fi

    if [[ ${reaped:-} == "$CORE_PID" ]]; then
        core_status=$rc
        CORE_PID=""
        # The core owns the shared memory and the doorbells, so its exit
        # invalidates the stack's mappings.  Nothing here can put that back:
        # take the container down and let the orchestrator's restart policy
        # own the recovery, exactly as it does in the multi-container form.
        (( STOPPING == 0 )) && log "the PLC core exited ($rc); stopping the container"
        [[ -n $STACK_PID ]] && kill -TERM "$STACK_PID" 2>/dev/null
        STOPPING=1
        continue
    fi

    if [[ ${reaped:-} == "$STACK_PID" ]]; then
        STACK_PID=""
        (( STOPPING == 1 )) && continue

        # This is the fault the whole process split exists to contain: the
        # stack is gone and the core is still scanning, on failsafe values,
        # because it never shared memory with it.
        log "the protocol stack exited ($rc); the core keeps scanning on failsafe values"
        if [[ $restart == on || $restart == 1 || $restart == true || $restart == yes ]]; then
            sleep "$delay"
            (( STOPPING == 0 )) && start_stack
        else
            log "stack_restart is off; leaving the core running without a stack"
        fi
        continue
    fi
done

[[ -n $CORE_PID  ]] && { wait "$CORE_PID"; core_status=$?; }
[[ -n $STACK_PID ]] && wait "$STACK_PID"

log "exiting ($core_status)"
exit "$core_status"

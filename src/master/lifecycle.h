/* SPDX-License-Identifier: GPL-2.0-only */
// Slave lifecycle state machine (docs/lifecycle.md). Pure transition table:
// the master performs every state change through lifecycle_next(), so an
// illegal transition is impossible by construction and unit-testable.
#pragma once

#include "common/types.h"

namespace dnuma {

enum class LifecycleEvent : uint8_t {
    Connect,          // administrative connect request accepted
    ConnectOk,        // attach handshake, RDMA link and lease all established
    ConnectFail,      // attach attempt failed or was abandoned
    Drain,            // graceful disconnect requested
    DrainDone,        // evacuation verified and detach acknowledged
    DrainCancel,      // drain failed, timed out or was cancelled: stay attached
    HardDisconnect,   // administrative forced disconnect
    Failure,          // unexpected failure detected
    FenceBegin,       // master starts isolating a failed slave
    FenceConfirmed,   // remote side proved all contexts destroyed
    FenceUnconfirmed, // master side done, remote side silent
    LeaseExpired,     // the old incarnation's lease has provably run out
};

const char* to_string(LifecycleEvent e);

// Computes the successor state. `fence_pending` says whether an earlier
// incarnation of this slave is still unproven-stopped; a failed reconnect
// attempt must then fall back to FENCE_INCOMPLETE, not DISCONNECTED.
// Returns false (and leaves *to untouched) if the event is not legal in `from`.
bool lifecycle_next(SlaveState from, LifecycleEvent ev, bool fence_pending, SlaveState* to);

// States in which the slave's resources may be used for new placement.
inline bool lifecycle_placeable(SlaveState s) { return s == SlaveState::Active; }
// States in which the slave is still serving existing pages and contexts.
inline bool lifecycle_serving(SlaveState s) { return s == SlaveState::Active || s == SlaveState::Draining; }

} // namespace dnuma

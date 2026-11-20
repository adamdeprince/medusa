// SPDX-License-Identifier: GPL-2.0-only
#include "master/lifecycle.h"

namespace dnuma {

const char* to_string(LifecycleEvent e) {
    switch (e) {
    case LifecycleEvent::Connect: return "connect";
    case LifecycleEvent::ConnectOk: return "connect-ok";
    case LifecycleEvent::ConnectFail: return "connect-fail";
    case LifecycleEvent::Drain: return "drain";
    case LifecycleEvent::DrainDone: return "drain-done";
    case LifecycleEvent::DrainCancel: return "drain-cancel";
    case LifecycleEvent::HardDisconnect: return "hard-disconnect";
    case LifecycleEvent::Failure: return "failure";
    case LifecycleEvent::FenceBegin: return "fence-begin";
    case LifecycleEvent::FenceConfirmed: return "fence-confirmed";
    case LifecycleEvent::FenceUnconfirmed: return "fence-unconfirmed";
    case LifecycleEvent::LeaseExpired: return "lease-expired";
    }
    return "?";
}

bool lifecycle_next(SlaveState from, LifecycleEvent ev, bool fence_pending, SlaveState* to) {
    using S = SlaveState;
    using E = LifecycleEvent;
    S next;
    switch (from) {
    case S::Disconnected:
        if (ev == E::Connect) next = S::Connecting;
        else return false;
        break;
    case S::Connecting:
        if (ev == E::ConnectOk) next = S::Active;
        else if (ev == E::ConnectFail) next = fence_pending ? S::FenceIncomplete : S::Disconnected;
        else if (ev == E::Failure) next = S::Failed;
        else if (ev == E::HardDisconnect) next = S::Fencing;
        else return false;
        break;
    case S::Active:
        if (ev == E::Drain) next = S::Draining;
        else if (ev == E::HardDisconnect) next = S::Fencing;
        else if (ev == E::Failure) next = S::Failed;
        else return false;
        break;
    case S::Draining:
        if (ev == E::DrainDone) next = S::Disconnected;
        else if (ev == E::DrainCancel) next = S::Active;
        else if (ev == E::HardDisconnect) next = S::Fencing;
        else if (ev == E::Failure) next = S::Failed;
        else return false;
        break;
    case S::Failed:
        if (ev == E::FenceBegin) next = S::Fencing;
        else return false;
        break;
    case S::Fencing:
        if (ev == E::FenceConfirmed) next = S::Disconnected;
        else if (ev == E::FenceUnconfirmed) next = S::FenceIncomplete;
        else return false;
        break;
    case S::FenceIncomplete:
        // A reconnect is allowed: the attach handshake makes the agent
        // destroy and report every stale context before it becomes ACTIVE.
        if (ev == E::Connect) next = S::Connecting;
        else if (ev == E::LeaseExpired || ev == E::FenceConfirmed) next = S::Disconnected;
        else return false;
        break;
    default:
        return false;
    }
    *to = next;
    return true;
}

} // namespace dnuma

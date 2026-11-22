// SPDX-License-Identifier: GPL-2.0-only
#include "master/coherence.h"

#include <algorithm>
#include <cinttypes>
#include <memory>

#include "common/log.h"
#include "common/util.h"

namespace dnuma {

namespace {
bool contains(const std::vector<MachineId>& v, MachineId m) {
    return std::find(v.begin(), v.end(), m) != v.end();
}
template <typename Regions>
auto covering(Regions& regions, PageNo page) {
    auto it = regions.upper_bound(page);
    if (it == regions.begin()) return regions.end();
    --it;
    return page - it->first < it->second.count ? it : regions.end();
}
bool exports(RevokeMode m) { return m != RevokeMode::Invalidate; }
std::string readers_str(const std::vector<MachineId>& r) {
    std::string s = "{";
    for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + std::to_string(r[i]);
    return s + "}";
}
} // namespace

// ---------------- spaces ----------------

Coherence::Space* Coherence::space(ProcId proc) {
    auto it = spaces_.find(proc);
    return it == spaces_.end() ? nullptr : &it->second;
}
const Coherence::Space* Coherence::space(ProcId proc) const {
    auto it = spaces_.find(proc);
    return it == spaces_.end() ? nullptr : &it->second;
}

void Coherence::create_space(ProcId proc, PageNo first, uint64_t count) {
    Space& sp = spaces_[proc];
    sp.first = first;
    sp.count = count;
}

void Coherence::destroy_space(ProcId proc) {
    auto sit = spaces_.find(proc);
    if (sit == spaces_.end()) return;
    std::vector<Done> dones;
    for (auto it = txns_.begin(); it != txns_.end();) {
        if (it->first.proc != proc) {
            ++it;
            continue;
        }
        Txn& t = it->second;
        if (t.phase == Phase::Granting && t.grant_source != GrantSource::Local) {
            auto in = inflight_.find(t.target);
            if (in != inflight_.end() && in->second > 0) --in->second;
        }
        if (t.done) dones.push_back(std::move(t.done));
        it = txns_.erase(it);
    }
    for (const auto& [m, u] : sit->second.usage) {
        MachineUsage& g = usage_[m];
        g.resident -= std::min(g.resident, u.resident);
        g.owned -= std::min(g.owned, u.owned);
        g.exclusive -= std::min(g.exclusive, u.exclusive);
    }
    spaces_.erase(sit);
    for (Done& d : dones) d(Status::err(ECANCELED, "process destroyed"));
}

Status Coherence::allocate(ProcId proc, uint64_t npages, NodeId home, PageNo* first) {
    Space* sp = space(proc);
    if (!sp) return Status::err(ESRCH, "no such process");
    if (npages == 0) return Status::err(EINVAL, "empty allocation");
    if (sp->first > page_of(UINT64_MAX) || sp->count > page_of(UINT64_MAX) + 1 - sp->first)
        return Status::err(EOVERFLOW, "address space exceeds virtual address range");
    // First fit over the gaps between existing regions.
    PageNo cursor = sp->first;
    PageNo end = sp->first + sp->count;
    for (const auto& [start, rg] : sp->regions) {
        if (start - cursor >= npages) break;
        cursor = start + rg.count;
    }
    if (cursor > end || end - cursor < npages)
        return Status::err(ENOMEM, "distributed address space exhausted (%" PRIu64 " pages requested)", npages);
    Region rg;
    rg.first = cursor;
    rg.count = npages;
    rg.home = home;
    try {
        sp->regions.emplace(cursor, std::move(rg));
    } catch (const std::bad_alloc&) {
        return Status::err(ENOMEM, "cannot allocate region metadata");
    }
    *first = cursor;
    return Status::ok();
}

Status Coherence::register_native_region(ProcId proc, PageNo first, uint64_t count, NodeId home) {
    Space* sp = space(proc);
    if (!sp) return Status::err(ESRCH, "no such process");
    if (!count || first > page_of(UINT64_MAX) || count - 1 > page_of(UINT64_MAX) - first ||
        first < sp->first || first - sp->first >= sp->count || count > sp->count - (first - sp->first))
        return Status::err(EINVAL, "native mapping outside the address space");
    auto next = sp->regions.lower_bound(first);
    if ((next != sp->regions.end() && next->first - first < count) || covering(sp->regions, first) != sp->regions.end())
        return Status::err(EEXIST, "native mapping overlaps a registered region");
    Region rg;
    rg.first = first;
    rg.count = count;
    rg.home = home;
    rg.initial.state = PageState::Exclusive;
    rg.initial.owner = kMasterMachine;
    try {
        sp->regions.emplace_hint(next, first, std::move(rg));
    } catch (const std::bad_alloc&) {
        return Status::err(ENOMEM, "cannot allocate native mapping metadata");
    }
    return Status::ok();
}

PageEntry* Coherence::entry(ProcId proc, PageNo page, Region** region) {
    Space* sp = space(proc);
    if (!sp) return nullptr;
    auto it = covering(sp->regions, page);
    if (it == sp->regions.end()) return nullptr;
    Region& rg = it->second;
    if (region) *region = &rg;
    if (rg.releasing && !rg.pages.count(page)) return nullptr;
    auto [p, fresh] = rg.pages.try_emplace(page, rg.initial);
    if (fresh) account(*sp, p->second, +1);
    return &p->second;
}

const PageEntry* Coherence::lookup(ProcId proc, PageNo page, NodeId* home) const {
    const Space* sp = space(proc);
    if (!sp) return nullptr;
    auto it = covering(sp->regions, page);
    if (it == sp->regions.end()) return nullptr;
    const Region& rg = it->second;
    if (home) *home = rg.home;
    auto p = rg.pages.find(page);
    return p == rg.pages.end() ? &rg.initial : &p->second;
}

void Coherence::set_region_home(ProcId proc, PageNo region_first, NodeId home) {
    Space* sp = space(proc);
    if (!sp) return;
    auto it = sp->regions.find(region_first);
    if (it != sp->regions.end()) it->second.home = home;
}

void Coherence::account(Space& sp, const PageEntry& e, int sign) {
    auto apply = [&](MachineId m, bool owner, bool exclusive) {
        for (MachineUsage* u : {&sp.usage[m], &usage_[m]}) {
            u->resident += uint64_t(sign);
            if (owner) u->owned += uint64_t(sign);
            if (exclusive) u->exclusive += uint64_t(sign);
        }
    };
    if (e.state == PageState::Exclusive) {
        apply(e.owner, true, true);
    } else if (e.state == PageState::Shared) {
        apply(e.owner, true, e.readers.empty());
        for (MachineId r : e.readers) apply(r, false, false);
    }
}

void Coherence::set_state(ProcId proc, Space& sp, PageNo page, PageEntry& e, PageState st, MachineId owner,
                          std::vector<MachineId> readers, Generation gen, const char* cause) {
    PageState old = e.state;
    MachineId old_owner = e.owner;
    account(sp, e, -1);
    e.state = st;
    e.owner = (st == PageState::Unbacked || st == PageState::Lost) ? kNoMachine : owner;
    e.readers = (st == PageState::Shared) ? std::move(readers) : std::vector<MachineId>{};
    e.gen = gen;
    e.last_ctx = 0;
    e.last_machine = kNoMachine;
    account(sp, e, +1);
    metrics_.add("page_ownership_transitions");
    PAGE_TRACE("PAGE dir proc=%" PRIu64 " page=0x%" PRIx64 " %s(owner=%d) -> %s(owner=%d readers=%s) gen=%" PRIu64 " cause=%s",
               proc, addr_of(page), to_string(old), int(old_owner), to_string(e.state), int(e.owner),
               readers_str(e.readers).c_str(), gen, cause);
}

void Coherence::mark_populated(ProcId proc, PageNo first, uint64_t count, MachineId owner) {
    Space* sp = space(proc);
    if (!sp) return;
    for (uint64_t i = 0; i < count; ++i) {
        Region* rg = nullptr;
        PageEntry* e = entry(proc, first + i, &rg);
        if (!e || rg->releasing || e->busy || e->state != PageState::Unbacked) continue;
        set_state(proc, *sp, first + i, *e, PageState::Exclusive, owner, {}, e->gen + 1, "populate");
    }
}

// ---------------- transactions ----------------

Coherence::Txn& Coherence::begin(ProcId proc, PageNo page, PageEntry& e, Kind kind, MachineId target) {
    e.busy = true;
    Txn& t = txns_[Key{proc, page}];
    t.kind = kind;
    t.gen = e.gen + 1;
    t.target = target;
    t.phase = Phase::Revoking;
    t.t0 = now_ns();
    return t;
}

void Coherence::revoke(ProcId proc, PageNo page, Txn& t, MachineId holder, RevokeMode mode) {
    t.awaiting.insert(holder);
    if (exports(mode)) {
        t.source = holder;
        t.source_mode = mode;
    }
    PageMsg m;
    m.proc = proc;
    m.page = page;
    m.gen = t.gen;
    m.mode = mode;
    metrics_.add(mode == RevokeMode::Invalidate || mode == RevokeMode::InvalidateExport ? "page_invalidations"
                                                                                         : "page_downgrades");
    io_.send_page(holder, MsgType::PageRevoke, m);
}

void Coherence::grant(ProcId proc, PageNo page, Txn& t) {
    Region* rg = nullptr;
    PageEntry* e = entry(proc, page, &rg);
    if (!e) return;
    t.phase = Phase::Granting;
    PageMsg g;
    g.proc = proc;
    g.page = page;
    g.gen = t.gen;
    g.access = t.access;
    g.source = t.grant_source;
    g.flags = uint8_t((t.park ? kPageFlagPark : 0) |
                      (t.kind == Kind::Migrate && t.grant_source == GrantSource::Local ? kPageFlagHandoff : 0));
    g.src_machine = t.source;
    g.src_inc = t.src_inc;
    g.slot = t.slot;
    g.home_node = rg->home;
    if (t.grant_source != GrantSource::Local) ++inflight_[t.target];
    io_.send_page(t.target, MsgType::PageGrant, g);
}

void Coherence::finish(ProcId proc, PageNo page, Status st) {
    auto it = txns_.find(Key{proc, page});
    if (it == txns_.end()) return;
    Txn t = std::move(it->second);
    txns_.erase(it);
    if (t.phase == Phase::Granting && t.grant_source != GrantSource::Local) {
        auto in = inflight_.find(t.target);
        if (in != inflight_.end() && in->second > 0) --in->second;
    }
    if (PageEntry* e = entry(proc, page)) e->busy = false;
    metrics_.observe("page_txn_ns", now_ns() - t.t0);
    if (t.done) t.done(st);
    // Work that queued behind this transaction runs in arrival order. As soon
    // as one item starts a new transaction, the rest queue behind that one.
    for (auto& fn : t.deferred) {
        auto nt = txns_.find(Key{proc, page});
        if (nt != txns_.end()) nt->second.deferred.push_back(std::move(fn));
        else fn();
    }
}

void Coherence::defer_or_run(ProcId proc, PageNo page, std::function<void()> fn) {
    auto it = txns_.find(Key{proc, page});
    if (it != txns_.end()) it->second.deferred.push_back(std::move(fn));
    else fn();
}

void Coherence::on_request(MachineId from, const PageMsg& m, CtxId ctx) {
    metrics_.add("page_fault_requests");
    ProcId proc = m.proc;
    PageNo page = m.page;
    Access access = m.access;
    defer_or_run(proc, page, [this, from, proc, page, access, ctx] { start_fault(from, proc, page, access, ctx); });
}

void Coherence::start_fault(MachineId from, ProcId proc, PageNo page, Access access, CtxId ctx) {
    Space* sp = space(proc);
    if (!sp || !io_.machine_usable(from)) return;
    Region* rg = nullptr;
    PageEntry* e = entry(proc, page, &rg);
    if (!e || rg->releasing) {
        // Inside the reserved range but not allocated: the node raises SIGSEGV.
        PageMsg g;
        g.proc = proc;
        g.page = page;
        g.status = EFAULT;
        io_.send_page(from, MsgType::PageGrant, g);
        return;
    }
    if (e->busy) {
        txns_[Key{proc, page}].deferred.push_back([this, from, proc, page, access, ctx] { start_fault(from, proc, page, access, ctx); });
        return;
    }
    switch (e->state) {
    case PageState::Lost:
        io_.coherence_failure(proc, format("access to lost page 0x%" PRIx64, addr_of(page)));
        return;
    case PageState::Unbacked: {
        // First touch: the toucher becomes exclusive owner of a zero page.
        Txn& t = begin(proc, page, *e, Kind::Fault, from);
        t.ctx = ctx;
        t.access = Access::Write;
        t.grant_source = GrantSource::Zero;
        t.new_state = PageState::Exclusive;
        t.new_owner = from;
        grant(proc, page, t);
        return;
    }
    case PageState::Exclusive: {
        MachineId owner = e->owner;
        Txn& t = begin(proc, page, *e, Kind::Fault, from);
        t.ctx = ctx;
        if (owner == from) {
            t.access = Access::Write;
            t.grant_source = GrantSource::Local;
            t.new_state = PageState::Exclusive;
            t.new_owner = from;
            grant(proc, page, t);
        } else if (access == Access::Read) {
            metrics_.add("remote_page_faults");
            t.access = Access::Read;
            t.grant_source = GrantSource::Remote;
            t.new_state = PageState::Shared;
            t.new_owner = owner;
            t.new_readers = {from};
            revoke(proc, page, t, owner, RevokeMode::DowngradeExport);
        } else {
            metrics_.add("remote_page_faults");
            t.access = Access::Write;
            t.grant_source = GrantSource::Remote;
            t.new_state = PageState::Exclusive;
            t.new_owner = from;
            revoke(proc, page, t, owner, RevokeMode::InvalidateExport);
        }
        return;
    }
    case PageState::Shared: {
        MachineId owner = e->owner;
        std::vector<MachineId> readers = e->readers;
        bool holds = from == owner || contains(readers, from);
        Txn& t = begin(proc, page, *e, Kind::Fault, from);
        t.ctx = ctx;
        if (access == Access::Read) {
            t.access = Access::Read;
            t.new_state = PageState::Shared;
            t.new_owner = owner;
            t.new_readers = readers;
            if (holds) {
                t.grant_source = GrantSource::Local;
                grant(proc, page, t);
            } else {
                metrics_.add("remote_page_faults");
                t.grant_source = GrantSource::Remote;
                t.new_readers.push_back(from);
                revoke(proc, page, t, owner, RevokeMode::ExportOnly);
            }
            return;
        }
        // Write: every other copy must be gone before write access exists.
        t.access = Access::Write;
        t.new_state = PageState::Exclusive;
        t.new_owner = from;
        if (holds) {
            t.grant_source = GrantSource::Local;
            if (owner != from) revoke(proc, page, t, owner, RevokeMode::Invalidate);
            for (MachineId r : readers)
                if (r != from) revoke(proc, page, t, r, RevokeMode::Invalidate);
            if (t.awaiting.empty()) grant(proc, page, t);
        } else {
            metrics_.add("remote_page_faults");
            t.grant_source = GrantSource::Remote;
            for (MachineId r : readers) revoke(proc, page, t, r, RevokeMode::Invalidate);
            revoke(proc, page, t, owner, RevokeMode::InvalidateExport);
        }
        return;
    }
    }
}

void Coherence::migrate(ProcId proc, PageNo page, MachineId dest, Done done) {
    defer_or_run(proc, page, [this, proc, page, dest, done = std::move(done)]() mutable {
        start_migrate(proc, page, dest, std::move(done));
    });
}

void Coherence::start_migrate(ProcId proc, PageNo page, MachineId dest, Done done) {
    Region* rg = nullptr;
    PageEntry* e = entry(proc, page, &rg);
    if (!e || rg->releasing) return done(Status::err(EFAULT, "page not allocated"));
    if (e->busy) {
        txns_[Key{proc, page}].deferred.push_back([this, proc, page, dest, done = std::move(done)]() mutable {
            start_migrate(proc, page, dest, std::move(done));
        });
        return;
    }
    if (e->state == PageState::Unbacked) return done(Status::ok()); // nothing exists to move
    if (e->state == PageState::Lost) return done(Status::err(EIO, "page is lost"));
    if (!io_.machine_usable(dest)) return done(Status::err(ENODEV, "destination machine is not available"));
    MachineId owner = e->owner;
    std::vector<MachineId> readers = e->readers;
    if (e->state == PageState::Exclusive && owner == dest) return done(Status::ok());

    Txn& t = begin(proc, page, *e, Kind::Migrate, dest);
    t.done = std::move(done);
    t.access = Access::Write;
    t.new_state = PageState::Exclusive;
    t.new_owner = dest;
    metrics_.add("page_migrations");
    bool holds = owner == dest || contains(readers, dest);
    if (holds) {
        // The destination already has identical contents: hand ownership
        // over without moving data.
        t.grant_source = GrantSource::Local;
        if (owner != dest) revoke(proc, page, t, owner, RevokeMode::Invalidate);
        for (MachineId r : readers)
            if (r != dest) revoke(proc, page, t, r, RevokeMode::Invalidate);
        if (t.awaiting.empty()) grant(proc, page, t);
    } else {
        t.grant_source = GrantSource::Remote;
        t.park = true;
        for (MachineId r : readers) revoke(proc, page, t, r, RevokeMode::Invalidate);
        revoke(proc, page, t, owner, RevokeMode::InvalidateExport);
    }
}

void Coherence::drop_replica(ProcId proc, PageNo page, MachineId holder, Done done) {
    defer_or_run(proc, page, [this, proc, page, holder, done = std::move(done)]() mutable {
        start_drop(proc, page, holder, std::move(done));
    });
}

void Coherence::start_drop(ProcId proc, PageNo page, MachineId holder, Done done) {
    PageEntry* e = entry(proc, page);
    if (!e) return done(Status::ok());
    if (e->busy) {
        txns_[Key{proc, page}].deferred.push_back([this, proc, page, holder, done = std::move(done)]() mutable {
            start_drop(proc, page, holder, std::move(done));
        });
        return;
    }
    if (e->state != PageState::Shared || !contains(e->readers, holder)) return done(Status::ok());
    MachineId owner = e->owner;
    std::vector<MachineId> rest;
    for (MachineId r : e->readers)
        if (r != holder) rest.push_back(r);
    Txn& t = begin(proc, page, *e, Kind::DropReplica, holder);
    t.done = std::move(done);
    t.new_owner = owner;
    t.new_state = rest.empty() ? PageState::Exclusive : PageState::Shared;
    t.new_readers = std::move(rest);
    revoke(proc, page, t, holder, RevokeMode::Invalidate);
}

void Coherence::start_discard(ProcId proc, PageNo page, Done done) {
    PageEntry* e = entry(proc, page);
    if (!e) return done(Status::ok());
    if (e->busy) {
        txns_[Key{proc, page}].deferred.push_back([this, proc, page, done = std::move(done)]() mutable {
            start_discard(proc, page, std::move(done));
        });
        return;
    }
    if (e->state == PageState::Unbacked) return done(Status::ok());
    Space* sp = space(proc);
    if (e->state == PageState::Lost) {
        set_state(proc, *sp, page, *e, PageState::Unbacked, kNoMachine, {}, e->gen + 1, "free");
        return done(Status::ok());
    }
    MachineId owner = e->owner;
    std::vector<MachineId> readers = e->readers;
    Txn& t = begin(proc, page, *e, Kind::Discard, kNoMachine);
    t.done = std::move(done);
    t.new_state = PageState::Unbacked;
    revoke(proc, page, t, owner, RevokeMode::Invalidate);
    for (MachineId r : readers) revoke(proc, page, t, r, RevokeMode::Invalidate);
}

void Coherence::free_region(ProcId proc, PageNo first, Done done) {
    Space* sp = space(proc);
    if (!sp) return done(Status::err(ESRCH, "no such process"));
    auto it = sp->regions.find(first);
    if (it == sp->regions.end()) return done(Status::err(EINVAL, "address is not the start of an allocation"));
    Region& rg = it->second;
    if (rg.releasing) return done(Status::err(EBUSY, "allocation already being released"));
    if (rg.pages.empty()) {
        sp->regions.erase(it);
        return done(Status::ok());
    }
    // A completion can synchronously erase the region. Walk an independent
    // list and keep new first touches out while invalidations are in flight.
    std::vector<PageNo> touched;
    touched.reserve(rg.pages.size());
    for (const auto& item : rg.pages) touched.push_back(item.first);
    rg.releasing = true;
    auto remaining = std::make_shared<size_t>(touched.size());
    auto first_err = std::make_shared<Status>();
    auto shared_done = std::make_shared<Done>(std::move(done));
    for (PageNo page : touched) {
        defer_or_run(proc, page, [this, proc, page, first, remaining, first_err, shared_done] {
            start_discard(proc, page, [this, proc, first, remaining, first_err, shared_done](Status st) {
                if (!st.is_ok() && first_err->is_ok()) *first_err = st;
                if (--*remaining != 0) return;
                if (Space* sp = space(proc)) {
                    auto it = sp->regions.find(first);
                    if (it != sp->regions.end()) {
                        if (first_err->is_ok()) sp->regions.erase(it);
                        else it->second.releasing = false;
                    }
                }
                (*shared_done)(*first_err);
            });
        });
    }
}

void Coherence::on_revoked(MachineId from, const PageMsg& m) {
    auto it = txns_.find(Key{m.proc, m.page});
    if (it == txns_.end() || it->second.gen != m.gen) return; // stale
    Txn& t = it->second;
    if (!t.awaiting.erase(from)) return;
    if (m.status != 0) {
        if (t.error.is_ok())
            t.error = Status::err(m.status, "machine %u could not revoke page 0x%" PRIx64, from, addr_of(m.page));
    } else if (from == t.source && exports(t.source_mode)) {
        t.source_staged = true;
        t.slot = m.slot;
        t.src_inc = io_.incarnation_of(from);
    }
    if (t.awaiting.empty()) after_revokes(m.proc, m.page);
}

void Coherence::after_revokes(ProcId proc, PageNo page) {
    auto it = txns_.find(Key{proc, page});
    if (it == txns_.end()) return;
    Txn& t = it->second;
    Space* sp = space(proc);
    PageEntry* e = entry(proc, page);
    if (!sp || !e) return finish(proc, page, Status::err(ESRCH, "process gone"));

    auto abort_source = [&] {
        if (!t.source_staged) return;
        PageMsg a;
        a.proc = proc;
        a.page = page;
        a.gen = t.gen;
        a.flags = t.source_mode == RevokeMode::DowngradeExport ? kPageFlagRestoreWrite : 0;
        io_.send_page(t.source, MsgType::PageAbort, a);
        // An invalidated source re-adopts its staged copy as sole owner.
        if (t.source_mode == RevokeMode::InvalidateExport)
            set_state(proc, *sp, page, *e, PageState::Exclusive, t.source, {}, t.gen, "transfer-aborted");
    };

    if (!t.error.is_ok()) {
        // A holder could not give the page up; its contents are in doubt.
        Status err = t.error;
        Kind kind = t.kind;
        abort_source();
        finish(proc, page, err);
        if (kind == Kind::Fault || kind == Kind::Migrate)
            io_.coherence_failure(proc, err.message());
        return;
    }
    if (t.kind == Kind::DropReplica || t.kind == Kind::Discard) {
        set_state(proc, *sp, page, *e, t.new_state, t.new_owner, t.new_readers, t.gen,
                  t.kind == Kind::Discard ? "free" : "drop-replica");
        return finish(proc, page, Status::ok());
    }
    if (t.cancelled || !io_.machine_usable(t.target)) {
        abort_source();
        return finish(proc, page, Status::err(ENODEV, "destination machine went away during the transfer"));
    }
    grant(proc, page, t);
}

void Coherence::on_complete(MachineId from, const PageMsg& m) {
    auto it = txns_.find(Key{m.proc, m.page});
    if (it == txns_.end() || it->second.gen != m.gen) return; // stale
    Txn& t = it->second;
    if (t.phase != Phase::Granting || t.target != from) return;
    Space* sp = space(m.proc);
    PageEntry* e = entry(m.proc, m.page);
    if (!sp || !e) return finish(m.proc, m.page, Status::err(ESRCH, "process gone"));

    PageMsg x;
    x.proc = m.proc;
    x.page = m.page;
    x.gen = t.gen;
    if (m.status == 0) {
        if (t.source_staged) io_.send_page(t.source, MsgType::PageRelease, x);
        if (t.grant_source == GrantSource::Remote) metrics_.add("page_transfers");
        const char* cause = t.kind == Kind::Migrate ? "migrate" : (t.access == Access::Write ? "write-fault" : "read-fault");
        CtxId previous = e->last_ctx;
        MachineId peer = e->last_machine;
        // Downgrading a writer so that others can read is ordinary replication.
        // Only a new writer taking access away from a remote holder votes.
        bool handoff = t.ctx && previous && t.ctx != previous && peer != from && t.access == Access::Write;
        set_state(m.proc, *sp, m.page, *e, t.new_state, t.new_owner, t.new_readers, t.gen, cause);
        e->last_ctx = t.ctx;
        e->last_machine = from;
        if (handoff) io_.page_handoff(m.proc, t.ctx, from, previous, peer, now_ns() - t.t0);
        return finish(m.proc, m.page, Status::ok());
    }

    // The destination could not take the page. Ownership stays with (or
    // returns to) the source: nothing is lost.
    Kind kind = t.kind;
    Status err = Status::err(m.status, "machine %u could not accept page 0x%" PRIx64, from, addr_of(m.page));
    if (t.source_staged) {
        x.flags = t.source_mode == RevokeMode::DowngradeExport ? kPageFlagRestoreWrite : 0;
        io_.send_page(t.source, MsgType::PageAbort, x);
        if (t.source_mode == RevokeMode::InvalidateExport)
            set_state(m.proc, *sp, m.page, *e, PageState::Exclusive, t.source, {}, t.gen, "transfer-aborted");
    }
    metrics_.add("page_transfer_failures");
    finish(m.proc, m.page, err);
    if (kind == Kind::Fault) io_.coherence_failure(m.proc, err.message());
}

void Coherence::machine_gone(MachineId m) {
    std::vector<Key> granting;
    for (auto& [k, t] : txns_) {
        if (t.target != m || (t.kind != Kind::Fault && t.kind != Kind::Migrate)) continue;
        if (t.phase == Phase::Revoking) t.cancelled = true;
        else granting.push_back(k);
    }
    for (const Key& k : granting) {
        auto it = txns_.find(k);
        if (it == txns_.end()) continue;
        PageMsg fail;
        fail.proc = k.proc;
        fail.page = k.page;
        fail.gen = it->second.gen;
        fail.status = ENODEV;
        // Same path as an explicit failure report from the destination.
        Kind kind = it->second.kind;
        it->second.kind = Kind::Migrate; // the requester is gone; nobody to fail towards
        (void)kind;
        on_complete(m, fail);
    }
}

// ---------------- queries ----------------

MachineUsage Coherence::usage(MachineId m) const {
    auto it = usage_.find(m);
    return it == usage_.end() ? MachineUsage{} : it->second;
}

MachineUsage Coherence::usage(ProcId proc, MachineId m) const {
    const Space* sp = space(proc);
    if (!sp) return {};
    auto it = sp->usage.find(m);
    return it == sp->usage.end() ? MachineUsage{} : it->second;
}

uint64_t Coherence::inflight_to(MachineId m) const {
    auto it = inflight_.find(m);
    return it == inflight_.end() ? 0 : it->second;
}

size_t Coherence::transactions_involving(MachineId m) const {
    size_t n = 0;
    for (const auto& [k, t] : txns_)
        if (t.target == m || t.source == m || t.awaiting.count(m)) ++n;
    return n;
}

std::vector<ProcId> Coherence::processes() const {
    std::vector<ProcId> out;
    for (const auto& [id, sp] : spaces_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

void Coherence::for_each_page_on(MachineId m,
                                 const std::function<void(ProcId, PageNo, const PageEntry&)>& fn) const {
    for (const auto& [proc, sp] : spaces_) {
        auto u = sp.usage.find(m);
        if (u == sp.usage.end() || u->second.resident == 0) continue;
        for (const auto& [first, rg] : sp.regions)
            for (const auto& [page, e] : rg.pages) {
                if (e.state != PageState::Exclusive && e.state != PageState::Shared) continue;
                if (e.owner == m || contains(e.readers, m)) fn(proc, page, e);
            }
    }
}

void Coherence::for_each_region(ProcId proc, const std::function<void(const Region&)>& fn) const {
    const Space* sp = space(proc);
    if (!sp) return;
    for (const auto& [first, rg] : sp->regions) fn(rg);
}

uint64_t Coherence::allocated_pages(ProcId proc) const {
    const Space* sp = space(proc);
    uint64_t n = 0;
    if (sp)
        for (const auto& [first, rg] : sp->regions) n += rg.count;
    return n;
}

} // namespace dnuma

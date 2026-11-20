// SPDX-License-Identifier: GPL-2.0-only
#include <cstring>

#include "transport/auth.h"
#include "transport/messages.h"
#include "utest.h"

using namespace dnuma;

TEST(page_message_roundtrip) {
    PageMsg a;
    a.proc = 7;
    a.page = 0x200000123;
    a.gen = 99;
    a.access = Access::Write;
    a.mode = RevokeMode::InvalidateExport;
    a.source = GrantSource::Remote;
    a.flags = kPageFlagPark;
    a.status = -5;
    a.src_machine = 3;
    a.src_inc = 0x1122334455667788ull;
    a.slot = 4242;
    a.home_node = 6;
    Msg m = make_msg(MsgType::PageGrant, a, 11, 22);
    PageMsg b;
    CHECK(parse_msg(m, &b));
    CHECK(b.proc == 7 && b.page == 0x200000123 && b.gen == 99 && b.access == Access::Write);
    CHECK(b.mode == RevokeMode::InvalidateExport && b.source == GrantSource::Remote && b.flags == kPageFlagPark);
    CHECK(b.status == -5 && b.src_machine == 3 && b.src_inc == 0x1122334455667788ull && b.slot == 4242 && b.home_node == 6);
    CHECK(m.req_id == 11 && m.inc == 22);
}

TEST(nested_message_roundtrip) {
    NodeAttachAckMsg a;
    a.resources.hostname = "node-b";
    a.resources.nodes.resize(2);
    a.resources.nodes[1].phys_index = 1;
    a.resources.nodes[1].cpus = {1, 5, 9};
    a.resources.nodes[1].mem_total = 126ull << 30;
    RdmaPortInfo p;
    p.device = "ib0";
    p.ipv4 = "192.0.2.10";
    p.numa_node = 1;
    a.resources.rdma_ports.push_back(p);
    a.rdma_ip = "192.0.2.10";
    a.rdma_port = 7711;
    a.capacity_bytes = 1ull << 40;
    Msg m = make_msg(MsgType::NodeAttachAck, a);
    NodeAttachAckMsg b;
    CHECK(parse_msg(m, &b));
    CHECK(b.resources.hostname == "node-b" && b.resources.nodes.size() == 2);
    CHECK(b.resources.nodes[1].cpus.size() == 3 && b.resources.nodes[1].cpus[2] == 9);
    CHECK(b.resources.rdma_ports.size() == 1 && b.resources.rdma_ports[0].ipv4 == "192.0.2.10");
    CHECK(b.rdma_port == 7711 && b.capacity_bytes == (1ull << 40));

    SyscallMsg s;
    s.nr = 202;
    s.args = {1, 2, 3, 4, 5, 6};
    s.ret = -1;
    s.err = 11;
    SyscallMsg t;
    CHECK(parse_msg(make_msg(MsgType::SyscallRequest, s), &t) && t.args[5] == 6 && t.ret == -1 && t.err == 11);
}

TEST(truncated_and_oversized_payloads_are_rejected) {
    ImageMsg im;
    im.exe = "/bin/x";
    im.argv = {"a", "bb", "ccc"};
    Msg m = make_msg(MsgType::ImageCreate, im);
    ImageMsg out;
    CHECK(parse_msg(m, &out));
    for (size_t cut = 0; cut < m.payload.size(); ++cut) { // every strict prefix must fail
        Msg t = m;
        t.payload.resize(cut);
        ImageMsg x;
        CHECK(!parse_msg(t, &x));
    }
    Msg extra = m; // trailing garbage must fail too
    extra.payload.push_back(0);
    CHECK(!parse_msg(extra, &out));
}

TEST(hostile_lengths_cannot_force_allocation) {
    // A vector count far larger than the bytes that follow.
    Writer w;
    w.u32(0xffffffff);
    Msg m;
    m.payload = w.take();
    CtlRequestMsg rq;
    CHECK(!parse_msg(m, &rq));
    // A string length pointing past the end.
    Writer w2;
    w2.u32(1);          // one string
    w2.u32(0x7fffffff); // of absurd length
    w2.raw("abc", 3);
    m.payload = w2.take();
    CHECK(!parse_msg(m, &rq));
}

TEST(header_validation) {
    Msg m;
    m.type = MsgType::Heartbeat;
    m.req_id = 5;
    m.inc = 9;
    m.payload.assign(10, 1);
    uint8_t raw[kHeaderSize];
    encode_header(m, raw);
    WireHeader h;
    std::string why;
    CHECK(decode_header(raw, &h, &why));
    CHECK(h.payload_len == 10 && h.req_id == 5 && h.incarnation == 9 && h.type == uint16_t(MsgType::Heartbeat));
    uint8_t bad[kHeaderSize];
    memcpy(bad, raw, sizeof bad);
    bad[0] ^= 0xff; // magic
    CHECK(!decode_header(bad, &h, &why));
    memcpy(bad, raw, sizeof bad);
    uint16_t v = 999; // version
    memcpy(bad + 4, &v, 2);
    CHECK(!decode_header(bad, &h, &why));
    memcpy(bad, raw, sizeof bad);
    uint32_t len = kMaxPayload + 1; // length
    memcpy(bad + 8, &len, 4);
    CHECK(!decode_header(bad, &h, &why));
    memcpy(bad, raw, sizeof bad);
    uint32_t nfds = kMaxFdsPerFrame + 1;
    memcpy(bad + 12, &nfds, 4);
    CHECK(!decode_header(bad, &h, &why));
}

TEST(mutual_authentication_proofs) {
    std::vector<uint8_t> key(48, 7), other(48, 8), nm(32, 1), na(32, 2);
    auto p = agent_proof(key, nm, na, "uuid-1");
    CHECK(proof_equal(p, agent_proof(key, nm, na, "uuid-1")));
    CHECK(!proof_equal(p, agent_proof(other, nm, na, "uuid-1"))); // wrong key
    CHECK(!proof_equal(p, agent_proof(key, na, nm, "uuid-1")));   // nonces swapped (reflection)
    CHECK(!proof_equal(p, agent_proof(key, nm, na, "uuid-2")));   // different identity
    auto q = master_proof(key, nm, na, 42);
    CHECK(!proof_equal(q, master_proof(key, nm, na, 43)));
    CHECK(!proof_equal(p, q)); // an agent proof can never pass as a master proof
    CHECK(session_key(key, nm, na) != session_key(key, na, nm));
    CHECK(!proof_equal({}, {}));
}

UTEST_MAIN()

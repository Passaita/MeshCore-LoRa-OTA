#!/bin/bash
# A11 (v1.17.25): LoRa-OTA channel payloads through MeshCore's REAL encryption. The v1.17.23 bug (flooded LIST_RSP MAC-checked over the
# AES-padded length - every flooded answer and heal notice dropped) passed all other host tests because they call the handlers directly.
# Here the real code is EXTRACTED on every run:
#   - from src/helpers/fw_ota/FwOtaTransfer.cpp: buildListRsp, floodListRsp, sendDiscovery, onDiscovery, listRspLen, macFill/macOk, keys;
#   - from src/Mesh.cpp: createGroupDatagram (channel hash + Utils::encryptThenMAC) - the receive side is Mesh::onRecvPacket's GRP_TXT case
#     (checked below to still read the same), MACThenDecrypt -> the handler gets the PADDED length, exactly as on the node;
# and compiled with the real src/Utils.cpp + src/Packet.cpp and the pinned rweather/Crypto in test/crypto (the RAK build has no CC310).
# Round trips: flooded answers + heal notices for every route size, discovery requests of every target length -> the server's flooded
# answer -> back at the puller; every padding remainder; single-byte corruption rejected.
# Then the MUTANT check: the v1.17.23 bug is put back into the extracted onDiscovery - the test MUST fail.
# usage: test/fw_ota_crypto_test.sh
set -e
cd "$(dirname "$0")/.."
grep -q 'int len = Utils::MACThenDecrypt(channels\[j\].secret, data, macAndData, pkt->payload_len - i);' src/Mesh.cpp \
  || { echo "src/Mesh.cpp GRP_TXT receive path changed - update the receiver in this test"; exit 1; }
W=$(mktemp -d)
mkdir -p "$W/shim"; cp test/mocks/Stream.h "$W/shim/"          # Utils.h includes <Stream.h>; nothing else from test/mocks (fake AES/SHA)
python3 - "$W" <<'PY'
import sys, re
W = sys.argv[1]
src = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
mesh = open("src/Mesh.cpp").read()
def fn(text, sig):
    i = text.index(sig); j = text.index("{", i); d = 0
    for k in range(j, len(text)):
        if text[k] == "{": d += 1
        elif text[k] == "}":
            d -= 1
            if d == 0: return text[i:k + 1]
def line(pat):
    m = re.search(pat, src, re.M); assert m, pat; return m.group(0)
HEAD = r"""#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cctype>
#include <vector>
#include <Packet.h>
#include <Utils.h>
#include <SHA256.h>
#include <AES.h>
#define MESH_DEBUG_PRINTLN(...) do {} while (0)
static void yield() {}
namespace mesh {
class GroupChannel { public: uint8_t hash[PATH_HASH_SIZE]; uint8_t secret[PUB_KEY_SIZE]; };   // = src/Mesh.h
struct Clock { uint32_t t = 1790600000; uint32_t getCurrentTime() { return t; } };
struct Rng { uint32_t s = 1; int nextInt(int a, int b) { s = s * 1103515245u + 12345u; return a + (int)((s >> 8) % (uint32_t)(b - a)); } };
struct MeshStub {
  Clock clk; Rng rng; Packet pool[2048]; int used = 0;
  Clock* getRTCClock() { return &clk; } Rng* getRNG() { return &rng; }
  Packet* obtainNewPacket() { assert(used < 2048); Packet* p = &pool[used++]; memset((void*)p, 0, sizeof(*p)); return p; }
  const char* getLogDateTime() { return ""; }
  std::vector<Packet*> zero_hop; void sendZeroHop(Packet* p, uint32_t) { zero_hop.push_back(p); }   // (rc15) zero-hop searches
  Packet* createGroupDatagram(uint8_t type, const GroupChannel& channel, const uint8_t* data, size_t data_len);
};
}
using mesh::Packet;
static const uint8_t XFER_MAGIC = 0xF7, MSG_OFFER_REQ = 1, MSG_OFFER_NAK = 3, MSG_CHUNK_REQ = 4, MSG_CHUNK = 5, MSG_ABORT = 7, MSG_LIST_REQ = 8, MSG_LIST_RSP = 9, MSG_BAD_AUTH = 10, MSG_OFFER_REQ2 = 11;
static const uint32_t XFER_MAC_LEN = 8; static const uint8_t MAX_HOPS = 6, MAX_ROUTE_BYTES = 12, KIND_APP = 0, KIND_BOOTLOADER = 1;
static const uint8_t KIND_SWARM = 0x80, SWARM_BLK = 11;   // (swarm v1) the block after a LIST_RSP's route
static inline uint8_t rtSize(uint8_t e){return (e>>6)+1;} static inline uint8_t rtCount(uint8_t e){return e&63;}
static inline uint8_t rtBytes(uint8_t e){return rtCount(e)*rtSize(e);}
static inline bool rtValid(uint8_t e){return rtSize(e)<=3&&rtCount(e)<=MAX_HOPS&&rtBytes(e)<=MAX_ROUTE_BYTES;}
static inline void rtReverse(uint8_t* dst, const uint8_t* src, uint8_t enc){ uint8_t s=rtSize(enc),c=rtCount(enc); for(uint8_t i=0;i<c;i++) memcpy(dst+i*s, src+(c-1-i)*s, s); }
struct { uint8_t board[8]; } g_fwpull_tag = {{'R','A','K','3','4','0','1',0}};
struct XferManifest { char name[32]; uint32_t payload_len; uint32_t fw_version; uint8_t kind; uint8_t sha256[32]; };
struct FloodHost { virtual bool sharedSecret(uint8_t* out, const uint8_t* peer) = 0; virtual bool sendOta(Packet*, const uint8_t*, uint8_t, uint32_t) { return false; } };
"""
CLASS = r"""
class FwOtaTransfer { public:
  mesh::MeshStub* _mesh; mesh::GroupChannel _disc_ch; uint32_t _disc_sess = 0; uint16_t _disc_seq = 0;
  const uint8_t* _pub = nullptr; const char* _name_src = nullptr;
  bool _serving = true, _bl_serving = false; XferManifest _serve_man = {}, _bl_man = {};
  int16_t _rx_rssi = 0; uint8_t _duty_pct = 30;
  uint8_t activePulls() const { return 1; } uint8_t powerState() const { return 1; }   // (swarm v1) in the answer's swarm block
  uint8_t _circle[32] = {0}; bool _circle_set = false; uint8_t _list_key[16] = {0}, _offer_key[16] = {0}, _sess_mac[16] = {0}, _sess_enc[16] = {0};
  bool _sess_set = false; FloodHost* _flood_host = nullptr;
  std::vector<Packet*> floods; int direct = 0;
  struct Got { std::vector<uint8_t> b; std::vector<uint8_t> ret; uint8_t ret_enc; bool heal; }; std::vector<Got> got;
  void noteRssi(float r) { _rx_rssi = (int16_t)r; }
  bool answerOnce(uint32_t, bool) { return true; }   // (v1.17.28) once-per-session rule: tested in fw_ota_duty_test.sh
  bool imageStillServed() { return true; }
  void floodOut(Packet* p, uint32_t) { if (p) floods.push_back(p); }
  void sendListRsp(const uint8_t*, const XferManifest&, uint32_t, const uint8_t* = nullptr, uint8_t = 0, const uint8_t* = nullptr) { direct++; }
  void onListRsp(const uint8_t* b, uint32_t len, int8_t, const uint8_t* ret = nullptr, uint8_t ret_enc = 0, bool heal = false) {
    Got g; g.b.assign(b, b + len); if (ret) g.ret.assign(ret, ret + rtBytes(ret_enc)); g.ret_enc = ret_enc; g.heal = heal; got.push_back(g); }
  void circleKeys(); void macFillKey(uint8_t* pkt, uint32_t len, const uint8_t* key) const;
  bool sessionKeys(const uint8_t* peer_pub, uint32_t sess, const uint8_t* np, const uint8_t* ns, uint8_t* kmac, uint8_t* kenc);
  const uint8_t* keyForSend(uint8_t type, uint8_t* tmp) const;
  void macFill(uint8_t* pkt, uint32_t len) const;
  bool macOk(const uint8_t* key, const uint8_t* pkt, uint32_t len) const;
  uint32_t buildListRsp(uint8_t* r, const uint8_t* sess_seq, const XferManifest& m, const uint8_t* fwd, uint8_t enc) const;
  void floodListRsp(const uint8_t* r, uint32_t n, uint32_t delay_ms, bool heal, bool ann = false);
  void sendDiscovery(const char* target, bool zero_hop = false);
  struct { uint32_t searches_zero = 0, searches_flood = 0, answers_sent = 0; } stats;   // (rc15)
  void answerHeard(const uint8_t*, uint32_t) {}                                           // (rc15) suppression: tested in the simulator
  void answerLater(const uint8_t* r, uint32_t n, uint32_t d) { floodListRsp(r, n, d, false); }
  static const uint32_t ANS_HOP_MS = 1500;
  void onClaim(const uint8_t*, uint32_t) {}                                                // (study C/D) election claims: simulator
  void onTime(const uint8_t*, uint32_t, uint8_t) {}                                        // (rc21, C57) swarm time: simulator (H15)
  void onDiscovery(Packet* pkt, const uint8_t* data, size_t len);
};
"""
TAIL = r"""
// the receive side of src/Mesh.cpp (GRP_TXT): channel hash, then MACThenDecrypt -> the handler gets the decrypted, PADDED data
static bool deliver(FwOtaTransfer& rx, Packet* p) {
  if (p->payload[0] != rx._disc_ch.hash[0]) return false;
  uint8_t data[MAX_PACKET_PAYLOAD];
  int len = mesh::Utils::MACThenDecrypt(rx._disc_ch.secret, data, &p->payload[1], p->payload_len - 1);
  if (len <= 0) return false;
  rx.onDiscovery(p, data, (size_t)len);
  return true;
}
static void relayPath(Packet* p, uint8_t hs, uint8_t hops, uint8_t seed) {   // the relays a flood passed: MeshCore appends one hash each
  p->setPathHashSizeAndCount(hs, hops); for (int i = 0; i < hops * hs; i++) p->path[i] = (uint8_t)(seed + 17 * i);
}
static void initNode(FwOtaTransfer& t, mesh::MeshStub* m, const uint8_t* pub, const char* name, const char* phrase = "owner circle phrase") {
  t._mesh = m; t._pub = pub; t._name_src = name;
  static const char salt[] = "LoRa-OTA circle v1";
  pbkdf2(phrase, (const uint8_t*)salt, sizeof(salt) - 1, 1000, t._circle);   // (the node uses 50,000 rounds; the key math is the same)
  t._circle_set = true; t.circleKeys();
  strcpy(t._serve_man.name, "fw-250"); t._serve_man.fw_version = 250; t._serve_man.payload_len = 250000; t._serve_man.kind = KIND_APP;
  strcpy(t._bl_man.name, "bl-4"); t._bl_man.fw_version = 4; t._bl_man.payload_len = 39000; t._bl_man.kind = KIND_BOOTLOADER;
}
int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  int checks = 0; bool rem[16] = {false};
  static mesh::MeshStub air;
  static const uint8_t PUB_S[32] = {0x1F,0x2E,0x3D,0x4C,1,2,3}, PUB_P[32] = {0x5A,0x6B,0x7C,0x00,9,8,7};
  // 1) flooded answers and heal notices: every valid route encoding, app and bootloader offers, relays on the way back
  for (int kind = 0; kind < 2; kind++) for (int heal = 0; heal < 2; heal++) for (int hs = 1; hs <= 3; hs++) for (int hops = 0; hops <= MAX_HOPS; hops++) {
    uint8_t enc = (uint8_t)(((hs - 1) << 6) | hops); if (!rtValid(enc)) continue;
    FwOtaTransfer srv, pul; initNode(srv, &air, PUB_S, "PERSIST-TEST-BOARD1"); initNode(pul, &air, PUB_P, "RAK3401-BOARD2");
    uint8_t sess_seq[6] = {0x44,0x33,0x22,0x11,0x07,0x00}, fwd[MAX_ROUTE_BYTES]; for (int i = 0; i < rtBytes(enc); i++) fwd[i] = (uint8_t)(0xB0 + i);
    uint8_t r[128]; uint32_t n = srv.buildListRsp(r, sess_seq, kind ? srv._bl_man : srv._serve_man, fwd, enc);
    srv.floodListRsp(r, n, 0, heal); assert(srv.floods.size() == 1); checks++;
    Packet* p = srv.floods[0]; rem[(11 + n) % 16] = true;
    uint8_t rhs = (uint8_t)(1 + (hops % 3)), rhops = (uint8_t)(hops % 4); relayPath(p, rhs, rhops, 0x0C);
    assert(deliver(pul, p)); checks++;
    if (pul.got.size() != 1) { printf("kind %d heal %d route %d x %d B: the answer (%u B, %u B on the air) did not reach onListRsp\n", kind, heal, hops, hs, n, 11 + n); return 1; }
    assert(pul.got[0].b.size() == n && !memcmp(pul.got[0].b.data(), r, n)); checks++;                  // exactly the bytes the server built
    assert(pul.got[0].heal == (bool)heal && pul.got[0].ret_enc == p->path_len && pul.got[0].ret.size() == (size_t)rhops * rhs); checks++;
  }
  // 2) discovery requests of every target length -> the server answers with a flood -> the answer is back at the puller, same session
  for (int tl = 1; tl <= 30; tl++) {
    FwOtaTransfer srv, pul; char name[40]; for (int i = 0; i < tl; i++) name[i] = (char)('a' + i % 26); name[tl] = 0;
    initNode(srv, &air, PUB_S, name); initNode(pul, &air, PUB_P, "RAK3401-BOARD2");
    pul._disc_sess = 0x1234ABCD + tl; pul.sendDiscovery(name);
    if (21 + tl >= 48) { assert(pul.floods.empty()); checks++; continue; }   // "LOTA1 SSSSSSSSQQQQ <target> F" too long for the request buffer: not sent
    assert(pul.floods.size() == 1); checks++;
    rem[(5 + 21 + tl) % 16] = true;                                    // this request's plaintext length mod the AES block
    Packet* q = pul.floods[0]; relayPath(q, 2, 1 + tl % 3, 0x50);
    assert(deliver(srv, q)); checks++;
    assert(srv.floods.size() == 1 && srv.direct == 0); checks++;                                          // " F": answered by a flood
    Packet* a = srv.floods[0]; relayPath(a, 3, tl % 4, 0x77);
    assert(deliver(pul, a) && pul.got.size() == 1); checks++;
    uint32_t s; memcpy(&s, pul.got[0].b.data() + 2, 4); assert(s == pul._disc_sess); checks++;
    uint8_t renc = pul.got[0].b[45]; assert(renc == q->path_len && !memcmp(pul.got[0].b.data() + 46, q->path, rtBytes(renc))); checks++;   // the way there it carries
  }
  { FwOtaTransfer srv, pul; initNode(srv, &air, PUB_S, "PERSIST-TEST-BOARD1"); initNode(pul, &air, PUB_P, "B2");   // "@ID8" and another node's id
    pul._disc_sess = 0x55; pul.sendDiscovery("@1F2E3D4C"); assert(deliver(srv, pul.floods[0]) && srv.floods.size() == 1); checks++;
    pul.sendDiscovery("@1F2E3D4D"); assert(deliver(srv, pul.floods[1]) && srv.floods.size() == 1); checks++; }
  { // (swarm v1) an IMAGE search ("=<16 hex of its SHA-256>") through the real encryption: the holder answers with its swarm block; another
    // image is not answered; an announcement (session 0, marker LOTA1A) arrives like an answer
    FwOtaTransfer srv, pul; initNode(srv, &air, PUB_S, "PERSIST-TEST-BOARD1"); initNode(pul, &air, PUB_P, "B2");
    for (int k = 0; k < 32; k++) srv._serve_man.sha256[k] = (uint8_t)(0xA1 + k);
    char t[20] = "="; for (int k = 0; k < 8; k++) snprintf(t + 1 + 2 * k, 3, "%02X", srv._serve_man.sha256[k]);
    pul._disc_sess = 0x66; pul.sendDiscovery(t); Packet* q = pul.floods[0]; relayPath(q, 2, 1, 0x50);
    assert(deliver(srv, q) && srv.floods.size() == 1); checks++;
    Packet* a = srv.floods[0]; relayPath(a, 2, 1, 0x77); assert(deliver(pul, a) && pul.got.size() == 1); checks++;
    const std::vector<uint8_t>& g = pul.got[0].b; const uint32_t blk = 46 + rtBytes(g[45]);
    assert((g[36] & KIND_SWARM) && g.size() == blk + SWARM_BLK + XFER_MAC_LEN && !memcmp(g.data() + blk, srv._serve_man.sha256, 8) && g[blk + 8] == 30 && g[blk + 9] == 1 && g[blk + 10] == 1); checks++;
    t[16] = t[16] == '0' ? '1' : '0'; pul.sendDiscovery(t); assert(deliver(srv, pul.floods[1]) && srv.floods.size() == 1); checks++;   // not this image: silent
    uint8_t zs[6] = {0}, r[128]; uint32_t n = srv.buildListRsp(r, zs, srv._serve_man, nullptr, 0); srv.floodListRsp(r, n, 0, false, true);
    Packet* an = srv.floods.back(); relayPath(an, 3, 2, 0x31); assert(deliver(pul, an) && pul.got.size() == 2); checks++;
    uint32_t s0; memcpy(&s0, pul.got[1].b.data() + 2, 4); assert(s0 == 0 && !pul.got[1].heal); checks++; }
  // 3) one flipped byte anywhere in the encrypted payload: nothing (or at least nothing wrong) reaches onListRsp
  { int rejected = 0, total = 0;
    FwOtaTransfer srv; initNode(srv, &air, PUB_S, "PERSIST-TEST-BOARD1");
    uint8_t ss[6] = {1,2,3,4,5,6}, fwd[6] = {1,2,3,4,5,6}; uint8_t r[128]; uint32_t n = srv.buildListRsp(r, ss, srv._serve_man, fwd, 0x82);
    srv.floodListRsp(r, n, 0, false); Packet* p0 = srv.floods[0];
    for (int pos = 1; pos < p0->payload_len; pos++) for (int bit = 0; bit < 8; bit++) {
      FwOtaTransfer pul; initNode(pul, &air, PUB_P, "B2"); Packet p = *p0; p.payload[pos] ^= (uint8_t)(1 << bit); total++;
      deliver(pul, &p);
      if (pul.got.empty()) rejected++; else { assert(pul.got[0].b.size() == n && !memcmp(pul.got[0].b.data(), r, n)); }
    }
    assert(rejected == total); checks++;
    printf("  corruption: %d/%d single-bit flips rejected\n", rejected, total); }
  // 4) (v1.17.28 security v2) PBKDF2-HMAC-SHA256 test vectors (RFC 6070 inputs, SHA-256 results)
  { struct { uint32_t c; const char* hex; } V[] = { {1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
                                                    {2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
                                                    {4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"} };
    for (auto& v : V) { uint8_t o[32]; pbkdf2("password", (const uint8_t*)"salt", 4, v.c, o); char h[65]; for (int i = 0; i < 32; i++) sprintf(h + 2*i, "%02x", o[i]);
      if (strcmp(h, v.hex)) { printf("PBKDF2 c=%u: %s\n", v.c, h); return 1; } checks++; } }
  // 5) session keys: both ends derive the same keys; any other nonce, circle or peer gives others (stub ECDH = symmetric hash of both keys)
  { struct StubHost : FloodHost { const uint8_t* me; bool sharedSecret(uint8_t* out, const uint8_t* peer) override {
        uint8_t m[64]; bool lo = memcmp(me, peer, 32) < 0; memcpy(m, lo ? me : peer, 32); memcpy(m + 32, lo ? peer : me, 32); mesh::Utils::sha256(out, 32, m, 64); return true; } };
    FwOtaTransfer a, b, c; StubHost ha, hb, hc; ha.me = PUB_P; hb.me = PUB_S; hc.me = PUB_S;
    initNode(a, &air, PUB_P, "B2"); initNode(b, &air, PUB_S, "B1"); initNode(c, &air, PUB_S, "B1", "another circle phrase");
    a._flood_host = &ha; b._flood_host = &hb; c._flood_host = &hc;
    uint8_t np[8] = {1,2,3,4,5,6,7,8}, ns[8] = {9,9,9,9,1,1,1,1}, ns2[8] = {9,9,9,9,1,1,1,2}, am[16], ae[16], bm[16], be[16], xm[16], xe[16];
    assert(a.sessionKeys(PUB_S, 0x1234, np, ns, am, ae) && b.sessionKeys(PUB_P, 0x1234, np, ns, bm, be)); checks++;
    assert(!memcmp(am, bm, 16) && !memcmp(ae, be, 16) && memcmp(am, ae, 16)); checks++;                     // same keys at both ends
    b.sessionKeys(PUB_P, 0x1234, np, ns2, xm, xe); assert(memcmp(am, xm, 16)); checks++;                      // another nonce (a replay) -> other key
    b.sessionKeys(PUB_P, 0x1235, np, ns, xm, xe); assert(memcmp(am, xm, 16)); checks++;                       // another session
    c.sessionKeys(PUB_P, 0x1234, np, ns, xm, xe); assert(memcmp(am, xm, 16)); checks++;                       // another circle
    // the circle keys differ, the discovery channel differs: a node of another circle neither reads nor answers
    assert(memcmp(a._list_key, c._list_key, 16) && memcmp(a._offer_key, c._offer_key, 16) && memcmp(a._disc_ch.secret, c._disc_ch.secret, 16)); checks++;
    a._disc_sess = 0x77; a.sendDiscovery("*"); Packet* q = a.floods.back(); assert(!deliver(c, q) && c.floods.empty()); checks++;
    // AES-128-CTR: round trip, length kept, key- and index-dependent
    uint8_t d[160], d0[160]; for (int i = 0; i < 160; i++) d0[i] = d[i] = (uint8_t)(i * 7);
    ctrXor(ae, 0x1234, 17, d, 160); assert(memcmp(d, d0, 160)); ctrXor(be, 0x1234, 17, d, 160); assert(!memcmp(d, d0, 160)); checks++;
    ctrXor(ae, 0x1234, 17, d, 160); uint8_t e[160]; memcpy(e, d0, 160); ctrXor(ae, 0x1234, 18, e, 160); assert(memcmp(d, e, 160)); checks++;
    printf("  security v2: PBKDF2 vectors, session keys (both ends / nonce / session / circle), other circle hidden, AES-CTR: ok\n"); }
  int nrem = 0; for (int i = 0; i < 16; i++) nrem += rem[i];
  printf("  AES padding remainders covered (answers + requests): %d of 16\n", nrem);
  assert(nrem == 16); checks++;
  printf("LoRa-OTA channel payloads through the real MeshCore encryption: %d passed\n", checks);
  return 0;
}
"""
cg = fn(mesh, "Packet* Mesh::createGroupDatagram(").replace("Packet* Mesh::createGroupDatagram(", "Packet* MeshStub::createGroupDatagram(", 1)
statics = "\n".join([fn(src, "static uint32_t listRspLen("), line(r"^static bool isListMsg\(.*$"),
                     fn(src, "static void hmac("), fn(src, "static void pbkdf2Start("), fn(src, "static uint32_t pbkdf2Steps("), fn(src, "static void pbkdf2("), fn(src, "static void ctrXor("),
                     line(r'^static const char\s+FLOOD_RSP\[\].*$'), line(r'^static const char\s+FLOOD_HEAL\[\].*$'), line(r'^static const char\s+FLOOD_ANN\[\].*$'), line(r'^static const char\s+FLOOD_CLAIM\[\].*$'), line(r'^static const char\s+FLOOD_TREQ\[\].*$'), line(r'^static const char\s+FLOOD_TANS\[\].*$'), fn(src, "static bool hexN("),
                     line(r'^static const uint32_t LIST_RSP_MAX = .*$'),
                     line(r'^static const uint32_t DISC_RSP_MIN_MS .*$'), line(r'^static const uint32_t DISC_RSP_SPREAD_MS .*$'),
                     fn(src, "static uint8_t discAnswer(")])
members = "\n".join(fn(src, s) for s in ("const uint8_t* FwOtaTransfer::keyForSend(", "void FwOtaTransfer::macFill(",
                                          "void FwOtaTransfer::macFillKey(", "void FwOtaTransfer::circleKeys()", "bool FwOtaTransfer::sessionKeys(",
                                          "bool FwOtaTransfer::macOk(", "uint32_t FwOtaTransfer::buildListRsp(", "void FwOtaTransfer::floodListRsp(",
                                          "void FwOtaTransfer::sendDiscovery(", "void FwOtaTransfer::onDiscovery("))
out = HEAD + "namespace mesh {\n" + cg + "\n}\n" + statics + CLASS + members + TAIL
open(W + "/crypto_test.cpp", "w").write(out)
# the mutant: v1.17.23's receiver (MAC over the padded length)
bug = "uint32_t bl = listRspLen(b, (uint32_t)len - 11);"
assert out.count(bug) == 1, "onDiscovery's padding cut moved - update the mutant"
open(W + "/crypto_mutant.cpp", "w").write(out.replace(bug, "uint32_t bl = (uint32_t)len - 11;"))
PY
CR=test/crypto
build() { g++ -std=c++17 -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=all -I"$W/shim" -I$CR -Isrc -o "$2" "$1" src/Utils.cpp src/Packet.cpp \
          $CR/AES128.cpp $CR/AESCommon.cpp $CR/BlockCipher.cpp $CR/Crypto.cpp $CR/Hash.cpp $CR/SHA256.cpp; }
build "$W/crypto_test.cpp" "$W/crypto_test"
"$W/crypto_test"
build "$W/crypto_mutant.cpp" "$W/crypto_mutant"
if "$W/crypto_mutant" > "$W/mutant.log" 2>&1; then echo "MUTANT SURVIVED: the v1.17.23 padding bug was NOT caught"; rm -rf "$W"; exit 1; fi
echo "mutant (v1.17.23 padding bug put back) caught: $(grep -m1 'did not reach' "$W/mutant.log" || tail -1 "$W/mutant.log")"
rm -rf "$W"

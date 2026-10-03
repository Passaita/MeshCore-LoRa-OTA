#!/bin/bash
# Host test of the LoRa-OTA route table (auto routing) and short-id collision logic. The functions are EXTRACTED from the real
# src/helpers/fw_ota/FwOtaTransfer.cpp on every run (not copies), compiled with ASan/UBSan and exercised with scripted scenarios.
# usage: test/fw_ota_route_test.sh
set -e
cd "$(dirname "$0")/.."
W=$(mktemp -d)
# (v1.17.28) fw_ota parses typed ids with its own validating hexN(); MeshCore's Utils::fromHex() accepts any letters ("fw pull status" -> 00A000)
python3 -c 'import re,sys; c=[l.split("//")[0] for l in open("src/helpers/fw_ota/FwOtaTransfer.cpp")]; b=[i+1 for i,l in enumerate(c) if "Utils::fromHex" in l]; b and sys.exit(f"FAIL: Utils::fromHex in FwOtaTransfer.cpp code at lines {b} - use hexN()")'
python3 - "$W/route_test.cpp" <<'PY'
import sys
HEAD = r"""#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <cctype>
#include <cstdlib>
enum { PH_IDLE = 0, PH_OFFERING, PH_DOWNLOADING };
static uint32_t g_ms = 1000; static uint32_t millis() { return g_ms; }
static const uint8_t MAX_HOPS = 6, MAX_ROUTE_BYTES = 12, MAX_PEERS = 8, KIND_APP = 0, KIND_BOOTLOADER = 1;
static inline uint8_t rtSize(uint8_t e){return (e>>6)+1;} static inline uint8_t rtCount(uint8_t e){return e&63;}
static inline uint8_t rtBytes(uint8_t e){return rtCount(e)*rtSize(e);}
static inline bool rtValid(uint8_t e){return rtSize(e)<=3&&rtCount(e)<=MAX_HOPS&&rtBytes(e)<=MAX_ROUTE_BYTES;}
static inline void rtReverse(uint8_t* dst, const uint8_t* src, uint8_t enc){ uint8_t s=rtSize(enc),c=rtCount(enc); for(uint8_t i=0;i<c;i++) memcpy(dst+i*s, src+(c-1-i)*s, s); }
static const uint32_t XFER_MAC_LEN = 8; static const uint8_t XFER_MAGIC = 0xF7, MSG_LIST_RSP = 9;
static const uint8_t HEAL_REPS = 3; static const uint32_t HEAL_NOPROG_MS = 3600000UL; static const uint8_t HEAL_MAX = 4, FLOOD_BUDGET_N = 4; static const uint32_t FLOOD_BUDGET_MS = 3600000UL;

struct { uint8_t board[8]; } g_fwpull_tag = {{'R','A','K','3','4','0','1',0}};
struct PeerInfo { uint8_t id[4]; char name[17]; uint32_t fw_version, payload_len; int16_t rssi; int8_t snr4; uint8_t kind; uint8_t route[MAX_ROUTE_BYTES]; uint8_t route_len;
                  uint8_t ret[MAX_ROUTE_BYTES]; uint8_t ret_len; bool has_ret; bool used; };
struct XferManifest { char name[32]; uint8_t kind; };
struct Rng { int nextInt(int a, int b) { return a; } } g_rng; struct MeshStub { Rng* getRNG() { return &g_rng; } } g_mesh;
class FwOtaTransfer { public:
  struct RouteRec { uint8_t path[MAX_ROUTE_BYTES]; uint8_t enc; uint8_t ret[MAX_ROUTE_BYTES]; uint8_t ret_enc; bool has_ret; bool used; uint16_t ok, fail; uint16_t rtt_ms; uint32_t last_ok_ms; float weight; };
  static const uint8_t MAX_ROUTES = 4;
  RouteRec _routes[MAX_ROUTES] = {}; int8_t _rt_cur = -1; int8_t _rt_recent[2] = {-1,-1}; uint8_t _rt_fail_run = 0;
  uint8_t _route[MAX_ROUTE_BYTES] = {}; uint8_t _route_len = 0; int saves = 0; PeerInfo _peers[MAX_PEERS] = {};
  uint8_t _ret[MAX_ROUTE_BYTES] = {}; uint8_t _ret_len = 0; bool _ret_set = false, _ret_typed = false;
  bool _route_manual = false; int _phase = 0; uint32_t _last_req_ms = 0, _retry_timeout_ms = 0;
  bool _scan_active = false; uint32_t _scan_end_ms = 0, _scan_sess = 0, _disc_sess = 0, _session = 0; int16_t _rx_rssi = 0; char _last_err[48] = {0};
  XferManifest _man = {}; uint8_t _pull_id[4] = {0}; bool _pull_by_id = false;
  struct HealRec { uint32_t sess; uint32_t key; uint8_t who[4]; uint8_t reps; uint8_t heals; uint16_t top; uint32_t last_ms; };
  static const uint8_t HEAL_N = 4; HealRec _heal[HEAL_N] = {}; uint8_t _heal_wr = 0; uint32_t _heals_sent = 0; int floods = 0; MeshStub* _mesh = &g_mesh;
  uint32_t _flood_ts[4] = {0, 0, 0, 0}; bool floodBudget(uint32_t now); bool _typed_failed = false;
  uint32_t _last_ans_ms = 0; void routeDropDirect(int keep);   // (rc4, S3/S4)
  bool cmdRoute(const char* arg, char* reply); bool cmdPeers(const char* arg, char* reply);
  void saveState() { saves++; }
  void routeReset(); int routeAdd(const uint8_t*, uint8_t, const uint8_t* = nullptr, uint8_t = 0); void routeUse(int); float routeScore(int) const; void routeResult(bool, uint32_t); bool routeFailover();
  int matchPeers(const uint8_t*, uint8_t, bool, int*, char*, size_t) const;
  void setRet(const uint8_t*, uint8_t, bool); uint32_t addReturnRoute(uint8_t*, uint32_t) const;
  void onListRsp(const uint8_t*, uint32_t, int8_t, const uint8_t* = nullptr, uint8_t = 0, bool = false);
  void noteReq(const uint8_t*, bool, uint16_t, const XferManifest&, const uint8_t* = nullptr);
  uint32_t buildListRsp(uint8_t* r, const uint8_t*, const XferManifest&, const uint8_t*, uint8_t) const { r[0]=XFER_MAGIC; return 16; }
  void floodListRsp(const uint8_t*, uint32_t, uint32_t, bool heal) { if (heal) floods++; }
  bool pullTargetId(uint8_t id[4]) const { if (_pull_by_id) memcpy(id, _pull_id, 4); return _pull_by_id; }
};
static const uint32_t LIST_RSP_MAX = 80;
"""
TAIL = r"""static uint8_t E(int hs, int hops){ return (uint8_t)(((hs-1)<<6)|hops); }
int main(){
  int checks=0; FwOtaTransfer t;
  // 1) a new pull: the table holds just the starting route (direct)
  t.routeReset(); assert(t._rt_cur==0 && t._routes[0].used && t._routes[0].enc==0); checks++;
  uint8_t A[6]={0xA1,0xB2,0xC3,0xD4,0xE5,0xF6}, B[3]={0x11,0x22,0x33}, C[6]={0x44,0x55,0x66,0x77,0x88,0x99}, D[3]={0xDD,0xEE,0xFF}, F[3]={1,2,3};
  int a=t.routeAdd(A,E(3,2)), b=t.routeAdd(B,E(3,1));
  assert(a==1 && b==2 && t.routeAdd(A,E(3,2))==a); checks++;          // same path = same record
  assert(t.routeAdd(A,E(3,7))==-1); checks++;                             // invalid encoding refused
  // 2) the direct route keeps failing -> failover picks the best other one, never the current, avoiding the last two tried
  for(int i=0;i<3;i++) t.routeResult(false,0);
  assert(t.routeFailover() && t._rt_cur!=0); int first=t._rt_cur; checks++;
  assert(!memcmp(t._route, t._routes[first].path, rtBytes(t._routes[first].enc)) && t._route_len==t._routes[first].enc); checks++;   // _route follows
  t.routeResult(false,0); t.routeResult(false,0);
  assert(t.routeFailover() && t._rt_cur!=first && t._rt_cur!=0); checks++;   // diversity: not back to direct (tried) or the one just failed
  // 3) a route that answers ranks above failing ones; its reply time is smoothed
  int good=t._rt_cur; for(int i=0;i<5;i++){ g_ms+=1000; t.routeResult(true,400); }
  assert(t._routes[good].ok==5 && t._routes[good].rtt_ms>0 && t._routes[good].rtt_ms<=400); checks++;
  for(int i=0;i<4;i++) if(i!=good && t._routes[i].used) assert(t.routeScore(good) > t.routeScore(i)); checks++;
  // 4) table full: a new route replaces the WEAKEST, never the one in use
  int c=t.routeAdd(C,E(3,2)); assert(c>=0 && c!=good && t._routes[good].used); checks++;
  int used=0; for(int i=0;i<4;i++) used+=t._routes[i].used; assert(used==4); checks++;
  int d=t.routeAdd(D,E(3,1)); assert(d>=0 && d!=good && t._routes[good].used && t._rt_cur==good); checks++;
  // 5) weight stays in 0.1 .. 5
  for(int i=0;i<30;i++) t.routeResult(true,300); assert(t._routes[good].weight<=5.0f); checks++;
  for(int i=0;i<30;i++) t.routeResult(false,0); assert(t._routes[good].weight>=0.1f); checks++;
  // 6) only one route: no failover
  FwOtaTransfer u; u.routeReset(); u.routeResult(false,0); assert(!u.routeFailover() && u._rt_cur==0); checks++;
  // 7) collisions: 2-byte prefix shared by two app servers -> 2; 3-byte prefix unique -> 1; kind is respected
  FwOtaTransfer p;
  uint8_t ids[4][4]={{0xAB,0xCD,0x01,0x02},{0xAB,0xCD,0x99,0x03},{0xAB,0xCD,0x01,0x77},{0x12,0x34,0x56,0x78}};
  const char* nm[4]={"Pager","Board-1","Board-BL","Other"}; uint8_t kd[4]={KIND_APP,KIND_APP,KIND_BOOTLOADER,KIND_APP};
  for(int i=0;i<4;i++){ p._peers[i].used=true; memcpy(p._peers[i].id,ids[i],4); strcpy(p._peers[i].name,nm[i]); p._peers[i].kind=kd[i]; }
  int f=-1; char list[90]; uint8_t q2[2]={0xAB,0xCD}, q3[3]={0xAB,0xCD,0x01}, qn[2]={0xFE,0xED};
  assert(p.matchPeers(q2,2,false,&f,list,sizeof list)==2 && strstr(list,"ABCD0102") && strstr(list,"ABCD9903")); checks++;
  printf("  collision list: %s\n", list);
  assert(p.matchPeers(q3,3,false,&f,list,sizeof list)==1 && f==0); checks++;
  assert(p.matchPeers(q3,3,true,&f,list,sizeof list)==1 && f==2); checks++;         // the bootloader offer is a different kind
  assert(p.matchPeers(qn,2,false,&f,list,sizeof list)==0); checks++;
  // 8) typed routes (the parser used mesh::Utils::fromHex, which failed every hop but the last: "fw route B5,A2" -> usage error, 2026-09-26)
  { FwOtaTransfer r; r.routeReset(); char rep[160];
    struct { const char* in; int hops, hs; } ok[] = { {"3C,92",2,1}, {"3C4D,92A3",2,2}, {"3C4D5E,92A3B4",2,3}, {"3c",1,1}, {"AA,BB,CC,DD,EE,FF",6,1}, {"A1B2,C3D4,E5F6",3,2} };
    for (auto& t : ok) { r.cmdRoute(t.in, rep); assert(strstr(rep, "TYPED") && r._route_manual && rtCount(r._route_len) == t.hops && rtSize(r._route_len) == t.hs); checks++; }
    r.cmdRoute("3C4D5E,92A3B4", rep); assert(r._route[0]==0x3C && r._route[1]==0x4D && r._route[2]==0x5E && r._route[3]==0x92 && r._route[4]==0xA3 && r._route[5]==0xB4); checks++;
    printf("  typed route shown as: %s\n", rep);
    const char* bad[] = { "3C,92A3", "3C,", "3C,,92", "AA,BB,CC,DD,EE,FF,11", "A1B2C3,D4E5F6,778899,AABBCC,DDEEFF", "B", "B5x", "G5" };
    for (auto b : bad) { r._route_manual = false; r.cmdRoute(b, rep); assert(strncmp(rep, "ERR", 3) == 0); checks++; }
    r.cmdRoute("auto", rep); assert(!r._route_manual); checks++; }
  // 9) (v1.17.23) route PAIRS: the same way there with and without a known way back are two records; the one in use sets what requests carry
  { FwOtaTransfer r; r.routeReset(); uint8_t F2[6]={0x3C,0x4D,0x5E,0x6F,0x70,0x81}, R2[6]={0x6F,0x70,0x81,0x3C,0x4D,0x5E}, W[3]={0x3C,0x4D,0x5E}, b[40];
    int x=r.routeAdd(W,E(3,1)), y=r.routeAdd(W,E(3,1),F2,E(3,2)); assert(x>=0 && y>=0 && x!=y && r.routeAdd(W,E(3,1),F2,E(3,2))==y); checks++;
    r.routeUse(x); uint32_t n=r.addReturnRoute(b,0); assert(n==4 && b[0]==E(3,1) && b[1]==0x3C && !r._ret_set); checks++;          // no way back known: the reverse
    r.routeUse(y); n=r.addReturnRoute(b,0); assert(n==7 && b[0]==E(3,2) && !memcmp(b+1,F2,6) && r._ret_set); checks++;              // the learnt way back, as is
    int z=r.routeAdd(F2,E(3,2)); r.routeUse(z); n=r.addReturnRoute(b,0); assert(n==7 && !memcmp(b+1,R2,6)); checks++;              // reverse of a 2-hop route
    assert(r.routeAdd(W,E(3,1),F2,E(3,7))==-1); checks++;                                                                               // invalid way back refused
    char rep[160]; r.cmdRoute("3C4D5E,6F7081/6F7081,3C4D5E", rep); assert(r._route_manual && r._ret_typed && r._ret_set && rtCount(r._ret_len)==2); checks++;
    printf("  typed pair shown as: %s\n", rep); assert(strstr(rep, "3C4D5E,6F7081/6F7081,3C4D5E")); checks++;
    r.cmdRoute("3C4D5E/direct", rep); assert(r._ret_set && r._ret_len==0 && r._ret_typed); checks++;
    r.cmdRoute("3C4D5E", rep); assert(!r._ret_set && !r._ret_typed); checks++;                                                          // a plain typed route: reverse again
    const char* bad[] = { "3C/", "/3C", "3C/92,", "3C/92A3,3C", "3C//92", "3C/directx" };
    for (auto s : bad) { r.cmdRoute(s, rep); assert(!strncmp(rep, "ERR", 3)); checks++; }
    r.cmdRoute("off", rep); assert(!r._ret_set && r._route_len==0); checks++; }
  // 9b) (rc4, S3 + S4) a discovery answer from behind relays drops the unanswered "direct" pair: no zero-hop offers only neighbours hear
  { FwOtaTransfer r; r.routeReset(); r._phase=PH_OFFERING; r._disc_sess=0x77; r._man.kind=KIND_APP; r._pull_by_id=true; uint8_t sid[4]={0xA7,0x68,0x3B,0x7E}; memcpy(r._pull_id,sid,4);
    int direct0 = 0; for (int i=0;i<4;i++) if (r._routes[i].used && !rtCount(r._routes[i].enc)) direct0++; assert(direct0 == 1 && r._last_ans_ms == 0); checks++;
    uint8_t b[64]={0}; b[0]=XFER_MAGIC; b[1]=MSG_LIST_RSP; uint32_t ds=0x77; memcpy(b+2,&ds,4); memcpy(b+8,sid,4); b[36]=KIND_APP; memcpy(b+37,g_fwpull_tag.board,8);
    FwOtaTransfer z = r; b[45]=0; z.onListRsp(b,46+XFER_MAC_LEN,0); int dz=0; for (int i=0;i<4;i++) if (z._routes[i].used && !rtCount(z._routes[i].enc)) dz++;
    assert(dz == 1 && z._last_ans_ms != 0); checks++;                                                        // the server answered directly: direct stays
    b[45]=E(2,1); b[46]=0x3C; b[47]=0x4D; r.onListRsp(b,46+2+XFER_MAC_LEN,0);
    int direct1 = 0; for (int i=0;i<4;i++) if (r._routes[i].used && !rtCount(r._routes[i].enc)) direct1++;
    assert(direct1 == 0 && rtCount(r._route_len) == 1 && r._route[0] == 0x3C && r._last_ans_ms != 0); checks++;
    r.routeResult(false, 0); r.routeResult(false, 0); r.routeFailover(); assert(rtCount(r._route_len) == 1); checks++;   // failing: no fall back to zero-hop
    FwOtaTransfer m; m.routeReset(); m._phase=PH_OFFERING; m._disc_sess=0x77; m._man.kind=KIND_APP; m._pull_by_id=true; memcpy(m._pull_id,sid,4);
    char rep[160]; m.cmdRoute("92A3B4", rep); m.onListRsp(b,46+2+XFER_MAC_LEN,0); assert(m._route_manual && m._route[0]==0x92); checks++; }   // typed: untouched
  // 10) (v1.17.23) heal: a flooded notice for OUR session sets the way back (not a typed one; not another session / server)
  { FwOtaTransfer r; r.routeReset(); r._phase=PH_OFFERING; r._session=0x11223344; r._man.kind=KIND_APP; r._pull_by_id=true; uint8_t sid[4]={0xA7,0x68,0x3B,0x7E}; memcpy(r._pull_id,sid,4);
    uint8_t b[64]={0}; b[0]=XFER_MAGIC; b[1]=MSG_LIST_RSP; uint32_t ss=0x11223344; memcpy(b+2,&ss,4); memcpy(b+8,sid,4); b[36]=KIND_APP; memcpy(b+37,g_fwpull_tag.board,8); b[45]=0;
    uint8_t back[6]={0x6F,0x70,0x81,0x3C,0x4D,0x5E}; uint32_t len=46+XFER_MAC_LEN;
    r.onListRsp(b,len,0,back,E(3,2),true); assert(r._ret_set && r._ret_len==E(3,2) && !memcmp(r._ret,back,6) && r._routes[r._rt_cur].has_ret && strstr(r._last_err,"healed")); checks++;
    FwOtaTransfer o = r; uint32_t other=0x55; memcpy(b+2,&other,4); uint8_t back2[3]={1,2,3}; o.onListRsp(b,len,0,back2,E(3,1),true); assert(o._ret_len==E(3,2)); checks++;   // other session
    memcpy(b+2,&ss,4); b[8]=0x99; o.onListRsp(b,len,0,back2,E(3,1),true); assert(o._ret_len==E(3,2)); checks++; b[8]=0xA7;                        // other server
    char rep[160]; o.cmdRoute("3C4D5E/3C4D5E", rep); o.onListRsp(b,len,0,back,E(3,2),true); assert(o._ret_typed && o._ret_len==E(3,1)); checks++;  // typed way back kept while it works
    { FwOtaTransfer q = o; q._typed_failed = true; q.onListRsp(b,len,0,back,E(3,2),true);                                                           // (v1.17.26, N1) typed route failed
      assert(!q._ret_typed && q._ret_set && q._ret_len==E(3,2) && !memcmp(q._ret,back,6) && strstr(q._last_err,"typed route failed")); checks++;
      int typed_kept = 0; for (int i=0;i<4;i++) if (q._routes[i].used && q._routes[i].has_ret && q._routes[i].ret_enc==E(3,1)) typed_kept++;
      assert(typed_kept == 1 && q._routes[q._rt_cur].ret_enc==E(3,2)); checks++;                                                                    // the typed pair stays in the table
      printf("  heal after the typed route failed: %s\n", q._last_err); }
    { // a re-discovery answer: ignored while the typed route works, taken (and automatic from then on) once it failed
      FwOtaTransfer d = o; d._phase = PH_DOWNLOADING; d._disc_sess = 0x77; uint8_t rb[64]; memcpy(rb, b, 46 + XFER_MAC_LEN); uint32_t ds = 0x77; memcpy(rb+2,&ds,4);
      rb[45]=E(3,2); uint8_t fwd2[6]={0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}; memcpy(rb+46,fwd2,6); uint32_t l2 = 46+6+XFER_MAC_LEN;
      d.onListRsp(rb,l2,0,back,E(3,2),false); assert(d._route_manual && d._route[0]==0x3C); checks++;
      d._typed_failed = true; d.onListRsp(rb,l2,0,back,E(3,2),false);
      assert(!d._route_manual && !d._ret_typed && d._route[0]==0xAA && rtCount(d._route_len)==2 && strstr(d._last_err,"rediscovered")); checks++;
      printf("  re-discovery after the typed route failed: %s\n", d._last_err); }
    o.cmdRoute("auto", rep); o.onListRsp(b,len,0,back2,E(3,1),true); assert(o._ret_len==E(3,1) && o._ret[0]==1); checks++; }
  // 11) server heal trigger: 3 identical requests -> 1 flood. (v1.17.28) Kept per PULLER: a heal it does not get past -> the next one no
  //     sooner than 1 h later, at most HEAL_MAX in a row without progress, whatever session it resumes under; progress starts over
  { FwOtaTransfer s; XferManifest m = {}; uint8_t q[16]={0}; uint32_t sa=0xAAAA0001, sb=0xBBBB0002, sc=0xCCCC0003; memcpy(q+2,&sa,4);
    const uint8_t P1[4]={1,2,3,4}, P2[4]={9,9,9,9};
    s.noteReq(q,true,0,m,P1); s.noteReq(q,true,0,m,P1); assert(s.floods==0); s.noteReq(q,true,0,m,P1); assert(s.floods==1); checks++;
    for(int i=0;i<3;i++) s.noteReq(q,true,0,m,P1); assert(s.floods==1); checks++;                     // the heal did not help: not again at once ...
    g_ms += 120001; for(int i=0;i<3;i++) s.noteReq(q,true,0,m,P1); assert(s.floods==1); checks++;     // ... nor 2 min later (v1.17.27 flooded here)
    g_ms += HEAL_NOPROG_MS; for(int i=0;i<3;i++) s.noteReq(q,true,0,m,P1); assert(s.floods==2); checks++;   // an hour later: one more
    s.noteReq(q,false,5,m); for(int i=0;i<3;i++) s.noteReq(q,false,5,m); assert(s.floods==2); checks++;   // where it is (first seen): not progress, still waits
    s.noteReq(q,false,6,m); s.noteReq(q,false,7,m); s.noteReq(q,false,8,m); assert(s.floods==2); checks++;   // progress: no heal
    for(int i=0;i<3;i++) s.noteReq(q,false,8,m); assert(s.floods==3); checks++;                        // got further than at the last heal: a new stall heals at once
    for (int k=0;k<10;k++) { g_ms += HEAL_NOPROG_MS + 1; for(int i=0;i<3;i++) s.noteReq(q,false,8,m); } assert(s.floods==2+HEAL_MAX); checks++;   // cap
    memcpy(q+2,&sb,4); g_ms += HEAL_NOPROG_MS + 1; for(int i=0;i<6;i++) s.noteReq(q,true,0,m,P1); assert(s.floods==2+HEAL_MAX); checks++;   // resumed (new session), same puller: still capped
    s.noteReq(q,false,9,m); s.noteReq(q,false,10,m); for(int i=0;i<3;i++) s.noteReq(q,false,10,m); assert(s.floods==3+HEAL_MAX); checks++;  // ... until it gets further
    memcpy(q+2,&sc,4); for(int i=0;i<3;i++) s.noteReq(q,true,0,m,P2); assert(s.floods==4+HEAL_MAX); checks++;   // another puller: its own history
    // today's board 2 stall (v1.17.25 both ends: 157 heals in ~12 h): a resume every 20 min for 12 h, never getting further -> heal floods
    { FwOtaTransfer g; int n = 0; for (uint32_t k = 0; k < 36; k++) { uint32_t ss = 0xD0000000 + k; memcpy(q+2,&ss,4); g_ms += 20UL * 60 * 1000; int f0 = g.floods;
        for(int i=0;i<3;i++) g.noteReq(q,true,0,m,P1); for(int i=0;i<3;i++) g.noteReq(q,false,1384,m); n += g.floods - f0; }
      assert(n <= HEAL_MAX); checks++; printf("  per-puller heal limit: %d heal floods in a 12 h stall of 36 resumes (v1.17.27: up to 48)\n", n); }
    // (v1.17.27, user) ONE node-wide budget for automatic floods: at most 4 per ROLLING hour, whatever the sessions
    { FwOtaTransfer g; int per_hour_max = 0; uint32_t t0 = g_ms; std::vector<uint32_t> at;
      for (uint32_t k = 0; k < 3 * 30; k++) { uint32_t ss = 0xE0000000 + k; memcpy(q+2,&ss,4); g_ms += 120001; int f0 = g.floods; for(int i=0;i<3;i++) g.noteReq(q,true,0,m); if (g.floods > f0) at.push_back(g_ms); }
      for (size_t i = 0; i < at.size(); i++) { int n = 0; for (size_t j = i; j < at.size() && at[j] - at[i] < FLOOD_BUDGET_MS; j++) n++; if (n > per_hour_max) per_hour_max = n; }
      assert(per_hour_max == FLOOD_BUDGET_N && at.size() >= 10); checks++;
      printf("  flood budget: %zu heal floods in %u h of a resume every 2 min, at most %d in any hour\n", at.size(), (unsigned)((g_ms - t0) / 3600000), per_hour_max);
      FwOtaTransfer h; for (int i = 0; i < 4; i++) assert(h.floodBudget(g_ms + i)); assert(!h.floodBudget(g_ms + 10)); checks++;   // 5th in the hour refused
      assert(!h.floodBudget(g_ms + FLOOD_BUDGET_MS - 1) && h.floodBudget(g_ms + FLOOD_BUDGET_MS)); checks++; } }                 // rolling: the oldest frees a slot
  // 12) (v1.17.23) fw peers grouped per host: 0-8 offers from 1-8 hosts - every #n exactly once over all pages, a host never split, replies < 160 B
  { int pages=0; for (int offers=1; offers<=8; offers++) for (int hosts=1; hosts<=offers; hosts++) for (int order=0; order<2; order++) {
      FwOtaTransfer t; for (int i=0;i<offers;i++) { PeerInfo& p=t._peers[i]; p.used=true; int h = order ? i % hosts : (i * hosts) / offers;
        p.id[0]=0x10+h; p.id[1]=0xAB; p.id[2]=0xCD; p.id[3]=h; snprintf(p.name,17,"Host-number-%02d",h); p.kind = (i%2) ? KIND_BOOTLOADER : KIND_APP; p.fw_version=200+i; p.rssi=-40-i; p.snr4=-3+i; p.route_len = E(3, i%3); }
      int seen[9]={0}; int from=1; char rep[200];
      for (int guard=0; guard<10; guard++) {
        memset(rep, 0x5A, sizeof rep); char a[8]; snprintf(a,8,"%d",from); t.cmdPeers(a, rep); pages++;
        assert(strlen(rep) < 160); checks++;
        for (const char* c=rep; (c=strstr(c," #")); c+=2) { int k=atoi(c+2); if (k>=1 && k<=8) seen[k]++; }
        for (int hh=0; hh<hosts; hh++) { char id[9]; snprintf(id,9,"%02XABCD%02X",0x10+hh,hh); int cnt=0; for(const char* c=rep;(c=strstr(c,id));c+=8) cnt++; assert(cnt<=1); }   // host once per page
        const char* nx=strstr(rep,"'fw peers "); if(!nx) break; from=atoi(nx+10);
      }
      for (int k=1;k<=offers;k++) { if (seen[k]!=1) { printf("offers=%d hosts=%d order=%d: #%d seen %d times\n",offers,hosts,order,k,seen[k]); assert(0); } } checks++;
    }
    FwOtaTransfer t; char rep[160]; t.cmdPeers("", rep); assert(strstr(rep,"no peers")); checks++;
    t._peers[0].used=true; memcpy(t._peers[0].id,"\x50\x35\x63\x01",4); strcpy(t._peers[0].name,"Space-Grey-DP"); t._peers[0].rssi=-37; t._peers[0].snr4=51; t._peers[0].fw_version=210;
    t._peers[1]=t._peers[0]; t._peers[1].kind=KIND_BOOTLOADER; t._peers[1].fw_version=3; t.cmdPeers("", rep); printf("  fw peers: %s\n", rep);
    assert(!strcmp(rep,"50356301 Space-Grey-DP -37dBm snr12.75 #1 fw-210 #2 bl-rev-3")); checks++;
    t.cmdPeers("2", rep); assert(strstr(rep,"#1 fw-210 #2 bl-rev-3")); checks++;                   // #2 is in the first host's block: the block is shown whole
    t.cmdPeers("3", rep); assert(strstr(rep,"no peer #3")); checks++;
    printf("  (%d fw peers pages checked)\n", pages); }
  // 13) a flooded answer arrives with its AES padding (length rounded up to 16): listRspLen cuts it to the real answer (v1.17.23 missed this)
  { uint8_t b[96]; memset(b,0,sizeof b); for (int hops=0; hops<=4; hops++) for (int hs=1; hs<=3; hs++) { if (hops*hs>MAX_ROUTE_BYTES) continue;
      b[45]=E(hs,hops); uint32_t real=46+hops*hs+XFER_MAC_LEN, padded=((11+real+15)/16)*16-11;
      assert(listRspLen(b,padded)==real && listRspLen(b,real)==real && listRspLen(b,real-1)==0); checks++; }
    b[45]=E(3,7); assert(listRspLen(b,80)==0); checks++; assert(listRspLen(b,40)==0); checks++; }
  // (v1.17.28) ids typed after "fw pull" must be real hex: MeshCore's Utils::fromHex() turned "status" (a typo of pull-status) into 00A000
  { uint8_t v[4]; assert(!hexN(v,3,"status") && !hexN(v,4,"stat1234") && !hexN(v,2,"a0g0")); assert(hexN(v,3,"5e6f70") && v[0]==0x5E && v[2]==0x70); checks++; }
  printf("route table + pairs + heal + collision + typed-route + peers + padding checks: %d passed\n", checks); return 0;
}
"""
src = open("src/helpers/fw_ota/FwOtaTransfer.cpp").read()
def fn(sig):
    i = src.index(sig); j = src.index("{", i); d = 0
    for k in range(j, len(src)):
        if src[k] == "{": d += 1
        elif src[k] == "}":
            d -= 1
            if d == 0: return src[i:k + 1]
pre = "#include <vector>\n" + fn("static bool hexN(") + "\n" + fn("static bool parseHops(") + "\n" + fn("static size_t printHops(") + "\n" + fn("static uint32_t listRspLen(")
sigs = ["bool FwOtaTransfer::cmdRoute(", "bool FwOtaTransfer::cmdPeers(", "void FwOtaTransfer::setRet(", "uint32_t FwOtaTransfer::addReturnRoute(",
        "void FwOtaTransfer::onListRsp(", "void FwOtaTransfer::noteReq(", "bool FwOtaTransfer::floodBudget(", "void FwOtaTransfer::routeReset()", "void FwOtaTransfer::routeDropDirect(", "int FwOtaTransfer::routeAdd(", "void FwOtaTransfer::routeUse(", "float FwOtaTransfer::routeScore(",
        "void FwOtaTransfer::routeResult(", "bool FwOtaTransfer::routeFailover()", "int FwOtaTransfer::matchPeers("]
open(sys.argv[1], "w").write(HEAD.replace("class FwOtaTransfer {", pre + "\nclass FwOtaTransfer {", 1) + "\n".join(fn(s) for s in sigs) + "\n" + TAIL)
PY
g++ -std=c++11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o "$W/route_test" "$W/route_test.cpp"
"$W/route_test"
rm -rf "$W"

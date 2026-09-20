#include "../support/event_queue_test_access.hpp"
#include "harness.hpp"
#include <leanat/admission.hpp>
#include <leanat/cancel_scope.hpp>
#include <leanat/crossbar.hpp>
#include <leanat/descriptor.hpp>
#include <leanat/external.hpp>
#include <leanat/managed.hpp>
#include <leanat/memory.hpp>
#include <leanat/process.hpp>
#include <leanat/register_bank.hpp>
#include <leanat/resource.hpp>
#include <leanat/structured.hpp>
#include <leanat/wait_group.hpp>
using namespace leanat;
using namespace conformance;
#include "lifecycle.hpp"
#include "ooo_routes.hpp"
#include "protocol_cases.hpp"
struct WireFixture {
  ProtocolEngine engine{{}, {}, 32, 32, 64};
  unsigned sequence{};
  PayloadSnapshot payload;
  WireFixture() {
    payload.command = Command::Read;
    payload.data = {0, 0};
    payload.streaming_width = 2;
  }
  Handle ledger(unsigned connection = 1, unsigned transport = 1) {
    return take(engine.create_ledger(ConnectionId{connection}, TransportId{transport}, 1, 0));
  }
  ExchangeResult call(Handle h, PhaseId phase, Sync sync, std::optional<PhaseId> returned = {},
                      Tick now = {}, Duration input = {}, Duration output = {}) {
    auto s = take(engine.inspect(h));
    WireCall c{CallId{++sequence},
               s.identity.connection,
               s.identity.transport,
               phase == end_req || phase == begin_resp ? Flow::Backward : Flow::Forward,
               phase,
               now,
               input,
               payload};
    if (phase == begin_resp) {
      c.request.status = ResponseStatus::Ok;
      c.request.data = {4, 9};
    }
    auto ticket = take(engine.begin_call(h, c));
    WireReturn r{sync, returned, output, {}};
    if (sync == Sync::Completed || (sync == Sync::Updated && returned == begin_resp) ||
        phase == begin_resp)
      r.response = ResponseSnapshot{ResponseStatus::Ok, {4, 9}, false, {}};
    return take(engine.end_call(ticket, r));
  }
};
static std::int32_t native_identity(const std::uint8_t *in, std::uint32_t n, std::uint8_t *out,
                                    std::uint32_t cap, std::uint32_t *size) {
  if (n > cap)
    return 8;
  std::copy(in, in + n, out);
  *size = n;
  return 0;
}
static std::int32_t native_error(const std::uint8_t *, std::uint32_t, std::uint8_t *, std::uint32_t,
                                 std::uint32_t *) {
  try {
    throw std::runtime_error("native failure");
  } catch (...) {
    return 17;
  }
}
static std::int32_t native_wrong(const std::uint8_t *, std::uint32_t, std::uint8_t *out,
                                 std::uint32_t, std::uint32_t *size) {
  out[0] = 1;
  *size = 1;
  return 0;
}
static ExternCallDesc external_desc(ExternalLayoutKind kind, std::uint32_t bytes) {
  ExternCallDesc d;
  d.id = 1;
  d.logical_id = "identity";
  d.reference_hash = "conformance.identity.v1";
  d.symbol_id = "identity";
  d.build_id = "conformance";
  d.contract_hash = "identity.pure";
  d.input = {kind, bytes};
  d.output = d.input;
  d.select_native = true;
  d.effects = {true, true, true, true, true, true};
  return d;
}
#include "waits.hpp"
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Report r(argv[1]);
  r.run("C-T01", "call=100,inDelay=10,outDelay=25; BEGIN_REQ COMPLETED",
        "request input110; terminal125, not135", [] {
          WireFixture f;
          auto h = f.ledger();
          auto e = f.call(h, begin_req, Sync::Completed, {}, Tick{100}, Duration{10}, Duration{25});
          require(e.wire_terminal, "not terminal");
          for (auto &m : e.milestones)
            require(m.time == Tick{125}, "return delay double-added");
          return "terminal and response milestones at125; call-relative return preserved";
        });
  r.run(
      "C-T02", "BEGIN_REQ accepted with returnedPhase999/outDelay99; input delay7",
      "caller ignores invalid ACCEPTED return fields",
      [] {
        WireFixture f;
        auto h = f.ledger();
        auto e = f.call(h, begin_req, Sync::Accepted, PhaseId{999}, {}, Duration{7}, Duration{99});
        require(e.next_state == WireState::Request && e.milestones.empty(),
                "accepted changed semantic state");
        require(take(f.engine.inspect(h)).last_timing == Tick{7}, "accepted used output delay");
        return "ledger Request, last_timing7, no return milestones; native callee mutation check "
               "requires SystemC fixture";
      },
      "NotRun");
  r.run("C-T03", "BEGIN_REQ UPDATED BEGIN_RESP at return12",
        "implicit requestReleased and responseReady; no END_REQ call", [] {
          WireFixture f;
          auto h = f.ledger();
          auto e = f.call(h, begin_req, Sync::Updated, begin_resp, {}, {}, Duration{12});
          require(e.needs_ack && !e.wire_terminal, "shortcut ack state");
          require(e.milestones.size() == 2, "shortcut milestone count");
          require(e.milestones[0].kind == MilestoneKind::RequestReleased &&
                      e.milestones[0].implicit,
                  "request release not implicit");
          require(take(f.engine.inspect(h)).call_ordinal == 1, "fabricated wire call");
          return "one recorded call; implicit RequestReleased12, ResponseReady12, needsAck";
        });
  r.run("C-T04", "BEGIN_REQ COMPLETED with READ bytes[4,9]",
        "owned response and terminal; no END_RESP", [] {
          WireFixture f;
          auto h = f.ledger();
          auto e = f.call(h, begin_req, Sync::Completed);
          require(e.wire_terminal && !e.needs_ack, "completed requires extra acknowledgement");
          bool data = false;
          for (auto &m : e.milestones)
            if (m.response)
              data = m.response->data == Bytes({4, 9});
          require(data, "missing owned response");
          require(take(f.engine.inspect(h)).call_ordinal == 1, "extra call");
          return "terminal, needsAck=false, owned response[4,9], one call";
        });
  r.run("C-T05", "BEGIN_REQ accepted; END_REQ accepted; BEGIN_RESP UPDATED END_RESP at25",
        "one terminal milestone at return-effective25", [] {
          WireFixture f;
          auto h = f.ledger();
          f.call(h, begin_req, Sync::Accepted);
          f.call(h, end_req, Sync::Accepted);
          auto e = f.call(h, begin_resp, Sync::Updated, end_resp, Tick{20}, {}, Duration{5});
          unsigned count = 0;
          for (auto &m : e.milestones)
            if (m.kind == MilestoneKind::Terminal) {
              ++count;
              require(m.time == Tick{25}, "early terminal");
            }
          require(count == 1 && e.wire_terminal, "terminal count");
          return "exactly one terminal at25";
        });
  auto endresp = []() {
    WireFixture f;
    auto h = f.ledger();
    f.call(h, begin_req, Sync::Updated, begin_resp);
    auto e = f.call(h, end_resp, Sync::Accepted, {}, Tick{3}, Duration{2}, Duration{99});
    require(e.wire_terminal && !e.needs_ack, "END_RESP ACCEPTED not terminal");
    require(e.milestones.back().time == Tick{5}, "terminal annotation");
    return std::string("END_RESP ACCEPTED terminal at5; no later callback needed");
  };
  r.run("C-T06", "response shortcut then END_RESP ACCEPTED, call3/input2/output99",
        "terminal5 without next callback", endresp);
  r.run("C-T07", "END_REQ returns UPDATED or COMPLETED in separate fresh ledgers",
        "both combinations rejected", [] {
          for (auto sync : {Sync::Updated, Sync::Completed}) {
            WireFixture f;
            auto h = f.ledger();
            f.call(h, begin_req, Sync::Accepted);
            bool rejected = false;
            try {
              f.call(h, end_req, sync, begin_resp);
            } catch (const std::exception &) {
              rejected = true;
            }
            require(rejected, "illegal END_REQ return accepted");
          }
          return "UPDATED and COMPLETED rejected independently";
        });
  r.run("C-T08", "END_REQ called at0 with delay100; next BEGIN_REQ called at1",
        "wire request lane released despite future business milestone", [] {
          WireFixture f;
          auto a = f.ledger(1, 1);
          f.call(a, begin_req, Sync::Accepted);
          auto released = f.call(a, end_req, Sync::Accepted, {}, Tick{}, Duration{100});
          auto b = f.ledger(1, 2);
          auto accepted = f.call(b, begin_req, Sync::Accepted, {}, Tick{1});
          require(accepted.next_state == WireState::Request, "wire lane still busy");
          require(released.milestones[0].time == Tick{100}, "effective release lost");
          return "second call accepted at1; first requestReleased remains100";
        });
  r.run("C-T10", "one service slot; two transactions on independent connections",
        "second accepted into bounded pending with owned request", [] {
          AdmissionStore s({}, {}, {4, 4, 1, 4, 4});
          AdmissionRequest a;
          a.connection = ConnectionId{1};
          a.transport = TransportId{1};
          a.request.data = {8};
          auto one = take(s.admit(a, true));
          a.connection = ConnectionId{2};
          a.transport = TransportId{2};
          a.request.data = {9};
          auto two = take(s.admit(a, true));
          require(one.service.has_value() && two.retained_pending && !two.service,
                  "pending admission lost");
          auto retained = take(s.inspect(two.hop));
          require(retained.owned_request.data == Bytes{9} && s.active_services() == 1,
                  "pending payload/service accounting");
          return "service1 retainedPending=true; second payload[9] still stored";
        });
  r.run("C-T16", "COMPLETED response at2 before process suspend registration at3",
        "saved continuation immediately ResumeQueued with latch value", [] {
          ProcessStore p({}, 1, 1, 1, 4);
          auto h = take(p.create(ProgramId{1}, 7));
          take(p.begin(h));
          WireFixture wire;
          auto ledger = wire.ledger();
          auto completed = wire.call(ledger, begin_req, Sync::Completed, {}, Tick{2});
          SingleWaitOutcome latched;
          for (auto &m : completed.milestones)
            if (m.response)
              latched.value = Value{m.response->data};
          require(latched.value == Value{Bytes{4, 9}}, "completion has no owned response");
          latched.source = {Tick{2}, 0};
          SingleWaitSpec s;
          s.kind = SingleWaitKind::Response;
          s.source = ledger;
          ResumeFrame frame{BlockId{3}, {Value{Bytes{1, 2}}}};
          auto ticket = take(p.suspend(h, s, frame, {Tick{3}, 0}, latched));
          require(take(p.inspect(h)).state == ProcessState::ResumeQueued, "lost ready latch");
          auto resumed = take(p.take_resume(ticket));
          require(resumed.frame.live == frame.live && resumed.outcome.value == Value{Bytes{4, 9}},
                  "frame/result corrupt");
          return "ResumeQueued immediately; continuation block3 and owned bytes[1,2] preserved";
        });
  r.run("C-T18", "Memory[1,2]; stage write[9]; full event queue rejects schedule; discard",
        "memory unchanged and no partially published event", [] {
          auto m = take(Memory::make(2, {1, 2}));
          EventQueue q(1);
          take(q.enqueue(EventDraft{}));
          EventTxn t({}, ExecutionContext{});
          take(m.write_bytes(t, 0, {9}));
          auto ev = t.stage_event(q, EventDraft{});
          require(!ev, "schedule unexpectedly fit");
          take(t.discard());
          require(m.data()[0] == 1 && q.occupied() == 1, "partial local commit");
          return "Memory[1,2], one preexisting event, zero newly published events";
        });
  r.run("C-T20", "base2,length6,width3,mask[FF,00]; initial memory[0..7]",
        "indices0,2,4 enabled; reads preserve other bytes; repeated writes last enabled wins", [] {
          auto m = take(Memory::make(8, {0, 1, 2, 3, 4, 5, 6, 7}));
          EventTxn t({}, ExecutionContext{});
          PayloadSnapshot p;
          p.command = Command::Read;
          p.address = 2;
          p.data = Bytes(6, 99);
          p.streaming_width = 3;
          p.byte_enable = {255, 0};
          auto got = take(m.transfer(t, p));
          require(got.data == Bytes({2, 99, 4, 99, 3, 99}), "read mask/streaming mismatch");
          p.command = Command::Write;
          p.data = {10, 11, 12, 13, 14, 15};
          take(m.transfer(t, p));
          take(t.commit());
          require(m.data()[2] == 10 && m.data()[3] == 14 && m.data()[4] == 12,
                  "write wrap mismatch");
          return "READ[2,99,4,99,3,99]; Memory2..4=[10,14,12]";
        });
  r.run("C-T21", "IGNORE addressMAX width0; WRITE addressMAX; READ length0",
        "IGNORE OK, invalid address/zero length rejected, no write", [] {
          auto m = take(Memory::make(2, {1, 2}));
          EventTxn t({}, ExecutionContext{});
          PayloadSnapshot p;
          p.address = UINT64_MAX;
          require(take(m.transfer(t, p)).status == ResponseStatus::Ok, "IGNORE error");
          p.command = Command::Write;
          p.data = {7};
          p.streaming_width = 1;
          require(take(m.transfer(t, p)).status == ResponseStatus::AddressError,
                  "overflow address accepted");
          p.command = Command::Read;
          p.data.clear();
          require(take(m.transfer(t, p)).status == ResponseStatus::BurstError,
                  "zero length not rejected");
          take(t.commit());
          require(m.data()[0] == 1 && m.data()[1] == 2, "invalid operation wrote memory");
          return "IGNORE OK; ADDRESS and BURST; memory unchanged";
        });
  r.run("C-T22", "8bit RC register resetAB, debug read2 bytes at0",
        "count1; RC not cleared; untransferred suffix preserved; no action", [] {
          RegisterSpec s;
          s.width_bits = 8;
          s.allowed_access_bytes = {1};
          s.fields = {{FieldId{1}, 0, 8, RegisterAccess::RC, {0xab}}};
          auto bank = take(RegisterBank::make({s}));
          ExecutionContext c;
          c.kind = ContextKind::Debug;
          EventTxn tx({}, c);
          auto read = take(bank.peek_poke(tx, Command::Read, 0, {0, 0x77}));
          require(read.count == 1 && read.data == Bytes({0xab, 0x77}), "debug count/data mismatch");
          auto committed = take(tx.commit());
          require(committed.actions.empty(), "debug published action");
          c.kind = ContextKind::Timed;
          EventTxn verify({}, c);
          require(take(bank.read_field(verify, FieldId{1})) == Bytes{0xab}, "debug cleared RC");
          return "count1,data[AB,77],RC remainsAB,actions0";
        });
  r.run("C-T25", "registered ignorable phase99 received in Idle ledger",
        "ACCEPTED ignored; no forward/milestone", [] {
          WireFixture f;
          take(f.engine.allow_ignorable(PhaseId{99}));
          auto h = f.ledger();
          auto e = f.call(h, PhaseId{99}, Sync::Accepted);
          require(e.ignored && e.milestones.empty() && e.next_state == WireState::Idle,
                  "unknown phase changed state");
          return "ignored=true,stateIdle,milestones0; no output intents";
        });
  r.run("E-T12", "Ext executable uses base ledger; END_RESP ACCEPTED",
        "same terminal rule as base, independently instantiated", endresp);
  r.run("E-T08", "one frame, two result slots; completed result not consumed; spawn next",
        "frame reused while old owned result readable", [] {
          ResultStore rs(4, 8, 8);
          TaskPoolDesc d;
          d.max_instances = 1;
          d.task_capacity = 2;
          d.result_capacity = 2;
          TaskPool pool({}, 1, d, rs);
          Handle scope{HandleKind::Scope, {}, 1, 0, 1, 0};
          auto first = take(pool.try_spawn({}, scope));
          auto consumer = take(pool.result_handle(first, scope));
          take(pool.complete(first, {TaskOutcome::Kind::Success, Value{Bytes{42}}}, {}));
          auto second = take(pool.try_spawn({}, scope));
          require(second != first && pool.active_frames() == 1, "frame did not recycle");
          auto result = take(pool.pin_result(first, consumer));
          require(!std::holds_alternative<std::monostate>(result.value().data), "old result gone");
          return "second task allocated; first result pinned after frame reuse; activeFrames1";
        });
  r.run("E-T21", "native U64LE identity 0x0102030405060708; malformed Bool bytes",
        "exact LE roundtrip and invalid representation rejection", [] {
          ExternalCallGate g(true);
          auto d = external_desc(ExternalLayoutKind::U64LE, 8);
          take(g.register_reference(
              d, [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; },
              [](const Value &) { return true; }));
          take(g.register_native(1, "identity", "conformance", 1, native_identity));
          take(g.seal());
          auto v = take(g.call_pure(1, Value{std::uint64_t{0x0102030405060708ULL}}));
          require(std::get<std::uint64_t>(v.data) == 0x0102030405060708ULL, "LE codec mismatch");
          require(!g.call_pure(1, Value{Bytes(9)}), "invalid representation accepted");
          return "U64LE identity exact; incompatible/oversized Bytes rejected";
        });
  r.run("E-T22", "native catches C++ exception and returns status17",
        "explicit ExternalFailure, no exception escapes ABI", [] {
          ExternalCallGate g(true);
          auto d = external_desc(ExternalLayoutKind::Bool, 1);
          take(g.register_reference(
              d, [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; },
              [](const Value &) { return true; }));
          take(g.register_native(1, "identity", "conformance", 1, native_error));
          take(g.seal());
          auto x = g.call_pure(1, Value{true});
          require(!x && x.error().code == ErrorCode::ExternalFailure, "native error hidden");
          return "native status17 becomes ExternalFailure";
        });
  r.run("E-T23", "Bool reference identity(false); intentionally wrong native true",
        "diff fails and retains actual input/native/reference values", [] {
          ExternalCallGate g(true);
          auto d = external_desc(ExternalLayoutKind::Bool, 1);
          take(g.register_reference(
              d, [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; },
              [](const Value &) { return true; }));
          take(g.register_native(1, "identity", "conformance", 1, native_wrong));
          take(g.seal());
          require(!g.compare(1, Value{false}), "wrong native passed diff");
          auto c = g.last_comparison();
          require(c && c->input == Value{false} && c->native_result == Value{true} &&
                      c->reference_result == Value{false},
                  "diff evidence missing");
          return "diff failed as expected; inputfalse,nativetrue,referencefalse retained";
        });
  r.run("E-T27", "managed-only manager region query for standard DMI",
        "false, no manufactured pointer", [] {
          ResultStore s;
          ManagedAccessManager m(s, {}, true);
          require(!m.standard_dmi_allowed(RegionId{1}), "managed exposed standard DMI");
          return "standard_dmi_allowed=false; no pointer descriptor produced";
        });
  for (bool turns : {false, true})
    r.run(turns ? "E-T03" : "E-T02",
          turns ? "timeout ready(8,0), response ready(8,1) priority-10"
                : "response/timeout ready(8,0), test priority then ordinal ties",
          turns ? "earlier timeout wins and late response cannot reverse"
                : "exactly one winner by lower priority then ordinal",
          [turns] {
            for (int tie = 0; tie < (turns ? 1 : 2); ++tie) {
              ResultStore results(16, 32, 32);
              WaitGroupStore waits({}, 9, 1, 2, 512, results);
              Handle owner{HandleKind::Process, {}, 1, 0, 1, 1},
                  scope{HandleKind::Scope, {}, 1, 0, 1, 0},
                  source{HandleKind::Event, {}, 2, 0, 1, 0};
              unsigned resumed = 0;
              WaitCreate c;
              c.owner_process = owner;
              c.owner_scope = scope;
              c.branch_count = 2;
              c.resume = [&](const Value &, ReadyKey) -> Expected<void> {
                ++resumed;
                return {};
              };
              auto h = take(waits.create(c));
              for (unsigned i = 0; i < 2; ++i) {
                WaitBranch b;
                b.ordinal = i;
                b.priority = i == 1 && !tie ? -10 : 0;
                b.source = source;
                take(waits.arm(h, b, BatchId{1}, {}));
              }
              take(waits.commit(h));
              take(waits.record({h,
                                 1,
                                 source,
                                 {Tick{8}, turns ? 1u : 0u},
                                 BatchId{1},
                                 Value{std::uint64_t{22}},
                                 false}));
              take(waits.record(
                  {h, 0, source, {Tick{8}, 0}, BatchId{1}, Value{std::uint64_t{11}}, false}));
              require(!waits.resolve_closed_batch(BatchId{1}, false), "resolved open batch");
              auto resolutions = take(waits.resolve_closed_batch(BatchId{1}, true));
              require(resolutions.size() == 1 && resumed == 1, "not unique resume");
              auto value = std::get<Value::Array>(resolutions[0].outcome.data);
              require(std::get<std::uint64_t>(value[0].data) == (turns || tie ? 0u : 1u),
                      "wrong winning branch");
              require(!waits.record({h, 1, source, {Tick{8}, 2}, BatchId{2}, Value{}, false}),
                      "late loser reversed result");
              auto consumer = take(waits.result_handle(h, scope));
              take(waits.release(h, consumer, scope));
              require(results.occupied() == 0, "result leaked");
            }
            return std::string(turns ? "timeout ordinal0 won earlier turn despite response "
                                       "priority-10; late notification rejected; one resume"
                                     : "response ordinal1 wins priority-10; equal priorities "
                                       "ordinal0 wins; each resumes once after closed batch");
          });
  r.run(
      "E-T06", "controller task in root and business task in child; child timeout cancellation",
      "business Cancelled; controller continues and returns42", [] {
        ResultStore results(16, 32, 16);
        CancelScopeStore scopes({}, 2, 4, 16, 16);
        TaskPoolDesc desc;
        desc.max_instances = 2;
        desc.task_capacity = 4;
        desc.result_capacity = 4;
        TaskPool tasks({}, 4, desc, results, &scopes);
        auto child = take(scopes.create(scopes.root()));
        auto controller = take(tasks.try_spawn({}, scopes.root()));
        auto business = take(tasks.try_spawn({}, child));
        auto output = take(tasks.result_handle(controller, scopes.root()));
        auto plan = take(scopes.cancel(child, "timeout"));
        for (auto &a : plan.actions)
          take(scopes.apply(child, a.id, [&](const ScopeCancelAction &action) {
            return tasks.execute_cancel_action(action, ReadyKey{});
          }));
        require(take(tasks.state(business)) == TaskState::Cancelled, "business not cancelled");
        require(take(tasks.state(controller)) == TaskState::Runnable, "controller not running");
        take(
            tasks.complete(controller, {TaskOutcome::Kind::Success, Value{std::uint64_t{42}}}, {}));
        auto result = take(tasks.owning_result(controller, output));
        require(result ==
                    encode_task_outcome({TaskOutcome::Kind::Success, Value{std::uint64_t{42}}}),
                "controller return lost");
        take(tasks.release_result(controller, output));
        return "child task Cancelled; root task still Runnable then completed with owned Success42";
      });
  r.run(
      "E-T04",
      "one WaitGroup slot; resolve/release generation1, arm generation2; deliver stale loser",
      "no new waiter resume from stale generation; only fresh notification resumes", [] {
        ResultStore results(8, 16, 16);
        WaitGroupStore waits({}, 9, 1, 2, 512, results);
        Handle scope{HandleKind::Scope, {}, 1, 0, 1, 0}, source{HandleKind::Event, {}, 2, 0, 1, 0};
        unsigned resumed = 0;
        WaitCreate c;
        c.owner_scope = scope;
        c.owner_process = {HandleKind::Process, {}, 3, 0, 1, 1};
        c.branch_count = 1;
        c.resume = [&](const Value &, ReadyKey) -> Expected<void> {
          ++resumed;
          return {};
        };
        auto arm = [&]() {
          auto h = take(waits.create(c));
          WaitBranch b;
          b.source = source;
          take(waits.arm(h, b, BatchId{1}, {}));
          take(waits.commit(h));
          return h;
        };
        auto old = arm();
        take(waits.record({old, 0, source, {}, BatchId{1}, Value{false}, false}));
        require(take(waits.resolve_closed_batch(BatchId{1}, true)).size() == 1, "first resolution");
        take(waits.release(old, take(waits.result_handle(old, scope)), scope));
        auto fresh = arm();
        require(fresh.slot == old.slot && fresh.generation != old.generation,
                "slot not recycled with generation");
        require(!waits.record({old, 0, source, {Tick{1}, 0}, BatchId{2}, Value{true}, false}),
                "old generation accepted");
        require(take(waits.resolve_closed_batch(BatchId{2}, true)).empty() && resumed == 1,
                "stale loser resumed new waiter");
        take(waits.record({fresh, 0, source, {Tick{2}, 0}, BatchId{3}, Value{true}, false}));
        require(take(waits.resolve_closed_batch(BatchId{3}, true)).size() == 1 && resumed == 2,
                "new generation failed");
        take(waits.release(fresh, take(waits.result_handle(fresh, scope)), scope));
        return "same slot changed generation; stale loser rejected; resume count1 until new "
               "source, then2";
      });
  r.run("E-T11",
        "ProbeV1 same begin call with Accepted, Updated(end), Updated(response), Completed",
        "full exchange selects first two; latter two fail", [] {
          for (auto sync : {Sync::Accepted, Sync::Updated, Sync::Completed})
            for (unsigned returned : {1u, 2u}) {
              ProtocolEngine e({}, {}, 2, 2, 8);
              take(e.install_package(conformance_probe()));
              auto h = take(e.create_ledger(ConnectionId{1}, TransportId{1}));
              WireCall c;
              c.id = CallId{1};
              c.connection = ConnectionId{1};
              c.transport = TransportId{1};
              c.phase = PhaseId{0};
              auto ticket = take(e.begin_call(h, c));
              WireReturn ret{sync, PhaseId{returned}, {}, {}};
              auto out = e.end_call(ticket, ret);
              bool accepted = sync == Sync::Accepted || (sync == Sync::Updated && returned == 1);
              require(bool(out) == accepted, "full exchange selection differs from finite table");
              if (out)
                require(!out.value().wire_terminal, "request incorrectly terminal");
              else
                require(take(e.inspect(h)).faulted, "invalid return not faulted");
            }
          return "six independent exchanges; Accepted2/2 and Updated(end) pass; "
                 "Updated(response)/Completed2/2 reject";
        });
  r.run("E-T17",
        "finite package action opcode255 representing unsupported user release-GP instruction",
        "descriptor validator rejects unsupported opcode before execution", [] {
          auto p = conformance_probe();
          p.rules[0].return_actions.push_back(
              {static_cast<ProtocolActionKind>(255), 0, "release_gp"});
          auto v = validate_protocol_package(p);
          require(!v && v.error().code == ErrorCode::InvalidArgument,
                  "unsupported lifetime opcode accepted");
          return "action opcode255 rejected InvalidArgument; no native GP release instruction "
                 "admitted";
        });
  r.run("E-T18", "user terminal state without CloseHop; accepted base request locally cancelled",
        "silent terminal rejected; local cancel preserves Request and drain waitResponse", [] {
          auto p = conformance_probe();
          p.rules[0].post = 4;
          require(!validate_protocol_package(p), "silent terminal package accepted");
          WireFixture f;
          auto h = f.ledger();
          f.call(h, begin_req, Sync::Accepted);
          auto plan = take(f.engine.request_local_cancel(h));
          require(plan.wait_response && !plan.terminal &&
                      take(f.engine.inspect(h)).state == WireState::Request,
                  "cancel killed wire lifecycle");
          return "silent terminal rejected; local cancellation yields waitResponse drain plan and "
                 "live Request ledger";
        });
  r.run("E-T19",
        "duplicate always guards with missing priorities; equal priorities; ordered priorities",
        "first two rejected; explicit distinct priority selects preferred rule", [] {
          auto p = conformance_probe();
          p.rules.push_back(p.rules[0]);
          require(!validate_protocol_package(p), "overlap without priorities accepted");
          p.rules[0].priority = 1;
          p.rules.back().priority = 1;
          require(!validate_protocol_package(p), "equal priority overlap accepted");
          p.rules.back().priority = 0;
          p.rules.back().return_actions = {{ProtocolActionKind::TraceTag, 0, "preferred"}};
          take(validate_protocol_package(p));
          ProtocolEngine e({}, {}, 1, 1, 8);
          take(e.install_package(p));
          auto h = take(e.create_ledger(ConnectionId{1}, TransportId{1}));
          WireCall c;
          c.id = CallId{1};
          c.connection = ConnectionId{1};
          c.transport = TransportId{1};
          c.phase = PhaseId{0};
          auto t = take(e.begin_call(h, c));
          auto out = take(e.end_call(t, {Sync::Accepted, {}, {}, {}}));
          require(out.trace_tags == std::vector<std::string>{"preferred"},
                  "priority winner incorrect");
          return "overlap absent/equal priorities rejected; explicit priority0 selected preferred "
                 "once";
        });
  r.run("E-T30", "managed WRITE admitted at0, service3; invalidate at1 and replace backing before3",
        "old backing pinned until old completion; new backing untouched", [] {
          ResultStore results;
          ManagedAccessManager manager(results, {}, true);
          auto old = std::make_shared<Bytes>(4, 0);
          std::weak_ptr<Bytes> weak = old;
          ManagedRegionDesc region;
          region.id = RegionId{1};
          region.end = 3;
          region.backing = old;
          region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{3}; };
          take(manager.add_region(region));
          ManagedRequest request;
          request.region = RegionId{1};
          request.end = 3;
          auto lease = take(manager.request(request));
          auto op = take(manager.begin({lease, Command::Write, 0, 1, {9}, {}}));
          take(manager.advance(Tick{}));
          take(manager.invalidate(RegionId{1}, 0, 3, Tick{1}));
          take(manager.advance(Tick{1}));
          auto replacement = std::make_shared<Bytes>(4, 0);
          take(manager.replace_backing(RegionId{1}, replacement, 2));
          region.backing.reset();
          old.reset();
          require(!weak.expired() && manager.backing_pins() == 1, "old backing unpinned early");
          take(manager.advance(Tick{3}));
          require(weak.expired() && manager.backing_pins() == 0 && (*replacement)[0] == 0,
                  "old completion touched replacement or leaked pin");
          require(take(manager.completion(op)).disposition == CommitDisposition::WriteCommitted,
                  "admitted old write not committed");
          return "old backing retained through3 then released; new byte0 remains0; old operation "
                 "WriteCommitted";
        });
  r.run("E-T31", "shared Resource: AT reserve arrival0 service3; managed WRITE arrival1 service2",
        "managed write commits5, not3; shared Memory visible only at5", [] {
          ResultStore results;
          ManagedAccessManager manager(results, {}, true);
          auto memory = std::make_shared<Memory>(take(Memory::make(4)));
          auto resource =
              take(Resource::make({ResourceKind::Serial, 1, Duration{}, Duration{}, 8}, {}, 7));
          take(manager.bind_shared_resource(resource));
          ManagedRegionDesc region;
          region.id = RegionId{1};
          region.end = 3;
          region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{2}; };
          take(manager.add_memory_region(region, memory));
          ManagedRequest request;
          request.region = RegionId{1};
          request.end = 3;
          auto lease = take(manager.request(request));
          ExecutionContext context;
          EventTxn at({}, context);
          Handle owner{HandleKind::Transaction, {}, 1, 0, 1, 0};
          auto reservation = take(resource.reserve(at, owner, Tick{}, Duration{3}));
          take(at.commit());
          auto op = take(manager.begin({lease, Command::Write, 0, 1, {6}, Tick{1}}));
          take(manager.advance(Tick{4}));
          require(memory->data()[0] == 0, "managed bypassed shared AT service");
          take(manager.advance(Tick{5}));
          require(memory->data()[0] == 6 && take(manager.completion(op)).ready.time == Tick{5},
                  "shared commit time wrong");
          context.ready = {Tick{5}, 0};
          EventTxn finish({}, context);
          take(resource.complete(finish, reservation.ticket, Tick{5}));
          take(finish.commit());
          return "shared resource AT[0,3), managed[3,5); Memory byte0 commits6 at5";
        });
  r.run("E-T32", "fresh managed writes cancelled before admission, after admission, after commit",
        "first two NotCommitted without writes; completed write remains WriteCommitted", [] {
          for (unsigned stage = 0; stage < 3; ++stage) {
            ResultStore results;
            ManagedAccessManager manager(results, {}, true);
            auto bytes = std::make_shared<Bytes>(2, 0);
            ManagedRegionDesc region;
            region.id = RegionId{1};
            region.end = 1;
            region.backing = bytes;
            region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{2}; };
            take(manager.add_region(region));
            ManagedRequest request;
            request.region = RegionId{1};
            request.end = 1;
            auto lease = take(manager.request(request));
            auto op = take(manager.begin({lease, Command::Write, 0, 1, {7}, Tick{1}}));
            if (stage)
              take(manager.advance(Tick{1}));
            if (stage == 2)
              take(manager.advance(Tick{3}));
            take(manager.cancel(op));
            take(manager.advance(Tick{4}));
            auto outcome = take(manager.completion(op));
            require((*bytes)[0] == (stage == 2 ? 7 : 0), "cancel write commit mismatch");
            require(outcome.disposition == (stage == 2 ? CommitDisposition::WriteCommitted
                                                       : CommitDisposition::NotCommitted),
                    "cancel disposition mismatch");
            require(manager.backing_pins() == 0, "cancel pin leak");
          }
          return "before/admitted cancellations leave byte0 and NotCommitted; postcommit keeps "
                 "byte7/WriteCommitted; pins0";
        });
  r.run("E-T34", "managed region requests timing_strict=true plus raw_alias=true",
        "configuration rejected rather than claiming exclusive timing", [] {
          ResultStore results;
          ManagedAccessManager manager(results, {}, true);
          ManagedRegionDesc region;
          region.id = RegionId{1};
          region.end = 1;
          region.backing = std::make_shared<Bytes>(2);
          region.raw_alias = true;
          region.timing_strict = true;
          region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{1}; };
          require(!manager.add_region(region), "strict raw alias accepted");
          return "timing_strict/raw_alias combination rejected at region configuration";
        });
  r.run("C-T29",
        "Runtime CrossbarSession: upstream1 address1080 and upstream3 address1040; downstream "
        "completes transaction2 at1 before1 at7",
        "responses route2then1 with ingress addresses restored and disabled READ bytes preserved; "
        "routes/drains empty",
        [] {
          conformance_ooo_routes();
          return "downstream addresses128/64; OOO completions2then1; upstream addresses1080/1040; "
                 "bytes[7,0,7,0]; routes0,drain responsibilities0";
        });
  r.run("C-T14", "READ accepted0; local timeout2; late response7",
        "local Timeout preserved; runtime drains exactly one ACK",
        [] { return conformance_late_response(false); });
  r.run("E-T09", "WRITE accepted0; timeout2; late response7",
        "no duplicate WRITE; immutable local timeout; wire ACK cleanup",
        [] { return conformance_late_response(true); });
  r.run("C-T17", "process repeatedly awaits already-ready deadline0, perTickBudget4, pumpBudget1",
        "bounded zero-time progression and deterministic stop before iteration5",
        [] { return conformance_zero_time(false); });
  r.run("E-T36", "empty-All causal event chain tick0 turns0.., perTickBudget4, pumpBudget1",
        "closed-batch causal turn order, yield per pump and bounded zero-time stop",
        [] { return conformance_zero_time(true); });
  r.run("E-T01", "completed task and COMPLETED response at2; wait registration3",
        "both late registrations resolve once without lost wakeup", conformance_latched_sources);
  r.run("E-T05",
        "All [cancelled source,error,duplicate cancelled source]; duplicate notification; empty "
        "All loop",
        "declaration order, independent source pins, collectAll terminal policy, bounded empty "
        "progress",
        conformance_all_matrix);
  r.run("C-T26",
        "valid serialized Bool state; independently re-sign invalid version/type enum/value "
        "tag/type index",
        "all malformed descriptors rejected by load before any execution", [] {
          leanat::exec::Project p;
          p.types = {{leanat::exec::TypeKind::Bool}};
          p.state_types = {0};
          p.initial_state = {Value{true}};
          auto valid = take(leanat::exec::serialize(take(leanat::exec::validate(p))));
          take(leanat::exec::load_descriptor(valid));
          auto type_start = 104 + 4 + p.profile.size() + 4;
          auto state_index = type_start + 17 + 4;
          auto value_tag = state_index + 4 + 4;
          require(valid.at(type_start) == static_cast<unsigned>(leanat::exec::TypeKind::Bool) &&
                      valid.at(value_tag) == 1,
                  "fixture wire offsets not matching schema");
          for (auto offset : {std::size_t{4}, type_start, state_index, value_tag}) {
            auto corrupt = valid;
            corrupt.at(offset) = 255;
            std::fill(corrupt.begin() + 72, corrupt.begin() + 104, 0);
            auto digest = leanat::exec::sha256(corrupt);
            std::copy(digest.begin(), digest.end(), corrupt.begin() + 72);
            require(!leanat::exec::load_descriptor(corrupt),
                    "resigned malformed descriptor accepted");
          }
          return "positive canonical descriptor loads; re-signed version/type/tag/index "
                 "corruptions each rejected before ValidatedProject creation";
        });
  r.run(
      "C-T19", "peer performs observable WRITE then returns illegal BEGIN_REQ UPDATED END_RESP",
      "fatal contract failure retained; external WRITE prefix is not rolled back", [] {
        struct ViolatingHost : ConformanceLifecycleHost {
          unsigned external_value{};
          Expected<WireReturn> transport(const SendIntent &i) override {
            take(ConformanceLifecycleHost::transport(i));
            external_value = 77;
            return WireReturn{Sync::Updated, end_resp, {}, {}};
          }
        } host;
        RuntimeConfig config;
        config.descriptor_identity = "violating-peer";
        config.connections = {ConnectionId{1}};
        Runtime runtime(config, host);
        host.runtime = &runtime;
        take(runtime.start({{}, config.descriptor_identity, config.connections, true, true}));
        auto ledger = take(runtime.protocol().create_ledger(ConnectionId{1}, TransportId{1}));
        SendIntent intent;
        intent.connection = ConnectionId{1};
        intent.transport = TransportId{1};
        intent.call_id = take(runtime.allocate_call_id(CallOrigin::Outgoing));
        intent.payload.command = Command::Write;
        intent.payload.data = {77};
        intent.payload.streaming_width = 1;
        take(runtime.publish(intent));
        auto pumped = runtime.pump_batch(Tick{}, 4);
        require(runtime.stopped() && !runtime.stop_detail().empty() &&
                    take(runtime.protocol().inspect(ledger)).faulted,
                "contract failure not retained");
        require(host.external_value == 77 && host.requests == 1,
                "external side effect rolled back or repeated");
        require(std::any_of(host.traces.begin(),host.traces.end(),[&](const TraceEvent& event){return event.kind=="runtime.stopped"&&event.detail==runtime.stop_detail();}),"fatal stop TraceEvent absent");
        require(!runtime.next_wakeup(), "fatal runtime still armed");
        return "fatal detail=" + runtime.stop_detail() +
               "; faulted ledger retained; peer external_value77, requestCalls1; dispatch stopped";
      });
  r.run(
      "C-T27",
      "test-only monotone seed positions EventQueue generation/sequence/batch at MAX-1; production "
      "allocation paths and checked time/value arithmetic",
      "no counter wraps or old-handle revival; exhausted slot retires; arithmetic overflows "
      "explicitly",
      [] {
        using Access = leanat::testing::EventQueueTestAccess;
        auto finish = [](EventQueue &q, Tick time) {
          auto batch = take(q.pop_batch(time, 1));
          require(batch.has_value(), "no boundary event");
          take(q.ack_executed(batch->id, batch->members[0].token, ExecutionDisposition::Committed));
          take(q.resolver_step(batch->id, 1, 0));
          take(q.finish_batch(batch->id));
        };
        EventQueue generation(1);
        take(Access::seed_free_generation(generation, 0, UINT64_MAX - 1));
        auto final = take(generation.enqueue(EventDraft{}));
        require(final.generation == UINT64_MAX, "boundary generation not exercised");
        finish(generation, Tick{});
        EventDraft later;
        later.key.time = Tick{1};
        require(!generation.enqueue(later) && Access::retired(generation, 0) &&
                    !generation.cancel(final),
                "generation wrapped or old event revived");
        EventQueue sequence(2);
        take(Access::seed_next_sequence(sequence, UINT64_MAX - 1));
        take(sequence.enqueue(EventDraft{}));
        auto overflow = sequence.enqueue(later);
        require(!overflow && overflow.error().code == ErrorCode::Overflow &&
                    Access::next_sequence(sequence) == UINT64_MAX,
                "sequence wrapped");
        EventQueue batch(2);
        take(Access::seed_next_batch(batch, UINT64_MAX - 1));
        take(batch.enqueue(EventDraft{}));
        finish(batch, Tick{});
        take(batch.enqueue(later));
        auto batch_overflow = batch.pop_batch(Tick{1}, 1);
        require(!batch_overflow && batch_overflow.error().code == ErrorCode::Overflow &&
                    Access::next_batch(batch) == UINT64_MAX,
                "batch counter wrapped");
        require(!add_time(Tick{UINT64_MAX}, Duration{1}) && !checked_add(UINT64_MAX, 1) &&
                    !checked_mul(UINT64_MAX, 2),
                "checked arithmetic wrapped");
        return "MAX generation allocated once then slot retired; stale final handle rejected; "
               "sequence/batch stay MAX and return Overflow; time/add/mul overflow rejected";
      });
  r.run("E-T10",
        "one active task frame bound to real parent process; parent requests child slot in same "
        "pool; unrelated waiter control",
        "closed parent-frame dependency rejected before suspension; ordinary external capacity "
        "wait remains legal",
        [] {
          ResultStore results(16, 32, 16);
          TaskPoolDesc desc;
          desc.max_instances = 1;
          desc.task_capacity = 4;
          desc.result_capacity = 4;
          desc.overflow = TaskOverflow::AwaitSlot;
          desc.waiter_limit = 2;
          TaskPool tasks({}, 21, desc, results);
          ProcessStore processes({}, 22, 2, 2, 16);
          Handle scope{HandleKind::Scope, {}, 23, 0, 1, 0};
          auto parent = take(processes.create(ProgramId{1}, 1));
          auto observer = take(processes.create(ProgramId{2}, 2));
          take(processes.begin(parent));
          take(processes.begin(observer));
          auto task = take(tasks.try_spawn({}, scope));
          take(tasks.bind_execution(task, parent));
          auto ticket = take(tasks.request_slot(parent, scope));
          require(take(tasks.ticket_info(ticket)).state == SpawnTicketState::Pending,
                  "recursive slot unexpectedly granted");
          auto rejected = tasks.begin_slot_wait(ticket, parent, scope);
          require(!rejected && rejected.error().code == ErrorCode::InvalidState &&
                      rejected.error().message.find("TaskSlotDeadlock") == 0,
                  "closed frame dependency undiagnosed");
          require(take(processes.inspect(parent)).state == ProcessState::Executing &&
                      tasks.active_frames() == 1,
                  "deadlock check suspended parent or lost frame");
          auto external = take(tasks.request_slot(observer, scope));
          take(tasks.begin_slot_wait(external, observer, scope));
          require(take(tasks.ticket_info(external)).state == SpawnTicketState::Pending,
                  "ordinary external wait mutated unexpectedly");
          take(tasks.cancel_ticket(ticket, parent, scope));
          take(tasks.cancel_ticket(external, observer, scope));
          return "TaskSlotDeadlock diagnosed from actual parent/task binding; parent remains "
                 "Executing/frame1; unrelated process Pending wait accepted and cancellable";
        });
  return r.failures ? 1 : 0;
}

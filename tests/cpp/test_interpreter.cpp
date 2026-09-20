#include "leanat/descriptor.hpp"
#include "leanat/interpreter.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iterator>
using namespace leanat;
using namespace leanat::exec;
struct EvidenceObserver : InterpreterObserver {
  struct Row {
    OpcodeObservation::Stage stage;
    Op op;
    std::uint32_t block;
    std::uint64_t before, after;
    std::vector<Value> args;
    std::optional<Value> result;
    std::optional<Error> error;
  };
  std::vector<Row> rows;
  bool throw_on_record{}, capacity_failure{};
  bool on_opcode(const OpcodeObservation &event) override {
    if (throw_on_record)
      throw 42;
    if (capacity_failure)
      return false;
    Row row{event.stage,
            event.instruction.op,
            event.block_id,
            event.fuel_before,
            event.fuel_after,
            {},
            {},
            {}};
    for (std::size_t n = 0; n < event.instruction.args.size(); ++n)
      row.args.push_back(event.argument(n));
    if (event.result)
      row.result = *event.result;
    if (event.error)
      row.error = *event.error;
    rows.push_back(std::move(row));
    return true;
  }
};
struct TestBackend : InterpreterBackend {
  mutable unsigned checked{};
  unsigned invoked{};
  bool throw_nonstandard{};
  VersionedCell cell{Value{std::uint64_t{0}}};
  Expected<void> check_signature(const ServiceSignature &s) const override {
    ++checked;
    if (s.provider_key != "test" || s.provider_version != "1" || s.op != Op::ObjectCall ||
        s.extra_fuel != 2 || s.effect_mask != Effect::Object)
      return fail(ErrorCode::Integrity, "provider metadata mismatch");
    return {};
  }
  Expected<std::vector<Value>> invoke(const ServiceSignature &, const std::vector<Value> &v,
                                      const ExecutionContext &, EventTxn &txn) override {
    ++invoked;
    auto staged = txn.buffer(cell, v[0]);
    if (!staged)
      return staged.error();
    if (throw_nonstandard)
      throw 17;
    return std::vector<Value>{v[0]};
  }
};
struct ContinuationBackend : InterpreterBackend {
  VersionedCell frame{Value{std::uint64_t{0}}};
  Expected<void> check_signature(const ServiceSignature &) const override {
    return {};
  }
  Expected<std::vector<Value>> invoke(const ServiceSignature &, const std::vector<Value> &,
                                      const ExecutionContext &, EventTxn &) override {
    return fail(ErrorCode::Unsupported, "no services");
  }
  Expected<void> prepare_suspend(Handle, BlockId block, std::vector<Value> live, TypeId type,
                                 const ExecutionContext &, EventTxn &txn) override {
    if (block.value != 1 || live.size() != 1 || type.value != 2)
      return fail(ErrorCode::Schema, "frame shape");
    return txn.buffer(frame, live[0]);
  }
  Expected<ResumeInput> prepare_resume(const SuspensionToken &token, const ExecutionContext &,
                                       EventTxn &txn) override {
    if (token.ordinal != 1)
      return fail(ErrorCode::StaleHandle, "suspension ordinal");
    auto old = txn.read(frame);
    if (!old)
      return old.error();
    auto staged = txn.buffer(frame, Value{std::uint64_t{99}});
    if (!staged)
      return staged.error();
    return ResumeInput{ProgramId{0}, BlockId{1}, {Value{true}, old.value()}};
  }
};
static Project arithmetic(Binary op) {
  Project p;
  p.types = {{TypeKind::Bits, 8}, {TypeKind::Bool}};
  Program pr;
  pr.input_types = {0, 0};
  pr.result_types = {0};
  Block b;
  b.parameters = {{0, 0}, {1, 0}};
  Instruction i;
  i.op = Op::Binary;
  i.args = {{0, 0}, {1, 0}};
  i.dest = Reg{2, 0};
  i.binary = op;
  b.instructions = {i};
  b.terminator.values = {{2, 0}};
  pr.blocks = {b};
  p.programs = {pr};
  return p;
}
int main(int argc, char **argv) {
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary);
    LEANAT_CHECK(file);
    Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    auto p = load_descriptor(bytes);
    LEANAT_CHECK(p);
    std::vector<VersionedCell> cells;
    for (auto &v : p.value().get().initial_state)
      cells.push_back(VersionedCell{v});
    std::vector<VersionedCell *> refs;
    for (auto &c : cells)
      refs.push_back(&c);
    ExecutionContext ctx;
    Interpreter vm(p.value(), refs);
    EventTxn tx({}, ctx);
    FuelCounter f{6};
    auto r = vm.execute_segment(0, ctx, {}, tx, f);
    if (!r)
      std::cerr << r.error().message << '\n';
    LEANAT_CHECK(r && f.remaining == 0 &&
                 r.value().values == std::vector<Value>{Value{std::uint64_t{42}}});
    LEANAT_CHECK(tx.commit() && cells[0].value == Value{std::uint64_t{42}});
    cells[0].value = Value{std::uint64_t{0}};
    EventTxn shorttx({}, ctx);
    FuelCounter shortfuel{5};
    auto shortresult = vm.execute_segment(0, ctx, {}, shorttx, shortfuel);
    LEANAT_CHECK(!shortresult && shortresult.error().code == ErrorCode::FuelExhausted &&
                 !shorttx.commit() && cells[0].value == Value{std::uint64_t{0}});
    return 0;
  }
  ExecutionContext ctx;
  {
    Project diamond;
    diamond.types = {{TypeKind::Bool}, {TypeKind::Bits, 8}};
    Program program;
    program.input_types = {0};
    program.result_types = {1};
    Block entry;
    entry.parameters = {{0, 0}};
    entry.terminator.kind = TermKind::Branch;
    entry.terminator.value = {0, 0};
    entry.terminator.yes = {1, {}};
    entry.terminator.no = {2, {}};
    program.blocks.push_back(entry);
    for (std::uint32_t branch = 1; branch <= 2; ++branch) {
      Block block;
      block.id = branch;
      const auto base = 1 + (branch - 1) * 3;
      Instruction first;
      first.op = Op::Const;
      first.dest = Reg{base, 1};
      first.value = Value{std::uint64_t{branch == 1 ? 1u : 255u}};
      Instruction second = first;
      second.dest = Reg{base + 1, 1};
      second.value = Value{std::uint64_t{1}};
      Instruction sum;
      sum.op = Op::Binary;
      sum.binary = Binary::AddChecked;
      sum.args = {{base, 1}, {base + 1, 1}};
      sum.dest = Reg{base + 2, 1};
      block.instructions = {first, second, sum};
      block.terminator.values = {{base + 2, 1}};
      program.blocks.push_back(block);
    }
    diamond.programs = {program};
    auto checked = validate(diamond);
    LEANAT_CHECK(checked);
    EvidenceObserver observer;
    Interpreter observed(checked.value(), {}, nullptr, &observer);
    EventTxn good_txn({}, ctx);
    FuelCounter good_fuel{5};
    auto good = observed.execute_segment(0, ctx, {Value{true}}, good_txn, good_fuel);
    LEANAT_CHECK(good && good_fuel.remaining == 0 && observer.complete() &&
                 observer.rows.size() == 6);
    for (const auto &row : observer.rows)
      LEANAT_CHECK(row.block == 1);
    LEANAT_CHECK(observer.rows.back().stage == OpcodeObservation::Stage::Completed &&
                 observer.rows.back().args ==
                     std::vector<Value>({Value{std::uint64_t{1}}, Value{std::uint64_t{1}}}) &&
                 observer.rows.back().result == std::optional<Value>{Value{std::uint64_t{2}}} &&
                 observer.rows.back().before == 2 && observer.rows.back().after == 1);
    EvidenceObserver failed;
    Interpreter failing(checked.value(), {}, nullptr, &failed);
    EventTxn failed_txn({}, ctx);
    FuelCounter failed_fuel{5};
    auto failure = failing.execute_segment(0, ctx, {Value{false}}, failed_txn, failed_fuel);
    LEANAT_CHECK(!failure && failure.error().code == ErrorCode::Overflow &&
                 failed_fuel.remaining == 1 && !failed_txn.commit() && failed.complete() &&
                 failed.rows.size() == 6);
    for (const auto &row : failed.rows)
      LEANAT_CHECK(row.block == 2);
    LEANAT_CHECK(failed.rows.back().stage == OpcodeObservation::Stage::Error &&
                 failed.rows.back().error->message == failure.error().message &&
                 !failed.rows.back().result);
    for (bool throwing : {false, true}) {
      EvidenceObserver broken;
      broken.throw_on_record = throwing;
      broken.capacity_failure = !throwing;
      Interpreter logging_failure(checked.value(), {}, nullptr, &broken);
      EventTxn log_txn({}, ctx);
      FuelCounter log_fuel{5};
      auto logged = logging_failure.execute_segment(0, ctx, {Value{true}}, log_txn, log_fuel);
      LEANAT_CHECK(logged && logged.value().values == good.value().values &&
                   log_fuel.remaining == good_fuel.remaining && !broken.complete() &&
                   log_txn.commit());
    }
    Interpreter plain(checked.value());
    EventTxn plain_txn({}, ctx);
    FuelCounter plain_fuel{5};
    auto plain_result = plain.execute_segment(0, ctx, {Value{false}}, plain_txn, plain_fuel);
    LEANAT_CHECK(!plain_result && plain_result.error().code == failure.error().code &&
                 plain_result.error().message == failure.error().message &&
                 plain_fuel.remaining == failed_fuel.remaining);
  }
  {
    auto traced = arithmetic(Binary::AddWrap);
    Instruction trace;
    trace.op = Op::Trace;
    trace.text = "sum";
    trace.args = {{2, 0}};
    traced.programs[0].blocks[0].instructions.push_back(trace);
    auto validated = validate(traced);
    LEANAT_CHECK(validated);
    Interpreter traced_vm(validated.value());
    ExecutionContext trace_context;
    trace_context.ready = {Tick{11}, 2};
    trace_context.instance = InstanceId{1};
    trace_context.connection = ConnectionId{2};
    EventTxn trace_txn({}, trace_context);
    FuelCounter trace_fuel{3};
    auto traced_result = traced_vm.execute_segment(
        0, trace_context, {Value{std::uint64_t{255}}, Value{std::uint64_t{2}}}, trace_txn,
        trace_fuel);
    LEANAT_CHECK(traced_result && traced_result.value().kind == SegmentResult::Kind::Returned &&
                 traced_result.value().traces.size() == 1 && trace_fuel.remaining == 0);
    const auto &event = traced_result.value().traces[0];
    LEANAT_CHECK(event.kind == "sum" && event.ready == trace_context.ready &&
                 event.instance == InstanceId{1} && event.connection == ConnectionId{2} &&
                 event.values == std::vector<Value>{Value{std::uint64_t{1}}});
    LEANAT_CHECK(trace_txn.commit());
  }
  auto p = validate(arithmetic(Binary::AddWrap));
  LEANAT_CHECK(p);
  Interpreter vm(p.value());
  EventTxn tx({}, ctx);
  FuelCounter f{2};
  auto result =
      vm.execute_segment(0, ctx, {Value{std::uint64_t{255}}, Value{std::uint64_t{2}}}, tx, f);
  LEANAT_CHECK(result && f.remaining == 0 &&
               std::get<std::uint64_t>(result.value().values[0].data) == 1);
  LEANAT_CHECK(tx.commit());
  EventTxn shorttx({}, ctx);
  FuelCounter shortfuel{1};
  auto exhausted = vm.execute_segment(0, ctx, {Value{std::uint64_t{1}}, Value{std::uint64_t{2}}},
                                      shorttx, shortfuel);
  LEANAT_CHECK(!exhausted && exhausted.error().code == ErrorCode::FuelExhausted);
  LEANAT_CHECK(!shorttx.commit());
  auto checked = validate(arithmetic(Binary::AddChecked));
  LEANAT_CHECK(checked);
  Interpreter cvm(checked.value());
  EventTxn ct({}, ctx);
  FuelCounter cf{2};
  auto overflow =
      cvm.execute_segment(0, ctx, {Value{std::uint64_t{255}}, Value{std::uint64_t{1}}}, ct, cf);
  LEANAT_CHECK(!overflow && overflow.error().code == ErrorCode::Overflow);
  auto malformed = arithmetic(Binary::AddWrap);
  malformed.programs[0].blocks[0].instructions[0].args[0].id = 100;
  LEANAT_CHECK(!validate(malformed));
  malformed = arithmetic(Binary::AddWrap);
  malformed.programs[0].blocks[0].instructions[0].dest->type = 1;
  LEANAT_CHECK(!validate(malformed));
  Project loop;
  loop.types = {{TypeKind::Bits, 64}};
  Program lp;
  lp.input_types = {0, 0};
  lp.result_types = {0, 0};
  Block entry;
  entry.parameters = {{0, 0}, {1, 0}};
  entry.terminator.kind = TermKind::Jump;
  entry.terminator.yes = {1, {{1, 0}, {0, 0}}};
  Block exit;
  exit.id = 1;
  exit.parameters = {{2, 0}, {3, 0}};
  exit.terminator.values = {{2, 0}, {3, 0}};
  lp.blocks = {entry, exit};
  loop.programs = {lp};
  auto swap = validate(loop);
  LEANAT_CHECK(swap);
  Interpreter svm(swap.value());
  EventTxn st({}, ctx);
  FuelCounter sf{2};
  auto sr = svm.execute_segment(0, ctx, {Value{std::uint64_t{4}}, Value{std::uint64_t{7}}}, st, sf);
  LEANAT_CHECK(sr && std::get<std::uint64_t>(sr.value().values[0].data) == 7 &&
               std::get<std::uint64_t>(sr.value().values[1].data) == 4);
  loop.programs[0].blocks[1].terminator.kind = TermKind::Jump;
  loop.programs[0].blocks[1].terminator.yes = {1, {{3, 0}, {2, 0}}};
  auto infinite = validate(loop);
  LEANAT_CHECK(infinite);
  Interpreter ivm(infinite.value());
  EventTxn it({}, ctx);
  FuelCounter inf{100};
  auto ir =
      ivm.execute_segment(0, ctx, {Value{std::uint64_t{4}}, Value{std::uint64_t{7}}}, it, inf);
  LEANAT_CHECK(!ir && ir.error().code == ErrorCode::FuelExhausted && inf.remaining == 0);
  Project writes;
  writes.types = {{TypeKind::Bits, 64}};
  writes.state_types = {0};
  writes.initial_state = {Value{std::uint64_t{9}}};
  Program wp;
  wp.input_types = {0};
  Block wb;
  wb.parameters = {{0, 0}};
  Instruction wi;
  wi.op = Op::BufferStateWrite;
  wi.args = {{0, 0}};
  wb.instructions = {wi};
  wb.terminator.kind = TermKind::Fail;
  wb.terminator.error = "abort";
  wp.blocks = {wb};
  writes.programs = {wp};
  auto wv = validate(writes);
  LEANAT_CHECK(wv);
  VersionedCell cell{Value{std::uint64_t{9}}};
  Interpreter wvm(wv.value(), {&cell});
  EventTxn wt({}, ctx);
  FuelCounter wf{2};
  auto wr = wvm.execute_segment(0, ctx, {Value{std::uint64_t{11}}}, wt, wf);
  LEANAT_CHECK(wr && wr.value().kind == SegmentResult::Kind::Failed &&
               std::get<std::uint64_t>(cell.value.data) == 9 && !wt.commit());
  Project cycle;
  cycle.types = {{TypeKind::Record, 0, {0}}};
  LEANAT_CHECK(!validate(cycle));
  auto unsupported = arithmetic(Binary::AddWrap);
  unsupported.programs[0].blocks[0].instructions[0].op = Op::SpawnProcess;
  auto un = validate(unsupported);
  LEANAT_CHECK(!un && un.error().code == ErrorCode::Unsupported);
  auto service = arithmetic(Binary::AddWrap);
  service.schema_major = 2;
  auto &spr = service.programs[0];
  spr.effect_mask = Effect::Object;
  auto &instruction = spr.blocks[0].instructions[0];
  instruction.op = Op::ObjectCall;
  instruction.args.resize(1);
  instruction.immediate = 7;
  ServiceSignature sig;
  sig.id = 7;
  sig.op = Op::ObjectCall;
  sig.input_types = {0};
  sig.result_types = {0};
  sig.effect_mask = Effect::Object;
  sig.extra_fuel = 2;
  sig.provider_key = "test";
  sig.provider_version = "1";
  service.services = {sig};
  auto sv = validate(service);
  LEANAT_CHECK(sv);
  TestBackend backend;
  Interpreter servicevm(sv.value(), {}, &backend);
  EventTxn service_tx({}, ctx);
  FuelCounter service_fuel{4};
  auto service_result = servicevm.execute_segment(
      0, ctx, {Value{std::uint64_t{4}}, Value{std::uint64_t{8}}}, service_tx, service_fuel);
  LEANAT_CHECK(service_result && backend.checked == 1 && backend.invoked == 1 &&
               service_fuel.remaining == 0 && backend.cell.value == Value{std::uint64_t{0}});
  LEANAT_CHECK(service_tx.commit() && backend.cell.value == Value{std::uint64_t{4}});
  backend.throw_nonstandard = true;
  EventTxn throw_tx({}, ctx);
  FuelCounter throw_fuel{9};
  auto thrown = servicevm.execute_segment(
      0, ctx, {Value{std::uint64_t{5}}, Value{std::uint64_t{8}}}, throw_tx, throw_fuel);
  LEANAT_CHECK(!thrown && thrown.error().code == ErrorCode::ExternalFailure && !throw_tx.commit() &&
               backend.cell.value == Value{std::uint64_t{4}});
  backend.throw_nonstandard = false;
  EventTxn service_short({}, ctx);
  FuelCounter only_op{2};
  auto refused = servicevm.execute_segment(
      0, ctx, {Value{std::uint64_t{5}}, Value{std::uint64_t{8}}}, service_short, only_op);
  LEANAT_CHECK(!refused && refused.error().code == ErrorCode::FuelExhausted &&
               backend.invoked == 2);
  service.programs[0].effect_mask = 0;
  LEANAT_CHECK(!validate(service));
  service.programs[0].effect_mask = Effect::Object;
  service.services[0].provider_version = "untrusted";
  auto mismatched = validate(service);
  LEANAT_CHECK(mismatched);
  Interpreter badvm(mismatched.value(), {}, &backend);
  EventTxn badtx({}, ctx);
  FuelCounter badfuel{9};
  auto refused_binding = badvm.execute_segment(
      0, ctx, {Value{std::uint64_t{5}}, Value{std::uint64_t{8}}}, badtx, badfuel);
  LEANAT_CHECK(!refused_binding && refused_binding.error().code == ErrorCode::Integrity &&
               badfuel.remaining == 9);
  auto pure_project = arithmetic(Binary::AddWrap);
  pure_project.schema_major = 2;
  auto callee = pure_project.programs[0];
  callee.id = 1;
  pure_project.programs.push_back(callee);
  pure_project.programs[0].blocks[0].instructions[0].op = Op::CallPure;
  pure_project.programs[0].blocks[0].instructions[0].immediate = 1;
  auto pure_valid = validate(pure_project);
  LEANAT_CHECK(pure_valid);
  Interpreter pure_vm(pure_valid.value());
  EventTxn pure_tx({}, ctx);
  FuelCounter pure_fuel{4};
  auto pure_result = pure_vm.execute_segment(
      0, ctx, {Value{std::uint64_t{250}}, Value{std::uint64_t{10}}}, pure_tx, pure_fuel);
  LEANAT_CHECK(pure_result && pure_fuel.remaining == 0 &&
               pure_result.value().values[0] == Value{std::uint64_t{4}});
  EventTxn pure_short({}, ctx);
  FuelCounter pure_short_fuel{3};
  auto pure_limit = pure_vm.execute_segment(
      0, ctx, {Value{std::uint64_t{250}}, Value{std::uint64_t{10}}}, pure_short, pure_short_fuel);
  LEANAT_CHECK(!pure_limit && pure_limit.error().code == ErrorCode::FuelExhausted);
  pure_project.programs[1].blocks[0].instructions[0].op = Op::CallPure;
  pure_project.programs[1].blocks[0].instructions[0].immediate = 0;
  LEANAT_CHECK(!validate(pure_project));
  auto run_numeric = [&](Project numeric_project, std::uint64_t x, std::uint64_t y) {
    auto valid = validate(std::move(numeric_project));
    LEANAT_CHECK(valid);
    Interpreter numeric_vm(valid.value());
    EventTxn numeric_tx({}, ctx);
    FuelCounter numeric_fuel{2};
    return numeric_vm.execute_segment(0, ctx, {Value{x}, Value{y}}, numeric_tx, numeric_fuel);
  };
  auto numeric_project = arithmetic(Binary::AddWrap);
  numeric_project.schema_major = 2;
  auto &unary = numeric_project.programs[0].blocks[0].instructions[0];
  unary.op = Op::Unary;
  unary.immediate = 1;
  unary.args.resize(1);
  auto complement = run_numeric(numeric_project, 0, 0);
  LEANAT_CHECK(complement && complement.value().values[0] == Value{std::uint64_t{255}});
  numeric_project = arithmetic(Binary::AddWrap);
  numeric_project.schema_major = 2;
  auto &compare = numeric_project.programs[0].blocks[0].instructions[0];
  compare.op = Op::Compare;
  compare.immediate = 5;
  compare.dest = Reg{2, 1};
  numeric_project.programs[0].result_types = {1};
  numeric_project.programs[0].blocks[0].terminator.values = {{2, 1}};
  auto ordered = run_numeric(numeric_project, 7, 3);
  LEANAT_CHECK(ordered && ordered.value().values[0] == Value{true});
  numeric_project = arithmetic(Binary::AddWrap);
  numeric_project.schema_major = 2;
  numeric_project.types.push_back({TypeKind::Bits, 16});
  auto &convert = numeric_project.programs[0].blocks[0].instructions[0];
  convert.op = Op::Convert;
  convert.args.resize(1);
  convert.dest = Reg{2, 2};
  numeric_project.programs[0].result_types = {2};
  numeric_project.programs[0].blocks[0].terminator.values = {{2, 2}};
  auto widened = run_numeric(numeric_project, 255, 0);
  LEANAT_CHECK(widened && widened.value().values[0] == Value{std::uint64_t{255}});
  numeric_project.types[0].bound = 16;
  numeric_project.types[2].bound = 8;
  numeric_project.programs[0].blocks[0].instructions[0].immediate = 2;
  auto narrowed = run_numeric(numeric_project, 256, 0);
  LEANAT_CHECK(!narrowed && narrowed.error().code == ErrorCode::Overflow);
  Project suspended;
  suspended.schema_major = 3;
  suspended.types = {{TypeKind::Handle, static_cast<unsigned>(HandleKind::Wait)},
                     {TypeKind::Bits, 64},
                     {TypeKind::Bool}};
  Program process;
  process.context = ContextKind::Process;
  process.effect_mask = Effect::Wait;
  process.input_types = {0, 1};
  process.result_types = {1};
  process.frame = {{1, 0, 8, 1}};
  process.frame_bytes = 64;
  Block before;
  before.parameters = {{0, 0}, {1, 1}};
  before.terminator.kind = TermKind::Suspend;
  before.terminator.value = {0, 0};
  before.terminator.yes.target = 1;
  before.terminator.values = {{1, 1}};
  Block after;
  after.id = 1;
  after.parameters = {{2, 2}, {3, 1}};
  after.terminator.values = {{3, 1}};
  process.blocks = {before, after};
  suspended.programs = {process};
  auto suspend_valid = validate(suspended);
  LEANAT_CHECK(suspend_valid);
  auto suspend_bytes = serialize(suspend_valid.value());
  LEANAT_CHECK(suspend_bytes && load_descriptor(suspend_bytes.value()));
  ContinuationBackend continuation;
  Interpreter process_vm(suspend_valid.value(), {}, &continuation);
  ExecutionContext pc;
  pc.kind = ContextKind::Process;
  EventTxn suspend_tx({}, pc);
  FuelCounter suspend_fuel{1};
  Handle wait{HandleKind::Wait};
  auto prepared = process_vm.execute_segment(0, pc, {Value{wait}, Value{std::uint64_t{7}}},
                                             suspend_tx, suspend_fuel);
  LEANAT_CHECK(prepared && prepared.value().kind == SegmentResult::Kind::Suspended &&
               continuation.frame.value == Value{std::uint64_t{0}});
  LEANAT_CHECK(suspend_tx.commit() && continuation.frame.value == Value{std::uint64_t{7}});
  EventTxn resume_tx({}, pc);
  FuelCounter resume_fuel{0};
  SuspensionToken token;
  token.process.kind = HandleKind::Process;
  token.wait.kind = HandleKind::Wait;
  token.ordinal = 1;
  auto resume_failed = process_vm.resume_segment(token, pc, resume_tx, resume_fuel);
  LEANAT_CHECK(!resume_failed && resume_failed.error().code == ErrorCode::FuelExhausted &&
               continuation.frame.value == Value{std::uint64_t{7}});
  EventTxn ready_tx({}, pc);
  FuelCounter ready_fuel{1};
  auto ready = process_vm.resume_segment(token, pc, ready_tx, ready_fuel);
  LEANAT_CHECK(ready && ready.value().values[0] == Value{std::uint64_t{7}} && ready_tx.commit());
  suspended.programs[0].blocks[1].terminator.values = {{1, 1}};
  LEANAT_CHECK(!validate(suspended));
}

#include "leanat/event_txn.hpp"
#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;
struct Delta : PreparedParticipant {
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  int &x;
  bool valid;
  Delta(int &n, bool v) : x(n), valid(v) {}
  Expected<void> validate() const override {
    if (!valid)
      return fail(ErrorCode::InvalidState, "conflict");
    return {};
  }
  void apply() noexcept override {
    ++x;
  }
  void discard() noexcept override {}
};
int main() {
  ExecutionContext c;
  VersionedCell wrong_epoch{Value{}, 0, 1};
  EventTxn epoch_check({}, c);
  LEANAT_CHECK(!epoch_check.read(wrong_epoch));
  wrong_epoch.epoch_independent = true;
  LEANAT_CHECK(epoch_check.read(wrong_epoch));
  VersionedCell a{Value(std::uint64_t{1})}, b{Value(std::uint64_t{2})};
  EventQueue q(1);
  EventTxn t({}, c);
  LEANAT_CHECK(t.buffer(a, Value(std::uint64_t{4})));
  LEANAT_CHECK(std::get<std::uint64_t>(t.read(a).value().data) == 4);
  auto sp = t.checkpoint();
  LEANAT_CHECK(sp);
  LEANAT_CHECK(t.buffer(a, Value(std::uint64_t{9})));
  LEANAT_CHECK(t.stage_event(q, {{Tick{2}, 0}, Value{}}));
  LEANAT_CHECK(!t.stage_event(q, {{Tick{3}, 0}, Value{}}));
  LEANAT_CHECK(t.rollback(std::move(sp.value())));
  LEANAT_CHECK(q.occupied() == 0);
  LEANAT_CHECK(std::get<std::uint64_t>(t.read(a).value().data) == 4);
  LEANAT_CHECK(t.buffer(b, Value(std::uint64_t{8})));
  ++b.version;
  LEANAT_CHECK(!t.commit());
  LEANAT_CHECK(std::get<std::uint64_t>(a.value.data) == 1);
  LEANAT_CHECK(t.discard());
  EventTxn ok({}, c);
  LEANAT_CHECK(ok.buffer(a, Value(std::uint64_t{5})));
  int n = 0;
  auto d = std::make_unique<Delta>(n, true);
  auto raw = d.get();
  LEANAT_CHECK(ok.stage_participant(std::move(d)));
  raw->valid = false;
  LEANAT_CHECK(!ok.commit());
  LEANAT_CHECK(n == 0 && std::get<std::uint64_t>(a.value.data) == 1);
  raw->valid = true;
  auto committed = ok.commit();
  LEANAT_CHECK(committed && n == 1);
  LEANAT_CHECK(!ok.commit());
  LEANAT_CHECK(!ok.discard());
  CommittedActions actions{{SendIntent{}, SendIntent{}, SendIntent{}}};
  int calls = 0;
  auto host = [&](const SendIntent &) -> Expected<void> {
    if (++calls == 2)
      return fail(ErrorCode::ExternalFailure, "host");
    return {};
  };
  LEANAT_CHECK(!publish_actions(actions, host));
  LEANAT_CHECK(actions.cursor == 1 && calls == 2);
  LEANAT_CHECK(!publish_actions(actions, host) && calls == 2);
  VersionedCell mem{Value(Bytes{1, 2}), 0, 0, true};
  auto ptr = std::get<Bytes>(mem.value.data).data();
  EventTxn stable({}, c);
  LEANAT_CHECK(stable.buffer(mem, Value(Bytes{3, 4})));
  LEANAT_CHECK(stable.commit());
  LEANAT_CHECK(ptr == std::get<Bytes>(mem.value.data).data());
  ResultStore rs;
  Handle src;
  auto rr = rs.reserve({src});
  EventTxn rt({}, c);
  auto rp = rs.prepare_publish(rr.value().reservation, Value(Bytes{1}), {src, {Tick{8}, 0}, true});
  LEANAT_CHECK(rp);
  LEANAT_CHECK(rt.stage_participant(std::move(rp.value())));
  LEANAT_CHECK(!rs.read(rr.value().consumer));
  LEANAT_CHECK(rt.commit());
  LEANAT_CHECK(rs.read(rr.value().consumer));
  SegmentBudget registry_budget;
  registry_budget.bytes = 1024 * 1024;
  EventTxn transfer(registry_budget, c);
  auto moved = rs.prepare_transfer(transfer, rr.value().consumer, 17);
  LEANAT_CHECK(moved);
  LEANAT_CHECK(rs.read(rr.value().consumer));
  auto save = transfer.checkpoint();
  LEANAT_CHECK(rs.prepare_release(transfer, moved.value()));
  LEANAT_CHECK(!rs.prepare_retain(transfer, moved.value(), 18));
  LEANAT_CHECK(transfer.rollback(std::move(save.value())));
  LEANAT_CHECK(transfer.commit());
  LEANAT_CHECK(!rs.read(rr.value().consumer));
  LEANAT_CHECK(rs.read(moved.value()));
  EventTxn dropped(registry_budget, c);
  auto speculative = rs.prepare_retain(dropped, moved.value(), 19);
  LEANAT_CHECK(speculative && dropped.discard());
  auto fresh = rs.retain(moved.value(), 19);
  LEANAT_CHECK(fresh && fresh.value().consumer != speculative.value().consumer);
  LEANAT_CHECK(!rs.read(speculative.value()));
  PayloadShadow shadows;
  Handle tx{HandleKind::Transaction}, hop{HandleKind::Hop};
  PayloadViewKey target{tx, hop, InstanceId{1}, 1}, initiator{tx, hop, InstanceId{2}, 0};
  ExecutionContext target_context = c;
  target_context.instance = InstanceId{1};
  ExecutionContext initiator_context = c;
  initiator_context.instance = InstanceId{2};
  PayloadSnapshot payload;
  payload.command = Command::Read;
  payload.data = {1, 2};
  payload.byte_enable = {1, 0};
  LEANAT_CHECK(shadows.create(target, payload, target_context, PayloadRole::Target, true));
  LEANAT_CHECK(
      shadows.create(initiator, payload, initiator_context, PayloadRole::Initiator, false));
  EventTxn target_tx({}, target_context);
  LEANAT_CHECK(shadows.buffer_data(target, Value(Bytes{8, 9}), target_tx));
  LEANAT_CHECK(target_tx.commit());
  EventTxn read_tx({}, initiator_context);
  LEANAT_CHECK(std::get<Bytes>(shadows.read_data(initiator, read_tx).value().data) ==
               Bytes({1, 2}));
  LEANAT_CHECK(!shadows.read_data(target, read_tx));
  ResponseSnapshot response;
  response.data = {8, 9};
  response.status = ResponseStatus::Ok;
  LEANAT_CHECK(shadows.deliver_response(initiator, response, read_tx));
  LEANAT_CHECK(std::get<Bytes>(shadows.read_data(initiator, read_tx).value().data) ==
               Bytes({8, 2}));
  LEANAT_CHECK(read_tx.commit());
}

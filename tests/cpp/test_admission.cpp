#include "leanat/admission.hpp"
#include "leanat/drain.hpp"
#include "test_support.hpp"
using namespace leanat;
static AdmissionRequest req(unsigned t, unsigned c = 1) {
  AdmissionRequest r;
  r.connection = ConnectionId{c};
  r.transport = TransportId{t};
  r.owner = 7;
  r.in_time = Tick{10};
  r.route = {PortId{3}, PortId{4}, 100, 200};
  return r;
}
int main() {
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {5, 5, 1, 2, 5});
    auto x = a.admit(req(1), true);
    LEANAT_CHECK(x && !x.value().retained_pending);
    LEANAT_CHECK(!a.admit(req(2), false));
    auto y = a.admit(req(2), true);
    LEANAT_CHECK(y && y.value().retained_pending);
    LEANAT_CHECK(!a.admit(req(3), true));
    LEANAT_CHECK(!a.promote_pending(ConnectionId{1}, Tick{20}).value());
    LEANAT_CHECK(
        a.queue_response(*x.value().service, {Tick{20}, 0}, {ResponseStatus::Ok, {1}, false, {}}));
    auto p = a.promote_pending(ConnectionId{1}, Tick{5});
    LEANAT_CHECK(p && p.value() && p.value()->ready == Tick{10});
    LEANAT_CHECK(a.inspect(y.value().hop).value().route.original_address == 100);
  }
  for (auto order : {ResponseOrder::InOrder, ResponseOrder::ReadyOrder}) {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {4, 4, 4, 4, 4}, order);
    auto x = a.admit(req(1), true).value();
    auto y = a.admit(req(2), true).value();
    LEANAT_CHECK(a.queue_response(*y.service, {Tick{11}, 0}, {ResponseStatus::Ok, {2}, false, {}}));
    auto p = a.select_response(ConnectionId{1}, true);
    LEANAT_CHECK(p);
    LEANAT_CHECK(bool(p.value()) == (order == ResponseOrder::ReadyOrder));
    if (p.value()) {
      LEANAT_CHECK(p.value()->hop == y.hop);
      LEANAT_CHECK(a.cancel_response_permit(*p.value()));
    }
    LEANAT_CHECK(a.queue_response(*x.service, {Tick{12}, 0}, {ResponseStatus::Ok, {1}, false, {}}));
    p = a.select_response(ConnectionId{1}, true);
    LEANAT_CHECK(p.value());
    LEANAT_CHECK(p.value()->hop == (order == ResponseOrder::InOrder ? x.hop : y.hop));
    LEANAT_CHECK(!a.select_response(ConnectionId{1}, true).value());
    LEANAT_CHECK(a.response_sent(*p.value()));
    LEANAT_CHECK(!a.cancel_response_permit(*p.value()));
  }
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {4, 4, 4, 4, 4});
    auto x = a.admit(req(1, 1), true).value();
    auto y = a.admit(req(2, 2), true).value();
    auto z = a.admit(req(3, 3), true).value();
    a.update_request_lane(ConnectionId{9}, false, {Tick{10}, 0});
    auto tx = a.request_request_gate(x.txn, ConnectionId{9}, {Tick{10}, 0}).value();
    auto ty = a.request_request_gate(y.txn, ConnectionId{9}, {Tick{10}, 0}).value();
    auto tz = a.request_request_gate(z.txn, ConnectionId{9}, {Tick{10}, 0}).value();
    LEANAT_CHECK(!a.gate_ready(tx));
    LEANAT_CHECK(a.cancel_request_gate(tx, {Tick{11}, 0}));
    LEANAT_CHECK(a.cancel_request_gate(tx, {Tick{11}, 0}));
    a.update_request_lane(ConnectionId{9}, true, {Tick{12}, 1});
    LEANAT_CHECK(a.gate_ready(ty).value() == ReadyKey{Tick{12}, 1});
    auto p = a.consume_request_gate(ty);
    LEANAT_CHECK(p);
    LEANAT_CHECK(!a.consume_request_gate(ty));
    a.update_request_lane(ConnectionId{9}, true, {Tick{13}, 0});
    LEANAT_CHECK(!a.gate_ready(tz));
    LEANAT_CHECK(a.cancel_request_permit(p.value(), {Tick{14}, 0}));
    a.update_request_lane(ConnectionId{9}, true, {Tick{14}, 0});
    LEANAT_CHECK(a.gate_ready(tz).value() == ReadyKey{Tick{14}, 0});
    LEANAT_CHECK(a.retire_request_ticket(ty));
    LEANAT_CHECK(!a.gate_state(ty));
    auto pz = a.consume_request_gate(tz).value();
    LEANAT_CHECK(a.request_sent(pz));
    LEANAT_CHECK(!a.cancel_request_permit(pz, {Tick{15}, 0}));
  }
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {2, 2, 2, 2, 2});
    auto x = a.admit(req(1), true).value();
    auto ticket = a.request_request_gate(x.txn, ConnectionId{9}, {Tick{10}, 0}).value();
    ExecutionContext ctx;
    ctx.domain = DomainId{1};
    ctx.instance = InstanceId{1};
    ctx.owner = 7;
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto prepared = a.prepare();
      LEANAT_CHECK(prepared);
      LEANAT_CHECK(!a.prepare());
      LEANAT_CHECK(!a.cancel_request_gate(ticket, {Tick{11}, 0}));
      LEANAT_CHECK(prepared.value()->draft().consume_request_gate(ticket));
      LEANAT_CHECK(a.gate_state(ticket).value() == RequestGateState::Granted);
      LEANAT_CHECK(tx.stage_participant(std::move(prepared.value())));
      LEANAT_CHECK(tx.stage_action(SendIntent{}));
      LEANAT_CHECK(tx.discard());
      LEANAT_CHECK(a.gate_state(ticket).value() == RequestGateState::Granted);
    }
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto prepared = a.prepare();
      LEANAT_CHECK(prepared.value()->draft().consume_request_gate(ticket));
      LEANAT_CHECK(tx.stage_participant(std::move(prepared.value())));
      LEANAT_CHECK(tx.stage_action(SendIntent{}));
      auto committed = tx.commit();
      LEANAT_CHECK(committed && committed.value().actions.size() == 1);
      LEANAT_CHECK(a.gate_state(ticket).value() == RequestGateState::Consumed);
    }
  }
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {3, 3, 3, 3, 3});
    auto x = a.create_initiator(req(1)).value();
    auto y = a.create_initiator(req(2)).value();
    auto z = a.create_initiator(req(3)).value();
    auto first = a.request_request_gate(x.txn, ConnectionId{8}, {Tick{10}, 0}).value();
    auto second = a.request_request_gate(y.txn, ConnectionId{8}, {Tick{10}, 0}).value();
    auto third = a.request_request_gate(z.txn, ConnectionId{8}, {Tick{10}, 0}).value();
    ExecutionContext ctx;
    ctx.domain = DomainId{1};
    ctx.instance = InstanceId{1};
    ctx.owner = 7;
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto update = a.prepare();
      LEANAT_CHECK(update.value()->draft().cancel_request_gate(first, {Tick{11}, 0}));
      LEANAT_CHECK(update.value()->draft().gate_ready(second).value() == ReadyKey{Tick{11}, 0});
      LEANAT_CHECK(tx.stage_participant(std::move(update.value())));
      LEANAT_CHECK(tx.discard());
      LEANAT_CHECK(a.gate_state(first).value() == RequestGateState::Granted);
      LEANAT_CHECK(!a.gate_ready(second));
    }
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto update = a.prepare();
      LEANAT_CHECK(update.value()->draft().cancel_request_gate(first, {Tick{12}, 0}));
      auto permit = update.value()->draft().consume_request_gate(second).value();
      LEANAT_CHECK(update.value()->draft().cancel_request_permit(permit, {Tick{13}, 0}));
      LEANAT_CHECK(tx.stage_participant(std::move(update.value())));
      LEANAT_CHECK(tx.commit());
      LEANAT_CHECK(a.gate_ready(third).value() == ReadyKey{Tick{13}, 0});
    }
  }
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {2, 2, 2, 2, 2});
    Handle discarded;
    {
      auto update = a.prepare();
      discarded = update.value()->draft().create_initiator(req(1)).value().hop;
    }
    auto committed = a.create_initiator(req(1)).value();
    LEANAT_CHECK(committed.hop != discarded);
    LEANAT_CHECK(!a.inspect(discarded));
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 2, 2, 10);
    LEANAT_CHECK(engine.bind_ledger(committed.hop, ConnectionId{1}, TransportId{1}));
    LEANAT_CHECK(a.retire_unstarted(committed.hop, engine));
    LEANAT_CHECK(!engine.inspect(committed.hop));
  }
  {
    AdmissionStore a(DomainId{1}, InstanceId{1}, {4, 4, 4, 4, 4});
    DrainStore drains(DomainId{1}, 5, 2, 1);
    LEANAT_CHECK(drains.add_instance(InstanceId{1}));
    Handle parent{HandleKind::Transaction, DomainId{1}, 99, 0, 100, 7};
    LEANAT_CHECK(drains.register_responsibility({parent, InstanceId{1}, 0, {}, {}, false, false}));
    LEANAT_CHECK(drains.reset(InstanceId{1}, ResetPolicy::DrainThenReset));
    ExecutionContext ctx;
    ctx.domain = DomainId{1};
    ctx.instance = InstanceId{1};
    ctx.owner = 7;
    AdmissionDisposition child, root;
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto result = stage_admission(tx, a, drains, req(2, 2), true, parent);
      LEANAT_CHECK(result && result.value().service);
      child = result.value();
      LEANAT_CHECK(tx.commit());
    }
    {
      EventTxn tx(SegmentBudget{}, ctx);
      auto result = stage_admission(tx, a, drains, req(3, 3), true);
      LEANAT_CHECK(result && result.value().retained_pending);
      root = result.value();
      LEANAT_CHECK(tx.commit());
    }
    LEANAT_CHECK(drains.is_deferred(root.txn));
    LEANAT_CHECK(!a.promote_pending(ConnectionId{3}, Tick{20}, drains).value());
    LEANAT_CHECK(
        a.queue_response(*child.service, {Tick{10}, 0}, {ResponseStatus::Ok, {1}, false, {}}));
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 4, 4, 10);
    LEANAT_CHECK(engine.bind_ledger(child.hop, ConnectionId{2}, TransportId{2}));
    WireCall c;
    c.id = CallId{1};
    c.connection = ConnectionId{2};
    c.transport = TransportId{2};
    c.call_time = Tick{10};
    auto ticket = engine.begin_call(child.hop, c).value();
    WireReturn r;
    r.sync = Sync::Completed;
    r.response = ResponseSnapshot{ResponseStatus::Ok, {1}, false, {}};
    auto preview = engine.validate_end_call(ticket, r).value();
    LEANAT_CHECK(!a.mark_wire_terminal(child.hop, preview));
    auto exchange = engine.end_call(ticket, r).value();
    LEANAT_CHECK(a.mark_wire_terminal(child.hop, exchange));
    LEANAT_CHECK(drains.set_hop(child.txn, {child.hop, true, true, true, false}));
    LEANAT_CHECK(drains.finish_local(child.txn));
    LEANAT_CHECK(drains.finish_local(parent));
    LEANAT_CHECK(drains.advance_reset(InstanceId{1}));
    LEANAT_CHECK(!drains.is_deferred(root.txn));
    auto service = a.promote_pending(ConnectionId{3}, Tick{20}, drains);
    LEANAT_CHECK(service && service.value() && service.value()->ready == Tick{20});
  }
  {
    AdmissionStore store(DomainId{1}, InstanceId{1}, {1, 1, 1, 1, 1});
    ExecutionContext ctx;
    ctx.domain = DomainId{1};
    ctx.instance = InstanceId{1};
    ctx.owner = 7;
    EventTxn tiny(SegmentBudget{2, 2, 1, 2}, ctx);
    auto prepared = store.prepare();
    LEANAT_CHECK(prepared.value()->draft().create_initiator(req(1)));
    LEANAT_CHECK(!tiny.stage_participant(std::move(prepared.value())));
    LEANAT_CHECK(store.create_initiator(req(1)));
  }
  {
    AdmissionStore store(DomainId{1}, InstanceId{1}, {1, 1, 1, 1, 1});
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 1, 1, 10);
    std::optional<Handle> previous;
    for (unsigned n = 0; n < 500; ++n) {
      auto local = store.create_initiator(req(1));
      LEANAT_CHECK(local);
      LEANAT_CHECK(!previous || local.value().hop != *previous);
      if (previous)
        LEANAT_CHECK(!store.inspect(*previous));
      LEANAT_CHECK(engine.bind_ledger(local.value().hop, ConnectionId{1}, TransportId{1}));
      previous = local.value().hop;
      LEANAT_CHECK(store.retire_unstarted(local.value().hop, engine));
    }
  }
  return 0;
}

#include <cassert>
#include <leanat/systemc/raw_dmi_services.hpp>
using namespace leanat;
struct Host final : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::Unsupported, "unused");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
int sc_main(int, char **) {
  sc_core::sc_set_time_resolution(1, sc_core::SC_PS);
  Host host;
  RuntimeConfig cfg;
  cfg.domain = DomainId{1};
  Runtime runtime(cfg, host);
  CoreRuntimeBackend backend(runtime);
  auto bytes = std::make_shared<Bytes>(4, 0);
  auto native = std::make_shared<leanat::systemc::RawDmiServiceHost>(
      bytes, 100, leanat::systemc::TimeCodec{}, Duration{2}, Duration{4});
  RawDmiServices services(true);
  assert(services.add_region(native->binding(3, DomainId{1}, 9)));
  exec::Project p;
  p.schema_major = 2;
  p.types = {{exec::TypeKind::Bits, 64},
             {exec::TypeKind::Bool},
             {exec::TypeKind::Record, 0, {1, 0, 0, 0, 0, 0, 0, 0}},
             {exec::TypeKind::Unit}};
  auto grant = raw_dmi_signature(p, 1, exec::Op::GrantRawDmi, {0, 0, 0}, 2).value();
  auto invalidate = raw_dmi_signature(p, 2, exec::Op::InvalidateRawDmi, {0, 0, 0}, 3).value();
  auto deny = raw_dmi_signature(p, 3, exec::Op::DenyDmi, {}, 2).value();
  p.services = {grant, invalidate, deny};
  assert(services.register_into(backend, p));
  assert(backend.freeze());
  ExecutionContext c;
  c.domain = DomainId{1};
  c.owner = 9;
  c.kind = ContextKind::Dmi;
  std::vector<Value> args = {Value(uint64_t(3)), Value(uint64_t(101)), Value(uint64_t(0))};
  EventTxn dropped({}, c);
  assert(backend.invoke(grant, args, c, dropped));
  assert(dropped.discard());
  assert(services.publish());
  assert(native->last_grant().get_dmi_ptr() == nullptr);
  EventTxn txn({}, c);
  auto out = backend.invoke(grant, args, c, txn);
  assert(out);
  assert(txn.commit());
  assert(native->last_grant().get_dmi_ptr() == nullptr);
  assert(services.publish());
  auto &dmi = native->last_grant();
  assert(dmi.get_dmi_ptr() == bytes->data());
  assert(dmi.get_start_address() == 100 && dmi.get_end_address() == 103);
  assert(dmi.get_read_latency().value() == 2 && dmi.get_write_latency().value() == 4);
  dmi.get_dmi_ptr()[1] = 7;
  assert((*bytes)[1] == 7);
  c.kind = ContextKind::Timed;
  EventTxn invalid({}, c);
  assert(backend.invoke(
      invalidate, {Value(uint64_t(3)), Value(uint64_t(101)), Value(uint64_t(101))}, c, invalid));
  assert(native->invalidations().empty());
  assert(invalid.commit());
  assert(services.publish());
  assert(native->invalidations().size() == 1);
  assert(native->invalidations()[0] == std::make_pair(uint64_t(100), uint64_t(103)));
  c.kind = ContextKind::Dmi;
  EventTxn denied({}, c);
  auto no = backend.invoke(deny, {}, c, denied);
  assert(no);
  assert(!std::get<bool>(std::get<Value::Array>(no.value()[0].data)[0].data));
  assert(denied.commit());
  assert(services.publish());
  assert(native->invalidations().size() == 1);
  return 0;
}

#include "leanat/memory.hpp"
#include "leanat/objects.hpp"
#include "leanat/register_bank.hpp"
#include "test_support.hpp"
using namespace leanat;
struct Counter : ObjectProvider {
  VersionedCell cell{Value(std::uint64_t(0))};
  Expected<ObjectResult> prepare(ObjectId, MethodId, const std::vector<Value> &,
                                 EventTxn &t) override {
    auto v = t.read(cell);
    if (!v)
      return v.error();
    auto n = std::get<std::uint64_t>(v.value().data) + 1;
    auto w = t.buffer(cell, Value(n));
    if (!w)
      return w.error();
    return ObjectResult{{Value(n)}};
  }
};
struct TypedEcho : ObjectProvider {
  unsigned calls{};
  VersionedCell cell{Value(std::uint64_t(0))};
  Expected<ObjectResult> prepare(ObjectId, MethodId, const std::vector<Value> &args,
                                 EventTxn &tx) override {
    ++calls;
    auto written = tx.buffer(cell, Value(std::uint64_t(calls)));
    if (!written)
      return written.error();
    return ObjectResult{{args[0]}};
  }
};
int main() {
  const std::vector<Value> representations = {Value{},
                                              Value(true),
                                              Value(std::uint64_t(8)),
                                              Value(std::int64_t(-8)),
                                              Value(Bytes{1}),
                                              Value(Value::Array{Value(false)}),
                                              Value(Handle{})};
  for (std::uint32_t type = 1; type <= 5; ++type) {
    ObjectRegistry typed;
    auto echo = std::make_shared<TypedEcho>();
    ProviderDesc desc;
    desc.kind = 1;
    desc.key = "typed";
    desc.version = "1";
    desc.hash = "test";
    desc.methods = {
        {MethodId{1}, {TypeId{type}}, {TypeId{type}}, ObjectWrite, {ContextKind::Timed}}};
    LEANAT_CHECK(typed.register_provider(desc, echo));
    LEANAT_CHECK(typed.register_object({ObjectId{1}, 1, {}, {}}));
    LEANAT_CHECK(typed.freeze());
    EventTxn tx({}, {});
    LEANAT_CHECK(tx.buffer(echo->cell, Value(std::uint64_t(99))));
    for (const auto &value : representations) {
      auto before = tx.read(echo->cell).value();
      auto calls = echo->calls;
      auto remaining = tx.remaining_bytes();
      std::vector<Value> args{value};
      auto result = typed.prepare(ObjectId{1}, MethodId{1}, args, {TypeId{type}}, tx);
      bool matches = type == 4 || (type == 1 && value.data.index() == 2) ||
                     (type == 2 && value.data.index() == 4) ||
                     (type == 3 && value.data.index() == 6) ||
                     (type == 5 && value.data.index() == 1);
      LEANAT_CHECK(bool(result) == matches);
      LEANAT_CHECK(args[0].data == value.data);
      LEANAT_CHECK(echo->calls == calls + (matches ? 1 : 0));
      if (!matches) {
        LEANAT_CHECK(result.error().code == ErrorCode::TypeMismatch);
        LEANAT_CHECK(tx.read(echo->cell).value().data == before.data);
        LEANAT_CHECK(tx.remaining_bytes() == remaining);
      } else {
        LEANAT_CHECK(result.value().values[0].data == value.data);
      }
    }
    LEANAT_CHECK(tx.commit());
    LEANAT_CHECK(std::get<std::uint64_t>(echo->cell.value.data) == echo->calls);
  }
  for (auto unknown : {0U, 6U, UINT32_MAX}) {
    for (bool output : {false, true}) {
      ObjectRegistry registry;
      auto echo = std::make_shared<TypedEcho>();
      ProviderDesc desc;
      desc.kind = 1;
      desc.key = "unknown";
      desc.version = "1";
      desc.hash = "test";
      desc.methods = {{MethodId{1},
                       {TypeId{output ? 4U : unknown}},
                       {TypeId{output ? unknown : 4U}},
                       ObjectWrite,
                       {ContextKind::Timed}}};
      auto rejected = registry.register_provider(desc, echo);
      LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::Schema);
      LEANAT_CHECK(echo->calls == 0);
      desc.methods[0].arguments = desc.methods[0].results = {TypeId{4}};
      LEANAT_CHECK(registry.register_provider(desc, echo));
    }
  }
  ObjectRegistry r;
  auto p = std::make_shared<Counter>();
  ProviderDesc d;
  d.kind = 1;
  d.key = "counter";
  d.version = "1";
  d.hash = "test";
  d.methods = {{MethodId{1}, {}, {TypeId{1}}, ObjectRead | ObjectWrite, {ContextKind::Timed}, 64}};
  LEANAT_CHECK(r.register_provider(d, p));
  LEANAT_CHECK(!r.register_provider(d, p));
  LEANAT_CHECK(r.register_object({ObjectId{1}, 1, DomainId{}, InstanceId{}}));
  LEANAT_CHECK(r.freeze());
  LEANAT_CHECK(!r.register_provider(d, p));
  EventTxn t({}, ExecutionContext{});
  LEANAT_CHECK(r.prepare(ObjectId{1}, MethodId{1}, {}, {}, t));
  LEANAT_CHECK(r.prepare(ObjectId{1}, MethodId{1}, {}, {}, t));
  LEANAT_CHECK(std::get<std::uint64_t>(p->cell.value.data) == 0);
  LEANAT_CHECK(t.commit());
  LEANAT_CHECK(std::get<std::uint64_t>(p->cell.value.data) == 2);
  ExecutionContext bad;
  bad.instance = InstanceId{2};
  EventTxn x({}, bad);
  LEANAT_CHECK(!r.prepare(ObjectId{1}, MethodId{1}, {}, {}, x));
  ObjectRegistry debug;
  d.methods[0].contexts = {ContextKind::Debug};
  LEANAT_CHECK(!debug.register_provider(d, p));
  ObjectRegistry standard;
  auto memory = std::make_shared<Memory>(std::move(Memory::make(4).value()));
  RegisterSpec spec;
  spec.width_bits = 8;
  spec.allowed_access_bytes = {1};
  spec.fields = {{FieldId{1}, 0, 8, RegisterAccess::RW, {7}}};
  auto bank = std::make_shared<RegisterBank>(std::move(RegisterBank::make({spec}).value()));
  LEANAT_CHECK(
      register_standard_objects(standard, {{{ObjectId{2}, 35, DomainId{}, InstanceId{}}, memory},
                                           {{ObjectId{3}, 36, DomainId{}, InstanceId{}}, bank}}));
  LEANAT_CHECK(standard.freeze());
  EventTxn st({}, ExecutionContext{});
  auto written = standard.prepare(
      ObjectId{2}, MethodId{static_cast<std::uint32_t>(StandardIntrinsic::WriteBytes)},
      {Value(std::uint64_t(0)), Value(Bytes{1, 2}), Value(Bytes{})},
      {TypeId{1}, TypeId{2}, TypeId{2}}, st);
  LEANAT_CHECK(written);
  auto read = standard.prepare(
      ObjectId{2}, MethodId{static_cast<std::uint32_t>(StandardIntrinsic::ReadBytes)},
      {Value(std::uint64_t(0)), Value(std::uint64_t(2))}, {TypeId{1}, TypeId{1}}, st);
  LEANAT_CHECK(read && std::get<Bytes>(read.value().values[0].data) == Bytes({1, 2}));
  auto oversized = standard.prepare(
      ObjectId{3}, MethodId{static_cast<std::uint32_t>(StandardIntrinsic::ReadField)},
      {Value(std::uint64_t(0x100000001ULL))}, {TypeId{1}}, st);
  LEANAT_CHECK(!oversized);
  LEANAT_CHECK(st.commit());
  ObjectRegistry restricted;
  d.methods[0].contexts = {ContextKind::Timed};
  d.methods[0].effects = ObjectRead;
  LEANAT_CHECK(restricted.register_provider(d, p));
  LEANAT_CHECK(restricted.register_object({ObjectId{1}, 1, DomainId{}, InstanceId{}}));
  LEANAT_CHECK(restricted.freeze());
  EventTxn forbidden({}, ExecutionContext{});
  LEANAT_CHECK(!restricted.prepare(ObjectId{1}, MethodId{1}, {}, {}, forbidden));
  LEANAT_CHECK(forbidden.commit());
  LEANAT_CHECK(std::get<std::uint64_t>(p->cell.value.data) == 2);
}

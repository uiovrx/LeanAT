#include "leanat/register_bank.hpp"
#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  RegisterSpec s;
  s.width_bits = 48;
  s.allowed_access_bytes = {1, 2, 6};
  s.alignment_bytes = 1;
  s.fields = {{FieldId{1}, 0, 8, RegisterAccess::RO, {255}},
              {FieldId{2}, 8, 8, RegisterAccess::RW, {0}},
              {FieldId{3}, 16, 8, RegisterAccess::WO, {255}},
              {FieldId{4}, 24, 8, RegisterAccess::W1C, {255}},
              {FieldId{5}, 32, 8, RegisterAccess::W1S, {0}},
              {FieldId{6}, 40, 8, RegisterAccess::RC, {255}}};
  auto made = RegisterBank::make({s});
  LEANAT_CHECK(made);
  auto b = std::move(made.value());
  ExecutionContext c;
  EventTxn t({}, c);
  PayloadSnapshot p;
  p.command = Command::Write;
  p.streaming_width = 6;
  p.data = {0, 15, 15, 15, 15, 0};
  LEANAT_CHECK(b.access(t, p).value().status == ResponseStatus::Ok);
  p.command = Command::Read;
  p.data = Bytes(6, 99);
  auto r = b.access(t, p);
  LEANAT_CHECK(r && r.value().data == Bytes({255, 15, 0, 240, 15, 255}));
  LEANAT_CHECK(b.read_field(t, FieldId{6}).value() == Bytes{0});
  LEANAT_CHECK(t.discard());
  EventTxn again({}, c);
  LEANAT_CHECK(b.read_field(again, FieldId{6}).value() == Bytes{255});
  LEANAT_CHECK(again.commit());
  c.kind = ContextKind::Debug;
  EventTxn d({}, c);
  auto x = b.peek_poke(d, Command::Write, 3, Bytes{1, 2, 3, 4});
  LEANAT_CHECK(x && x.value().count == 3);
  LEANAT_CHECK(d.commit());
  c.kind = ContextKind::Timed;
  EventTxn v({}, c);
  LEANAT_CHECK(b.read_field(v, FieldId{4}).value() == Bytes{1});
  LEANAT_CHECK(b.read_field(v, FieldId{6}).value() == Bytes{3});
  auto invalid = s;
  invalid.fields.push_back({FieldId{99}, 0, 1, RegisterAccess::RW, {0}});
  LEANAT_CHECK(!RegisterBank::make({invalid}));
  RegisterSpec big;
  big.width_bits = 24;
  big.endian = RegisterEndian::Big;
  big.allowed_access_bytes = {3};
  big.fields = {{FieldId{42}, 4, 12, RegisterAccess::RW, {0xbc, 0x0a}}};
  auto bm = RegisterBank::make({big});
  LEANAT_CHECK(bm);
  EventTxn be({}, c);
  p.address = 0;
  p.command = Command::Read;
  p.streaming_width = 3;
  p.data = Bytes(3);
  LEANAT_CHECK(bm.value().access(be, p).value().data == Bytes({0, 0xab, 0xc0}));
  PayloadShadow shadows;
  PayloadViewKey key{{HandleKind::Transaction, DomainId{}, 1, 0, 1, 0},
                     {HandleKind::Hop, DomainId{}, 1, 0, 1, 0},
                     InstanceId{},
                     0};
  PayloadSnapshot denied_read;
  denied_read.command = Command::Read;
  denied_read.address = 5;
  denied_read.data = {0};
  denied_read.streaming_width = 1;
  LEANAT_CHECK(shadows.create(key, denied_read, c, PayloadRole::Target, false));
  EventTxn denied({}, c);
  LEANAT_CHECK(!b.access(denied, shadows, key));
  LEANAT_CHECK(b.read_field(denied, FieldId{6}).value() == Bytes{3});
}

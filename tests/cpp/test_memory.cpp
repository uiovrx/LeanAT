#include "leanat/memory.hpp"
#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  auto made = Memory::make(4, Bytes{1, 2, 3, 4});
  LEANAT_CHECK(made);
  auto m = std::move(made.value());
  auto ptr = m.data();
  ExecutionContext c;
  EventTxn t({}, c);
  PayloadSnapshot p;
  p.command = Command::Write;
  p.data = {9, 8, 7, 6};
  p.streaming_width = 2;
  p.byte_enable = {255, 0};
  auto w = m.transfer(t, p);
  LEANAT_CHECK(w && w.value().status == ResponseStatus::Ok);
  LEANAT_CHECK(m.data()[0] == 1);
  auto r = m.read_bytes(t, 0, 4);
  LEANAT_CHECK(r && r.value() == Bytes({7, 2, 3, 4}));
  LEANAT_CHECK(t.commit());
  LEANAT_CHECK(m.data() == ptr && m.data()[0] == 7);
  EventTxn bad({}, c);
  p.address = 3;
  p.byte_enable = {0};
  auto f = m.transfer(bad, p);
  LEANAT_CHECK(f && f.value().status == ResponseStatus::AddressError);
  LEANAT_CHECK(bad.commit());
  LEANAT_CHECK(m.data()[3] == 4);
  EventTxn ignore({}, c);
  p.command = Command::Ignore;
  p.streaming_width = 0;
  p.address = UINT64_MAX;
  p.data.clear();
  LEANAT_CHECK(m.transfer(ignore, p).value().status == ResponseStatus::Ok);
  LEANAT_CHECK(ignore.commit());
  EventTxn reset({}, c);
  LEANAT_CHECK(m.reset(reset));
  LEANAT_CHECK(reset.commit());
  LEANAT_CHECK(ptr == m.data() && m.data()[0] == 1);
  c.kind = ContextKind::Debug;
  EventTxn debug({}, c);
  auto d = m.debug_transfer(debug, Command::Write, 3, Bytes{5, 6});
  LEANAT_CHECK(d && d.value().count == 1);
  LEANAT_CHECK(debug.commit());
  LEANAT_CHECK(ptr == m.data() && m.data()[3] == 5);
  EventTxn partial({}, c);
  auto read = m.debug_transfer(partial, Command::Read, 3, Bytes{99, 88});
  LEANAT_CHECK(read && read.value().data == Bytes({5, 88}));
  LEANAT_CHECK(!Memory::make(0));
  c.kind = ContextKind::Timed;
  PayloadShadow shadows;
  PayloadViewKey key{{HandleKind::Transaction, DomainId{}, 1, 0, 1, 0},
                     {HandleKind::Hop, DomainId{}, 1, 0, 1, 0},
                     InstanceId{},
                     0};
  PayloadSnapshot input;
  input.command = Command::Read;
  input.data = {0, 0};
  input.streaming_width = 2;
  LEANAT_CHECK(shadows.create(key, input, c, PayloadRole::Target, false));
  EventTxn denied({}, c);
  LEANAT_CHECK(!m.transfer(denied, shadows, key));
  LEANAT_CHECK(denied.commit());
}

#include "leanat/common.hpp"
#include "test_support.hpp"
#include <type_traits>
using namespace leanat;
int main() {
  static_assert(!std::is_convertible<Tick,Duration>::value,"time and duration must differ");
  static_assert(!std::is_convertible<CallId,TransportId>::value,"identities must differ");
  auto last=add_time(Tick{UINT64_MAX-1},Duration{1});
  LEANAT_CHECK(last && last.value().value==UINT64_MAX);
  LEANAT_CHECK(!add_time(Tick{UINT64_MAX},Duration{1}));
  LEANAT_CHECK(!checked_mul(UINT64_MAX,2));
  LEANAT_CHECK(checked_mul(0,UINT64_MAX).value()==0);
  Expected<void> good;
  LEANAT_CHECK(good);
  Expected<void> bad=fail(ErrorCode::InvalidArgument,"bad");
  LEANAT_CHECK(!bad && bad.error().message=="bad");
  Value a{Bytes{1,2,3}},b{Bytes{1,2,3}};
  LEANAT_CHECK(a==b);
  LEANAT_CHECK((ReadyKey{Tick{3},1}<ReadyKey{Tick{3},2}));
}

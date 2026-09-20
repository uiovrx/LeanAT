#include "leanat/numeric.hpp"
#include "test_support.hpp"
using namespace leanat;
using namespace leanat::numeric;
static Value n(std::uint64_t x) { return Value{x}; }
static std::uint64_t number(Expected<Value> x) {
  LEANAT_CHECK(x); return std::get<std::uint64_t>(x.value().data);
}
static bool boolean(Expected<Value> x) {
  LEANAT_CHECK(x); return std::get<bool>(x.value().data);
}
static void error(Expected<Value> x, ErrorCode code, const char *message) {
  LEANAT_CHECK(!x && x.error().code == code && x.error().message == message);
}
int main() {
  LEANAT_CHECK(boolean(eval_unary(0, Value{false})));
  LEANAT_CHECK(!boolean(eval_unary(0, Value{true})));
  LEANAT_CHECK(boolean(eval_compare(0, Value{true}, Value{true})));
  LEANAT_CHECK(boolean(eval_compare(1, Value{true}, Value{false})));
  LEANAT_CHECK(boolean(eval_compare(0, Value{Value::Array{n(1)}}, Value{Value::Array{n(1)}})));
  for (std::uint32_t w = 1; w <= 64; ++w) {
    auto m = w == 64 ? UINT64_MAX : (std::uint64_t{1} << w) - 1;
    auto sign = std::uint64_t{1} << (w-1);
    for (auto x : {std::uint64_t{0}, std::uint64_t{1}, sign, m}) {
      LEANAT_CHECK(number(eval_unary(1,n(x),w)) == m-x);
      LEANAT_CHECK(number(eval_unary(2,n(x),w)) == ((std::uint64_t{0}-x)&m));
      for (auto y : {std::uint64_t{0}, std::uint64_t{1}, sign, m}) {
        bool expected[] = {x==y,x!=y,x<y,x<=y,x>y,x>=y};
        for (std::uint32_t mode=0; mode<6; ++mode)
          LEANAT_CHECK(boolean(eval_compare(mode,n(x),n(y),w)) == expected[mode]);
      }
      for (std::uint32_t dest=1; dest<=64; ++dest) {
        auto dm = dest == 64 ? UINT64_MAX : (std::uint64_t{1} << dest)-1;
        if (dest>=w) LEANAT_CHECK(number(eval_convert(0,n(x),w,dest))==x);
        else error(eval_convert(0,n(x),w,dest),ErrorCode::TypeMismatch,"NumericTypeMismatch");
        if (dest<=w) {
          LEANAT_CHECK(number(eval_convert(1,n(x),w,dest))==(x&dm));
          if(x<=dm) LEANAT_CHECK(number(eval_convert(2,n(x),w,dest))==x);
          else error(eval_convert(2,n(x),w,dest),ErrorCode::Overflow,"ArithmeticOverflow");
        } else {
          error(eval_convert(1,n(x),w,dest),ErrorCode::TypeMismatch,"NumericTypeMismatch");
          error(eval_convert(2,n(x),w,dest),ErrorCode::TypeMismatch,"NumericTypeMismatch");
        }
      }
      LEANAT_CHECK(number(checked_shift(n(x),0,w,true))==x);
      LEANAT_CHECK(number(checked_shift(n(x),w-1,w,false))==(x>>(w-1)));
      LEANAT_CHECK(number(checked_shift(n(x),w-1,w,true))==((x<<(w-1))&m));
      error(checked_shift(n(x),w,w,true),ErrorCode::InvalidArgument,"ShiftOutOfRange");
      error(checked_shift(n(x),UINT64_MAX,w,false),ErrorCode::InvalidArgument,"ShiftOutOfRange");
      error(checked_divide(n(x),n(0),w),ErrorCode::InvalidArgument,"DivisionByZero");
      LEANAT_CHECK(number(checked_divide(n(x),n(1),w))==x);
    }
    error(checked_divide(n(sign),n(m),w,true),ErrorCode::Overflow,"ArithmeticOverflow");
    if(w>1) LEANAT_CHECK(number(checked_divide(n(sign),n(1),w,true))==sign);
    if(w<64) error(eval_unary(1,n(m+1),w),ErrorCode::TypeMismatch,"NumericTypeMismatch");
  }
  // Exhaust all signed pairs through width 5; native oracle stays far from signed UB.
  for(unsigned w=1;w<=5;++w) {
    auto modulus=1u<<w,sign=modulus/2;
    for(unsigned a=0;a<modulus;++a) for(unsigned b=0;b<modulus;++b) {
      int x=a>=sign?int(a)-int(modulus):int(a), y=b>=sign?int(b)-int(modulus):int(b);
      auto r=checked_divide(n(a),n(b),w,true);
      if(!y) error(r,ErrorCode::InvalidArgument,"DivisionByZero");
      else if(x==-int(sign)&&y==-1) error(r,ErrorCode::Overflow,"ArithmeticOverflow");
      else LEANAT_CHECK(number(r)==(std::uint64_t(x/y)&(modulus-1)));
    }
  }
  for(auto w:{0u,65u,UINT32_MAX}) {
    error(eval_unary(1,n(0),w),ErrorCode::TypeMismatch,"NumericTypeMismatch");
    error(eval_convert(1,n(0),8,w),ErrorCode::TypeMismatch,"NumericTypeMismatch");
  }
  error(eval_unary(3,n(0),8),ErrorCode::Unsupported,"UnknownUnaryMode");
  error(eval_compare(6,n(0),n(0),8),ErrorCode::Unsupported,"UnknownCompareMode");
  error(eval_convert(3,n(0),8,8),ErrorCode::Unsupported,"UnknownConvertMode");
  error(eval_unary(0,n(0)),ErrorCode::TypeMismatch,"NumericTypeMismatch");
  error(eval_unary(1,Value{false},8),ErrorCode::TypeMismatch,"NumericTypeMismatch");
  error(eval_compare(0,Value{false},n(0)),ErrorCode::TypeMismatch,"NumericTypeMismatch");
}

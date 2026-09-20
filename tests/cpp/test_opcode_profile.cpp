#include "../../tools/cpp/opcode_profile.hpp"
#include <cassert>
using namespace leanat;
using namespace leanat::opcode_test;
int main() {
  const std::string rule =
      R"({"kind":5,"store":"20","group":"result","perSlot":false,"persistent":true,"allowMax":true,"capacity":"17"})";
  RuntimeConfig config;
  auto world =
      InputJsonReader(
          "{\"maxPins\":\"23\",\"allocationRules\":[" + rule +
          "],\"allocationCounters\":[{\"group\":\"result\",\"domain\":\"1\",\"slot\":null,"
          "\"nextGeneration\":\"18446744073709551616\",\"persistent\":true,\"retired\":true}]}")
          .parse();
  apply_world_profile(world, config);
  assert(config.result_capacity == 17 && config.pin_capacity == 23);
  for (
      const auto &bad :
      {"{\"allocationRules\":[" + rule + "," + rule + "]}",
       "{\"allocationRules\":[" + rule +
           "],\"allocationCounters\":[{\"group\":\"result\",\"domain\":\"1\",\"slot\":\"0\"}]}",
       std::string(
           R"({"allocationCounters":[{"group":"result","domain":"1","nextGeneration":"2","retired":true}]})"),
       std::string(
           R"({"allocationCounters":[{"group":"result","domain":"1","nextGeneration":"18446744073709551617"}]})")}) {
    bool rejected = false;
    try {
      apply_world_profile(InputJsonReader(bad).parse(), config);
    } catch (const std::exception &) {
      rejected = true;
    }
    assert(rejected);
  }
}

#include "Facade.hpp"
#include "facade_observations.hpp"
#include <stdexcept>
int sc_main(int argc,char**argv) {
  leanat_generated::Top top("top");
  auto before=top.engine.state();
  if(before.size()!=2 || std::get<std::uint64_t>(before[0].data)!=0 || std::get<std::uint64_t>(before[1].data)!=0)
    throw std::runtime_error("initial per-instance state mismatch");
  auto first=top.engine.execute(0);
  if(!first)throw std::runtime_error(first.error().message);
  auto middle=top.engine.state();
  if(std::get<std::uint64_t>(middle[0].data)!=42 || std::get<std::uint64_t>(middle[1].data)!=0)
    throw std::runtime_error("handler wrote another instance's state");
  auto second=top.engine.execute(1);
  if(!second)throw std::runtime_error(second.error().message);
  auto after=top.engine.state();
  if(std::get<std::uint64_t>(after[0].data)!=42 || std::get<std::uint64_t>(after[1].data)!=42)
    throw std::runtime_error("second instance handler failed");
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  facade_observations(argc,argv,"scalar","{\"state\":["+std::to_string(std::get<std::uint64_t>(after[0].data))+","+
    std::to_string(std::get<std::uint64_t>(after[1].data))+"],\"runtimeTick\":"+std::to_string(top.engine.host.runtime.now().value)+"}");
  return 0;
}

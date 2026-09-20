#include "facade_observations.hpp"
int sc_main(int argc,char**argv) {
  leanat_generated::Top top("top");
  auto started=leanat_generated::require(top.engine.execute(0));
  if(started.kind!=leanat::exec::SegmentResult::Kind::Suspended)throw std::runtime_error("process did not await");
  sc_core::sc_start(sc_core::sc_time::from_value(6));
  auto first=std::get<std::uint64_t>(top.engine.state().at(0).data);
  if(first!=41)throw std::runtime_error("first resume did not preserve live local");
  sc_core::sc_start(sc_core::sc_time::from_value(2));
  auto final=std::get<std::uint64_t>(top.engine.state().at(0).data);
  if(top.engine.host.runtime.stopped() || final!=42)throw std::runtime_error("second resume failed");
  facade_observations(argc,argv,"process","{\"stateAfterFirst\":"+std::to_string(first)+",\"state\":["+
    std::to_string(final)+"],\"runtimeTick\":"+std::to_string(top.engine.host.runtime.now().value)+"}");
  return 0;
}

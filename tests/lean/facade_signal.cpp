#include "Facade.hpp"
#include "facade_observations.hpp"
#include <stdexcept>
using namespace leanat;
int sc_main(int argc,char**argv) {
  leanat_generated::Top top("top");
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  ExecutionContext context;context.domain=DomainId{1};context.instance=InstanceId{0};context.owner=1;
  context.epoch=leanat_generated::require(top.engine.host.runtime.drains().epoch(context.instance));
  context.ready={Tick{0},0};
  EventTxn txn(SegmentBudget{},context);
  leanat_generated::require(top.engine.host.runtime.stage_output(txn,PortId{0},Value{std::uint64_t(99)}));
  leanat_generated::require(top.engine.host.runtime.commit_segment(txn));
  sc_core::sc_start(sc_core::sc_time::from_value(2));
  if(top.engine.host.runtime.stopped())throw std::runtime_error(top.engine.host.runtime.stop_detail());
  auto a=top.engine.input_value(0,1),b=top.engine.input_value(1,1);
  if(!a || !b || std::get<std::uint64_t>(a->data)!=7 || std::get<std::uint64_t>(b->data)!=99)
    throw std::runtime_error("instance-scoped generated sideband output/input routing failed");
  leanat_generated::require(top.engine.execute(0));leanat_generated::require(top.engine.execute(1));
  auto state=top.engine.state();
  if(std::get<std::uint64_t>(state.at(0).data)!=7 || std::get<std::uint64_t>(state.at(1).data)!=99)
    throw std::runtime_error("loaded VM input provider instance routing failed");
  facade_observations(argc,argv,"signal","{\"inputs\":["+std::to_string(std::get<std::uint64_t>(a->data))+","+
    std::to_string(std::get<std::uint64_t>(b->data))+"],\"outputs\":["+std::to_string(top.signal_0_0.read().to_uint64())+","+
    std::to_string(top.signal_1_0.read().to_uint64())+"],\"state\":["+std::to_string(std::get<std::uint64_t>(state[0].data))+","+
    std::to_string(std::get<std::uint64_t>(state[1].data))+"],\"runtimeTick\":"+std::to_string(top.engine.host.runtime.now().value)+"}");
  return 0;
}

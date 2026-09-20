#include "Facade.hpp"
#include "facade_observations.hpp"
#include <leanat/admission.hpp>
#include <stdexcept>
using namespace leanat;
int sc_main(int argc,char**argv) {
  // This explicit test host prepares protocol effects; generated business handlers remain VM code.
  Runtime* runtime=nullptr;std::size_t calls=0;
  std::string wire="[";
  AdmissionStore initiator_admission(DomainId{1},InstanceId{0},{4,4,4,4,4});
  AdmissionStore target_admission(DomainId{1},InstanceId{1},{4,4,4,4,4});
  std::map<std::uint32_t,Handle> transactions;
  leanat_generated::HostBindings bindings;
  bindings.providers=[&](CoreRuntimeBackend&,const exec::Project&,Runtime& r)->Expected<void>{runtime=&r;return {};};
  bindings.wire_inputs=[&](const WireCall& call,const ExecutionContext& context,EventTxn& txn)->Expected<std::vector<Value>>{
    ++calls;
    if(calls>1)wire+=",";
    wire+="{\"phase\":"+std::to_string(call.phase.value)+",\"tick\":"+std::to_string(context.ready.time.value)+
      ",\"instance\":"+std::to_string(context.instance.value)+",\"firstByte\":"+
      std::to_string(call.request.data.empty()?0:call.request.data.front())+"}";
    if(call.phase==end_resp)return std::vector<Value>{};
    if(call.phase==begin_req) {
      AdmissionRequest request;request.connection=call.connection;request.transport=call.transport;
      request.owner=context.owner;request.in_time=context.ready.time;request.request=call.request;
      auto admitted=target_admission.admit(request,true);if(!admitted)return admitted.error();
      transactions[context.instance.value]=admitted.value().txn;
      auto side=runtime->protocol(context.instance);if(!side)return side.error();
      auto hop=side.value()->find_ledger(call.connection,call.transport);if(!hop)return hop.error();
      auto registered=runtime->drains().register_responsibility(
        {admitted.value().txn,context.instance,context.epoch,{},{{hop.value(),false,false,false,false}},false,false});
      if(!registered)return registered.error();
      auto tracked=runtime->track_cleanup(admitted.value().txn,hop.value(),false,call.request);
      if(!tracked)return tracked.error();
    }
    auto id=runtime->stage_allocate_call_id(txn,CallOrigin::Outgoing);if(!id)return id.error();
    SendIntent response;response.call_id=id.value();response.connection=call.connection;response.transport=call.transport;
    response.txn=transactions.at(context.instance.value);
    response.payload=call.request;response.not_before=Tick{context.ready.time.value+1};
    response.flow=call.phase==begin_req?Flow::Backward:Flow::Forward;
    response.phase=call.phase==begin_req?begin_resp:end_resp;
    if(call.phase==begin_req){response.payload.status=ResponseStatus::Ok;response.payload.data={77};}
    else if(call.request.data!=Bytes{77})return fail(ErrorCode::InvalidState,"response payload not copied to GP");
    auto staged=runtime->stage_publish(txn,std::move(response));if(!staged)return staged.error();return std::vector<Value>{};
  };
  leanat_generated::Top top("top",std::move(bindings));
  auto side=leanat_generated::require(runtime->protocol(InstanceId{0}));
  SendIntent initial;initial.connection=ConnectionId{0};initial.transport=TransportId{1};
  initial.call_id=leanat_generated::require(runtime->allocate_call_id(CallOrigin::Outgoing));
  initial.payload.command=Command::Read;initial.payload.data={4};initial.payload.streaming_width=1;
  AdmissionRequest request;request.connection=initial.connection;request.transport=initial.transport;
  request.owner=1;request.request=initial.payload;
  auto admitted=leanat_generated::require(initiator_admission.create_initiator(request));
  initial.txn=admitted.txn;transactions[0]=admitted.txn;
  leanat_generated::require(side->bind_ledger(admitted.hop,initial.connection,initial.transport));
  leanat_generated::require(runtime->drains().register_responsibility(
    {admitted.txn,InstanceId{0},0,{},{{admitted.hop,false,false,false,false}},false,false}));
  leanat_generated::require(runtime->track_cleanup(admitted.txn,admitted.hop,true,initial.payload));
  leanat_generated::require(runtime->publish(initial));
  sc_core::sc_start(sc_core::sc_time::from_value(5));
  if(runtime->stopped())throw std::runtime_error(runtime->stop_detail());
  auto state=top.engine.state();
  if(calls!=3 || std::get<std::uint64_t>(state.at(0).data)!=84 || std::get<std::uint64_t>(state.at(1).data)!=42)
    throw std::runtime_error("generated socket callback/VM handler routing failed");
  for(auto id:{InstanceId{0},InstanceId{1}}){
    auto engine=leanat_generated::require(runtime->protocol(id));
    auto ledger=leanat_generated::require(engine->find_ledger(ConnectionId{0},TransportId{1}));
    auto view=leanat_generated::require(engine->inspect(ledger));
    if(view.state!=WireState::Terminal || view.call_ordinal!=3)throw std::runtime_error("shared local ledger did not finish");
  }
  facade_observations(argc,argv,"wire","{\"wire\":"+wire+"],\"state\":["+std::to_string(std::get<std::uint64_t>(state[0].data))+","+
    std::to_string(std::get<std::uint64_t>(state[1].data))+"],\"runtimeTick\":"+std::to_string(runtime->now().value)+"}");
  return 0;
}

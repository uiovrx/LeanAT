#include "profile_context.hpp"
#include "harness.hpp"
#include <leanat/admission.hpp>
#include <leanat/memory.hpp>
#include <leanat/object_services.hpp>
#include <leanat/register_bank.hpp>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <fstream>
#include <iostream>

using namespace leanat;
using namespace conformance;
using namespace conformance::profile;
namespace {
struct QuietHost : RuntimeHost {
  std::uint64_t outputs{};
  Expected<WireReturn> transport(const SendIntent&) override {return fail(ErrorCode::Unsupported,"unexpected transport");}
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId,const Value&) override {++outputs;}
  void emit_trace(const TraceEvent&) override {}
};
struct Engine {
  ProfileContext& context;Runtime& runtime;CoreRuntimeBackend backend;
  std::shared_ptr<Memory> memory;
  std::shared_ptr<RegisterBank> registers;
  std::shared_ptr<ObjectServices> services;
  Recorder inputs,records;
  std::uint64_t fuel_used{},operations{};
  InstanceId target;ConnectionId connection;
  static std::shared_ptr<RegisterBank> make_registers(){
    RegisterSpec a;a.offset=0;a.width_bits=32;a.allowed_access_bytes={1,2,4};
    a.fields={{FieldId{1},0,32,RegisterAccess::RC,{17,34,51,68}}};
    return std::make_shared<RegisterBank>(take(RegisterBank::make({a})));
  }
  Engine(ProfileContext& c,Runtime& r):context(c),runtime(r),backend(r,c.project()),
    memory(std::make_shared<Memory>(take(Memory::make(8,{10,11,12,13,14,15,16,17})))),registers(make_registers()) {
    const auto& b=c.project().system_metadata->bindings.at(0);
    target=InstanceId{b.sink_endpoint.instance_id};connection=ConnectionId{b.id};
    ObjectServiceConfig options;options.max_bytes=64;options.max_entries=8;
    services=take(ObjectServices::bind_existing(c.project(),{
      {{ObjectId{1},35,DomainId{c.project().system_metadata->runtime_domain},target},memory},{{ObjectId{2},36,DomainId{c.project().system_metadata->runtime_domain},target},registers}},options));
    take(services->register_into(backend));take(backend.freeze());
    inputs.add("initialMemory",Bytes(memory->data(),memory->data()+memory->size()));
    inputs.add("registerReset",Bytes{17,34,51,68});
  }
  std::vector<Value> state()const {std::vector<Value> out;for(const auto& s:context.state())out.push_back(s.value);return out;}
  std::vector<Value> call(std::uint32_t local,const std::vector<Value>& args){
    const auto& handler=context.handler(target.value,local);
    auto execution=context.execution(handler.program_id,{runtime.now(),0},connection,0,take(runtime.drains().epoch(target)));
    inputs.add("operation",Json::object({{"handler",observe(local)},{"arguments",observe(args)},{"ready",observe(execution.ready)}}));
    EventTxn txn({},execution);exec::FuelCounter fuel{10000};
    exec::Interpreter vm(context.validated(),context.state_refs(),&backend);
    auto result=take(vm.execute_segment(handler.program_id,execution,args,txn,fuel));
    records.add("segment",result);
    require(result.kind==exec::SegmentResult::Kind::Returned,"memory model did not return");
    records.add("commit",take(runtime.commit_segment(txn)));
    fuel_used+=10000-fuel.remaining;++operations;
    records.add("fuelUsed",10000-fuel.remaining);
    records.add("state",state());
    records.add("memory",Bytes(memory->data(),memory->data()+memory->size()));
    records.add("return",result.values);
    return result.values;
  }
  MemoryTransferResult transfer(const PayloadSnapshot& request,bool reg=false){
    inputs.add("payload",request);
    auto out=call(reg?2:0,{Value{std::uint64_t(request.command)},Value{request.address},Value{request.data},
      Value{request.streaming_width},Value{request.byte_enable}});
    const auto& result=std::get<Value::Array>(out.at(0).data);
    return {static_cast<ResponseStatus>(std::get<std::uint64_t>(result.at(0).data)),std::get<Bytes>(result.at(1).data)};
  }
  MemoryDebugResult debug(Command command,std::uint64_t address,Bytes bytes,bool reg=false){
    auto out=call(reg?3:1,{Value{std::uint64_t(command)},Value{address},Value{std::move(bytes)}});
    const auto& result=std::get<Value::Array>(out.at(0).data);
    return {std::size_t(std::get<std::uint64_t>(result.at(0).data)),std::get<Bytes>(result.at(1).data)};
  }
  Json result(const std::string& id,const std::vector<std::string>& branches){
    return context.record(id,inputs.json(),Json::object({{"records",records.json()},{"state",observe(state())},
      {"fuel",observe(fuel_used)},{"operations",observe(operations)},{"stop",Json("ScenarioCompleted")}}),branches);
  }
};
PayloadSnapshot request(Command command,std::uint64_t address,Bytes data,std::uint64_t streaming=0,Bytes mask={}){
  PayloadSnapshot p;p.command=command;p.address=address;p.data=std::move(data);p.streaming_width=streaming;p.byte_enable=std::move(mask);return p;
}
Json ordinary(ProfileContext& context,const std::string& id){
  QuietHost host;auto config=context.config();Runtime runtime(config,host);take(runtime.start(context.manifest(config)));Engine e(context,runtime);
  if(id=="C-T20"){
    auto write=e.transfer(request(Command::Write,1,{1,2,3,4,5,6},3,{255,0}));
    require(write.status==ResponseStatus::Ok,"masked streaming write");
    require(Bytes(e.memory->data(),e.memory->data()+8)==Bytes({10,1,5,3,14,15,16,17}),"all masked wrapping addresses");
    auto read=e.transfer(request(Command::Read,1,{90,91,92,93,94,95},3,{255,0}));
    require(read.status==ResponseStatus::Ok && read.data==Bytes({1,91,3,93,5,95}),"disabled read bytes retained");
    return e.result(id,{"masked-streaming-bytes"});
  }
  if(id=="C-T21"){
    auto initial=Bytes(e.memory->data(),e.memory->data()+8);
    auto ignore=e.transfer(request(Command::Ignore,UINT64_MAX,{9,8},0,{0}));
    require(ignore.status==ResponseStatus::Ok,"Ignore is not harmless");
    auto overflow=e.transfer(request(Command::Write,UINT64_MAX,{99,98},2));
    require(overflow.status==ResponseStatus::AddressError,"overflow address accepted");
    auto zero=e.transfer(request(Command::Write,UINT64_MAX,{},0));
    require(zero.status==ResponseStatus::BurstError,"zero-length request must report burst error");
    auto zero_width=e.transfer(request(Command::Read,0,{0},0));
    require(zero_width.status==ResponseStatus::BurstError,"zero streaming width must report burst error");
    require(Bytes(e.memory->data(),e.memory->data()+8)==initial,"invalid or empty write mutated memory");
    return e.result(id,{"ignore-overflow-zero"});
  }
  auto before=e.state();
  auto first=e.debug(Command::Read,0,{0,0,0,0},true);
  auto again=e.debug(Command::Read,0,{0,0,0,0},true);
  require(first.count==4 && first.data==Bytes({17,34,51,68}) && again.data==first.data,"debug read cleared RC register");
  auto partial=e.debug(Command::Write,2,{85,102,119,136},true);
  require(partial.count==2,"debug partial write count");
  auto observed=e.debug(Command::Read,0,{0,0,0,0},true);
  require(observed.data==Bytes({17,34,85,102}),"debug poke did not update shared register bytes");
  require(e.state()==before && host.outputs==0,"debug changed modeled state or IRQ");
  e.records.add("irqCount",host.outputs);
  auto normal=e.transfer(request(Command::Read,0,{0,0,0,0},4),true);
  require(normal.data==observed.data,"normal read did not see debug bytes");
  auto cleared=e.debug(Command::Read,0,{0,0,0,0},true);
  require(cleared.data==Bytes({0,0,0,0}),"normal RC read did not clear");
  return e.result(id,{"debug-rc-partial-noirq"});
}

struct ObservedHost : systemc::RuntimeHostAdapter {
  Recorder* records{};
  using systemc::RuntimeHostAdapter::RuntimeHostAdapter;
  Expected<WireReturn> transport(const SendIntent& intent) override {
    if(records)records->add("nativeSendIntent",intent);
    auto result=systemc::RuntimeHostAdapter::transport(intent);
    if(records && result)records->add("nativeWireReturn",Json::object({{"callId",observe(intent.call_id)},{"return",observe(result.value())}}));
    return result;
  }
  void observe_milestone(const RuntimeMilestoneObservation& value) override {
    if(records)records->add("protocolMilestone",value);
  }
  void recorded_return(CallId id,bool terminal) override {
    if(records)records->add("recordedReturn",Json::object({{"callId",observe(id)},{"terminal",terminal},{"tick",observe(runtime.now())}}));
    systemc::RuntimeHostAdapter::recorded_return(id,terminal);
  }
};
struct Mixed : sc_core::sc_module {
  ProfileContext& context;
  ObservedHost host;
  Engine engine;
  tlm_utils::simple_target_socket<Mixed> target;
  tlm_utils::simple_initiator_socket<Mixed> initiator;
  struct Manager : tlm::tlm_mm_interface {std::uint64_t freed{};void free(tlm::tlm_generic_payload*)override{++freed;}} manager;
  tlm::tlm_generic_payload gp{&manager};
  sc_core::sc_event response;
  AdmissionStore admission;
  std::uint64_t wire_calls{},irq{};
  bool finished{};
  std::exception_ptr error;
  SC_HAS_PROCESS(Mixed);
  Mixed(sc_core::sc_module_name name,ProfileContext& c):sc_module(name),context(c),host("host",c.config()),engine(c,host.runtime),
    target("target"),initiator("initiator"),admission(c.config().domain,engine.target,{8,8,8,8,8}) {
    host.records=&engine.records;
    host.trace=[this](const TraceEvent& event){engine.records.add("runtimeTrace",event);};
    target.register_nb_transport_fw(this,&Mixed::fw);target.register_b_transport(this,&Mixed::blocking);
    target.register_transport_dbg(this,&Mixed::debug);initiator.register_nb_transport_bw(this,&Mixed::bw);
    target.register_get_direct_mem_ptr(this,&Mixed::dmi);initiator.bind(target);
    take(systemc::validate_native_profile({},initiator,target));
    host.instance_output=[this](InstanceId,PortId,const Value&){++irq;};
    take(host.bind(engine.connection,[this](const SendIntent& intent){return host.resolve_transport(intent.transport);},
      [this](auto& payload,auto& phase,auto& delay){return target->nb_transport_bw(payload,phase,delay);}));
    host.runtime.set_blocking_handler([this](const BlockingRequest& call,Runtime& runtime)->Expected<void>{
      engine.records.add("blockingArrival",call.arrival);
      // Fixed trusted b_transport provider uses the same object as the descriptor ObjectCall.
      ExecutionContext execution;execution.domain=context.config().domain;execution.instance=call.instance;
      execution.owner=call.token.owner;execution.epoch=call.epoch;execution.ready={runtime.now(),0};
      EventTxn txn({},execution);auto transferred=engine.memory->transfer(txn,call.request);if(!transferred)return transferred.error();
      auto committed=runtime.commit_segment(txn);if(!committed)return committed.error();
      engine.records.add("blockingCommit",committed.value());
      engine.records.add("memory",Bytes(engine.memory->data(),engine.memory->data()+engine.memory->size()));
      engine.records.add("state",engine.state());engine.records.add("blockingFuelUsed",std::uint64_t{0});
      ResponseSnapshot response{transferred.value().status,
        call.request.command==Command::Read?transferred.value().data:Bytes{},false,{}};
      engine.records.add("blockingResponse",response);
      return runtime.complete_blocking(call.token,std::move(response));
    });
    host.runtime.set_input_handler([this](const WireCall& call,ReadyKey ready,Runtime& runtime)->Expected<void>{
      engine.records.add("wireCall",call);++wire_calls;
      require(call.phase==begin_req,"mixed unexpected request phase");
      auto transferred=engine.transfer(call.request);
      AdmissionRequest req;req.connection=call.connection;req.transport=call.transport;req.owner=0;req.in_time=ready.time;req.request=call.request;
      auto admitted=admission.admit(req,true);if(!admitted)return admitted.error();
      auto side=runtime.protocol(engine.target);if(!side)return side.error();
      auto hop=side.value()->find_ledger(call.connection,call.transport);if(!hop)return hop.error();
      auto tracked=runtime.drains().register_responsibility({admitted.value().txn,engine.target,0,{},{{hop.value(),false,false,false,false}},false,false});
      if(!tracked)return tracked.error();
      tracked=runtime.track_cleanup(admitted.value().txn,hop.value(),false,call.request);if(!tracked)return tracked.error();
      auto execution=context.execution(context.handler(engine.target.value,0).program_id,ready,engine.connection);
      EventTxn txn({},execution);auto id=runtime.stage_allocate_call_id(txn,CallOrigin::Outgoing);if(!id)return id.error();
      SendIntent send;send.txn=admitted.value().txn;send.connection=call.connection;send.transport=call.transport;
      send.call_id=id.value();send.flow=Flow::Backward;send.phase=begin_resp;send.not_before=Tick{ready.time.value+1};
      send.payload=call.request;send.payload.status=transferred.status;send.payload.data=transferred.data;
      auto staged=runtime.stage_publish(txn,send);if(!staged)return staged.error();
      auto committed=runtime.commit_segment(txn);if(!committed)return committed.error();
      engine.records.add("responseIntent",send);
      engine.records.add("responseCommit",committed.value());
      return {};
    });
    take(host.runtime.start(context.manifest(context.config())));
    SC_THREAD(run);
  }
  tlm::tlm_sync_enum fw(tlm::tlm_generic_payload& payload,tlm::tlm_phase& phase,sc_core::sc_time& delay){
    return take(host.receive(engine.connection,Flow::Forward,payload,phase,delay));
  }
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload& payload,tlm::tlm_phase& phase,sc_core::sc_time& delay){
    engine.records.add("nativeResponse",take(systemc::PayloadBridge{}.snapshot(payload)));
    engine.records.add("nativeResponsePhase",std::uint64_t(phase));engine.records.add("nativeResponseDelay",delay.value());
    engine.records.add("nativeResponseTick",sc_core::sc_time_stamp().value());
    require(phase==tlm::BEGIN_RESP && payload.get_response_status()==tlm::TLM_OK_RESPONSE,"mixed response");
    response.notify(sc_core::SC_ZERO_TIME);return tlm::TLM_COMPLETED;
  }
  void blocking(tlm::tlm_generic_payload& payload,sc_core::sc_time& delay){take(host.blocking(engine.connection,payload,delay));}
  unsigned debug(tlm::tlm_generic_payload& payload){
    systemc::NativeGuard guard;
    auto snapshot=take(systemc::PayloadBridge{}.snapshot(payload));
    auto result=engine.debug(snapshot.command,snapshot.address,snapshot.data);
    std::copy(result.data.begin(),result.data.end(),payload.get_data_ptr());
    payload.set_dmi_allowed(false);take(guard.unchanged());return unsigned(result.count);
  }
  bool dmi(tlm::tlm_generic_payload& payload,tlm::tlm_dmi& out){payload.set_dmi_allowed(false);out.init();return false;}
  void configure(Command command,std::uint64_t address,Bytes& bytes){
    gp.set_command(command==Command::Read?tlm::TLM_READ_COMMAND:tlm::TLM_WRITE_COMMAND);gp.set_address(address);
    gp.set_data_ptr(bytes.data());gp.set_data_length(unsigned(bytes.size()));gp.set_streaming_width(unsigned(bytes.size()));
    gp.set_byte_enable_ptr(nullptr);gp.set_byte_enable_length(0);gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    engine.inputs.add("nativeRequest",take(systemc::PayloadBridge{}.snapshot(gp)));
  }
  void run(){try{
    Bytes write{1,2,3,4};configure(Command::Write,0,write);auto delay=sc_core::sc_time::from_value(2);
    engine.inputs.add("blockingInputDelay",delay.value());initiator->b_transport(gp,delay);
    require(delay==sc_core::SC_ZERO_TIME && gp.get_response_status()==tlm::TLM_OK_RESPONSE,"blocking write result");
    engine.records.add("blockingReturn",take(systemc::PayloadBridge{}.snapshot(gp)));engine.records.add("blockingReturnDelay",delay.value());
    Bytes read(4);configure(Command::Read,0,read);delay=sc_core::sc_time::from_value(1);tlm::tlm_phase phase=tlm::BEGIN_REQ;
    gp.acquire();engine.inputs.add("nbInputDelay",delay.value());auto sync=initiator->nb_transport_fw(gp,phase,delay);
    require(sync==tlm::TLM_ACCEPTED,"mixed request not accepted");gp.release();
    engine.records.add("nbReturnSync",std::uint64_t(sync));engine.records.add("nbReturnPhase",std::uint64_t(phase));engine.records.add("nbReturnDelay",delay.value());
    wait(response);wait(sc_core::sc_time::from_value(1));
    require(read==Bytes({1,2,3,4}),"nb did not read b backing");
    Bytes poke{9,8};configure(Command::Write,1,poke);auto count=initiator->transport_dbg(gp);
    require(count==2,"debug write count");engine.records.add("debugCount",count);
    engine.records.add("debugNativeReturn",take(systemc::PayloadBridge{}.snapshot(gp)));engine.records.add("debugTick",sc_core::sc_time_stamp().value());
    Bytes final(4);configure(Command::Read,0,final);delay=sc_core::SC_ZERO_TIME;initiator->b_transport(gp,delay);
    require(final==Bytes({1,9,8,4}),"blocking did not read debug backing");
    engine.records.add("finalBlocking",take(systemc::PayloadBridge{}.snapshot(gp)));engine.records.add("finalDelay",delay.value());
    engine.records.add("mmFreeCount",manager.freed);engine.records.add("gpRefCount",gp.get_ref_count());engine.records.add("irqCount",irq);
    require(manager.freed==1 && gp.get_ref_count()==0 && irq==0 && wire_calls==1,"mixed pin/IRQ/call accounting");
    auto side=take(host.runtime.protocol(engine.target));auto ledger=take(side->find_ledger(engine.connection,TransportId{1}));
    auto state=take(side->inspect(ledger));engine.records.add("finalWireState",state);
    require(state.state==WireState::Terminal,"mixed wire not terminal");
    finished=true;
  }catch(...){error=std::current_exception();sc_core::sc_stop();}}
};
} // namespace

int sc_main(int argc,char**argv){
  if(argc!=6)return 2;
  try{
    require(std::string(argv[5])=="memory","memory scenario name");
    std::ofstream output(argv[1]);require(bool(output),"memory output path");
    for(const std::string id:{"C-T20","C-T21","C-T22"}){
      auto context=take(ProfileContext::load(argv[2],argv[3],argv[4]));
      output<<ordinary(*context,id).dump()<<'\n';
    }
    auto mixed_context=take(ProfileContext::load(argv[2],argv[3],argv[4]));
    Mixed mixed("mixed",*mixed_context);sc_core::sc_start(sc_core::sc_time::from_value(20));
    if(mixed.error)std::rethrow_exception(mixed.error);
    require(mixed.finished && !mixed.host.runtime.stopped(),"mixed interface scenario did not finish");
    output<<mixed.engine.result("C-T23",{"mixed-sockets-one-backing"}).dump()<<'\n';
    require(bool(output),"memory output write");return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}


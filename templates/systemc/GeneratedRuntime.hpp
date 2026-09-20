#pragma once
#include <leanat/descriptor.hpp>
#include <leanat/runtime_services.hpp>
#include <leanat/object_services.hpp>
#include <leanat/sideband_services.hpp>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <sstream>
#include <algorithm>

namespace leanat_generated {
using namespace leanat;
const Bytes& descriptor_bytes();
inline void require(Expected<void> result) {
  if (!result) throw std::runtime_error(result.error().message);
}
template<class T> T require(Expected<T> result) {
  if (!result) throw std::runtime_error(result.error().message);
  return std::move(result.value());
}
struct HandlerRow {
  std::uint32_t instance, program;
  std::optional<std::uint32_t> endpoint;
  ContextKind context;
  std::string trigger, source;
  std::size_t max_instances{1};
};
struct ConnectionRow {
  std::uint32_t id, source_instance, source_endpoint, source_index;
  std::uint32_t sink_instance, sink_endpoint, sink_index;
};
struct HostBindings {
  systemc::NativeProfile native_profile;
  std::vector<StandardObjectBinding> objects;
  ObjectServiceConfig object_config;
  // Host providers must independently check their descriptor signatures before registration.
  std::function<Expected<void>(CoreRuntimeBackend&,const exec::Project&,Runtime&)> providers;
  std::function<Expected<std::vector<Value>>(const WireCall&,const ExecutionContext&,EventTxn&)> wire_inputs;
  // An owning guard keeps provider-local payload/access scopes alive across the complete segment.
  std::function<Expected<std::shared_ptr<void>>(const WireCall&,const ExecutionContext&)> wire_scope;
  std::function<Expected<ResponseSnapshot>(InstanceId,std::uint32_t,const PayloadSnapshot&)> blocking;
  std::function<Expected<unsigned>(InstanceId,std::uint32_t,tlm::tlm_generic_payload&)> debug;
  std::function<void(const TraceEvent&)> trace;
};
class Engine {
  exec::ValidatedProject descriptor_;
  std::vector<VersionedCell> cells_;
  std::vector<VersionedCell*> state_;
  std::vector<HandlerRow> handlers_;
  std::vector<ConnectionRow> connections_;
  HostBindings bindings_;
  std::shared_ptr<ObjectServices> objects_;
  std::uint64_t fuel_;
  SegmentBudget segment_budget_;
  std::map<std::pair<std::uint32_t,std::uint32_t>,Value> inputs_;
  SidebandInputStore input_store_;
  std::map<std::pair<std::uint32_t,std::uint32_t>,std::function<void(const Value&)>> outputs_;
  std::map<TransportId,systemc::TransportPin> originating_;
  std::size_t originating_capacity_{};
  std::map<std::uint32_t,std::vector<Handle>> active_processes_;
  static exec::ValidatedProject load(const std::array<std::uint8_t,32>& hash,const std::string& profile) {
    if (exec::sha256(descriptor_bytes())!=hash)
      throw std::runtime_error("generated facade/descriptor identity mismatch");
    exec::LoadPolicy policy;policy.expected_profile=profile;
    return require(exec::load_descriptor(descriptor_bytes(),policy));
  }
  Expected<void> finish(exec::SegmentResult result,EventTxn& txn) {
    if (result.kind==exec::SegmentResult::Kind::Failed)
      return fail(ErrorCode::InvalidState,result.error);
    if (result.kind==exec::SegmentResult::Kind::TransportReturned)
      return fail(ErrorCode::Unsupported,"transport return requires a synchronous ingress policy");
    auto committed=host.runtime.commit_segment(txn);
    if(!committed)return committed.error();
    for(const auto& trace:result.traces)host.emit_trace(trace);
    return {};
  }
  Expected<exec::SegmentResult> run_handler(const HandlerRow& handler,const ExecutionContext& context,
                                           const std::vector<Value>& args,EventTxn& txn) {
    std::optional<Handle> process;
    if(handler.context==ContextKind::Process) {
      auto& active=active_processes_[handler.program];
      active.erase(std::remove_if(active.begin(),active.end(),[this](Handle handle){
        auto state=host.runtime.processes().inspect(handle);
        return !state || state.value().state==ProcessState::Terminal;
      }),active.end());
      if(active.size()>=handler.max_instances)return fail(ErrorCode::Capacity,"generated process instance limit");
      auto created=host.runtime.processes().create(ProgramId{handler.program},context.owner);
      if(!created)return created.error();
      process=created.value();active.push_back(*process);
      auto begun=host.runtime.processes().begin(*process,txn);
      if(!begun){(void)host.runtime.processes().cancel(*process);return begun.error();}
      auto bound=backend.bind_current_process(*process);
      if(!bound){(void)host.runtime.processes().cancel(*process);return bound.error();}
    }
    exec::Interpreter vm(descriptor_,state_,&backend);exec::FuelCounter fuel{fuel_};
    auto result=vm.execute_segment(handler.program,context,args,txn,fuel);
    if(!result){if(process)(void)host.runtime.processes().cancel(*process);return result.error();}
    if(process && result.value().kind==exec::SegmentResult::Kind::Returned) {
      auto complete=host.runtime.processes().complete(*process,txn);
      if(!complete){(void)host.runtime.processes().cancel(*process);return complete.error();}
    }
    auto committed=finish(result.value(),txn);
    if(!committed){if(process)(void)host.runtime.processes().cancel(*process);return committed.error();}
    return std::move(result.value());
  }
public:
  systemc::RuntimeDomain domain;
  systemc::RuntimeHostAdapter host;
  CoreRuntimeBackend backend;
  Engine(const RuntimeConfig& config,const std::array<std::uint8_t,32>& hash,const std::string& profile,
         std::vector<HandlerRow> handlers,std::vector<ConnectionRow> connections,
         std::uint64_t fuel,SegmentBudget budget,HostBindings bindings)
    :descriptor_(load(hash,profile)),handlers_(std::move(handlers)),connections_(std::move(connections)),
     bindings_(std::move(bindings)),fuel_(fuel),segment_budget_(budget),input_store_(config.domain),
     domain("domain",config.domain),host("host",config),backend(host.runtime,descriptor_.get()) {
    require(systemc::validate_native_host(bindings_.native_profile));
    originating_capacity_=config.intent_capacity;
    if(!fuel_)throw std::invalid_argument("zero generated segment fuel");
    cells_.reserve(descriptor_.get().initial_state.size());
    for(const auto& value:descriptor_.get().initial_state)cells_.push_back({value,0,0,false});
    for(auto& cell:cells_)state_.push_back(&cell);
    for(const auto& instance:descriptor_.get().instances) {
      auto definition=std::find_if(descriptor_.get().components.begin(),descriptor_.get().components.end(),
        [&](const auto& component){return component.id==instance.definition;});
      if(definition==descriptor_.get().components.end())throw std::runtime_error("sideband instance definition missing");
      for(const auto& port:definition->sidebands)if(port.direction==exec::PortDirection::Input) {
        require(input_store_.define_port(InstanceId{instance.id},std::uint64_t(instance.id)+1,port.id,
          port.direction,descriptor_.get().types.at(port.type_id),port.initial));
        if(port.initial)inputs_[{instance.id,port.id}]=*port.initial;
      }
    }
    require(input_store_.freeze());
    require(register_sideband_services(backend,input_store_,descriptor_.get()));
    if(!bindings_.objects.empty()) {
      objects_=require(ObjectServices::bind_existing(descriptor_.get(),bindings_.objects,bindings_.object_config));
      require(objects_->register_into(backend));
    }
    for(const auto& signature:descriptor_.get().services) {
      switch(signature.op) {
      case exec::Op::GetContextField:case exec::Op::RegisterWait:case exec::Op::ScheduleEvent:
      case exec::Op::CancelEvent:case exec::Op::ResultGet:case exec::Op::ResultRelease:
      case exec::Op::ReadWaitResult:case exec::Op::CancelLocal:case exec::Op::BufferOutputWrite:
        require(backend.register_core(signature));break;
      default:break;
      }
    }
    if(bindings_.providers)require(bindings_.providers(backend,descriptor_.get(),host.runtime));
    require(backend.freeze());
    for(const auto& signature:descriptor_.get().services)require(backend.check_signature(signature));
    host.trace=bindings_.trace;
    host.instance_output=[this](InstanceId instance,PortId port,const Value& value) {
      auto it=outputs_.find({instance.value,port.value});
      if(it==outputs_.end())throw std::runtime_error("unbound generated sideband output");
      it->second(value);
    };
    host.runtime.set_blocking_handler([this](const BlockingRequest& request,Runtime& runtime)->Expected<void>{
      if(!bindings_.blocking)return fail(ErrorCode::Unsupported,"blocking provider not installed");
      const ConnectionRow* connection=nullptr;
      for(const auto& row:connections_)if(row.id==request.connection.value){connection=&row;break;}
      if(!connection || connection->sink_instance!=request.instance.value)
        return fail(ErrorCode::Schema,"blocking endpoint instance mismatch");
      auto response=bindings_.blocking(request.instance,connection->sink_endpoint,request.request);
      if(!response)return response.error();
      return runtime.complete_blocking(request.token,std::move(response.value()));
    });
    host.runtime.set_input_handler([this](const WireCall& call,ReadyKey ready,Runtime&)->Expected<void>{
      const ConnectionRow* connection=nullptr;
      for(const auto& row:connections_)if(row.id==call.connection.value){connection=&row;break;}
      if(!connection)return fail(ErrorCode::InvalidArgument,"unknown generated connection");
      auto instance=call.flow==Flow::Forward?connection->sink_instance:connection->source_instance;
      auto endpoint=call.flow==Flow::Forward?connection->sink_endpoint:connection->source_endpoint;
      const char* event=call.phase==begin_req?"beginReq":call.phase==end_req?"endReq":call.phase==begin_resp?"beginResp":"endResp";
      const HandlerRow* handler=nullptr;
      for(const auto& row:handlers_) {
        auto suffix=row.trigger.substr(row.trigger.find_last_of('.')==std::string::npos?0:row.trigger.find_last_of('.')+1);
        if(row.instance==instance && row.endpoint==endpoint && suffix==event) {
          if(handler)return fail(ErrorCode::Schema,"ambiguous generated wire handler");
          handler=&row;
        }
      }
      if(!handler)return fail(ErrorCode::Unsupported,"missing generated wire handler");
      auto context=make_context(*handler,ready);context.connection=call.connection;
      EventTxn txn(segment_budget_,context);
      std::shared_ptr<void> scope;
      if(bindings_.wire_scope) {
        auto bound=bindings_.wire_scope(call,context);if(!bound)return bound.error();scope=std::move(bound.value());
      }
      std::vector<Value> args;
      if(bindings_.wire_inputs) {
        auto inputs=bindings_.wire_inputs(call,context,txn);if(!inputs)return inputs.error();args=std::move(inputs.value());
      }
      auto result=run_handler(*handler,context,args,txn);
      if(!result)return fail(result.error().code,handler->source+": "+result.error().message);
      return {};
    });
    host.runtime.set_resume_handler([this](const SuspensionToken& token,ReadyKey ready,Runtime&)->Expected<void>{
      auto process=host.runtime.processes().inspect(token.process);if(!process)return process.error();
      const HandlerRow* handler=nullptr;
      for(const auto& row:handlers_)if(row.program==process.value().program.value){handler=&row;break;}
      if(!handler)return fail(ErrorCode::Schema,"resume program missing generated source binding");
      auto context=make_context(*handler,ready);context.owner=token.process.owner;
      EventTxn txn(segment_budget_,context);exec::FuelCounter fuel{fuel_};exec::Interpreter vm(descriptor_,state_,&backend);
      auto result=vm.resume_segment(token,context,txn,fuel);if(!result)return result.error();
      if(result.value().kind==exec::SegmentResult::Kind::Returned) {
        auto complete=host.runtime.processes().complete(token.process,txn);if(!complete)return complete.error();
      }
      return finish(std::move(result.value()),txn);
    });
    host.runtime.set_handler([this](const QueuedEvent& queued,Runtime&)->Expected<void>{
      if(queued.event.key.stage!=EventStage::Input)return fail(ErrorCode::Unsupported,"unbound generated internal event");
      auto values=std::get_if<Value::Array>(&queued.event.value.data);
      if(!values || values->size()!=2)return fail(ErrorCode::Schema,"sideband input event layout");
      auto port=std::get_if<std::uint64_t>(&(*values)[0].data);
      if(!port)return fail(ErrorCode::Schema,"sideband port identity");
      if(*port>UINT32_MAX)return fail(ErrorCode::Schema,"sideband port identity overflow");
      auto updated=input_store_.update(queued.event.key.instance,std::uint32_t(*port),(*values)[1]);
      if(!updated)return updated.error();
      inputs_[{queued.event.key.instance.value,std::uint32_t(*port)}]=(*values)[1];
      return {};
    });
  }
  ~Engine() {domain.stop();}
  ExecutionContext make_context(const HandlerRow& row,ReadyKey ready) {
    ExecutionContext result;result.domain=domain.id;result.instance=InstanceId{row.instance};
    result.kind=row.context;result.ready=ready;result.owner=std::uint64_t(row.instance)+1;
    result.epoch=require(host.runtime.drains().epoch(result.instance));
    // The full source identity accompanies failures; line details remain in the descriptor.
    result.source.file_hash=row.source;return result;
  }
  Expected<exec::SegmentResult> execute(std::uint32_t program,const std::vector<Value>& args={}) {
    const HandlerRow* handler=nullptr;
    for(const auto& row:handlers_)if(row.program==program){handler=&row;break;}
    if(!handler)return fail(ErrorCode::InvalidArgument,"program not a generated instance handler");
    auto context=make_context(*handler,{host.runtime.now(),0});EventTxn txn(segment_budget_,context);
    return run_handler(*handler,context,args,txn);
  }
  void start(const RuntimeConfig& config) {
    HostBindingManifest binding{config.domain,config.descriptor_identity,config.connections,true,true};
    binding.instances=config.instances;binding.connection_bindings=config.connection_bindings;
    require(host.runtime.start(binding));
    require(domain.start());
  }
  std::vector<Value> state()const {std::vector<Value> result;for(const auto& c:cells_)result.push_back(c.value);return result;}
  const HostBindings& bindings()const{return bindings_;}
  Expected<tlm::tlm_generic_payload*> resolve(const SendIntent& intent) {
    auto existing=host.resolve_transport(intent.transport);if(existing)return existing;
    auto owned=originating_.find(intent.transport);if(owned!=originating_.end())return &owned->second.get();
    if(intent.flow!=Flow::Forward || intent.phase!=begin_req)return existing.error();
    if(originating_.size()>=originating_capacity_)return fail(ErrorCode::Capacity,"generated payload origin budget");
    auto payload=systemc::PayloadBridge{}.make_managed(intent.payload);if(!payload)return payload.error();
    auto inserted=originating_.emplace(intent.transport,std::move(payload.value()));return &inserted.first->second.get();
  }
  void release_origin(tlm::tlm_generic_payload& gp) {
    for(auto it=originating_.begin();it!=originating_.end();++it)if(&it->second.get()==&gp){originating_.erase(it);return;}
  }
  void bind_output(std::uint32_t instance,std::uint32_t id,std::function<void(const Value&)> sink) {
    if(!outputs_.emplace(std::make_pair(instance,id),std::move(sink)).second)throw std::invalid_argument("duplicate output port identity");
  }
  void input(std::uint32_t instance,std::uint32_t port,Value value) {
    auto now=require(domain.time.to_tick(sc_core::sc_time_stamp()));
    auto ready=require(host.runtime.queue().successor(now));
    EventDraft event;event.key.time=ready.time;event.key.turn=ready.turn;event.key.stage=EventStage::Input;
    event.key.instance=InstanceId{instance};event.owner=std::uint64_t(instance)+1;
    event.epoch=require(host.runtime.drains().epoch(event.key.instance));
    event.value=Value{Value::Array{Value{std::uint64_t(port)},std::move(value)}};
    require(host.runtime.schedule(std::move(event)));
  }
  std::optional<Value> input_value(std::uint32_t instance,std::uint32_t port)const {
    auto found=inputs_.find({instance,port});if(found==inputs_.end())return {};return found->second;
  }
};

template<class T> struct SignalInput : sc_core::sc_module {
  sc_core::sc_in<T> input;Engine& engine;std::uint32_t instance,port;
  SC_HAS_PROCESS(SignalInput);
  SignalInput(sc_core::sc_module_name name,Engine& e,std::uint32_t i,std::uint32_t p)
    :sc_module(name),input("input"),engine(e),instance(i),port(p) {
    SC_METHOD(sample);sensitive<<input;
  }
  void sample() {
    if constexpr(std::is_same_v<T,bool>)engine.input(instance,port,Value{input.read()});
    else engine.input(instance,port,Value{std::uint64_t(input.read().to_uint64())});
  }
};

template<unsigned Width> class TargetEndpoint : public sc_core::sc_module {
  Engine& engine_;std::uint32_t instance_,endpoint_;std::vector<ConnectionId> connections_;
public:
  using Socket=tlm_utils::simple_target_socket_tagged<TargetEndpoint<Width>,Width>;
  std::vector<std::unique_ptr<Socket>> sockets;
  TargetEndpoint(sc_core::sc_module_name name,Engine& engine,std::uint32_t instance,std::uint32_t endpoint,std::vector<ConnectionId> connections)
    :sc_module(name),engine_(engine),instance_(instance),endpoint_(endpoint),connections_(std::move(connections)) {
    for(std::size_t i=0;i<connections_.size();++i) {
      sockets.push_back(std::make_unique<Socket>(("binding_"+std::to_string(i)).c_str()));
      sockets.back()->register_nb_transport_fw(this,&TargetEndpoint::nb,int(i));
      sockets.back()->register_b_transport(this,&TargetEndpoint::blocking,int(i));
      sockets.back()->register_transport_dbg(this,&TargetEndpoint::debug,int(i));
      sockets.back()->register_get_direct_mem_ptr(this,&TargetEndpoint::dmi,int(i));
    }
  }
  tlm::tlm_sync_enum nb(int index,tlm::tlm_generic_payload& gp,tlm::tlm_phase& phase,sc_core::sc_time& delay) {
    return require(engine_.host.receive(connections_.at(index),Flow::Forward,gp,phase,delay));
  }
  void blocking(int index,tlm::tlm_generic_payload& gp,sc_core::sc_time& delay) {
    require(engine_.host.blocking(connections_.at(index),gp,delay));
  }
  unsigned debug(int,tlm::tlm_generic_payload& gp) {
    gp.set_dmi_allowed(false);
    if(!engine_.bindings().debug)return 0;
    systemc::NativeGuard guard;auto result=engine_.bindings().debug(InstanceId{instance_},endpoint_,gp);require(guard.unchanged());return require(std::move(result));
  }
  bool dmi(int,tlm::tlm_generic_payload& gp,tlm::tlm_dmi& value) {gp.set_dmi_allowed(false);value.init();return false;}
};
template<unsigned Width> class InitiatorEndpoint : public sc_core::sc_module {
  Engine& engine_;std::vector<ConnectionId> connections_;
public:
  using Socket=tlm_utils::simple_initiator_socket_tagged<InitiatorEndpoint<Width>,Width>;
  std::vector<std::unique_ptr<Socket>> sockets;
  InitiatorEndpoint(sc_core::sc_module_name name,Engine& engine,std::vector<ConnectionId> connections)
    :sc_module(name),engine_(engine),connections_(std::move(connections)) {
    for(std::size_t i=0;i<connections_.size();++i) {
      sockets.push_back(std::make_unique<Socket>(("binding_"+std::to_string(i)).c_str()));
      sockets.back()->register_nb_transport_bw(this,&InitiatorEndpoint::nb,int(i));
      sockets.back()->register_invalidate_direct_mem_ptr(this,&InitiatorEndpoint::invalidate,int(i));
    }
  }
  tlm::tlm_sync_enum nb(int index,tlm::tlm_generic_payload& gp,tlm::tlm_phase& phase,sc_core::sc_time& delay) {
    return require(engine_.host.receive(connections_.at(index),Flow::Backward,gp,phase,delay));
  }
  void invalidate(int,sc_dt::uint64,sc_dt::uint64) {throw std::runtime_error("unexpected invalidation on DMI-disabled endpoint");}
};
} // namespace leanat_generated

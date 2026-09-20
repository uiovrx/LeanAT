#pragma once
#include "opcode_fixture.hpp"
#include <leanat/structured_services.hpp>
#include "../../tests/support/event_queue_test_access.hpp"

namespace leanat::opcode_test {
namespace structured_fixture_detail {
inline bool supported(exec::Op op) {
  switch(op) {
  case exec::Op::SpawnProcess: case exec::Op::TrySpawnProcess: case exec::Op::SubmitTask:
  case exec::Op::CancelTask: case exec::Op::TaskResultGet: case exec::Op::TaskResultRelease:
  case exec::Op::RequestTaskSlot: case exec::Op::ScopeNew: case exec::Op::ScopeTransfer:
  case exec::Op::ScopeCancel: case exec::Op::ScopeClose: case exec::Op::WaitGroupNew:
  case exec::Op::WaitArm: case exec::Op::WaitResultGet: case exec::Op::WaitGroupRelease:return true;
  default:return false;
  }
}
inline Value optional(std::optional<Handle> h) {
  return Value{Value::Array{Value{std::uint64_t(h?1:0)},Value{h?Value::Array{Value{*h}}:Value::Array{}}}};
}
inline Value identities(const std::vector<Handle>& hs) {Value::Array out;for(auto h:hs)out.push_back(Value{h});return Value{out};}
struct Owned {
  Runtime &runtime;
  ExecutionContext context;
  CancelScopeStore scopes;
  TaskPool tasks;
  WaitGroupStore waits;
  StructuredServices services;
  std::map<Handle,Handle> aliases;
  Handle process;
  std::vector<Handle> seeded_processes;
  std::optional<Value> completed;
  std::vector<Json> observations;
  std::vector<Json> child_fuel_scopes;
  Owned(Runtime&r,ExecutionContext c,TaskPoolDesc d):runtime(r),context(c),scopes(c.domain,130,16,64,256),tasks(c.domain,131,d,r.results(),&scopes),waits(c.domain,132,16,64,d.result_bytes,r.results()),services(r,scopes,tasks,waits){}
  Value remap(const Value&v)const {
    if(auto h=std::get_if<Handle>(&v.data)){
      auto i=aliases.find(*h);if(i!=aliases.end())return Value{i->second};
      for(const auto&entry:aliases){const auto&logical=entry.first;
        if(h->kind==logical.kind&&h->domain==logical.domain&&h->store==logical.store&&h->slot==logical.slot){
          auto actual=entry.second;actual.owner=h->owner;
          if(h->generation>=logical.generation){auto delta=h->generation-logical.generation;actual.generation=delta<=UINT64_MAX-actual.generation?actual.generation+delta:0;}
          else{auto delta=logical.generation-h->generation;actual.generation=delta<=actual.generation?actual.generation-delta:0;}
          return Value{actual};
        }
      }
      return v;
    }
    if(auto a=std::get_if<Value::Array>(&v.data)){Value::Array out;for(const auto&x:*a)out.push_back(remap(x));return Value{out};}
    return v;
  }
  void record_observation(const std::string&kind, const std::string&opcode, ReadyKey ready, Value::Array values) {
    observations.push_back(Json::object({{"kind",kind},{"opcode",opcode},{"source",""},{"time",observe(ready.time)},{"turn",ready.turn},{"values",observe(Value{std::move(values)})}}));
  }
  void record_completion(Handle h, const ProcessSnapshot&p, const std::vector<Value>&values, ReadyKey ready) {
    record_observation("process.completed","return",ready,{Value{h},Value{std::uint64_t(p.program.value)},Value{p.suspension_ordinal},Value{values}});
  }
  Expected<void> run_child(const exec::Project&project, exec::InterpreterObserver*observer) {
    auto validated=exec::validate(project);if(!validated)return validated.error();
    auto before=tasks.snapshot();if(!before)return before.error();
    if(before.value().tasks.size()!=1)return fail(ErrorCode::InvalidState,"child fixture requires one task");
    auto task=before.value().tasks.front();auto wake=runtime.next_wakeup();
    if(!wake)return fail(ErrorCode::NotReady,"child fixture missing actual start event");
    unsigned executed=0;
    runtime.set_handler([&](const QueuedEvent&e,Runtime&r)->Expected<void>{
      auto values=std::get_if<Value::Array>(&e.event.value.data);
      if(++executed!=1||!values||values->size()!=2||(*values)[0]!=Value{task.identity})return fail(ErrorCode::InvalidState,"child fixture event identity");
      auto ph=std::get_if<Handle>(&(*values)[1].data);if(!ph)return fail(ErrorCode::TypeMismatch,"child fixture process identity");
      auto child=context;child.kind=ContextKind::Process;child.instance=e.event.key.instance;child.connection=e.event.key.connection;child.owner=ph->owner;child.ready={e.event.key.time,e.event.key.turn};
      SegmentBudget budget{4096,4096,128*1024*1024,4096};EventTxn tx(budget,child);
      auto start=services.start_task(task.identity,child,tx);if(!start)return start.error();
      if(start.value().process!=*ph)return fail(ErrorCode::WrongOwner,"child fixture execution binding");
      auto program=std::find_if(project.programs.begin(),project.programs.end(),[&](const auto&p){return p.id==start.value().program.value;});
      if(program==project.programs.end()||!program->instruction_fuel||program->owner_policy!="caller"||program->result_lifetime_policy!="until-release")return fail(ErrorCode::InvalidArgument,"child fixture requires declared ProcessIR policy");
      CoreRuntimeBackend backend(r);auto bound=backend.bind_current_process(*ph);if(!bound)return bound.error();
      for(const auto&sig:project.services)if(supported(sig.op)){auto b=services.bind(backend,sig,project,start.value().program);if(!b)return b.error();}
      auto frozen=backend.freeze();if(!frozen)return frozen.error();
      exec::Interpreter vm(validated.value(),{},&backend,observer);exec::FuelCounter fuel{program->instruction_fuel};
      const auto initial_fuel=fuel.remaining;
      auto ran=vm.execute_segment(program->id,child,start.value().arguments,tx,fuel);
      child_fuel_scopes.push_back(Json::object({{"program",program->id},{"inputFuel",initial_fuel},{"runtimeCap",program->instruction_fuel},{"remainingFuel",fuel.remaining}}));
      if(!ran)return ran.error();
      if(ran.value().kind!=exec::SegmentResult::Kind::Returned||ran.value().values.size()!=1)return fail(ErrorCode::InvalidState,"child fixture did not return one value");
      auto snapshot=r.processes().inspect(*ph);if(!snapshot)return snapshot.error();
      auto completed=services.complete(task.identity,{TaskOutcome::Kind::Success,ran.value().values.front()},child.ready,tx);if(!completed)return completed.error();
      auto committed=r.commit_segment(tx);if(!committed)return committed.error();
      record_completion(*ph,snapshot.value(),ran.value().values,child.ready);
      auto cap=tasks.result_handle(task.identity,task.scope);if(!cap)return cap.error();
      auto emit=[&](const std::string&phase,const Value&value)->Expected<void>{
        auto state=tasks.snapshot();if(!state)return state.error();auto raw=r.results().snapshot();
        auto active=std::any_of(raw.consumer_slots.begin(),raw.consumer_slots.end(),[&](const auto&c){return c.identity==cap.value().consumer&&c.active;});
        record_observation(phase,"",child.ready,{Value{task.identity},Value{cap.value().result},Value{cap.value().consumer},value,Value{std::uint64_t(state.value().frames)},Value{r.results().alive({cap.value().result})},Value{active},Value{fuel.remaining}});return {};
      };
      for(const auto&phase:{"structured.child.completed","structured.child.retained"}){auto value=tasks.owning_result(task.identity,cap.value());if(!value)return value.error();auto emitted=emit(phase,value.value());if(!emitted)return emitted.error();}
      auto owning=tasks.owning_result(task.identity,cap.value());if(!owning)return owning.error();
      auto released=tasks.release_result(task.identity,cap.value());if(!released)return released.error();
      return emit("structured.child.released",owning.value());
    });
    auto pumped=runtime.pump_batch(wake->time,256);runtime.set_handler({});
    if(!pumped)return pumped.error();
    if(executed!=1)return fail(ErrorCode::InvalidState,"child fixture did not execute");
    return {};
  }
  Json snapshot() const {
    auto ts=tasks.snapshot();auto ss=scopes.snapshot();auto ws=waits.snapshot();auto bs=services.snapshot();
    if(!ts||!ss||!ws||!bs)return Json::object({{"error","StructuredSnapshotPending"}});
    std::vector<Json> scopes_json,tasks_json,tickets_json,waits_json,results_json,consumers_json,bindings_json,processes_json,external_waits_json,events_json,wait_owners_json;
    for(const auto&s:ss.value().scopes){std::vector<Json> owned,actions;
      for(const auto&o:s.owned)owned.push_back(Json::object({{"handle",observe(o.handle)},{"kind",observe(o.kind)},{"published",o.published}}));
      if(s.plan)for(const auto&a:s.plan->actions)actions.push_back(Json::object({{"id",a.id},{"scope",observe(a.scope)},{"handle",observe(a.owned.handle)},{"ownedKind",observe(a.owned.kind)},{"kind",observe(a.kind)},{"applied",a.applied}}));
      scopes_json.push_back(Json::object({{"identity",observe(s.identity)},{"parent",s.parent.generation?observe(s.parent):Json{}},{"status",observe(s.state)},{"owned",Json::array(owned)},{"reason",s.plan?Json{s.plan->reason}:Json{}},{"actions",Json::array(actions)}}));
    }
    for(const auto&t:ts.value().tasks){auto p=bs.value().processes.find(t.identity),e=bs.value().starts.find(t.identity);
      tasks_json.push_back(Json::object({{"identity",observe(t.identity)},{"scope",observe(t.scope)},{"status",observe(t.state)},{"args",observe(Value{t.arguments})},{"result",observe(t.result.reservation.result)},{"consumer",observe(t.result.consumer.consumer)},{"process",p==bs.value().processes.end()?Json{}:observe(p->second)},{"start",e==bs.value().starts.end()?Json{}:observe(e->second)},{"producerReleased",t.producer_released}}));
    }
    for(const auto&t:ts.value().tickets)tickets_json.push_back(Json::object({{"identity",observe(t.info.ticket)},{"process",observe(t.info.waiter)},{"scope",observe(t.info.scope)},{"status",observe(t.info.state)},{"waiting",t.waiting},{"consumerReleased",t.consumer_released},{"reservedTask",t.task?observe(*t.task):Json{}},{"result",t.reservation?observe(t.reservation->reservation.result):Json{}},{"consumer",t.reservation?observe(t.reservation->consumer.consumer):Json{}}}));
    for(const auto&w:ws.value()){std::vector<Json> branches;Handle external{};
      for(const auto&b:bs.value().waits)if(b.second==w.identity)external=b.first;
      for(const auto&b:w.branches)branches.push_back(Json::object({{"ordinal",b.ordinal},{"priority",b.priority},{"source",observe(b.source)},{"type",observe(b.type)},{"field",b.field},{"result",b.consumer?observe(b.consumer->result):Json{}},{"consumer",b.consumer?observe(b.consumer->consumer):Json{}},{"ready",b.ready?observe(*b.ready):Json{}},{"outcome",b.outcome?observe(*b.outcome):Json{}},{"failed",b.failed},{"pinned",b.pinned},{"transferred",b.transferred}}));
      waits_json.push_back(Json::object({{"identity",observe(w.identity)},{"external",observe(external)},{"process",observe(w.process)},{"scope",observe(w.scope)},{"mode",observe(w.mode)},{"policy",observe(w.policy)},{"count",w.branch_count},{"resultType",observe(w.result_type)},{"resultBytes",w.result_bytes},{"result",observe(w.result.reservation.result)},{"consumer",observe(w.result.consumer.consumer)},{"committed",w.committed},{"resolved",w.resolved},{"resumePending",w.resume_pending},{"branches",Json::array(branches)}}));
    }
    auto rs=runtime.results().snapshot(); auto pc=runtime.processes().counter_snapshot(); const auto& td=tasks.descriptor();
    for(const auto&r:rs.result_slots)results_json.push_back(Json::object({{"identity",observe(r.identity)},{"alive",r.alive},{"producerAlive",r.producer_alive},{"publishing",r.publishing},{"source",observe(r.create.source)},{"producerOwner",r.create.producer_owner},{"initialConsumerOwner",r.create.consumer_owner},{"type",observe(r.create.type)},{"maxBytes",r.create.max_bytes},{"value",r.value?observe(*r.value):Json{}},{"ready",r.ready?observe(*r.ready):Json{}},{"consumerCount",r.consumer_count},{"pinCount",r.pin_count}}));
    for(const auto&c:rs.consumer_slots)consumers_json.push_back(Json::object({{"identity",observe(c.identity)},{"result",observe(c.result)},{"active",c.active}}));
    for(const auto&p:bs.value().processes)bindings_json.push_back(Json::object({{"task",observe(p.first)},{"process",observe(p.second)}}));
    for(const auto&p:bs.value().wait_owners)wait_owners_json.push_back(Json::object({{"external",observe(p.first)},{"process",observe(p.second.process)},{"scope",observe(p.second.scope)},{"result",observe(p.second.primary.result)},{"consumer",observe(p.second.primary.consumer)}}));
    std::vector<Handle> processes=seeded_processes;for(const auto&p:bs.value().processes)processes.push_back(p.second);
    for(auto h:processes){auto p=runtime.processes().inspect(h);if(!p){processes_json.push_back(Json::object({{"identity",observe(h)},{"alive",false},{"completed",h==process&&completed?observe(*completed):Json{}}}));continue;}
      processes_json.push_back(Json::object({{"identity",observe(h)},{"alive",true},{"program",observe(p.value().program)},{"status",observe(p.value().state)},{"instance",observe(p.value().instance)},{"instanceBound",p.value().instance_bound},{"ordinal",p.value().suspension_ordinal},{"wait",p.value().suspension?observe(p.value().suspension->wait):Json{}}}));}
    std::set<Handle> registered_waits;for(const auto&mapping:bs.value().waits)registered_waits.insert(mapping.first);
    for(auto h:processes){auto p=runtime.processes().inspect(h);if(p&&p.value().suspension)registered_waits.insert(p.value().suspension->wait);}
    for(auto wait:registered_waits){auto spec=runtime.processes().wait_spec(wait);auto outcome=runtime.processes().read_wait_result(wait);if(spec)external_waits_json.push_back(Json::object({{"identity",observe(wait)},{"group",observe(spec.value().source)},{"kind",observe(spec.value().kind)},{"source",observe(spec.value().source)},{"deadline",observe(spec.value().until)},{"value",outcome?observe(outcome.value().value):Json{}},{"ready",outcome?observe(outcome.value().source):Json{}}}));}
    auto queue=testing::EventQueueTestAccess::snapshot(runtime.queue());
    for(const auto&e:queue.slots)events_json.push_back(Json::object({{"identity",observe(e.queued.token)},{"state",e.state},{"cancelled",e.queued.cancelled},{"key",observe(e.queued.event.key)},{"value",observe(e.queued.event.value)},{"owner",e.queued.event.owner},{"epoch",e.queued.event.epoch}}));
    std::vector<Json> observers;for(const auto&o:ss.value().observers)observers.push_back(Json::object({{"id",o.id},{"source",observe(o.source)},{"process",observe(o.process)},{"wait",observe(o.wait)}}));
    return Json::object({{"observations",Json::array(observations)},{"childFuelScopes",Json::array(child_fuel_scopes)},{"limits",Json::object({{"scopes",scopes.control_capacity()},{"tasks",td.task_capacity},{"tickets",td.waiter_limit},{"groups",waits.control_capacity()},{"results",rs.result_slots.size()},{"consumers",rs.consumer_slots.size()},{"events",queue.slots.size()},{"processes",pc.frame_capacity},{"processWaits",pc.wait_capacity},{"liveValues",pc.live_capacity}})},{"nextProcessGeneration",pc.next_generation},{"nextTaskGeneration",tasks.next_allocation_generation()},{"nextScopeGeneration",scopes.next_allocation_generation()},{"nextGroupGeneration",waits.next_allocation_generation()},{"waitOwners",Json::array(wait_owners_json)},{"scopes",Json::array(scopes_json)},{"tasks",Json::array(tasks_json)},{"tickets",Json::array(tickets_json)},{"waits",Json::array(waits_json)},{"results",Json::array(results_json)},{"consumers",Json::array(consumers_json)},{"bindings",Json::array(bindings_json)},{"processes",Json::array(processes_json)},{"externalWaits",Json::array(external_waits_json)},{"events",Json::array(events_json)},{"frontier",queue.frontier?observe(*queue.frontier):Json{}},{"nextSequence",queue.next_sequence},{"nextBatch",queue.next_batch},{"observers",Json::array(observers)},{"cancelSequence",ss.value().sequence},{"frames",ts.value().frames},{"grants",ts.value().grants},{"queue",observe(identities(ts.value().queue))},{"pending",observe(identities(ts.value().pending))},{"pins",rs.pin_count},{"pinLimit",rs.pin_limit},{"nextResultGeneration",rs.next_result_generation},{"nextConsumerGeneration",rs.next_consumer_generation}});
  }
};
inline Expected<std::uint64_t> number(const Value&v){if(auto n=std::get_if<std::uint64_t>(&v.data))return *n;return fail(ErrorCode::TypeMismatch,"structured fixture integer");}
inline Expected<Handle> output_handle(const std::vector<Value>&values){
  if(values.size()!=1)return fail(ErrorCode::TypeMismatch,"structured setup output arity");
  if(auto h=std::get_if<Handle>(&values[0].data))return *h;
  if(auto a=std::get_if<Value::Array>(&values[0].data);a&&a->size()==2){auto tag=number((*a)[0]);auto fields=std::get_if<Value::Array>(&(*a)[1].data);if(tag&&tag.value()==0&&fields&&fields->size()==1)if(auto h=std::get_if<Handle>(&(*fields)[0].data))return *h;}
  return fail(ErrorCode::InvalidState,"structured setup did not create handle");
}
}

inline Expected<ProviderFixture> make_structured_fixture(Runtime&r,CoreRuntimeBackend&backend,const exec::Project&project,const FixtureConfig&config){
  using namespace structured_fixture_detail;
  auto p=config.environment.find("structured.pool");
  if(p==config.environment.end())return fail(ErrorCode::InvalidArgument,"structured.pool source missing");
  auto fields=std::get_if<Value::Array>(&p->second.data);if(!fields||fields->size()!=9)return fail(ErrorCode::TypeMismatch,"structured.pool source codec");
  std::vector<std::uint64_t> ns;for(auto&v:*fields){auto n=number(v);if(!n)return n.error();ns.push_back(n.value());}
  if(ns[7]>2||ns[8]>=project.types.size())return fail(ErrorCode::InvalidArgument,"structured.pool source bounds");
  TaskPoolDesc d;d.max_instances=ns[0];d.task_capacity=ns[1];d.result_capacity=ns[2];d.queue_depth=ns[3];d.waiter_limit=ns[4];d.args_bytes=ns[5];d.result_bytes=ns[6];d.overflow=TaskOverflow(ns[7]);d.result_type=TypeId{std::uint32_t(ns[8])};
  auto owned=std::make_shared<Owned>(r,config.context,d);
  if(!r.queue().next_wakeup()){
    EventDraft marker;marker.key={config.context.ready.time,config.context.ready.turn,EventStage::Internal,config.context.instance,config.context.connection,0};marker.owner=config.context.owner;marker.epoch=config.context.epoch;
    auto token=r.queue().enqueue(marker);if(!token)return token.error();auto batch=r.queue().pop_batch(config.context.ready.time,1);if(!batch||!batch.value())return fail(ErrorCode::InvalidState,"structured frontier seed");
    auto ack=r.queue().ack_executed(batch.value()->id,token.value(),ExecutionDisposition::Committed);if(!ack)return ack.error();auto resolved=r.queue().resolver_step(batch.value()->id,1,0);if(!resolved)return resolved.error();auto finished=r.queue().finish_batch(batch.value()->id);if(!finished)return finished.error();
  }
  auto root=config.environment.find("structured.root"),process=config.environment.find("structured.process");
  if(root==config.environment.end()||process==config.environment.end()||!std::holds_alternative<Handle>(root->second.data)||!std::holds_alternative<Handle>(process->second.data))return fail(ErrorCode::TypeMismatch,"structured source root/process required");
  owned->aliases.emplace(std::get<Handle>(root->second.data),owned->scopes.root());
  auto created=r.processes().create(ProgramId{0},config.context.owner);if(!created)return created.error();owned->process=created.value();owned->seeded_processes.push_back(created.value());
  // The source seed creates this frame with its owning instance. Bind through
  // the real context-bearing begin before setup or the tested segment runs.
  SegmentBudget bootstrap_budget{4096,4096,128*1024*1024,4096};EventTxn bootstrap(bootstrap_budget,config.context);
  auto begun=r.processes().begin(created.value(),bootstrap);if(!begun)return begun.error();
  auto bootstrap_committed=r.commit_segment(bootstrap);if(!bootstrap_committed)return bootstrap_committed.error();
  auto initialized=r.processes().inspect(created.value());if(!initialized)return initialized.error();
  if(!initialized.value().instance_bound||initialized.value().instance!=config.context.instance)return fail(ErrorCode::WrongOwner,"structured source process instance seed");
  owned->aliases.emplace(std::get<Handle>(process->second.data),created.value());
  auto bound=backend.bind_current_process(created.value());if(!bound)return bound.error();
  ProgramId program{};if(auto pi=config.environment.find("structured.program");pi!=config.environment.end()){auto n=number(pi->second);if(!n||n.value()>UINT32_MAX)return fail(ErrorCode::InvalidArgument,"structured program source");program=ProgramId{std::uint32_t(n.value())};}
  for(const auto&s:project.services)if(supported(s.op)){auto b=owned->services.bind(backend,s,project,program);if(!b)return b.error();}
  SegmentBudget budget{4096,4096,128*1024*1024,4096};EventTxn seed(budget,config.context);bool resolve=false;
  auto setup=config.environment.find("structured.setup");
  if(setup!=config.environment.end()){
    auto actions=std::get_if<Value::Array>(&setup->second.data);if(!actions)return fail(ErrorCode::TypeMismatch,"structured setup source list");
    std::uint32_t id=100000;
    for(const auto&action:*actions){auto row=std::get_if<Value::Array>(&action.data);if(!row||row->size()!=3)return fail(ErrorCode::TypeMismatch,"structured setup action");auto op=number((*row)[0]);auto args_value=owned->remap((*row)[2]);auto args=std::get_if<Value::Array>(&args_value.data);if(!op||!args)return fail(ErrorCode::TypeMismatch,"structured setup operands");
      if(op.value()==1000){if(args->size()!=2)return fail(ErrorCode::TypeMismatch,"completeTask setup");auto task=std::get_if<Handle>(&(*args)[0].data);auto outcome=std::get_if<Value::Array>(&(*args)[1].data);if(!task||!outcome||outcome->size()!=2)return fail(ErrorCode::TypeMismatch,"completeTask outcome");auto kind=number((*outcome)[0]);if(!kind||kind.value()>2)return fail(ErrorCode::TypeMismatch,"completeTask kind");auto done=owned->services.complete(*task,{TaskOutcome::Kind(kind.value()),(*outcome)[1]},config.context.ready,seed);if(!done)return done.error();continue;}
      if(op.value()==1001){resolve=true;continue;}
      if(op.value()==1002){if(args->size()!=1||!std::holds_alternative<Handle>((*args)[0].data))return fail(ErrorCode::TypeMismatch,"suspend setup");auto token=r.processes().suspend_registered(std::get<Handle>((*args)[0].data),{},seed);if(!token)return token.error();continue;}
      if(op.value()==1003){auto alias=std::get_if<Handle>(&(*row)[1].data);if(!alias||!args->empty())return fail(ErrorCode::TypeMismatch,"caller process source");auto caller=r.processes().create(ProgramId{0},config.context.owner,seed);if(!caller)return caller.error();auto begun=r.processes().begin(caller.value(),seed);if(!begun)return begun.error();owned->process=caller.value();owned->seeded_processes.push_back(caller.value());owned->aliases.emplace(*alias,caller.value());continue;}
      auto opcode=exec::Op(op.value());if(!supported(opcode))return fail(ErrorCode::Unsupported,"structured setup opcode");
      exec::ServiceSignature sig;sig.id=id++;sig.op=opcode;sig.context_mask=2;sig.effect_mask=exec::required_effect(opcode);sig.provider_key="leanat.fixture.structured.seed";sig.provider_version="1";sig.input_types.resize(args->size(),0);sig.result_types={0};
      CoreRuntimeBackend initial(r);auto registered=owned->services.bind(initial,sig,project,program);if(!registered)return registered.error();auto frozen=initial.freeze();if(!frozen)return frozen.error();auto result=initial.invoke(sig,*args,config.context,seed);if(!result)return result.error();
      if(auto alias=std::get_if<Handle>(&(*row)[1].data)){auto actual=output_handle(result.value());if(!actual)return actual.error();if(!owned->aliases.emplace(*alias,actual.value()).second)return fail(ErrorCode::Duplicate,"structured setup logical alias");}
    }
  }
  auto committed=r.commit_segment(seed);if(!committed)return committed.error();if(resolve){auto done=owned->services.resolve(BatchId{1},config.context.ready);if(!done)return done.error();}
  auto current=backend.bind_current_process(owned->process);if(!current)return current.error();
  bool run_child=false;if(auto flag=config.environment.find("structured.runChild");flag!=config.environment.end()){auto enabled=std::get_if<bool>(&flag->second.data);if(!enabled)return fail(ErrorCode::TypeMismatch,"child lifecycle flag");run_child=*enabled;}
  ProviderFixture out;for(const auto&v:config.inputs)out.inputs.push_back(owned->remap(v));out.lifetime.push_back(owned);out.snapshot=[owned]{return owned->snapshot();};out.identities=[owned]{std::vector<Json> entries;for(const auto&m:owned->aliases)entries.push_back(Json::object({{"logical",observe(m.first)},{"actual",observe(m.second)}}));return Json::array(entries);};
  out.after_segment=[owned,project,observer=config.observer,run_child](const exec::SegmentResult&segment)->Expected<void>{
    if(segment.kind==exec::SegmentResult::Kind::Returned){
      if(owned->context.kind==ContextKind::Process){
      auto snapshot=owned->runtime.processes().inspect(owned->process);if(!snapshot)return snapshot.error();
      auto done=owned->runtime.processes().complete(owned->process);if(!done)return done.error();
      owned->completed=Value{segment.values};owned->record_completion(owned->process,snapshot.value(),segment.values,owned->context.ready);
      }
      if(run_child)return owned->run_child(project,observer);
    }
    return {};
  };return out;
}
} // namespace leanat::opcode_test

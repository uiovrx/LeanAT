#pragma once
#include "opcode_profile.hpp"
#include <leanat/descriptor.hpp>

namespace leanat::opcode_test {
inline exec::ValidatedProject load_opcode_request(const InputJson &request) {
  exec::LoadPolicy policy;
  policy.expected_profile=request.at("profile").string();
  policy.allowed_capabilities={"raw-dmi","managed-access","extern-pure"};
  std::ifstream file(request.at("descriptor").string(),std::ios::binary|std::ios::ate);
  if(!file)throw std::runtime_error("descriptor unavailable");
  auto length=file.tellg();
  if(length<0||uint64_t(length)>policy.max_file_bytes)throw std::runtime_error("descriptor bound");
  Bytes bytes(size_t(length),0);file.seekg(0);
  if(!bytes.empty()&&!file.read(reinterpret_cast<char *>(bytes.data()),length))throw std::runtime_error("descriptor short read");
  std::string hash;const char* digits="0123456789abcdef";
  for(auto byte:exec::sha256(bytes)){hash+=digits[byte>>4];hash+=digits[byte&15];}
  if(hash!=request.at("descriptorHash").string())throw std::runtime_error("descriptor hash mismatch");
  auto loaded=exec::load_descriptor(bytes,policy);
  if(!loaded)throw std::runtime_error(loaded.error().message);
  return std::move(loaded.value());
}
inline Json describe_opcode_request(const InputJson &request) {
  auto loaded=load_opcode_request(request);const auto &p=loaded.get();
  std::vector<Json> types,programs,services;
  for(const auto &t:p.types)types.push_back(Json::object({{"kind",unsigned(t.kind)},{"bound",t.bound},{"fields",observe(t.fields)},{"constructors",observe(t.constructors)}}));
  auto reg_types=[](const std::vector<exec::Reg>&regs){std::vector<uint32_t> out;for(auto r:regs)out.push_back(r.type);return observe(out);};
  for(const auto &program:p.programs) {
    std::vector<Json> blocks;
    for(const auto &block:program.blocks) {
      std::vector<Json> instructions;
      for(const auto &i:block.instructions)instructions.push_back(Json::object({{"opcode",unsigned(i.op)},{"args",reg_types(i.args)},{"destType",i.dest?Json(i.dest->type):Json()},{"immediate",i.immediate},{"binary",unsigned(i.binary)},{"value",observe(i.value)},{"text",i.text},{"source",i.source}}));
      const auto &t=block.terminator;std::vector<uint32_t> targets;
      if(t.kind==exec::TermKind::Jump)targets.push_back(t.yes.target);
      if(t.kind==exec::TermKind::Branch){targets.push_back(t.yes.target);targets.push_back(t.no.target);}
      if(t.kind==exec::TermKind::Switch){targets.push_back(t.no.target);for(auto &branch:t.cases)targets.push_back(branch.second.target);}
      if(t.kind==exec::TermKind::Suspend)targets.push_back(t.yes.target);
      blocks.push_back(Json::object({{"id",block.id},{"parameters",reg_types(block.parameters)},{"instructions",Json::array(instructions)},
        {"terminator",Json::object({{"kind",unsigned(t.kind)},{"valueType",t.value.type},{"valueTypes",reg_types(t.values)},{"targets",observe(targets)}})}}));
    }
    programs.push_back(Json::object({{"id",program.id},{"entry",program.entry},{"inputTypes",observe(program.input_types)},{"resultTypes",observe(program.result_types)},{"frameBytes",program.frame_bytes},{"instructionFuel",program.instruction_fuel},{"blocks",Json::array(blocks)}}));
  }
  for(const auto&s:p.services)services.push_back(Json::object({{"id",s.id},{"opcode",unsigned(s.op)},{"inputTypes",observe(s.input_types)},{"resultTypes",observe(s.result_types)},{"extraFuel",s.extra_fuel},{"providerKey",s.provider_key},{"providerVersion",s.provider_version}}));
  RuntimeConfig config;
  const auto process_footprint = ProcessStore::staging_footprint();
  if(auto world=request.at("input").find("world")) {
    if(auto events=world->find("maxEvents"))config.event_capacity=size_t(events->natural(1048576));
    apply_world_profile(*world,config);
  }
  return Json::object({{"schema","leanat.opcode-static.v1"},{"descriptorHash",request.at("descriptorHash").string()},{"profile",p.profile},{"valueNodeBytes",sizeof(Value)},
    {"types",Json::array(types)},{"stateTypes",observe(p.state_types)},{"programs",Json::array(programs)},{"services",Json::array(services)},
    {"processStagingFootprint",Json::object({{"mutationBytes",process_footprint.mutation_bytes},{"frameEntryBytes",process_footprint.frame_entry_bytes},{"waitBytes",process_footprint.wait_bytes}})},
    {"runtimeCapacities",Json::object({{"events",config.event_capacity},{"results",config.result_capacity},{"consumers",config.consumer_capacity},{"pins",config.pin_capacity},{"frames",config.frame_capacity},{"waits",config.wait_capacity},{"live",config.live_capacity}})}});
}
} // namespace leanat::opcode_test

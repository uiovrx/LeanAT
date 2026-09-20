#include "leanat/exec_ir.hpp"
#include <tuple>
namespace leanat::exec {
Expected<void> validate_metadata(const Project &p, Limits l) {
  auto bad = [](const char *s) -> Expected<void> { return fail(ErrorCode::Schema, s); };
  if (p.schema_major < 4) {
    if (!p.components.empty() || !p.instances.empty() || p.system_metadata ||
        !p.external_contracts.empty() || !p.capabilities.empty())
      return bad("metadata requires schema4");
    return {};
  }
  if (p.schema_major >= 5 && p.components.empty() && p.instances.empty() &&
      !p.system_metadata && p.external_contracts.empty() && p.capabilities.empty())
    return {};
  if (!p.system_metadata)
    return bad("schema4 requires selected system metadata");
  std::size_t work = 0;
  auto step = [&]() { return ++work <= l.max_work; };
  std::map<std::uint32_t, const ComponentDesc *> components;
  std::map<std::uint32_t, const InstanceDesc *> instances;
  std::map<std::uint32_t, const Program *> programs;
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> parameter_types;
  std::map<std::pair<std::uint32_t, std::uint32_t>, const EndpointDesc *> endpoint_table;
  std::map<std::pair<std::uint32_t, std::uint32_t>, const PortDesc *> instance_ports;
  auto source = [](const MetadataSource &s) {
    return s.line && s.column && s.file.size() <= 65536;
  };
  auto port = [&](const PortDesc &v) {
    return static_cast<unsigned>(v.direction) <= 1 && v.type_id < p.types.size() &&
           (!v.initial || conforms(p, v.type_id, *v.initial)) && source(v.source);
  };
  if (p.components.size() > l.max_rows || p.instances.size() > l.max_rows ||
      p.external_contracts.size() > l.max_rows || p.capabilities.size() > l.max_rows)
    return bad("metadata table bound");
  for (auto &pr : p.programs)
    programs.emplace(pr.id, &pr);
  for (auto &c : p.components) {
    if (!step() || !components.emplace(c.id, &c).second)
      return bad("component ID/work");
    std::set<std::uint32_t> ids;
    for (auto &v : c.parameters) {
      if (!step() || !ids.insert(v.id).second || v.type_id >= p.types.size() ||
          (v.default_value && !conforms(p, v.type_id, *v.default_value)) || !source(v.source))
        return bad("component parameter");
      parameter_types.emplace(std::make_pair(c.id, v.id), v.type_id);
    }
    ids.clear();
    for (auto &v : c.states)
      if (!step() || !ids.insert(v.id).second || !conforms(p, v.type_id, v.initial))
        return bad("component state");
    ids.clear();
    for (auto &e : c.endpoints) {
      if (!step() || !ids.insert(e.id).second || static_cast<unsigned>(e.role) > 1 ||
          !e.bus_width || e.bus_width % 8 || !e.max_bindings || !e.max_outstanding ||
          e.protocol_ref.empty() || !source(e.source))
        return bad("component endpoint");
      endpoint_table.emplace(std::make_pair(c.id, e.id), &e);
    }
    ids.clear();
    for (auto &v : c.sidebands) {
      if (!step() || !ids.insert(v.id).second || !port(v))
        return bad("component sideband");
      instance_ports.emplace(std::make_pair(c.id, v.id), &v);
    }
  }
  auto config = [&](const ComponentDesc &c, const ResolvedConfig &cfg) {
    if (cfg.size() != c.parameters.size())
      return false;
    std::set<std::uint32_t> seen;
    for (auto &v : cfg) {
      if (!step() || !seen.insert(v.first).second)
        return false;
      auto it = parameter_types.find({c.id, v.first});
      if (it == parameter_types.end() || !conforms(p, it->second, v.second))
        return false;
    }
    return true;
  };
  std::set<std::uint32_t> owned;
  std::vector<bool> covered(p.state_types.size());
  for (auto &i : p.instances) {
    if (!step() || !instances.emplace(i.id, &i).second || !components.count(i.definition))
      return bad("instance ID/definition");
    auto &c = *components.at(i.definition);
    if (!config(c, i.resolved_config) || i.state_count != c.states.size() ||
        i.state_base > p.state_types.size() || i.state_count > p.state_types.size() - i.state_base)
      return bad("instance config/state range");
    for (std::size_t n = 0; n < i.state_count; ++n) {
      auto index = i.state_base + n;
      if (covered[index] || p.state_types[index] != c.states[n].type_id ||
          p.initial_state[index] != c.states[n].initial)
        return bad("flattened instance state mismatch/overlap");
      covered[index] = true;
    }
    std::set<std::tuple<std::uint32_t, ContextKind, std::optional<std::uint32_t>>> locals;
    for (auto &h : i.handlers) {
      if (!step() || !locals.emplace(h.local_id, h.context, h.endpoint).second ||
          !owned.insert(h.program_id).second ||
          !programs.count(h.program_id) || static_cast<unsigned>(h.context) > 4 ||
          h.trigger.empty())
        return bad("handler ownership/ID");
      auto &pr = *programs.at(h.program_id);
      if (pr.context != h.context)
        return bad("handler program context");
      if (h.endpoint && !endpoint_table.count({c.id, *h.endpoint}))
        return bad("handler endpoint");
      if (h.context == ContextKind::Process) {
        if (!h.process_capacity)
          return bad("process capacity missing");
        auto &cap = *h.process_capacity;
        if (!cap.max_instances || !cap.result_capacity || cap.frame_bytes_limit < pr.frame_bytes ||
            static_cast<unsigned>(cap.overflow) > 2 ||
            (cap.overflow == ProcessOverflow::Reject ? cap.policy_bound != 0
                                                     : cap.policy_bound == 0))
          return bad("process capacity");
      } else if (h.process_capacity)
        return bad("nonprocess capacity");
      for (auto &b : pr.blocks)
        for (auto &ins : b.instructions) {
          if (!step())
            return bad("handler instruction validation budget");
          if (ins.op == Op::LoadState || ins.op == Op::BufferStateWrite) {
            if (ins.immediate < i.state_base || ins.immediate - i.state_base >= i.state_count)
              return bad("cross-instance state access");
          }
        }
    }
  }
  if (owned.size() != p.programs.size() ||
      std::any_of(covered.begin(), covered.end(), [](bool b) { return !b; }))
    return bad("unowned flattened program/state");
  auto &system = *p.system_metadata;
  if (!source(system.source) || system.original_instances.size() != instances.size())
    return bad("selected system instance shape");
  std::set<std::uint32_t> ids;
  for (auto &i : system.original_instances) {
    if (!step() || !ids.insert(i.id).second || !instances.count(i.id) || !source(i.source))
      return bad("original instance identity");
    auto &resolved = *instances.at(i.id);
    if (i.definition != resolved.definition || i.resolved_config != resolved.resolved_config)
      return bad("original/resolved instance mismatch");
  }
  auto endpoint = [&](const EndpointRef &r) -> const EndpointDesc * {
    if (!instances.count(r.instance_id))
      return nullptr;
    if (!step())
      return nullptr;
    auto it = endpoint_table.find({instances.at(r.instance_id)->definition, r.endpoint});
    return it != endpoint_table.end() && r.binding_index < it->second->max_bindings ? it->second
                                                                                    : nullptr;
  };
  ids.clear();
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> used_endpoints;
  for (auto &b : system.bindings) {
    if (!step() || !ids.insert(b.id).second || !source(b.source))
      return bad("binding ID");
    auto a = endpoint(b.source_endpoint), z = endpoint(b.sink_endpoint);
    if (!a || !z || a->role != EndpointRole::Initiator || z->role != EndpointRole::Target ||
        a->bus_width != z->bus_width || a->protocol_ref != z->protocol_ref)
      return bad("binding endpoint role/width/protocol");
    for (auto r : {b.source_endpoint, b.sink_endpoint})
      if (!used_endpoints.emplace(r.instance_id, r.endpoint, r.binding_index).second)
        return bad("duplicate endpoint binding index");
  }
  ids.clear();
  std::map<std::uint32_t, const PortDesc *> top_ports;
  for (auto &v : system.top_ports)
    if (!step() || !ids.insert(v.id).second || !port(v))
      return bad("top port");
    else
      top_ports.emplace(v.id, &v);
  auto lookup_port = [&](const PortRef &r) -> const PortDesc * {
    if (!step())
      return nullptr;
    if (r.tag == 0) {
      if (r.instance_id)
        return nullptr;
      auto it = top_ports.find(r.port_id);
      return it == top_ports.end() ? nullptr : it->second;
    } else if (r.tag == 1 && instances.count(r.instance_id)) {
      auto it = instance_ports.find({instances.at(r.instance_id)->definition, r.port_id});
      return it == instance_ports.end() ? nullptr : it->second;
    } else
      return nullptr;
  };
  ids.clear();
  std::set<std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>> drivers;
  for (auto &b : system.sideband_bindings) {
    if (!step() || !ids.insert(b.id).second || !source(b.source))
      return bad("sideband binding ID");
    auto a = lookup_port(b.source_port), z = lookup_port(b.sink_port);
    if (!a || !z || a->type_id != z->type_id ||
        (b.source_port.tag == 0 ? a->direction != PortDirection::Input
                                : a->direction != PortDirection::Output) ||
        (b.sink_port.tag == 0 ? z->direction != PortDirection::Output
                              : z->direction != PortDirection::Input) ||
        !drivers.emplace(b.sink_port.tag, b.sink_port.instance_id, b.sink_port.port_id).second)
      return bad("sideband direction/type/driver");
  }
  ids.clear();
  for (std::size_t n = 0; n < system.address_maps.size(); ++n) {
    auto &m = system.address_maps[n];
    if (!step() || !ids.insert(m.id).second || !m.size || !source(m.source) ||
        !checked_add(m.source_start, m.size) || !checked_add(m.target_start, m.size))
      return bad("address map range");
    EndpointRef ref{m.decoder_instance_id, m.output_endpoint, m.binding_index};
    auto e = endpoint(ref);
    if (!e || e->role != EndpointRole::Initiator ||
        !used_endpoints.count({ref.instance_id, ref.endpoint, ref.binding_index}))
      return bad("address map output binding");
    for (std::size_t j = 0; j < n; ++j) {
      if (!step())
        return bad("address map validation budget");
      auto &a = system.address_maps[j];
      if (a.decoder_instance_id == m.decoder_instance_id &&
          a.source_start < m.source_start + m.size && m.source_start < a.source_start + a.size &&
          !(a.alias_declared && m.alias_declared))
        return bad("undeclared address alias");
    }
  }
  std::function<bool(const MetadataLiteral &, std::size_t)> literal = [&](const MetadataLiteral &v,
                                                                          std::size_t depth) {
    if (!step() || depth > l.max_depth || v.tag > 7 || v.children.size() > l.max_rows ||
        (v.tag != 2 && v.width))
      return false;
    if (v.tag != 3 && v.tag != 4 && v.tag != 5 && !v.children.empty())
      return false;
    switch (v.tag) {
    case 0:
    case 3:
    case 5:
      if (!std::holds_alternative<std::monostate>(v.value.data))
        return false;
      break;
    case 1:
      if (!std::holds_alternative<bool>(v.value.data))
        return false;
      break;
    case 2: {
      auto n = std::get_if<std::uint64_t>(&v.value.data);
      if (!n || !v.width || v.width > 64 || (v.width < 64 && *n >= (std::uint64_t{1} << v.width)))
        return false;
      break;
    }
    case 4:
      if (!std::holds_alternative<std::uint64_t>(v.value.data))
        return false;
      break;
    case 6: {
      auto bytes = std::get_if<Bytes>(&v.value.data);
      if (!bytes || bytes->size() > l.max_value_elements)
        return false;
      break;
    }
    case 7: {
      auto handle = std::get_if<Handle>(&v.value.data);
      if (!handle ||
          static_cast<unsigned>(handle->kind) > static_cast<unsigned>(HandleKind::GateTicket))
        return false;
      break;
    }
    }
    for (auto &child : v.children)
      if (!literal(child, depth + 1))
        return false;
    return true;
  };
  ids.clear();
  for (auto &e : p.external_contracts) {
    if (!step() || !ids.insert(e.id).second || e.cpp_type.empty() || e.header.empty() ||
        !source(e.source))
      return bad("external contract identity");
    std::set<std::string> keys;
    for (auto &v : e.constructor_mapping)
      if (!step() || v.first.empty() || !keys.insert(v.first).second || !literal(v.second, 1))
        return bad("external constructor key");
  }
  std::set<std::string> caps;
  for (auto &c : p.capabilities)
    if (c.empty() || !caps.insert(c).second)
      return bad("capability identity");
  return {};
}
} // namespace leanat::exec

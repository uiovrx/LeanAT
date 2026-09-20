#include "leanat/exec_ir.hpp"
#include <functional>
namespace leanat::exec {
std::uint64_t required_effect(Op op) {
  switch (op) {
  case Op::LoadState:
    return StateRead;
  case Op::BufferStateWrite:
    return StateWrite;
  case Op::BufferOutputWrite:
    return OutputWrite;
  case Op::PayloadGet:
  case Op::BufferPayloadWrite:
  case Op::ExtensionGet:
  case Op::BufferExtensionWrite:
  case Op::NewTransaction:
  case Op::StagePhase:
  case Op::AckResponse:
  case Op::CancelLocal:
  case Op::ScheduleEvent:
  case Op::CancelEvent:
  case Op::SetTransportReturn:
    return Transaction;
  case Op::RegisterWait:
  case Op::ReadWaitResult:
  case Op::WaitGroupNew:
  case Op::WaitArm:
  case Op::WaitResultGet:
  case Op::WaitGroupRelease:
    return Wait;
  case Op::SpawnProcess:
  case Op::TrySpawnProcess:
  case Op::SubmitTask:
  case Op::CancelTask:
  case Op::RequestTaskSlot:
  case Op::ScopeNew:
  case Op::ScopeTransfer:
  case Op::ScopeCancel:
  case Op::ScopeClose:
    return Spawn;
  case Op::ResultGet:
  case Op::ResultRelease:
  case Op::TaskResultGet:
  case Op::TaskResultRelease:
    return Result;
  case Op::ObjectCall:
  case Op::DebugTransfer:
    return Object;
  case Op::RequestManaged:
  case Op::BeginManagedRead:
  case Op::BeginManagedWrite:
  case Op::InvalidateManaged:
  case Op::ReleaseLease:
    return Managed;
  case Op::CallExternPure:
    return External;
  case Op::GrantRawDmi:
  case Op::DenyDmi:
  case Op::InvalidateRawDmi:
    return RawDmi;
  default:
    return 0;
  }
}
std::uint32_t allowed_contexts(Op op) {
  switch (op) {
  case Op::GetNow:
  case Op::PayloadGet:
  case Op::BufferPayloadWrite:
  case Op::ExtensionGet:
  case Op::BufferExtensionWrite:
    return 7;
  case Op::CallPure:
  case Op::Unary:
  case Op::Compare:
  case Op::Convert:
    return 31;
  case Op::RegisterWait:
  case Op::ReadWaitResult:
  case Op::WaitGroupNew:
  case Op::WaitArm:
  case Op::WaitResultGet:
  case Op::WaitGroupRelease:
  case Op::RequestTaskSlot:
    return 2;
  case Op::SetTransportReturn:
    return 4;
  case Op::DebugTransfer:
    return 8;
  case Op::GrantRawDmi:
  case Op::DenyDmi:
    return 16;
  case Op::InvalidateRawDmi:
    return 3;
  default:
    return static_cast<unsigned>(op) < 12 || op == Op::Check || op == Op::Trace ? 31 : 3;
  }
}
bool conforms(const Project &p, std::uint32_t t, const Value &v, std::size_t d) {
  if (!d || t >= p.types.size())
    return false;
  const auto &s = p.types[t];
  const auto *n = std::get_if<std::uint64_t>(&v.data);
  const auto *a = std::get_if<Value::Array>(&v.data);
  auto fields = [&](const std::vector<std::uint32_t> &fs, const Value::Array &vs) {
    if (fs.size() != vs.size())
      return false;
    for (std::size_t i = 0; i < fs.size(); ++i)
      if (!conforms(p, fs[i], vs[i], d - 1))
        return false;
    return true;
  };
  switch (s.kind) {
  case TypeKind::Bytes: {
    auto b = std::get_if<Bytes>(&v.data);
    return b && b->size() <= s.bound;
  }
  case TypeKind::Handle: {
    auto h = std::get_if<Handle>(&v.data);
    return h && static_cast<std::uint64_t>(h->kind) == s.bound;
  }
  case TypeKind::Unit:
    return std::holds_alternative<std::monostate>(v.data);
  case TypeKind::Bool:
    return std::holds_alternative<bool>(v.data);
  case TypeKind::Bits:
    return n && s.bound && s.bound <= 64 && (s.bound == 64 || *n < (std::uint64_t{1} << s.bound));
  case TypeKind::Fin:
    return n && ((p.schema_major >= 5 && !s.bound) || *n < s.bound);
  case TypeKind::Record:
    return a && fields(s.fields, *a);
  case TypeKind::Variant:
    if (!a || a->size() != 2)
      return false;
    {
      auto tag = std::get_if<std::uint64_t>(&(*a)[0].data);
      auto f = std::get_if<Value::Array>(&(*a)[1].data);
      return tag && f && *tag < s.constructors.size() && fields(s.constructors[*tag], *f);
    }
  case TypeKind::Vec:
  case TypeKind::BoundedVec:
    if (!a || s.fields.size() != 1 ||
        (s.kind == TypeKind::Vec ? a->size() != s.bound : a->size() > s.bound))
      return false;
    for (auto &x : *a)
      if (!conforms(p, s.fields[0], x, d - 1))
        return false;
    return true;
  }
  return false;
}
Expected<ValidatedProject> validate(Project p, Limits l) {
  l.max_depth = std::min(l.max_depth, std::size_t{64});
  auto bad = [](std::string s) -> Expected<ValidatedProject> {
    return fail(ErrorCode::Schema, std::move(s));
  };
  if (!l.max_depth || !l.max_value_elements || !l.max_work || !l.max_registers)
    return bad("zero validation budget");
  if (p.schema_major < 1 || p.schema_major > 5)
    return fail(ErrorCode::Unsupported, "schema version");
  if (p.schema_major == 1 && !p.services.empty())
    return bad("v1 service table");
  if (p.types.size() > l.max_rows || p.programs.size() > l.max_rows ||
      p.state_types.size() > l.max_rows)
    return bad("table bound");
  std::size_t work = 0;
  std::vector<unsigned char> colors(p.types.size());
  std::vector<std::size_t> sizes(p.types.size()), heights(p.types.size());
  std::function<bool(std::uint32_t, std::size_t)> visit = [&](std::uint32_t id, std::size_t depth) {
    if (++work > l.max_work || depth > l.max_depth || id >= p.types.size() || colors[id] == 1)
      return false;
    if (colors[id] == 2)
      return depth + heights[id] - 1 <= l.max_depth;
    colors[id] = 1;
    auto &t = p.types[id];
    if (t.fields.size() > l.max_rows || t.constructors.size() > l.max_rows)
      return false;
    for (auto &fields : t.constructors)
      if (fields.size() > l.max_rows)
        return false;
    std::size_t size = 1, height = 1;
    auto add = [&](std::uint32_t c) {
      if (!visit(c, depth + 1) || sizes[c] > l.max_value_elements - size)
        return false;
      size += sizes[c];
      height = std::max(height, heights[c] + 1);
      return true;
    };
    if (static_cast<unsigned>(t.kind) > (p.schema_major == 1 ? 7u : 9u))
      return false;
    if (t.kind == TypeKind::Handle && t.bound > static_cast<unsigned>(HandleKind::GateTicket))
      return false;
    if (t.kind == TypeKind::Bytes) {
      if (t.bound > l.max_value_elements - 1)
        return false;
      size = 1 + static_cast<std::size_t>(t.bound);
    }
    if (t.kind == TypeKind::Bits && (!t.bound || t.bound > 64))
      return false;
    if (t.kind == TypeKind::Fin && !t.bound && p.schema_major < 5)
      return false;
    if ((t.kind == TypeKind::Unit || t.kind == TypeKind::Bool || t.kind == TypeKind::Record ||
         t.kind == TypeKind::Variant) &&
        t.bound)
      return false;
    if (t.kind == TypeKind::Record) {
      for (auto x : t.fields)
        if (!add(x))
          return false;
    } else if (t.kind == TypeKind::Variant) {
      if (t.constructors.empty())
        return false;
      if (l.max_value_elements < 3)
        return false;
      std::size_t max = 3;
      for (auto &fs : t.constructors) {
        size = 3;
        for (auto x : fs)
          if (!add(x))
            return false;
        max = std::max(max, size);
      }
      size = max;
    } else if (t.kind == TypeKind::Vec || t.kind == TypeKind::BoundedVec) {
      if (t.fields.size() != 1 || !add(t.fields[0]) ||
          t.bound > l.max_value_elements / (size ? size : 1))
        return false;
      size = std::max(std::size_t{1}, size * static_cast<std::size_t>(t.bound));
    }
    if (t.kind != TypeKind::Record && t.kind != TypeKind::Vec && t.kind != TypeKind::BoundedVec &&
        !t.fields.empty())
      return false;
    if (t.kind != TypeKind::Variant && !t.constructors.empty())
      return false;
    if (size > l.max_value_elements)
      return false;
    colors[id] = 2;
    sizes[id] = size;
    heights[id] = height;
    return true;
  };
  for (std::uint32_t t = 0; t < p.types.size(); ++t)
    if (!visit(t, 1))
      return bad("invalid/cyclic/oversized type graph");
  if (p.state_types.size() != p.initial_state.size())
    return bad("state shape");
  for (std::size_t i = 0; i < p.state_types.size(); ++i)
    if (!conforms(p, p.state_types[i], p.initial_state[i], l.max_depth))
      return bad("state value type");
  std::map<std::uint32_t, const ServiceSignature *> services;
  if (p.services.size() > l.max_rows)
    return bad("service table bound");
  for (auto &s : p.services) {
    if (s.op >= Op::WaitGroupNew && p.profile != "AT-Ext-1.1-draft")
      return fail(ErrorCode::Unsupported, "Ext service requires AT-Ext-1.1-draft profile");
    if (!services.emplace(s.id, &s).second || static_cast<unsigned>(s.op) < 21 ||
        s.op >= Op::Count || s.result_types.size() > 1 || s.provider_key.empty() ||
        s.provider_version.empty() || !s.context_mask ||
        (s.context_mask & ~allowed_contexts(s.op)) ||
        (s.effect_mask & required_effect(s.op)) != required_effect(s.op) ||
        (s.effect_mask & ~std::uint64_t{2047}))
      return bad("invalid service signature/effect/context");
    for (auto t : s.input_types)
      if (t >= p.types.size())
        return bad("service input type");
    for (auto t : s.result_types)
      if (t >= p.types.size())
        return bad("service result type");
  }
  std::map<std::uint32_t, const Program *> program_map;
  for (auto &program : p.programs)
    if (!program_map.emplace(program.id, &program).second)
      return bad("duplicate program");
  std::map<std::uint32_t, unsigned> pure_colors;
  std::map<std::uint32_t, std::size_t> pure_heights;
  std::function<bool(std::uint32_t, std::size_t)> closed_pure = [&](std::uint32_t id,
                                                                    std::size_t depth) {
    if (depth > l.max_depth || ++work > l.max_work || !program_map.count(id) ||
        pure_colors[id] == 1)
      return false;
    if (pure_colors[id] == 2)
      return depth + pure_heights[id] - 1 <= l.max_depth;
    pure_colors[id] = 1;
    auto &target = *program_map.at(id);
    if (target.effect_mask || !target.frame.empty() || target.context != ContextKind::Timed)
      return false;
    std::size_t height = 1;
    for (auto &block : target.blocks) {
      if (block.terminator.kind == TermKind::Suspend ||
          block.terminator.kind == TermKind::TransportReturn)
        return false;
      for (auto &i : block.instructions) {
        if (++work > l.max_work)
          return false;
        if (i.op == Op::CallPure) {
          if (!closed_pure(i.immediate, depth + 1))
            return false;
          height = std::max(height, pure_heights[i.immediate] + 1);
        } else if (static_cast<unsigned>(i.op) > 11 && i.op != Op::Unary && i.op != Op::Compare &&
                   i.op != Op::Convert)
          return false;
      }
    }
    pure_colors[id] = 2;
    pure_heights[id] = height;
    return true;
  };
  std::set<std::uint32_t> program_ids;
  for (auto &pr : p.programs) {
    const bool policy =
        pr.instruction_fuel || !pr.owner_policy.empty() || !pr.result_lifetime_policy.empty();
    if (policy &&
        (p.schema_major < 5 || pr.context != ContextKind::Process || !pr.instruction_fuel))
      return bad("process policy requires schema5 process and positive fuel");
    if (policy && (pr.owner_policy != "caller" || pr.result_lifetime_policy != "until-release"))
      return fail(ErrorCode::Unsupported, "unknown process owner/result lifetime policy");
    if (static_cast<unsigned>(pr.context) > 4 || (pr.effect_mask & ~std::uint64_t{2047}))
      return bad("program context/effects");
    if (p.schema_major == 1 &&
        (!pr.frame.empty() || pr.frame_bytes || pr.effect_mask || pr.context != ContextKind::Timed))
      return bad("v1 program metadata");
    std::uint64_t previous_end = 0;
    std::set<std::uint32_t> frame_registers;
    if (pr.frame.size() > l.max_rows)
      return bad("frame slot bound");
    for (auto &slot : pr.frame) {
      if (slot.type >= p.types.size() || !slot.align || (slot.align & (slot.align - 1)) ||
          slot.align > 64 || slot.offset % slot.align || slot.offset < previous_end)
        return bad("frame layout");
      if (p.schema_major >= 3 && !frame_registers.insert(slot.register_id).second)
        return bad("duplicate frame register");
      // A portable owning-value frame reserves a 64-byte cell for every expanded element.
      if (sizes[slot.type] > UINT64_MAX / 64)
        return bad("frame layout size overflow");
      auto width = std::uint64_t(sizes[slot.type]) * 64;
      if (width > pr.frame_bytes || slot.offset > pr.frame_bytes - width)
        return bad("frame byte bound");
      previous_end = slot.offset + width;
    }
    if (!program_ids.insert(pr.id).second || pr.blocks.empty() || pr.blocks.size() > l.max_rows)
      return bad("program IDs/block bound");
    for (auto t : pr.input_types)
      if (t >= p.types.size())
        return bad("input type");
    for (auto t : pr.result_types)
      if (t >= p.types.size())
        return bad("result type");
    std::map<std::uint32_t, const Block *> blocks;
    for (auto &b : pr.blocks)
      if (!blocks.emplace(b.id, &b).second)
        return bad("duplicate block");
    if (!blocks.count(pr.entry))
      return bad("entry missing");
    const auto &ep = blocks.at(pr.entry)->parameters;
    if (ep.size() != pr.input_types.size())
      return bad("entry arity");
    for (std::size_t i = 0; i < ep.size(); ++i)
      if (ep[i].type != pr.input_types[i])
        return bad("entry type");
    struct Def {
      std::uint32_t type, block;
      std::size_t order;
    };
    std::map<std::uint32_t, Def> defs;
    for (auto &b : pr.blocks) {
      auto def = [&](Reg r, std::size_t o) {
        return ++work <= l.max_work && r.id < l.max_registers && r.type < p.types.size() &&
               defs.emplace(r.id, Def{r.type, b.id, o}).second;
      };
      for (auto r : b.parameters)
        if (!def(r, 0))
          return bad("duplicate/bad register");
      if (b.instructions.size() > l.max_rows)
        return bad("instruction bound");
      for (std::size_t i = 0; i < b.instructions.size(); ++i)
        if (++work > l.max_work || b.instructions[i].args.size() > l.max_rows ||
            (b.instructions[i].dest && !def(*b.instructions[i].dest, i + 1)))
          return bad("duplicate/bad destination");
    }
    std::size_t register_elements = 0;
    if (p.schema_major >= 3)
      for (auto &slot : pr.frame) {
        auto found = defs.find(slot.register_id);
        if (found == defs.end() || found->second.type != slot.type)
          return bad("frame register definition/type");
      }
    for (auto &entry : defs) {
      auto size = sizes[entry.second.type];
      if (size > l.max_value_elements - register_elements)
        return bad("total register layout bound");
      register_elements += size;
    }
    if (pr.blocks.size() > l.max_work / pr.blocks.size())
      return bad("dominance memory/work bound");
    std::map<std::uint32_t, std::set<std::uint32_t>> pred, dom;
    std::set<std::uint32_t> segment_roots{pr.entry};
    std::set<std::uint32_t> all;
    for (auto &b : pr.blocks)
      all.insert(b.id);
    for (auto &b : pr.blocks) {
      auto edge = [&](const Edge &e) {
        if (!blocks.count(e.target))
          return false;
        auto &pars = blocks.at(e.target)->parameters;
        if (e.args.size() != pars.size())
          return false;
        for (std::size_t i = 0; i < pars.size(); ++i)
          if (pars[i].type != e.args[i].type)
            return false;
        pred[e.target].insert(b.id);
        return true;
      };
      auto &t = b.terminator;
      switch (t.kind) {
      case TermKind::Jump:
        if (!edge(t.yes))
          return bad("jump edge");
        break;
      case TermKind::Branch:
        if (!edge(t.yes) || !edge(t.no))
          return bad("branch edge");
        break;
      case TermKind::Switch: {
        std::set<std::uint64_t> tags;
        for (auto &c : t.cases)
          if (!tags.insert(c.first).second || !edge(c.second))
            return bad("switch edge/tag");
        if (!edge(t.no))
          return bad("switch default");
        break;
      }
      case TermKind::Return:
      case TermKind::Fail:
        break;
      case TermKind::TransportReturn:
        if (p.schema_major < 2 || pr.context != ContextKind::Transport)
          return bad("transport terminator context");
        break;
      case TermKind::Suspend: {
        if (p.schema_major < 2 || pr.context != ContextKind::Process ||
            !(pr.effect_mask & Effect::Wait) || !blocks.count(t.yes.target))
          return bad("suspend context/target");
        auto &params = blocks.at(t.yes.target)->parameters;
        if (params.size() != t.values.size() + 1 ||
            (p.schema_major == 2 && pr.frame.size() != t.values.size()))
          return bad("suspend live/frame arity");
        std::set<std::uint32_t> saved;
        for (std::size_t n = 0; n < t.values.size(); ++n) {
          const FrameSlot *slot = nullptr;
          if (p.schema_major == 2)
            slot = &pr.frame[n];
          else
            for (auto &candidate : pr.frame)
              if (candidate.register_id == t.values[n].id) {
                slot = &candidate;
                break;
              }
          if (!saved.insert(t.values[n].id).second || params[n + 1].type != t.values[n].type ||
              !slot || slot->type != t.values[n].type)
            return bad("suspend live/frame types");
        }
        pred[t.yes.target].insert(b.id);
        segment_roots.insert(t.yes.target);
        break;
      }
      default:
        return fail(ErrorCode::Unsupported, "terminator requires unavailable typed service schema");
      }
    }
    std::set<std::uint32_t> reached{pr.entry};
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto &b : pr.blocks) {
        if (++work > l.max_work)
          return bad("validation work limit");
        if (reached.count(b.id))
          continue;
        for (auto parent : pred[b.id])
          if (reached.count(parent)) {
            reached.insert(b.id);
            changed = true;
            break;
          }
      }
    }
    if (reached != all)
      return bad("unreachable block");
    for (auto id : all)
      dom[id] = segment_roots.count(id) ? std::set<std::uint32_t>{id} : all;
    changed = true;
    while (changed) {
      changed = false;
      for (auto id : all) {
        if (segment_roots.count(id))
          continue;
        auto next = all;
        for (auto parent : pred[id]) {
          if (work + next.size() > l.max_work)
            return bad("dominance work limit");
          work += next.size();
          for (auto it = next.begin(); it != next.end();)
            if (!dom[parent].count(*it))
              it = next.erase(it);
            else
              ++it;
        }
        next.insert(id);
        if (next != dom[id]) {
          dom[id] = std::move(next);
          changed = true;
        }
      }
    }
    for (auto &b : pr.blocks) {
      auto use = [&](Reg r, std::size_t order) {
        auto it = defs.find(r.id);
        return it != defs.end() && it->second.type == r.type && dom[b.id].count(it->second.block) &&
               (it->second.block != b.id || it->second.order < order);
      };
      for (std::size_t k = 0; k < b.instructions.size(); ++k) {
        if (++work > l.max_work)
          return bad("validation work limit");
        auto &i = b.instructions[k];
        if (i.op == Op::SetTransportReturn &&
            (k + 1 != b.instructions.size() || b.terminator.kind != TermKind::TransportReturn))
          return bad("prepared transport reply must terminate immediately");
        if (static_cast<unsigned>(i.binary) > 8 ||
            (i.op != Op::Const && !std::holds_alternative<std::monostate>(i.value.data)))
          return bad("noncanonical instruction fields");
        for (auto a : i.args)
          if (!use(a, k + 1))
            return bad("undefined/non-dominating operand");
        if (p.schema_major >= 2) {
          auto effect = required_effect(i.op);
          if ((pr.effect_mask & effect) != effect ||
              !(allowed_contexts(i.op) & (1u << static_cast<unsigned>(pr.context))))
            return bad("program opcode effect/context");
        }
        if (i.op == Op::CallPure) {
          if (p.schema_major < 2 || !closed_pure(i.immediate, 1))
            return bad("callPure body missing/impure/cyclic");
          auto &callee = *program_map.at(i.immediate);
          if (i.args.size() != callee.input_types.size() || callee.result_types.size() != 1 ||
              !i.dest || i.dest->type != callee.result_types[0])
            return bad("callPure signature");
          for (std::size_t n = 0; n < i.args.size(); ++n)
            if (i.args[n].type != callee.input_types[n])
              return bad("callPure argument");
          continue;
        }
        if (static_cast<unsigned>(i.op) >= 16 && i.op != Op::GetNow && i.op != Op::Unary &&
            i.op != Op::Compare && i.op != Op::Convert) {
          if (p.schema_major < 2)
            return fail(ErrorCode::Unsupported, "opcode requires v2 service schema");
          auto entry = services.find(i.immediate);
          if (entry == services.end() || entry->second->op != i.op)
            return bad("missing/mismatched service signature");
          auto &s = *entry->second;
          if (!(s.context_mask & (1u << static_cast<unsigned>(pr.context))) ||
              (pr.effect_mask & s.effect_mask) != s.effect_mask ||
              i.args.size() != s.input_types.size() || bool(i.dest) != !s.result_types.empty())
            return bad("service call shape/context/effect");
          for (std::size_t n = 0; n < i.args.size(); ++n)
            if (i.args[n].type != s.input_types[n])
              return bad("service argument type");
          if (i.dest && i.dest->type != s.result_types[0])
            return bad("service result type");
          continue;
        }
        if (i.op == Op::GetNow && p.schema_major < 2)
          return fail(ErrorCode::Unsupported, "getNow requires v2");
        auto ar = [&](std::size_t n, bool dst = true) {
          return i.args.size() == n && bool(i.dest) == dst;
        };
        auto kind = [&](Reg r, TypeKind t) { return p.types[r.type].kind == t; };
        auto dst = [&](std::uint32_t t) { return i.dest && i.dest->type == t; };
        bool ok = false;
        switch (i.op) {
        case Op::Unary:
          ok = p.schema_major >= 2 && ar(1) && dst(i.args[0].type) &&
               (i.immediate == 0 ? kind(i.args[0], TypeKind::Bool)
                                 : i.immediate <= 2 && kind(i.args[0], TypeKind::Bits));
          break;
        case Op::Compare:
          ok = p.schema_major >= 2 && ar(2) && i.args[0].type == i.args[1].type &&
               kind(*i.dest, TypeKind::Bool) && i.immediate <= 5 &&
               (i.immediate <= 1 || kind(i.args[0], TypeKind::Bits));
          break;
        case Op::Convert:
          if (p.schema_major >= 2 && ar(1) && kind(i.args[0], TypeKind::Bits) &&
              kind(*i.dest, TypeKind::Bits) && i.immediate <= 2)
            ok = i.immediate == 0 ? p.types[i.args[0].type].bound <= p.types[i.dest->type].bound
                                  : p.types[i.args[0].type].bound >= p.types[i.dest->type].bound;
          break;
        case Op::GetNow:
          ok = ar(0) && kind(*i.dest, TypeKind::Bits) && p.types[i.dest->type].bound == 64;
          break;
        case Op::Const:
          ok = ar(0) && conforms(p, i.dest->type, i.value, l.max_depth);
          break;
        case Op::Move:
          ok = ar(1) && dst(i.args[0].type);
          break;
        case Op::Binary:
          if (ar(2) && i.args[0].type == i.args[1].type) {
            auto a = i.args[0];
            auto op = i.binary;
            if (op == Binary::Eq)
              ok = kind(*i.dest, TypeKind::Bool);
            else if (op == Binary::And || op == Binary::Or)
              ok = kind(a, TypeKind::Bool) && dst(a.type);
            else if (static_cast<unsigned>(op) <= 6)
              ok = (kind(a, TypeKind::Bits) ||
                    (p.schema_major >= 5 && op == Binary::Lt && kind(a, TypeKind::Fin))) &&
                   (op == Binary::Lt ? kind(*i.dest, TypeKind::Bool) : dst(a.type));
          }
          break;
        case Op::SelectValue:
          ok = ar(3) && kind(i.args[0], TypeKind::Bool) && i.args[1].type == i.args[2].type &&
               dst(i.args[1].type);
          break;
        case Op::MakeRecord:
        case Op::MakeVariant:
        case Op::MakeVec:
          if (i.dest) {
            auto &s = p.types[i.dest->type];
            std::vector<std::uint32_t> fs;
            if (i.op == Op::MakeRecord && s.kind == TypeKind::Record)
              fs = s.fields;
            else if (i.op == Op::MakeVariant && s.kind == TypeKind::Variant &&
                     i.immediate < s.constructors.size())
              fs = s.constructors[i.immediate];
            else if (i.op == Op::MakeVec &&
                     (s.kind == TypeKind::Vec || s.kind == TypeKind::BoundedVec) &&
                     (s.kind == TypeKind::Vec ? i.args.size() == s.bound
                                              : i.args.size() <= s.bound))
              fs.assign(i.args.size(), s.fields[0]);
            else
              break;
            ok = fs.size() == i.args.size();
            for (std::size_t j = 0; j < fs.size() && ok; ++j)
              ok = fs[j] == i.args[j].type;
          }
          break;
        case Op::GetField:
          if (ar(1)) {
            auto &s = p.types[i.args[0].type];
            ok = s.kind == TypeKind::Record && i.immediate < s.fields.size() &&
                 dst(s.fields[i.immediate]);
          }
          break;
        case Op::VariantTag:
          ok = ar(1) && kind(i.args[0], TypeKind::Variant) && kind(*i.dest, TypeKind::Bits) &&
               p.types[i.dest->type].bound == 64;
          break;
        case Op::VariantGet:
          if (ar(1)) {
            auto &s = p.types[i.args[0].type];
            auto tag = i.immediate >> 16, field = i.immediate & 65535;
            ok = s.kind == TypeKind::Variant && tag < s.constructors.size() &&
                 field < s.constructors[tag].size() && dst(s.constructors[tag][field]);
          }
          break;
        case Op::VecGet:
        case Op::VecSet:
          if (ar(i.op == Op::VecGet ? 2 : 3)) {
            auto &s = p.types[i.args[0].type];
            ok = (s.kind == TypeKind::Vec || s.kind == TypeKind::BoundedVec) &&
                 ((kind(i.args[1], TypeKind::Bits) &&
                   (p.schema_major >= 5 || p.types[i.args[1].type].bound == 64)) ||
                  (p.schema_major >= 5 && kind(i.args[1], TypeKind::Fin))) &&
                 (i.op == Op::VecGet ? dst(s.fields[0])
                                     : dst(i.args[0].type) && i.args[2].type == s.fields[0]);
          }
          break;
        case Op::LoadState:
          ok = ar(0) && i.immediate < p.state_types.size() && dst(p.state_types[i.immediate]);
          break;
        case Op::BufferStateWrite:
          ok = ar(1, false) && i.immediate < p.state_types.size() &&
               i.args[0].type == p.state_types[i.immediate];
          break;
        case Op::Check:
          ok = ar(1, false) && kind(i.args[0], TypeKind::Bool);
          break;
        case Op::Trace:
          ok = !i.dest;
          break;
        default:
          break;
        }
        if (!ok)
          return bad("instruction type/arity/immediate mismatch");
      }
      auto &t = b.terminator;
      auto order = b.instructions.size() + 1;
      auto edgeuse = [&](const Edge &e) {
        for (auto r : e.args)
          if (!use(r, order))
            return false;
        return true;
      };
      if (t.kind == TermKind::Jump && !edgeuse(t.yes))
        return bad("edge operand");
      if (t.kind == TermKind::Branch &&
          (!use(t.value, order) || p.types[t.value.type].kind != TypeKind::Bool ||
           !edgeuse(t.yes) || !edgeuse(t.no)))
        return bad("branch condition/operand");
      if (t.kind == TermKind::Switch) {
        if (!use(t.value, order) || p.types[t.value.type].kind != TypeKind::Bits || !edgeuse(t.no))
          return bad("switch value");
        for (auto &c : t.cases)
          if (!edgeuse(c.second) || (p.types[t.value.type].bound < 64 &&
                                     c.first >= (std::uint64_t{1} << p.types[t.value.type].bound)))
            return bad("switch case operand/range");
      }
      if (t.kind == TermKind::Return) {
        if (p.schema_major >= 2 && pr.context == ContextKind::Transport)
          return bad("transport program requires transport return");
        if (t.values.size() != pr.result_types.size())
          return bad("return arity");
        for (std::size_t i = 0; i < t.values.size(); ++i)
          if (!use(t.values[i], order) || t.values[i].type != pr.result_types[i])
            return bad("return type");
      }
      if (t.kind == TermKind::TransportReturn) {
        if (!use(t.value, order) || pr.result_types.size() != 1 ||
            t.value.type != pr.result_types[0] || b.instructions.empty() ||
            b.instructions.back().op != Op::SetTransportReturn ||
            b.instructions.back().args.size() != 1 ||
            b.instructions.back().args[0].id != t.value.id)
          return bad("transport return value/preparation");
      }
      if (t.kind == TermKind::Suspend) {
        if (!use(t.value, order) || p.types[t.value.type].kind != TypeKind::Handle ||
            p.types[t.value.type].bound != static_cast<unsigned>(HandleKind::Wait))
          return bad("suspend wait handle");
        for (auto r : t.values)
          if (!use(r, order))
            return bad("suspend undefined live register");
      }
    }
  }
  auto metadata_limits = l;
  metadata_limits.max_work = l.max_work - work;
  auto metadata = validate_metadata(p, metadata_limits);
  if (!metadata)
    return metadata.error();
  return ValidatedProject{std::move(p)};
}
} // namespace leanat::exec

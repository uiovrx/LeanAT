#include "leanat/optional_services.hpp"
#include "leanat/descriptor.hpp"
namespace leanat {
namespace {
bool type_is(const exec::Project &p, std::uint32_t i, exec::TypeKind k, std::uint64_t bound) {
  return i < p.types.size() && p.types[i].kind == k && p.types[i].bound == bound;
}
bool u64(const exec::Project &p, std::uint32_t i) {
  return type_is(p, i, exec::TypeKind::Bits, 64);
}
bool handle_type(const exec::Project &p, std::uint32_t i, HandleKind k) {
  return type_is(p, i, exec::TypeKind::Handle, static_cast<std::uint64_t>(k));
}
bool bytes_type(const exec::Project &p, std::uint32_t i) {
  return i < p.types.size() && p.types[i].kind == exec::TypeKind::Bytes &&
         p.types[i].bound <= 65536;
}
Expected<std::uint32_t> except_payload(const exec::Project &p, std::uint32_t i) {
  if (i >= p.types.size()) {
    return fail(ErrorCode::Schema, "optional Except type missing");
  }
  auto &t = p.types[i];
  if (t.kind != exec::TypeKind::Variant || t.constructors.size() != 2 ||
      t.constructors[0].size() != 1 || t.constructors[1].size() != 1 ||
      !u64(p, t.constructors[0][0])) {
    return fail(ErrorCode::Schema, "optional ABI requires Except U64 T");
  }
  return t.constructors[1][0];
}
bool layout_type(const exec::Project &p, std::uint32_t i, ExternalLayout l) {
  if (l.kind == ExternalLayoutKind::Bytes) {
    return i < p.types.size() && p.types[i].kind == exec::TypeKind::Bytes &&
           p.types[i].bound == l.max_bytes;
  }
  if (l.kind == ExternalLayoutKind::U64LE) {
    return u64(p, i);
  }
  return type_is(p, i, exec::TypeKind::Bool, 0);
}
Value ok(Value value) {
  return Value{Value::Array{Value{std::uint64_t{1}}, Value{Value::Array{std::move(value)}}}};
}
Value error_value(ErrorCode code) {
  return Value{Value::Array{Value{std::uint64_t{0}},
                            Value{Value::Array{Value{static_cast<std::uint64_t>(code)}}}}};
}
Expected<std::vector<Value>> admission(Expected<Handle> h) {
  if (h) {
    return std::vector<Value>{ok(Value{h.value()})};
  }
  if (h.error().code == ErrorCode::StaleHandle || h.error().code == ErrorCode::WrongOwner ||
      h.error().code == ErrorCode::WrongDomain || h.error().code == ErrorCode::Integrity) {
    return h.error();
  }
  return std::vector<Value>{error_value(h.error().code)};
}
Expected<std::uint64_t> number(const Value &v) {
  auto p = std::get_if<std::uint64_t>(&v.data);
  if (!p) {
    return fail(ErrorCode::TypeMismatch, "optional U64 argument");
  }
  return *p;
}
Expected<Handle> owned_handle(const Value &v, HandleKind k, const ExecutionContext &c) {
  auto p = std::get_if<Handle>(&v.data);
  if (!p || p->kind != k) {
    return fail(ErrorCode::TypeMismatch, "optional handle kind");
  }
  if (p->domain != c.domain) {
    return fail(ErrorCode::WrongDomain, "optional handle domain");
  }
  if (p->owner != c.owner) {
    return fail(ErrorCode::WrongOwner, "optional handle owner");
  }
  return *p;
}
void append(Bytes &b, std::uint64_t n) {
  for (unsigned i = 0; i < 8; ++i) {
    b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
}
void append_text(Bytes &b, const std::string &s) {
  append(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}
std::array<std::uint8_t, 32> abi(const exec::Project &p, const exec::ServiceSignature &s,
                                 const std::string &semantics) {
  Bytes b;
  append_text(b, semantics);
  append(b, static_cast<unsigned>(s.op));
  // Include the complete finite type table, so changing any referenced structural layout
  // invalidates binding identity.
  append(b, p.types.size());
  for (auto &t : p.types) {
    append(b, static_cast<unsigned>(t.kind));
    append(b, t.bound);
    append(b, t.fields.size());
    for (auto f : t.fields) {
      append(b, f);
    }
    append(b, t.constructors.size());
    for (auto &c : t.constructors) {
      append(b, c.size());
      for (auto f : c) {
        append(b, f);
      }
    }
  }
  for (auto i : s.input_types) {
    append(b, i);
  }
  for (auto i : s.result_types) {
    append(b, i);
  }
  return exec::sha256(b);
}
bool identity_matches(const exec::ServiceSignature &a, const exec::ServiceSignature &b) {
  return a.op == b.op && a.provider_key == b.provider_key &&
         a.provider_version == b.provider_version && a.abi_hash == b.abi_hash &&
         a.context_mask == b.context_mask && a.effect_mask == b.effect_mask &&
         a.extra_fuel == b.extra_fuel;
}
} // namespace
Expected<exec::ServiceSignature>
external_service_signature(const exec::Project &p, std::uint32_t id, std::uint32_t input,
                           std::uint32_t output, const ExternalCallGate &gate, ExternCallId call) {
  auto d = gate.descriptor(call);
  if (!d) {
    return d.error();
  }
  auto payload = except_payload(p, output);
  if (!payload) {
    return payload.error();
  }
  if (!layout_type(p, input, d.value()->input) ||
      !layout_type(p, payload.value(), d.value()->output)) {
    return fail(ErrorCode::Schema, "external service concrete layout mismatch");
  }
  exec::ServiceSignature s;
  s.id = id;
  s.op = exec::Op::CallExternPure;
  s.input_types = {input};
  s.result_types = {output};
  s.context_mask = 3;
  s.effect_mask = exec::External;
  s.provider_key = "leanat.external." + d.value()->logical_id;
  s.provider_version = d.value()->reference_hash + "|" +
                       (d.value()->select_native ? d.value()->build_id : "reference") + "|" +
                       d.value()->contract_hash;
  s.abi_hash =
      abi(p, s,
          "external-v1:" + std::to_string(call) + ":" + std::to_string(d.value()->abi_version) +
              ":" + d.value()->symbol_id + ":" + s.provider_version);
  return s;
}
Expected<void> register_external_service(CoreRuntimeBackend &backend, const exec::Project &p,
                                         exec::ServiceSignature s, ExternalCallGate &gate,
                                         ExternCallId call) {
  if (s.input_types.size() != 1 || s.result_types.size() != 1) {
    return fail(ErrorCode::Schema, "external service arity");
  }
  auto concrete =
      external_service_signature(p, s.id, s.input_types[0], s.result_types[0], gate, call);
  if (!concrete) {
    return concrete.error();
  }
  if (!identity_matches(s, concrete.value())) {
    return fail(ErrorCode::Integrity, "external service identity mismatch");
  }
  return backend.register_provider(std::move(concrete.value()),
                                   [&gate, call](const std::vector<Value> &args,
                                                 const ExecutionContext &,
                                                 EventTxn &) -> Expected<std::vector<Value>> {
                                     auto result = gate.call_pure(call, args[0]);
                                     if (!result) {
                                       return result.error();
                                     }
                                     return std::vector<Value>{ok(std::move(result.value()))};
                                   });
}
Expected<exec::ServiceSignature> managed_service_signature(const exec::Project &p, std::uint32_t id,
                                                           exec::Op op,
                                                           std::vector<std::uint32_t> inputs,
                                                           std::uint32_t result) {
  using exec::Op;
  bool valid = false;
  if (result >= p.types.size()) {
    return fail(ErrorCode::Schema, "managed result type missing");
  }
  if (op == Op::RequestManaged || op == Op::BeginManagedRead || op == Op::BeginManagedWrite) {
    auto payload = except_payload(p, result);
    if (!payload) {
      return payload.error();
    }
    auto kind = op == Op::RequestManaged ? HandleKind::Lease : HandleKind::Access;
    if (!handle_type(p, payload.value(), kind) || inputs.size() != 4) {
      return fail(ErrorCode::Schema, "managed admission shape");
    }
    if (op == Op::RequestManaged) {
      valid = std::all_of(inputs.begin(), inputs.end(), [&](auto i) { return u64(p, i); });
    } else {
      valid = handle_type(p, inputs[0], HandleKind::Lease) && u64(p, inputs[1]) &&
              u64(p, inputs[3]) &&
              (op == Op::BeginManagedRead ? u64(p, inputs[2]) : bytes_type(p, inputs[2]));
    }
  } else if (op == Op::ReleaseLease || op == Op::ResultRelease) {
    valid = inputs.size() == 1 &&
            handle_type(p, inputs[0],
                        op == Op::ReleaseLease ? HandleKind::Lease : HandleKind::Access) &&
            p.types[result].kind == exec::TypeKind::Unit;
  } else if (op == Op::InvalidateManaged) {
    valid = inputs.size() == 4 &&
            std::all_of(inputs.begin(), inputs.end(), [&](auto i) { return u64(p, i); }) &&
            p.types[result].kind == exec::TypeKind::Unit;
  } else if (op == Op::ResultGet) {
    const auto &t = p.types[result];
    valid = inputs.size() == 1 && handle_type(p, inputs[0], HandleKind::Access) &&
            t.kind == exec::TypeKind::Record && t.fields.size() == 3 && u64(p, t.fields[0]) &&
            u64(p, t.fields[1]) && bytes_type(p, t.fields[2]);
  }
  if (!valid) {
    return fail(ErrorCode::Schema, "unsupported managed concrete signature");
  }
  exec::ServiceSignature s;
  s.id = id;
  s.op = op;
  s.input_types = std::move(inputs);
  s.result_types = {result};
  s.context_mask = 3;
  s.effect_mask = exec::required_effect(op);
  s.provider_key = "leanat.managed." + std::to_string(static_cast<unsigned>(op));
  s.provider_version = "1";
  s.abi_hash = abi(p, s, "managed-v1");
  return s;
}
Expected<void> register_managed_service(CoreRuntimeBackend &backend, const exec::Project &p,
                                        exec::ServiceSignature s, ManagedAccessManager &manager,
                                        ResultStore &results, ManagedProviderOptions options) {
  if (!manager.uses_results(results)) {
    return fail(ErrorCode::WrongOwner, "managed result registry mismatch");
  }
  if (s.result_types.size() != 1) {
    return fail(ErrorCode::Schema, "managed service arity");
  }
  auto concrete = managed_service_signature(p, s.id, s.op, s.input_types, s.result_types[0]);
  if (!concrete) {
    return concrete.error();
  }
  if (!identity_matches(s, concrete.value())) {
    return fail(ErrorCode::Integrity, "managed service identity mismatch");
  }
  if (s.op == exec::Op::InvalidateManaged && !options.permit_invalidation) {
    return fail(ErrorCode::Unsupported, "managed invalidation provider not authorized");
  }
  auto op = s.op;
  return backend.register_provider(
      std::move(concrete.value()),
      [&manager, &results, op, options](const std::vector<Value> &a, const ExecutionContext &c,
                                        EventTxn &txn) -> Expected<std::vector<Value>> {
        using exec::Op;
        if (op == Op::RequestManaged || op == Op::InvalidateManaged) {
          std::uint64_t n[4];
          for (unsigned i = 0; i < 4; ++i) {
            auto v = number(a[i]);
            if (!v) {
              return v.error();
            }
            n[i] = v.value();
          }
          if (n[0] > UINT32_MAX) {
            return fail(ErrorCode::InvalidArgument, "managed region id overflow");
          }
          if (op == Op::RequestManaged) {
            if (n[3] < 1 || n[3] > 3) {
              return std::vector<Value>{error_value(ErrorCode::InvalidArgument)};
            }
            ManagedRequest request;
            request.region = RegionId{static_cast<std::uint32_t>(n[0])};
            request.connection = c.connection;
            request.domain = c.domain;
            request.owner = c.owner;
            request.start = n[1];
            request.end = n[2];
            request.permission = static_cast<ManagedPermission>(n[3]);
            return admission(manager.prepare_request(txn, request));
          }
          if (c.owner != options.invalidation_owner) {
            return fail(ErrorCode::WrongOwner, "managed invalidation authority");
          }
          auto changed = manager.prepare_invalidate(txn, RegionId{static_cast<std::uint32_t>(n[0])},
                                                    n[1], n[2], Tick{n[3]});
          if (!changed) {
            return changed.error();
          }
          return std::vector<Value>{Value{}};
        }
        auto kind =
            (op == Op::BeginManagedRead || op == Op::BeginManagedWrite || op == Op::ReleaseLease)
                ? HandleKind::Lease
                : HandleKind::Access;
        auto handle = owned_handle(a[0], kind, c);
        if (!handle) {
          return handle.error();
        }
        if (op == Op::BeginManagedRead || op == Op::BeginManagedWrite) {
          auto address = number(a[1]), arrival = number(a[3]);
          if (!address) {
            return address.error();
          }
          if (!arrival) {
            return arrival.error();
          }
          OwnedAccessRequest request;
          request.lease = handle.value();
          request.address = address.value();
          request.arrival = Tick{arrival.value()};
          request.command = op == Op::BeginManagedRead ? Command::Read : Command::Write;
          if (op == Op::BeginManagedRead) {
            auto count = number(a[2]);
            if (!count) {
              return count.error();
            }
            if (count.value() > SIZE_MAX) {
              return fail(ErrorCode::Overflow, "managed count");
            }
            request.count = count.value();
          } else {
            auto bytes = std::get_if<Bytes>(&a[2].data);
            if (!bytes) {
              return fail(ErrorCode::TypeMismatch, "managed write buffer");
            }
            if (bytes->size() > txn.remaining_bytes()) {
              return std::vector<Value>{error_value(ErrorCode::Capacity)};
            }
            request.input = *bytes;
            request.count = bytes->size();
          }
          return admission(manager.prepare_begin(txn, std::move(request)));
        }
        if (op == Op::ReleaseLease) {
          auto released = manager.prepare_release_lease(txn, handle.value());
          if (!released) {
            return released.error();
          }
          return std::vector<Value>{Value{}};
        }
        if (op == Op::ResultRelease) {
          auto released = manager.prepare_release_result(txn, handle.value());
          if (!released) {
            return released.error();
          }
          return std::vector<Value>{Value{}};
        }
        auto result = manager.prepare_result_handle(txn, handle.value());
        if (!result) {
          return result.error();
        }
        auto value = results.prepare_read(txn, result.value());
        if (!value) {
          return value.error();
        }
        return std::vector<Value>{std::move(value.value())};
      });
}
} // namespace leanat
namespace leanat {
Expected<void>
register_external_reference_program(ExternalCallGate &gate, ExternCallDesc descriptor,
                                    std::shared_ptr<const exec::ValidatedProject> project,
                                    ProgramId reference, ProgramId precondition,
                                    std::uint64_t fuel) {
  if (!project || !fuel || fuel > 1000000) {
    return fail(ErrorCode::InvalidArgument, "external reference program/fuel");
  }
  const auto &p = project->get();
  if (!p.services.empty() || !p.state_types.empty() || !descriptor.dependencies.empty()) {
    return fail(ErrorCode::Unsupported, "reference closure must be self-contained pure programs");
  }
  const exec::Program *ref = nullptr;
  const exec::Program *pre = nullptr;
  for (const auto &program : p.programs) {
    if (program.effect_mask || program.context != ContextKind::Timed) {
      return fail(ErrorCode::Unsupported, "external reference program is not pure timed code");
    }
    for (const auto &block : program.blocks) {
      if (block.terminator.kind == exec::TermKind::Suspend ||
          block.terminator.kind == exec::TermKind::TransportReturn) {
        return fail(ErrorCode::Unsupported, "external reference control effect");
      }
      for (const auto &instruction : block.instructions) {
        if (instruction.op > exec::Op::CallPure || instruction.op == exec::Op::Trace ||
            instruction.op == exec::Op::LoadState || instruction.op == exec::Op::BufferStateWrite) {
          return fail(ErrorCode::Unsupported, "external reference ambient effect");
        }
      }
    }
    if (program.id == reference.value) {
      ref = &program;
    }
    if (program.id == precondition.value) {
      pre = &program;
    }
  }
  if (!ref || !pre || ref->input_types.size() != 1 || ref->result_types.size() != 1 ||
      pre->input_types.size() != 1 || pre->result_types.size() != 1 ||
      !layout_type(p, ref->input_types[0], descriptor.input) ||
      !layout_type(p, ref->result_types[0], descriptor.output) ||
      !layout_type(p, pre->input_types[0], descriptor.input) ||
      !type_is(p, pre->result_types[0], exec::TypeKind::Bool, 0)) {
    return fail(ErrorCode::Schema, "external reference/precondition signature");
  }
  auto bytes = exec::serialize(*project);
  if (!bytes) {
    return bytes.error();
  }
  auto hash = exec::sha256(bytes.value());
  std::string identity;
  static const char hex[] = "0123456789abcdef";
  for (auto b : hash) {
    identity.push_back(hex[b >> 4]);
    identity.push_back(hex[b & 15]);
  }
  identity += ":" + std::to_string(reference.value) + ":" + std::to_string(precondition.value);
  if (!descriptor.reference_hash.empty() && descriptor.reference_hash != identity) {
    return fail(ErrorCode::Integrity, "external reference artifact mismatch");
  }
  descriptor.reference_hash = identity;
  auto evaluate = [project, fuel](ProgramId program, const Value &input) -> Expected<Value> {
    ExecutionContext context;
    EventTxn txn(SegmentBudget{}, context);
    exec::FuelCounter budget{fuel};
    exec::Interpreter vm(*project);
    auto result = vm.execute_segment(program.value, context, {input}, txn, budget);
    if (!result) {
      return result.error();
    }
    if (result.value().kind != exec::SegmentResult::Kind::Returned ||
        result.value().values.size() != 1) {
      return fail(ErrorCode::ExternalFailure, "external reference did not return one value");
    }
    auto discarded = txn.discard();
    if (!discarded) {
      return discarded.error();
    }
    return std::move(result.value().values[0]);
  };
  return gate.register_reference(
      std::move(descriptor),
      [evaluate, reference](ExternalCallGate &, const Value &input) {
        return evaluate(reference, input);
      },
      [evaluate, precondition](const Value &input) -> Expected<bool> {
        auto result = evaluate(precondition, input);
        if (!result) {
          return result.error();
        }
        auto value = std::get_if<bool>(&result.value().data);
        if (!value) {
          return fail(ErrorCode::TypeMismatch, "external precondition result");
        }
        return *value;
      });
}
} // namespace leanat

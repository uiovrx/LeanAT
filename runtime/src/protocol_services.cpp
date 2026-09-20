#include "leanat/protocol_services.hpp"
namespace leanat {
namespace {
Expected<std::uint64_t> number(const Value &v) {
  auto n = std::get_if<std::uint64_t>(&v.data);
  if (!n)
    return fail(ErrorCode::TypeMismatch, "protocol service expects U64");
  return *n;
}
Expected<Handle> transaction(const Value &v, const ExecutionContext &ctx) {
  auto h = std::get_if<Handle>(&v.data);
  if (!h || h->kind != HandleKind::Transaction)
    return fail(ErrorCode::TypeMismatch, "protocol service expects transaction handle");
  if (h->domain != ctx.domain || h->owner != ctx.owner)
    return fail(ErrorCode::WrongOwner, "protocol service transaction context");
  return *h;
}
} // namespace
Expected<void> register_protocol_service(CoreRuntimeBackend &backend, Runtime &runtime,
                                         AdmissionStore &admission,
                                         exec::ServiceSignature signature,
                                         ProtocolServiceConfig config) {
  using exec::Op;
  auto op = signature.op;
  std::size_t arity = op == Op::NewTransaction ? 6
                      : op == Op::StagePhase   ? 4
                      : op == Op::AckResponse  ? 3
                                               : 0;
  if (!arity)
    return fail(ErrorCode::Unsupported, "not a protocol service opcode");
  if (signature.input_types.size() != arity || signature.result_types.size() != 1)
    return fail(ErrorCode::Schema, "protocol service ABI arity");
  if (!config.max_payload_bytes)
    return fail(ErrorCode::InvalidArgument, "protocol service payload bound required");
  if (signature.provider_key != "leanat.protocol" || signature.provider_version != "1")
    return fail(ErrorCode::Integrity, "protocol provider identity mismatch");
  return backend.register_provider(
      std::move(signature),
      [&runtime, &admission, op, config](const std::vector<Value> &args,
                                         const ExecutionContext &ctx,
                                         EventTxn &tx) -> Expected<std::vector<Value>> {
        if (ctx.domain != admission.domain() || ctx.instance != admission.local_side() ||
            (ctx.kind != ContextKind::Timed && ctx.kind != ContextKind::Process))
          return fail(ErrorCode::WrongOwner, "protocol service local instance/context");
        auto selected = runtime.protocol(ctx.instance);
        if (!selected)
          return selected.error();
        auto &protocol = *selected.value();
        if (!protocol.base_protocol())
          return fail(ErrorCode::Unsupported,
                      "base protocol services cannot execute standalone phase IDs");
        auto saved = tx.checkpoint();
        if (!saved)
          return saved.error();
        auto prepared = admission.prepare(tx);
        if (!prepared)
          return prepared.error();
        auto update = prepared.value().get();
        auto staged_update = tx.stage_participant(std::move(prepared.value()));
        if (!staged_update)
          return staged_update.error();
        auto abort = [&](Error error) -> Expected<std::vector<Value>> {
          auto reverted = tx.rollback(std::move(saved.value()));
          if (!reverted)
            return reverted.error();
          return error;
        };
        auto finish = [&](Value value) -> Expected<std::vector<Value>> {
          {
            auto charged = tx.reserve_participant_growth(*update, update->reserved_bytes());
            if (!charged)
              return abort(charged.error());
          }
          return std::vector<Value>{std::move(value)};
        };
        if (op == Op::NewTransaction) {
          auto connection = number(args[0]), transport = number(args[1]),
               generation = number(args[2]), command = number(args[3]), address = number(args[4]);
          auto data = std::get_if<Bytes>(&args[5].data);
          if (!connection || !transport || !generation || !command || !address || !data)
            return abort(fail(ErrorCode::TypeMismatch, "new transaction operands"));
          if (connection.value() > UINT32_MAX ||
              command.value() > static_cast<std::uint64_t>(Command::Ignore) ||
              !generation.value() || data->size() > config.max_payload_bytes)
            return abort(fail(ErrorCode::InvalidArgument, "new transaction finite operands"));
          AdmissionRequest request;
          request.connection = ConnectionId{static_cast<std::uint32_t>(connection.value())};
          request.transport = TransportId{transport.value()};
          request.transport_generation = generation.value();
          request.owner = ctx.owner;
          request.in_time = ctx.ready.time;
          request.request.command = static_cast<Command>(command.value());
          request.request.address = address.value();
          request.request.data = *data;
          request.request.streaming_width = std::max<std::size_t>(1, data->size());
          request.route.original_address = address.value();
          request.route.local_address = address.value();
          auto created = update->draft().create_initiator(request);
          if (!created)
            return abort(created.error());
          auto bound =
              protocol.prepare_bind_ledger(tx, created.value().hop, request.connection,
                                           request.transport, request.transport_generation);
          if (!bound)
            return abort(bound.error());
          auto drain = runtime.drains().prepare(tx);
          if (!drain)
            return abort(drain.error());
          Responsibility responsibility{created.value().txn,
                                        ctx.instance,
                                        ctx.epoch,
                                        {},
                                        {{created.value().hop, false, false, false, false}},
                                        false,
                                        false};
          auto registered =
              drain.value()->view().register_responsibility(std::move(responsibility));
          if (!registered)
            return abort(registered.error());
          auto staged = tx.stage_participant(std::move(drain.value()));
          if (!staged)
            return abort(staged.error());
          staged = runtime.stage_track_cleanup(tx, created.value().txn, created.value().hop, true,
                                               request.request, &admission);
          if (!staged)
            return abort(staged.error());
          return finish(Value{created.value().txn});
        }
        auto txn = transaction(args[0], ctx);
        auto connection = number(args[1]);
        auto phase =
            op == Op::StagePhase ? number(args[2]) : Expected<std::uint64_t>{end_resp.value};
        auto when = number(args[op == Op::StagePhase ? 3 : 2]);
        if (!txn || !connection || !phase || !when)
          return abort(fail(ErrorCode::TypeMismatch, "phase service operands"));
        if (connection.value() > UINT32_MAX || phase.value() < 1 || phase.value() > 4 ||
            when.value() < ctx.ready.time.value)
          return abort(fail(ErrorCode::InvalidArgument, "phase service finite operands/time"));
        ConnectionId link{static_cast<std::uint32_t>(connection.value())};
        if (phase.value() == begin_req.value) {
          auto pending_drain = dynamic_cast<DrainUpdate *>(tx.participant(&runtime.drains()));
          const auto &drain_view = pending_drain ? pending_drain->view() : runtime.drains();
          if (drain_view.is_cancelled(txn.value()))
            return abort(fail(ErrorCode::InvalidState, "cancelled transaction cannot start a request"));
        }
        auto hop = update->draft().lookup(txn.value(), link);
        if (!hop)
          return abort(hop.error());
        auto payload = hop.value().owned_request;
        PhaseId phase_id{static_cast<std::uint32_t>(phase.value())};
        std::optional<RequestPermit> permit;
        if (phase_id == begin_req) {
          auto available = update->draft().update_request_lane(
              link, protocol.request_lane_free(link), ctx.ready);
          if (!available)
            return abort(available.error());
          auto granted = update->draft().try_request_permit(txn.value(), link, ctx.ready);
          if (!granted)
            return abort(granted.error());
          if (!granted.value())
            return abort(fail(ErrorCode::NotReady, "request gate busy; no implicit wait"));
          permit = *granted.value();
        }
        if (phase_id == begin_resp) {
          if (!config.payload_snapshot)
            return abort(fail(ErrorCode::Unsupported,
                              "BEGIN_RESP requires current payload snapshot provider"));
          auto response = config.payload_snapshot(txn.value(), link, ctx, tx);
          if (!response)
            return abort(response.error());
          payload = std::move(response.value());
          if (payload.status == ResponseStatus::Incomplete)
            return abort(fail(ErrorCode::ProtocolViolation, "BEGIN_RESP response unavailable"));
        }
        if (phase_id == end_resp) {
          auto cell = protocol.ack_cell(hop.value().hop, ctx.epoch);
          if (!cell)
            return abort(cell.error());
          auto previous = tx.read(*cell.value());
          if (!previous)
            return abort(previous.error());
          auto acknowledged = std::get_if<bool>(&previous.value().data);
          if (!acknowledged)
            return abort(fail(ErrorCode::TypeMismatch, "ack cell type"));
          if (*acknowledged)
            return abort(fail(ErrorCode::Duplicate, "response acknowledgement already staged"));
          auto staged = tx.buffer(*cell.value(), Value{true});
          if (!staged)
            return abort(staged.error());
        }
        if (payload.data.size() > config.max_payload_bytes)
          return abort(fail(ErrorCode::Capacity, "phase payload exceeds provider bound"));
        auto id = runtime.stage_allocate_call_id(tx, CallOrigin::Outgoing);
        if (!id)
          return abort(id.error());
        SendIntent intent;
        intent.connection = link;
        intent.txn = txn.value();
        intent.flow =
            phase_id == end_req || phase_id == begin_resp ? Flow::Backward : Flow::Forward;
        intent.phase = phase_id;
        intent.not_before = Tick{when.value()};
        intent.call_id = id.value();
        intent.transport = hop.value().transport;
        intent.payload = std::move(payload);
        auto staged = runtime.stage_publish(tx, std::move(intent));
        if (!staged)
          return abort(staged.error());
        if (permit) {
          staged = runtime.stage_request_permit(tx, id.value(), admission, *permit);
          if (!staged)
            return abort(staged.error());
        }
        return finish(Value{});
      });
}
} // namespace leanat

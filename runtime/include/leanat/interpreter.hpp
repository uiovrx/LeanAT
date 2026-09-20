#pragma once
#include "event_txn.hpp"
#include "exec_ir.hpp"
#include "process.hpp"
namespace leanat::exec {
struct FuelCounter {
  std::uint64_t remaining{};
  Expected<void> consume(std::uint64_t amount = 1) {
    if (remaining < amount)
      return fail(ErrorCode::FuelExhausted, "instruction fuel exhausted");
    remaining -= amount;
    return {};
  }
};
struct SegmentResult {
  enum class Kind { Returned, Failed, Suspended, TransportReturned };
  Kind kind{Kind::Returned};
  std::vector<Value> values;
  std::string error;
  std::uint64_t fuel_used{};
  std::vector<TraceEvent> traces;
  std::optional<Handle> suspended_wait;
  BlockId resume_block{};
  std::vector<Value> live_values;
};
struct ResumeInput {
  ProgramId program;
  BlockId block;
  std::vector<Value> arguments;
};
class InterpreterBackend {
public:
  virtual ~InterpreterBackend() = default;
  // Must compare the declared signature with the actual frozen provider/registry binding.
  virtual Expected<void> check_signature(const ServiceSignature &) const = 0;
  // Operations may only prepare effects through txn; owned values are checked by the VM.
  virtual Expected<std::vector<Value>> invoke(const ServiceSignature &, const std::vector<Value> &,
                                              const ExecutionContext &, EventTxn &) = 0;
  virtual Expected<void> prepare_suspend(Handle, BlockId, std::vector<Value>, TypeId,
                                         const ExecutionContext &, EventTxn &) {
    return fail(ErrorCode::Unsupported, "suspension adapter unavailable");
  }
  virtual Expected<ResumeInput> prepare_resume(const SuspensionToken &, const ExecutionContext &,
                                               EventTxn &) {
    return fail(ErrorCode::Unsupported, "resume adapter unavailable");
  }
};
// Borrowed values are valid only for the synchronous callback. A sink that retains
// evidence must copy it and report capacity failure rather than truncate silently.
struct OpcodeObservation {
  enum class Stage { Entered, Completed, Error };
  Stage stage;
  std::uint32_t program_id, block_id;
  std::size_t instruction_index, call_depth;
  const Instruction &instruction;
  const ExecutionContext &context;
  const std::map<std::uint32_t, Value> &registers;
  std::uint64_t fuel_before, fuel_after;
  const Value *result{};
  const leanat::Error *error{};
  const Value &argument(std::size_t index) const {
    return registers.at(instruction.args.at(index).id);
  }
};
class InterpreterObserver {
  bool complete_{true};

protected:
  virtual bool on_opcode(const OpcodeObservation &) = 0;

public:
  virtual ~InterpreterObserver() = default;
  void record(const OpcodeObservation &value) noexcept {
    if (!complete_)
      return;
    try {
      if (!on_opcode(value))
        complete_ = false;
    } catch (...) {
      complete_ = false;
    }
  }
  bool complete() const noexcept {
    return complete_;
  }
};
class Interpreter {
  const ValidatedProject &project_;
  std::vector<VersionedCell *> state_;
  InterpreterBackend *backend_{};
  InterpreterObserver *observer_{};
  Expected<SegmentResult> execute_at(std::uint32_t, const ExecutionContext &,
                                     const std::vector<Value> &, EventTxn &, FuelCounter &,
                                     std::optional<std::uint32_t>, std::size_t call_depth = 0,
                                     bool pure = false) const;

public:
  Interpreter(const ValidatedProject &p, std::vector<VersionedCell *> state = {},
              InterpreterBackend *backend = nullptr, InterpreterObserver *observer = nullptr)
      : project_(p), state_(std::move(state)), backend_(backend), observer_(observer) {}
  Expected<SegmentResult> execute_segment(std::uint32_t, const ExecutionContext &,
                                          const std::vector<Value> &, EventTxn &,
                                          FuelCounter &) const;
  Expected<SegmentResult> resume_segment(const SuspensionToken &, const ExecutionContext &,
                                         EventTxn &, FuelCounter &) const;
};
} // namespace leanat::exec

#pragma once
#include "adapter.hpp"
#include <leanat/raw_dmi_services.hpp>
namespace leanat::systemc {
// Native addresses remain private to this host adapter. VM values contain only mapped identity.
class RawDmiServiceHost : public std::enable_shared_from_this<RawDmiServiceHost> {
  RawDmiHost host_;
  std::shared_ptr<Bytes> backing_;
  std::uint64_t base_;
  Duration read_, write_;
  tlm::tlm_dmi last_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> invalidations_;

public:
  RawDmiServiceHost(std::shared_ptr<Bytes> backing, std::uint64_t base, TimeCodec codec,
                    Duration read, Duration write, std::size_t capacity = 128)
      : host_(backing, base, codec, read, write, false, capacity), backing_(std::move(backing)),
        base_(base), read_(read), write_(write) {}
  RawDmiRegion binding(std::uint64_t id, DomainId domain, std::uint64_t owner,
                       std::size_t capacity = 128) {
    auto self = shared_from_this();
    RawDmiRegion r;
    r.id = id;
    r.start = base_;
    r.end = base_ + backing_->size() - 1;
    r.domain = domain;
    r.owner = owner;
    r.read_latency = read_.value;
    r.write_latency = write_.value;
    r.generation = host_.generation();
    r.capacity = capacity;
    r.grant = [self](std::uint64_t address, Command command) -> Expected<void> {
      tlm::tlm_generic_payload payload;
      payload.set_address(address);
      payload.set_command(command == Command::Read ? tlm::TLM_READ_COMMAND
                                                   : tlm::TLM_WRITE_COMMAND);
      auto granted =
          self->host_.get(payload, self->last_,
                          [weak = std::weak_ptr<RawDmiServiceHost>(self)](auto start, auto end) {
                            if (auto live = weak.lock())
                              live->invalidations_.emplace_back(start, end);
                          });
      if (!granted)
        return granted.error();
      if (!granted.value())
        return fail(ErrorCode::ExternalFailure, "native raw grant rejected after logical commit");
      return {};
    };
    r.invalidate = [self](auto start, auto end) { return self->host_.invalidate(start, end); };
    return r;
  }
  const tlm::tlm_dmi &last_grant() const {
    return last_;
  }
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> &invalidations() const {
    return invalidations_;
  }
};
} // namespace leanat::systemc

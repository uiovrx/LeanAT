#pragma once
#include "profile_context.hpp"
namespace conformance::profile {
// Native host/protocol branches only. C-T02 additionally requires the real-GP branch.
Json run_profile_wire(ProfileContext &, const std::string &case_id);
} // namespace conformance::profile

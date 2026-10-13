#pragma once

#include <filesystem>

#include "ipc/protocol.hpp"

namespace opt::ipc {

/// One request/response round trip. Transport errors are returned as Error;
/// daemon-side errors come back as Response{ok=false}.
Result<Response> call(const std::filesystem::path& socket_path, const Request& request,
                      int timeout_ms = 3000);

}  // namespace opt::ipc

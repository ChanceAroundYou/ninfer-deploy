// Online cache release HTTP handlers (isolation v1, 2026-10-14).
// POST /v1/admin/cache/clear -> 202 clear_id (busy surfaces as 409 on poll).
// GET  /v1/admin/cache/clear/CLEAR_ID -> terminal state plus before/after gauges.
// Worker executes at most one clear per boundary under execution_mutex_.
#include "serve/http_server.h"

#include "serve/http_transport.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace ninfer::serve {
namespace {

using Json = nlohmann::json;

std::uint32_t parse_scope_mask(const Json& body) {
  std::uint32_t mask = 0;
  auto it = body.find("scope");
  if (it == body.end() || it->is_null()) {
    return 0x7Fu;
  }
  if (it->is_array()) {
    for (const auto& entry : *it) {
      if (!entry.is_string()) {
        continue;
      }
      const std::string name = entry.get<std::string>();
      if (name == "device_main") {
        mask |= 1u << 0;
      } else if (name == "backend") {
        mask |= 1u << 1;
      } else if (name == "host_kv") {
        mask |= 1u << 2;
      } else if (name == "host_state") {
        mask |= 1u << 3;
      } else if (name == "shared_prefix") {
        mask |= 1u << 4;
      } else if (name == "continuation") {
        mask |= 1u << 5;
      } else if (name == "anchors") {
        mask |= 1u << 6;
      } else if (name == "all") {
        mask |= 0x7Fu;
      }
    }
    return mask == 0 ? 0x7Fu : mask;
  }
  return 0x7Fu;
}

std::uint8_t parse_mode(const Json& body) {
  auto it = body.find("mode");
  if (it == body.end() || !it->is_string()) {
    return 0;
  }
  const std::string mode = it->get<std::string>();
  if (mode == "drain_and_fail") {
    return 1;
  }
  if (mode == "abort_and_clear") {
    return 2;
  }
  return 0;
}

const char* state_name(std::uint8_t state) noexcept {
  switch (state) {
    case 1:
      return "running";
    case 2:
      return "done";
    case 3:
      return "refused_busy";
    case 4:
      return "failed";
    default:
      return "queued";
  }
}

}  // namespace

void HttpServer::handle_cache_clear(const httplib::Request& req, httplib::Response& res) {
  Json body = Json::object();
  if (!req.body.empty()) {
    try {
      body = Json::parse(req.body);
    } catch (...) {
      ApiError error;
      error.status = 400;
      error.type = "invalid_request_error";
      error.code = "invalid_json";
      error.message = "request body is not valid JSON";
      write_openai_error(res, error);
      return;
    }
  }
  const std::uint32_t scope_mask = parse_scope_mask(body);
  const std::uint8_t mode = parse_mode(body);
  const bool dry_run = body.value("dry_run", false);
  if (mode > 2) {
    ApiError error;
    error.status = 400;
    error.type = "invalid_request_error";
    error.code = "invalid_mode";
    error.message = "mode must be try_idle, drain_and_fail or abort_and_clear";
    write_openai_error(res, error);
    return;
  }
  // Scope honesty (batch-a): every mask bit is served by Program::scoped_release;
  // All (0x7F) keeps the fail_all_cleanup path. Empty scope defaults to All above.
  if (scope_mask == 0 || (scope_mask & ~0x7Fu) != 0) {
    ApiError error;
    error.status = 400;
    error.type = "invalid_request_error";
    error.code = "invalid_scope";
    error.message = "scope must be a non-empty subset of [device_main, backend, host_kv,"
                    " host_state, shared_prefix, continuation, anchors, all]";
    write_openai_error(res, error);
    return;
  }
  std::uint64_t clear_id = 0;
  try {
    clear_id = service_->submit_cache_clear(scope_mask, mode, dry_run, "http-admin");
  } catch (const std::exception& ex) {
    ApiError error;
    error.status = 500;
    error.type = "internal_error";
    error.message = ex.what();
    write_openai_error(res, error);
    return;
  }
  res.status = 202;
  res.set_content(Json{{"clear_id", clear_id}, {"state", "queued"}}.dump(), "application/json");
}

void HttpServer::handle_cache_clear_get(const httplib::Request& req, httplib::Response& res) {
  const std::string raw = req.matches.size() > 1 ? req.matches[1].str() : std::string();
  std::uint64_t clear_id = 0;
  try {
    std::size_t pos = 0;
    clear_id = std::stoull(raw, &pos);
    if (pos != raw.size()) {
      throw std::invalid_argument("trailing");
    }
  } catch (...) {
    ApiError error;
    error.status = 404;
    error.type = "invalid_request_error";
    error.code = "not_found";
    error.message = "unknown cache-clear id";
    write_openai_error(res, error);
    return;
  }
  ninfer::Engine::CacheClearStatus status;
  if (!service_->cache_clear_status(clear_id, status)) {
    ApiError error;
    error.status = 404;
    error.type = "invalid_request_error";
    error.code = "not_found";
    error.message = "unknown cache-clear id";
    write_openai_error(res, error);
    return;
  }
  if (status.state == 3) {
    ApiError error;
    error.status = 409;
    error.type = "invalid_request_error";
    error.code = "busy";
    error.message = "engine busy; retry when idle (in_flight=" +
                    std::to_string(status.in_flight) + ")";
    write_openai_error(res, error);
    return;
  }
  res.set_content(Json{{"clear_id", status.id},
                       {"state", state_name(status.state)},
                       {"detail", status.detail},
                       {"in_flight", status.in_flight},
                       {"materializing", status.materializing},
                       {"has_transaction", status.has_transaction},
                       {"failed_requests", status.failed_requests},
                       {"kv_tokens_before", status.kv_tokens_before},
                       {"kv_tokens_after", status.kv_tokens_after},
                       {"host_bytes_before", status.host_bytes_before},
                       {"host_bytes_after", status.host_bytes_after},
                       {"duration_ms", status.duration_ms}}
                      .dump(),
                  "application/json");
}

}  // namespace ninfer::serve

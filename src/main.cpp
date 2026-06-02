// Copyright (c) 2025 [caomengxuan666]

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cxxmcp/client.hpp"
#include "cxxmcp/gateway/config_io.hpp"
#include "cxxmcp/gateway/runtime.hpp"
#include "cxxmcp/peer.hpp"
#include "cxxmcp/protocol/tool.hpp"
#include "cxxmcp/protocol/types.hpp"
#include "cxxmcp/server/auth.hpp"
#include "cxxmcp/server/authoring.hpp"
#include "cxxmcp/server/rate_limit.hpp"
#include "cxxmcp/service.hpp"

#ifndef CXXMCP_GATEWAYD_VERSION
#define CXXMCP_GATEWAYD_VERSION "0.0.0"
#endif

namespace {

using Json = mcp::protocol::Json;
using ToolResult = mcp::protocol::ToolResult;

std::atomic_bool g_stop_requested{false};

void request_stop(int) { g_stop_requested.store(true); }

struct AdminEndpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 39932;
  std::string path = "/admin";
};

struct SecurityConfig {
  bool allow_non_loopback = false;
  std::vector<mcp::gateway::BearerTokenAuthEntry> bearer_tokens;
  mcp::gateway::FixedWindowRateLimit rate_limit;
};

struct ProfileSpec {
  std::string id = "default";
  mcp::gateway::HttpEndpoint endpoint;
  mcp::gateway::GatewayConfig config;
  mcp::gateway::GatewayRuntimeConfig runtime_config;
};

struct ProfileRuntime {
  ProfileSpec spec;
  std::unique_ptr<mcp::gateway::GatewayRuntime> runtime;
};

struct GatewaydState {
  AdminEndpoint admin;
  SecurityConfig security;
  std::string config_path;
  std::uint64_t next_event_id = 1;
  std::weak_ptr<GatewaydState> self;
  mutable std::recursive_mutex mutex;
  std::vector<std::shared_ptr<ProfileRuntime>> profiles;
  std::vector<Json> events;
};

struct LoadedGatewaydConfig {
  AdminEndpoint admin;
  SecurityConfig security;
  std::vector<ProfileSpec> profiles;
};

void print_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  cxxmcp-gatewayd --help\n"
      << "  cxxmcp-gatewayd --version\n"
      << "  cxxmcp-gatewayd validate [--config <file>]\n"
      << "  cxxmcp-gatewayd run [--config <file>]\n"
      << "  cxxmcp-gatewayd status [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd profiles [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd upstreams [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd catalog <tools|resources|prompts> "
         "[--admin-url <url>] [--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd dashboard [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd dashboard --html <file> [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd events [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd diagnostics [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd reload [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd upstream enable <profile> <upstream> "
         "[--admin-url <url>] [--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd upstream disable <profile> <upstream> "
         "[--admin-url <url>] [--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd profile restart <profile> "
         "[--admin-url <url>] [--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd profile runtime set <profile> "
         "[--session-mode <per-call|persistent>] [--pool-size <n>] "
         "[--prewarm <true|false>] [--restart] [--admin-url <url>] "
         "[--bearer-token <token>]\n"
      << "  cxxmcp-gatewayd --config <file>   # legacy alias for run\n\n"
      << "Config discovery without --config:\n"
      << "  1. CXXMCP_GATEWAYD_CONFIG\n"
      << "  2. ./gatewayd.json\n"
      << "  3. ./gatewayd.config.json\n\n"
      << "Admin CLI token discovery:\n"
      << "  1. --bearer-token <token>\n"
      << "  2. CXXMCP_GATEWAYD_ADMIN_TOKEN\n\n"
      << "Config shape:\n"
      << "  {\n"
      << "    \"admin\": {\"host\":\"127.0.0.1\", \"port\":39932, "
         "\"path\":\"/admin\"},\n"
      << "    \"profiles\": [\n"
      << "      {\"id\":\"default\", \"endpoint\":{\"port\":39931, "
         "\"path\":\"/mcp/default\"}, \"upstreams\":[...]}\n"
      << "    ]\n"
      << "  }\n";
}

std::string read_text_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void write_text_file(const std::string& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to open " + path + " for writing");
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!output) {
    throw std::runtime_error("failed to write " + path);
  }
}

bool readable_file(std::string_view path) {
  std::ifstream input(std::string(path), std::ios::binary);
  return static_cast<bool>(input);
}

std::optional<std::string> discover_config_path() {
  if (const char* env = std::getenv("CXXMCP_GATEWAYD_CONFIG");
      env != nullptr && *env != '\0') {
    return std::string(env);
  }
  for (std::string_view candidate : {"gatewayd.json", "gatewayd.config.json"}) {
    if (readable_file(candidate)) {
      return std::string(candidate);
    }
  }
  return std::nullopt;
}

std::optional<std::string> resolve_environment(std::string_view name) {
  const auto key = std::string(name);
  const char* value = std::getenv(key.c_str());
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

std::int64_t unix_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void record_event(GatewaydState& state,
                  std::string type,
                  Json detail = Json::object()) {
  Json event{
      {"id", state.next_event_id++},
      {"unixMs", unix_time_ms()},
      {"type", std::move(type)},
      {"detail", std::move(detail)},
  };
  state.events.push_back(std::move(event));
  if (state.events.size() > 256) {
    state.events.erase(state.events.begin(),
                       state.events.begin() +
                           static_cast<std::ptrdiff_t>(state.events.size() -
                                                       256));
  }
}

std::optional<std::string> require_string(const Json& object,
                                          std::string_view key,
                                          std::string_view path) {
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->is_string()) {
    return std::string(path) + "." + std::string(key) +
           " must be a string";
  }
  return std::nullopt;
}

std::string http_url(const mcp::gateway::HttpEndpoint& endpoint) {
  return "http://" + endpoint.host + ":" + std::to_string(endpoint.port) +
         endpoint.path;
}

std::string admin_url(const AdminEndpoint& endpoint) {
  return "http://" + endpoint.host + ":" + std::to_string(endpoint.port) +
         endpoint.path;
}

bool is_loopback_host(std::string_view host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
         host == "[::1]";
}

std::unique_ptr<mcp::server::StaticBearerAuthProvider> make_auth_provider(
    const std::vector<mcp::gateway::BearerTokenAuthEntry>& entries) {
  auto auth = std::make_unique<mcp::server::StaticBearerAuthProvider>();
  for (const auto& entry : entries) {
    auth->add_token(entry.token,
                    mcp::server::AuthIdentity{
                        .subject = entry.subject,
                        .claims = {{"service", "cxxmcp-gatewayd"}},
                    });
  }
  return auth;
}

class FixedWindowRateLimiter final : public mcp::server::RateLimiter {
 public:
  explicit FixedWindowRateLimiter(mcp::gateway::FixedWindowRateLimit config)
      : requests_per_window_(config.requests_per_window),
        window_(config.window.count() > 0 ? config.window
                                          : std::chrono::milliseconds{1000}) {}

  mcp::core::Result<mcp::server::RateLimitDecision> check(
      const mcp::server::RateLimitRequest& /*request*/) override {
    if (requests_per_window_ == 0) {
      return mcp::server::RateLimitDecision{};
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    if (window_start_.time_since_epoch().count() == 0 ||
        now - window_start_ >= window_) {
      window_start_ = now;
      count_ = 0;
    }

    if (count_ >= requests_per_window_) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - window_start_);
      const auto retry_after = elapsed >= window_
                                   ? std::chrono::milliseconds{0}
                                   : window_ - elapsed;
      return mcp::server::RateLimitDecision{.allowed = false,
                                            .retry_after = retry_after};
    }

    ++count_;
    return mcp::server::RateLimitDecision{};
  }

 private:
  std::size_t requests_per_window_ = 0;
  std::chrono::milliseconds window_{1000};
  std::mutex mutex_;
  std::chrono::steady_clock::time_point window_start_;
  std::size_t count_ = 0;
};

std::unique_ptr<mcp::server::RateLimiter> make_rate_limiter(
    mcp::gateway::FixedWindowRateLimit config) {
  return std::make_unique<FixedWindowRateLimiter>(config);
}

std::optional<mcp::protocol::JsonRpcResponse> rate_limited_response(
    mcp::server::RateLimiter& limiter,
    const mcp::protocol::JsonRpcRequest& request) {
  mcp::server::RateLimitRequest rate_request;
  rate_request.method = request.method;
  const auto decision = limiter.check(rate_request);
  if (!decision) {
    return mcp::protocol::make_error_response(
        request.id,
        mcp::protocol::make_error(
            static_cast<int>(mcp::protocol::ErrorCode::RateLimited),
            "rate limiting failed",
            decision.error().message.empty()
                ? std::nullopt
                : std::optional<mcp::protocol::Json>{
                      decision.error().message}));
  }
  if (!decision->allowed) {
    return mcp::protocol::make_error_response(
        request.id,
        mcp::protocol::make_error(
            static_cast<int>(mcp::protocol::ErrorCode::RateLimited),
            "request rate limited"));
  }
  return std::nullopt;
}

mcp::core::Result<SecurityConfig> parse_security_config(const Json& root) {
  SecurityConfig security;
  const auto item = root.find("security");
  if (item == root.end()) {
    return security;
  }
  if (!item->is_object()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "security must be an object", "", "gatewayd.config"});
  }

  if (const auto allow = item->find("allowNonLoopback");
      allow != item->end()) {
    if (!allow->is_boolean()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security.allowNonLoopback must be a boolean", "",
          "gatewayd.config"});
    }
    security.allow_non_loopback = allow->get<bool>();
  }

  if (const auto rate_limit = item->find("rateLimit");
      rate_limit != item->end()) {
    if (!rate_limit->is_object()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security.rateLimit must be an object", "", "gatewayd.config"});
    }
    const auto requests = rate_limit->find("requestsPerWindow");
    if (requests == rate_limit->end() || !requests->is_number_unsigned()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security.rateLimit.requestsPerWindow must be a positive integer",
          "", "gatewayd.config"});
    }
    const auto request_count = requests->get<std::size_t>();
    if (request_count == 0) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security.rateLimit.requestsPerWindow must be positive", "",
          "gatewayd.config"});
    }
    security.rate_limit.requests_per_window = request_count;

    if (const auto window = rate_limit->find("windowMs");
        window != rate_limit->end()) {
      if (!window->is_number_unsigned()) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security.rateLimit.windowMs must be a positive integer", "",
            "gatewayd.config"});
      }
      const auto window_ms = window->get<std::uint64_t>();
      if (window_ms == 0) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security.rateLimit.windowMs must be positive", "",
            "gatewayd.config"});
      }
      security.rate_limit.window =
          std::chrono::milliseconds{static_cast<std::int64_t>(window_ms)};
    }
  }

  const auto tokens = item->find("bearerTokens");
  if (tokens == item->end()) {
    return security;
  }
  if (!tokens->is_array()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "security.bearerTokens must be an array", "", "gatewayd.config"});
  }

  for (std::size_t i = 0; i < tokens->size(); ++i) {
    const auto& token_item = (*tokens)[i];
    const auto path =
        "security.bearerTokens[" + std::to_string(i) + "]";
    if (!token_item.is_object()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security bearer token entry must be an object", path,
          "gatewayd.config"});
    }

    std::optional<std::string> token;
    if (const auto value = token_item.find("token");
        value != token_item.end()) {
      if (!value->is_string()) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security bearer token must be a string", path + ".token",
            "gatewayd.config"});
      }
      token = value->get<std::string>();
    }
    if (const auto env = token_item.find("tokenEnv");
        env != token_item.end()) {
      if (!env->is_string()) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security bearer tokenEnv must be a string", path + ".tokenEnv",
            "gatewayd.config"});
      }
      const auto env_name = env->get<std::string>();
      auto env_value = resolve_environment(env_name);
      if (!env_value.has_value() || env_value->empty()) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security bearer tokenEnv is not set", env_name,
            "gatewayd.config"});
      }
      token = std::move(*env_value);
    }
    if (!token.has_value() || token->empty()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security bearer token entry requires token or tokenEnv", path,
          "gatewayd.config"});
    }

    std::string subject = "gatewayd-user";
    if (const auto subject_value = token_item.find("subject");
        subject_value != token_item.end()) {
      if (!subject_value->is_string()) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "security bearer token subject must be a string",
            path + ".subject", "gatewayd.config"});
      }
      subject = subject_value->get<std::string>();
    }
    if (subject.empty()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "security bearer token subject must not be empty", path + ".subject",
          "gatewayd.config"});
    }

    security.bearer_tokens.push_back(
        mcp::gateway::BearerTokenAuthEntry{.token = std::move(*token),
                                           .subject = std::move(subject)});
  }

  return security;
}

std::string status_to_string(mcp::gateway::UpstreamRuntimeStatus status) {
  switch (status) {
    case mcp::gateway::UpstreamRuntimeStatus::configured:
      return "configured";
    case mcp::gateway::UpstreamRuntimeStatus::connecting:
      return "connecting";
    case mcp::gateway::UpstreamRuntimeStatus::initialized:
      return "initialized";
    case mcp::gateway::UpstreamRuntimeStatus::healthy:
      return "healthy";
    case mcp::gateway::UpstreamRuntimeStatus::degraded:
      return "degraded";
    case mcp::gateway::UpstreamRuntimeStatus::stopping:
      return "stopping";
    case mcp::gateway::UpstreamRuntimeStatus::stopped:
      return "stopped";
  }
  return "unknown";
}

std::string transport_to_string(
    mcp::gateway::UpstreamTransportKind transport) {
  switch (transport) {
    case mcp::gateway::UpstreamTransportKind::process_stdio:
      return "stdio";
    case mcp::gateway::UpstreamTransportKind::streamable_http:
      return "http";
  }
  return "unknown";
}

std::string session_mode_to_string(mcp::gateway::UpstreamSessionMode mode) {
  switch (mode) {
    case mcp::gateway::UpstreamSessionMode::per_call:
      return "per-call";
    case mcp::gateway::UpstreamSessionMode::persistent:
      return "persistent";
  }
  return "per-call";
}

Json runtime_config_json(const mcp::gateway::GatewayRuntimeConfig& runtime) {
  return Json{
      {"upstreamSessionMode", session_mode_to_string(runtime.upstream_session_mode)},
      {"persistentSessionPoolSize", runtime.persistent_session_pool_size},
      {"persistentSessionAcquireTimeoutMs",
       runtime.persistent_session_acquire_timeout.count()},
      {"activeCallDrainTimeoutMs", runtime.active_call_drain_timeout.count()},
      {"prewarmCapabilities", runtime.prewarm_capabilities},
  };
}

Json state_health_json(const GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"mcpUrl", http_url(profile->spec.endpoint)},
        {"upstreamCount", profile->spec.config.upstreams.size()},
    });
  }
  return Json{
      {"status", "ok"},
      {"adminUrl", admin_url(state.admin)},
      {"configPath", state.config_path},
      {"security",
       Json{{"bearerAuthEnabled", !state.security.bearer_tokens.empty()},
            {"rateLimitEnabled",
             state.security.rate_limit.requests_per_window > 0}}},
      {"profiles", std::move(profiles)},
  };
}

Json state_events_json(const GatewaydState& state) {
  return Json{{"events", state.events}};
}

Json state_upstreams_json(const GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    Json configured = Json::array();
    for (const auto& upstream : profile->spec.config.upstreams) {
      configured.push_back(Json{
          {"id", upstream.id},
          {"enabled", upstream.enabled},
          {"transport", transport_to_string(upstream.transport)},
      });
    }

    Json runtime_states = Json::array();
    if (profile->runtime) {
      for (const auto& runtime_state : profile->runtime->upstream_states()) {
        Json item{
            {"id", runtime_state.upstream_id},
            {"status", status_to_string(runtime_state.status)},
            {"activeCalls", runtime_state.active_calls},
            {"persistentSessionPoolSize",
             runtime_state.persistent_session_pool_size},
            {"initializedPersistentSessions",
             runtime_state.initialized_persistent_sessions},
            {"busyPersistentSessions", runtime_state.busy_persistent_sessions},
        };
        if (runtime_state.last_error.has_value()) {
          item["lastError"] = Json{
              {"code", runtime_state.last_error->code},
              {"message", runtime_state.last_error->message},
              {"detail", runtime_state.last_error->detail},
              {"category", runtime_state.last_error->category},
          };
        }
        runtime_states.push_back(std::move(item));
      }
    }

    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"configured", std::move(configured)},
        {"runtime", std::move(runtime_states)},
    });
  }
  return Json{{"profiles", std::move(profiles)}};
}

Json state_profiles_json(const GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    Json upstreams = Json::array();
    for (const auto& upstream : profile->spec.config.upstreams) {
      upstreams.push_back(Json{
          {"id", upstream.id},
          {"displayName", upstream.display_name},
          {"enabled", upstream.enabled},
          {"transport", transport_to_string(upstream.transport)},
      });
    }
    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"endpoint",
         Json{{"host", profile->spec.endpoint.host},
              {"port", profile->spec.endpoint.port},
              {"path", profile->spec.endpoint.path},
              {"url", http_url(profile->spec.endpoint)}}},
        {"runtime",
         runtime_config_json(profile->spec.runtime_config)},
        {"upstreams", std::move(upstreams)},
    });
  }
  return Json{{"profiles", std::move(profiles)}};
}

Json state_catalog_tools_json(GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    Json tools = Json::array();
    if (!profile->runtime) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", static_cast<int>(mcp::protocol::ErrorCode::InternalError)},
                {"message", "profile runtime is not running"},
                {"detail", ""},
                {"category", "gatewayd.runtime"}}},
      });
      continue;
    }
    auto listed = profile->runtime->list_tools();
    if (!listed) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", listed.error().code},
                {"message", listed.error().message},
                {"detail", listed.error().detail},
                {"category", listed.error().category}}},
      });
      continue;
    }
    for (const auto& tool : *listed) {
      tools.push_back(Json{
          {"name", tool.name},
          {"title", tool.title},
          {"description", tool.description},
      });
    }
    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"tools", std::move(tools)},
    });
  }
  return Json{{"profiles", std::move(profiles)}};
}

Json state_catalog_resources_json(GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    Json resources = Json::array();
    if (!profile->runtime) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", static_cast<int>(mcp::protocol::ErrorCode::InternalError)},
                {"message", "profile runtime is not running"},
                {"detail", ""},
                {"category", "gatewayd.runtime"}}},
      });
      continue;
    }
    auto listed = profile->runtime->list_resources();
    if (!listed) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", listed.error().code},
                {"message", listed.error().message},
                {"detail", listed.error().detail},
                {"category", listed.error().category}}},
      });
      continue;
    }
    for (const auto& resource : *listed) {
      Json item{
          {"uri", resource.uri},
          {"name", resource.name},
          {"title", resource.title},
          {"description", resource.description},
          {"mimeType", resource.mime_type},
      };
      if (resource.size.has_value()) {
        item["size"] = *resource.size;
      }
      resources.push_back(std::move(item));
    }
    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"resources", std::move(resources)},
    });
  }
  return Json{{"profiles", std::move(profiles)}};
}

Json state_catalog_prompts_json(GatewaydState& state) {
  Json profiles = Json::array();
  for (const auto& profile : state.profiles) {
    Json prompts = Json::array();
    if (!profile->runtime) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", static_cast<int>(mcp::protocol::ErrorCode::InternalError)},
                {"message", "profile runtime is not running"},
                {"detail", ""},
                {"category", "gatewayd.runtime"}}},
      });
      continue;
    }
    auto listed = profile->runtime->list_prompts();
    if (!listed) {
      profiles.push_back(Json{
          {"id", profile->spec.id},
          {"error",
           Json{{"code", listed.error().code},
                {"message", listed.error().message},
                {"detail", listed.error().detail},
                {"category", listed.error().category}}},
      });
      continue;
    }
    for (const auto& prompt : *listed) {
      Json arguments = Json::array();
      for (const auto& argument : prompt.arguments) {
        arguments.push_back(Json{
            {"name", argument.name},
            {"description", argument.description},
            {"required", argument.required},
        });
      }
      prompts.push_back(Json{
          {"name", prompt.name},
          {"title", prompt.title},
          {"description", prompt.description},
          {"arguments", std::move(arguments)},
      });
    }
    profiles.push_back(Json{
        {"id", profile->spec.id},
        {"prompts", std::move(prompts)},
    });
  }
  return Json{{"profiles", std::move(profiles)}};
}

Json error_json(const mcp::core::Error& error) {
  return Json{
      {"ok", false},
      {"error",
       Json{{"code", error.code},
            {"message", error.message},
            {"detail", error.detail},
            {"category", error.category}}},
  };
}

Json usage_error_json(std::string message, std::string detail = {}) {
  return Json{
      {"ok", false},
      {"error",
       Json{{"code", static_cast<int>(mcp::protocol::ErrorCode::InvalidParams)},
            {"message", std::move(message)},
            {"detail", std::move(detail)},
      {"category", "gatewayd.admin"}}},
  };
}

std::string runtime_event_type(
    mcp::gateway::GatewayRuntimeEventKind kind) {
  switch (kind) {
    case mcp::gateway::GatewayRuntimeEventKind::upstream_status_changed:
      return "runtime.upstream_status_changed";
    case mcp::gateway::GatewayRuntimeEventKind::runtime_stopping:
      return "runtime.stopping";
    case mcp::gateway::GatewayRuntimeEventKind::runtime_stopped:
      return "runtime.stopped";
    case mcp::gateway::GatewayRuntimeEventKind::tools_listed:
      return "data.tools_listed";
    case mcp::gateway::GatewayRuntimeEventKind::tool_called:
      return "data.tool_called";
    case mcp::gateway::GatewayRuntimeEventKind::tool_denied:
      return "data.tool_denied";
    case mcp::gateway::GatewayRuntimeEventKind::resource_denied:
      return "data.resource_denied";
    case mcp::gateway::GatewayRuntimeEventKind::prompt_denied:
      return "data.prompt_denied";
  }
  return "runtime.unknown";
}

mcp::gateway::GatewayRuntimeObserver make_runtime_observer(
    std::weak_ptr<GatewaydState> weak_state,
    std::string profile_id) {
  return [weak_state = std::move(weak_state),
          profile_id = std::move(profile_id)](
             const mcp::gateway::GatewayRuntimeEvent& event) {
    auto state = weak_state.lock();
    if (!state) {
      return;
    }

    Json detail{{"profile", profile_id}};
    if (!event.upstream_id.empty()) {
      detail["upstream"] = event.upstream_id;
    }
    if (!event.method.empty()) {
      detail["method"] = event.method;
    }
    if (!event.exposed_name.empty()) {
      detail["exposedName"] = event.exposed_name;
    }
    if (event.item_count != 0) {
      detail["itemCount"] = event.item_count;
    }
    if (event.error.has_value()) {
      detail["error"] = error_json(*event.error)["error"];
    }

    std::lock_guard lock(state->mutex);
    record_event(*state, runtime_event_type(event.kind), std::move(detail));
  };
}

std::shared_ptr<ProfileRuntime> find_profile(GatewaydState& state,
                                             std::string_view profile_id) {
  for (auto& profile : state.profiles) {
    if (profile->spec.id == profile_id) {
      return profile;
    }
  }
  return {};
}

mcp::core::Result<std::shared_ptr<ProfileRuntime>> start_profile(
    ProfileSpec spec,
    std::weak_ptr<GatewaydState> state = {}) {
  auto observer = make_runtime_observer(state, spec.id);
  auto runtime_options =
      mcp::gateway::make_gateway_runtime_options(spec.runtime_config,
                                                 std::move(observer));
  if (!runtime_options) {
    return mcp::core::unexpected(runtime_options.error());
  }

  auto profile = std::make_shared<ProfileRuntime>();
  profile->spec = std::move(spec);
  auto next = std::make_unique<mcp::gateway::GatewayRuntime>(
      profile->spec.config, std::move(*runtime_options));
  if (profile->spec.runtime_config.prewarm_capabilities) {
    auto refreshed = next->refresh_upstream_capabilities();
    if (!refreshed) {
      return mcp::core::unexpected(refreshed.error());
    }
  }
  auto started = next->start_http(profile->spec.endpoint);
  if (!started) {
    return mcp::core::unexpected(started.error());
  }
  profile->runtime = std::move(next);
  return profile;
}

mcp::core::Result<std::vector<std::shared_ptr<ProfileRuntime>>> start_profiles(
    std::vector<ProfileSpec> specs,
    std::weak_ptr<GatewaydState> state = {}) {
  std::vector<std::shared_ptr<ProfileRuntime>> profiles;
  for (auto& spec : specs) {
    auto profile = start_profile(std::move(spec), state);
    if (!profile) {
      for (auto& started : profiles) {
        if (started->runtime) {
          (void)started->runtime->stop();
        }
      }
      return mcp::core::unexpected(profile.error());
    }
    profiles.push_back(std::move(*profile));
  }
  return profiles;
}

void stop_profiles(
    const std::vector<std::shared_ptr<ProfileRuntime>>& profiles) noexcept {
  for (const auto& profile : profiles) {
    if (profile && profile->runtime) {
      (void)profile->runtime->stop();
      profile->runtime.reset();
    }
  }
}

mcp::core::Result<mcp::core::Unit> restart_profile(
    ProfileRuntime& profile,
    std::weak_ptr<GatewaydState> state = {}) {
  if (profile.runtime) {
    (void)profile.runtime->stop();
    profile.runtime.reset();
  }

  auto restarted = start_profile(profile.spec, state);
  if (!restarted) {
    return mcp::core::unexpected(restarted.error());
  }
  profile.runtime = std::move((*restarted)->runtime);
  return mcp::core::Unit{};
}

Json restart_profile_json(GatewaydState& state, const Json& args) {
  if (!args.is_object()) {
    return usage_error_json("gatewayd.profile.restart expects an object");
  }
  const auto profile_arg = args.find("profile");
  if (profile_arg == args.end() || !profile_arg->is_string()) {
    return usage_error_json("missing profile", "expected string field profile");
  }

  std::lock_guard lock(state.mutex);
  auto profile = find_profile(state, profile_arg->get<std::string>());
  if (!profile) {
    return usage_error_json("unknown profile", profile_arg->get<std::string>());
  }

  auto restarted = restart_profile(*profile, state.self);
  if (!restarted) {
    record_event(state, "profile.restart.failed",
                 Json{{"profile", profile->spec.id},
                      {"error", error_json(restarted.error())["error"]}});
    return error_json(restarted.error());
  }
  record_event(state, "profile.restarted",
               Json{{"profile", profile->spec.id},
                    {"mcpUrl", http_url(profile->spec.endpoint)}});
  return Json{
      {"ok", true},
      {"profile", profile->spec.id},
      {"mcpUrl", http_url(profile->spec.endpoint)},
  };
}

mcp::core::Result<mcp::core::Unit> persist_profile_runtime(
    const std::string& path,
    const std::string& profile_id,
    const mcp::gateway::GatewayRuntimeConfig& runtime) {
  Json root;
  try {
    root = Json::parse(read_text_file(path));
  } catch (const std::exception& ex) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::ParseError),
        "failed to parse gatewayd config for update", ex.what(),
        "gatewayd.config"});
  }

  auto profiles = root.find("profiles");
  if (profiles == root.end() || !profiles->is_array()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "gatewayd config requires a profiles array", "",
        "gatewayd.config"});
  }

  for (auto& profile : *profiles) {
    const auto id = profile.find("id");
    if (id == profile.end() || !id->is_string() ||
        id->get<std::string>() != profile_id) {
      continue;
    }
    profile["runtime"] = runtime_config_json(runtime);
    try {
      write_text_file(path, root.dump(2) + "\n");
    } catch (const std::exception& ex) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InternalError),
          "failed to write gatewayd config", ex.what(),
          "gatewayd.config"});
    }
    return mcp::core::Unit{};
  }

  return mcp::core::unexpected(mcp::core::Error{
      static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
      "unknown profile in config", profile_id, "gatewayd.config"});
}

Json set_profile_runtime_json(GatewaydState& state, const Json& args) {
  if (!args.is_object()) {
    return usage_error_json("gatewayd.profile.runtime.set expects an object");
  }
  const auto profile_arg = args.find("profile");
  if (profile_arg == args.end() || !profile_arg->is_string()) {
    return usage_error_json("missing profile", "expected string field profile");
  }

  std::lock_guard lock(state.mutex);
  auto profile = find_profile(state, profile_arg->get<std::string>());
  if (!profile) {
    return usage_error_json("unknown profile", profile_arg->get<std::string>());
  }
  if (state.config_path.empty()) {
    return usage_error_json("missing config path",
                            "persistent updates require a loaded config file");
  }

  auto next = profile->spec.runtime_config;
  if (const auto mode = args.find("upstreamSessionMode");
      mode != args.end()) {
    if (!mode->is_string()) {
      return usage_error_json("invalid upstreamSessionMode",
                              "expected string");
    }
    const auto value = mode->get<std::string>();
    if (value == "per_call" || value == "per-call") {
      next.upstream_session_mode = mcp::gateway::UpstreamSessionMode::per_call;
    } else if (value == "persistent") {
      next.upstream_session_mode =
          mcp::gateway::UpstreamSessionMode::persistent;
    } else {
      return usage_error_json("invalid upstreamSessionMode",
                              "expected per-call or persistent");
    }
  }
  if (const auto pool = args.find("persistentSessionPoolSize");
      pool != args.end()) {
    if (!pool->is_number_unsigned() || pool->get<std::size_t>() == 0) {
      return usage_error_json("invalid persistentSessionPoolSize",
                              "expected positive integer");
    }
    next.persistent_session_pool_size = pool->get<std::size_t>();
  }
  if (const auto acquire = args.find("persistentSessionAcquireTimeoutMs");
      acquire != args.end()) {
    if (!acquire->is_number_unsigned()) {
      return usage_error_json("invalid persistentSessionAcquireTimeoutMs",
                              "expected non-negative integer");
    }
    next.persistent_session_acquire_timeout =
        std::chrono::milliseconds{acquire->get<std::int64_t>()};
  }
  if (const auto drain = args.find("activeCallDrainTimeoutMs");
      drain != args.end()) {
    if (!drain->is_number_unsigned()) {
      return usage_error_json("invalid activeCallDrainTimeoutMs",
                              "expected non-negative integer");
    }
    next.active_call_drain_timeout =
        std::chrono::milliseconds{drain->get<std::int64_t>()};
  }
  if (const auto prewarm = args.find("prewarmCapabilities");
      prewarm != args.end()) {
    if (!prewarm->is_boolean()) {
      return usage_error_json("invalid prewarmCapabilities",
                              "expected boolean");
    }
    next.prewarm_capabilities = prewarm->get<bool>();
  }

  auto valid = mcp::gateway::validate_gateway_runtime_config(next);
  if (!valid) {
    return error_json(valid.error());
  }

  auto persisted = persist_profile_runtime(state.config_path, profile->spec.id,
                                           next);
  if (!persisted) {
    record_event(state, "profile.runtime.update.failed",
                 Json{{"profile", profile->spec.id},
                      {"error", error_json(persisted.error())["error"]}});
    return error_json(persisted.error());
  }
  profile->spec.runtime_config = next;
  record_event(state, "profile.runtime.updated",
               Json{{"profile", profile->spec.id},
                    {"runtime", runtime_config_json(profile->spec.runtime_config)},
                    {"persisted", true}});
  return Json{
      {"ok", true},
      {"profile", profile->spec.id},
      {"runtime", runtime_config_json(profile->spec.runtime_config)},
      {"persisted", true},
      {"runtimeRestartRequired", true},
  };
}

mcp::core::Result<mcp::core::Unit> persist_upstream_enabled(
    const std::string& path,
    const std::string& profile_id,
    const std::string& upstream_id,
    bool enabled) {
  Json root;
  try {
    root = Json::parse(read_text_file(path));
  } catch (const std::exception& ex) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::ParseError),
        "failed to parse gatewayd config for update", ex.what(),
        "gatewayd.config"});
  }

  auto profiles = root.find("profiles");
  if (profiles == root.end() || !profiles->is_array()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "gatewayd config requires a profiles array", "",
        "gatewayd.config"});
  }

  for (auto& profile : *profiles) {
    const auto id = profile.find("id");
    if (id == profile.end() || !id->is_string() ||
        id->get<std::string>() != profile_id) {
      continue;
    }
    auto upstreams = profile.find("upstreams");
    if (upstreams == profile.end() || !upstreams->is_array()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "gatewayd profile requires an upstreams array", profile_id,
          "gatewayd.config"});
    }
    for (auto& upstream : *upstreams) {
      const auto upstream_json_id = upstream.find("id");
      if (upstream_json_id == upstream.end() ||
          !upstream_json_id->is_string() ||
          upstream_json_id->get<std::string>() != upstream_id) {
        continue;
      }
      upstream["enabled"] = enabled;
      try {
        write_text_file(path, root.dump(2) + "\n");
      } catch (const std::exception& ex) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InternalError),
            "failed to write gatewayd config", ex.what(),
            "gatewayd.config"});
      }
      return mcp::core::Unit{};
    }
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "unknown upstream in config", upstream_id, "gatewayd.config"});
  }

  return mcp::core::unexpected(mcp::core::Error{
      static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
      "unknown profile in config", profile_id, "gatewayd.config"});
}

Json set_upstream_enabled_json(GatewaydState& state,
                               const Json& args,
                               bool enabled) {
  if (!args.is_object()) {
    return usage_error_json("gatewayd.upstream enable/disable expects an object");
  }
  const auto profile_arg = args.find("profile");
  const auto upstream_arg = args.find("upstream");
  if (profile_arg == args.end() || !profile_arg->is_string()) {
    return usage_error_json("missing profile", "expected string field profile");
  }
  if (upstream_arg == args.end() || !upstream_arg->is_string()) {
    return usage_error_json("missing upstream", "expected string field upstream");
  }

  std::lock_guard lock(state.mutex);
  auto profile = find_profile(state, profile_arg->get<std::string>());
  if (!profile) {
    return usage_error_json("unknown profile", profile_arg->get<std::string>());
  }
  if (state.config_path.empty()) {
    return usage_error_json("missing config path",
                            "persistent updates require a loaded config file");
  }

  const auto upstream_id = upstream_arg->get<std::string>();
  for (auto& upstream : profile->spec.config.upstreams) {
    if (upstream.id != upstream_id) {
      continue;
    }
    const bool changed = upstream.enabled != enabled;
    auto persisted = persist_upstream_enabled(
        state.config_path, profile->spec.id, upstream.id, enabled);
    if (!persisted) {
      record_event(state, enabled ? "upstream.enable.failed"
                                  : "upstream.disable.failed",
                   Json{{"profile", profile->spec.id},
                        {"upstream", upstream.id},
                        {"error", error_json(persisted.error())["error"]}});
      return error_json(persisted.error());
    }
    upstream.enabled = enabled;
    record_event(state, enabled ? "upstream.enabled" : "upstream.disabled",
                 Json{{"profile", profile->spec.id},
                      {"upstream", upstream.id},
                      {"changed", changed},
                      {"persisted", true}});
    return Json{
        {"ok", true},
        {"profile", profile->spec.id},
        {"upstream", upstream.id},
        {"enabled", upstream.enabled},
        {"changed", changed},
        {"persisted", true},
        {"runtimeRestartRequired", true},
    };
  }

  return usage_error_json("unknown upstream", upstream_id);
}

std::optional<std::string> parse_endpoint(const Json& json,
                                          mcp::gateway::HttpEndpoint& endpoint,
                                          std::string_view path) {
  if (!json.is_object()) {
    return std::string(path) + " must be an object";
  }
  if (const auto host = json.find("host");
      host != json.end() && host->is_string()) {
    endpoint.host = host->get<std::string>();
  }
  if (const auto port = json.find("port");
      port != json.end() && port->is_number_unsigned()) {
    const auto value = port->get<unsigned>();
    if (value == 0 || value > 65535) {
      return std::string(path) + ".port must be between 1 and 65535";
    }
    endpoint.port = static_cast<std::uint16_t>(value);
  }
  if (const auto path_value = json.find("path");
      path_value != json.end() && path_value->is_string()) {
    endpoint.path = path_value->get<std::string>();
  }
  return std::nullopt;
}

std::optional<std::string> parse_admin(const Json& json,
                                       AdminEndpoint& endpoint) {
  if (!json.is_object()) {
    return "admin must be an object";
  }
  if (const auto host = json.find("host");
      host != json.end() && host->is_string()) {
    endpoint.host = host->get<std::string>();
  }
  if (const auto port = json.find("port");
      port != json.end() && port->is_number_unsigned()) {
    const auto value = port->get<unsigned>();
    if (value == 0 || value > 65535) {
      return "admin.port must be between 1 and 65535";
    }
    endpoint.port = static_cast<std::uint16_t>(value);
  }
  if (const auto path = json.find("path");
      path != json.end() && path->is_string()) {
    endpoint.path = path->get<std::string>();
  }
  return std::nullopt;
}

mcp::core::Result<LoadedGatewaydConfig> load_gatewayd_config(
    const std::string& path) {
  Json root;
  try {
    root = Json::parse(read_text_file(path));
  } catch (const std::exception& ex) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::ParseError),
        "failed to parse gatewayd config", ex.what(), "gatewayd.config"});
  }

  if (!root.is_object()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "gatewayd config root must be an object", "", "gatewayd.config"});
  }

  auto security = parse_security_config(root);
  if (!security) {
    return mcp::core::unexpected(security.error());
  }

  AdminEndpoint admin;
  const bool allow_non_loopback = security->allow_non_loopback;
  if (const auto admin_json = root.find("admin");
      admin_json != root.end()) {
    if (auto error = parse_admin(*admin_json, admin)) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "invalid admin endpoint", *error, "gatewayd.config"});
    }
  }
  if (!allow_non_loopback && !is_loopback_host(admin.host)) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "admin endpoint host is not loopback",
        "set security.allowNonLoopback=true to bind outside loopback",
        "gatewayd.config"});
  }
  if (allow_non_loopback && !is_loopback_host(admin.host) &&
      security->bearer_tokens.empty()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "admin endpoint non-loopback bind requires bearer auth",
        "set security.bearerTokens before binding outside loopback",
        "gatewayd.config"});
  }

  const auto profiles_json = root.find("profiles");
  if (profiles_json == root.end() || !profiles_json->is_array()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
        "gatewayd config requires a profiles array", "",
        "gatewayd.config"});
  }

  std::vector<ProfileSpec> profiles;
  for (std::size_t i = 0; i < profiles_json->size(); ++i) {
    const auto& item = (*profiles_json)[i];
    const auto item_path = "profiles[" + std::to_string(i) + "]";
    if (!item.is_object()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "gatewayd profile must be an object", item_path,
          "gatewayd.config"});
    }
    if (auto error = require_string(item, "id", item_path)) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "invalid profile id", *error, "gatewayd.config"});
    }

    ProfileSpec profile;
    profile.id = item.at("id").get<std::string>();
    profile.endpoint.port =
        static_cast<std::uint16_t>(39931 + profiles.size());
    profile.endpoint.path = "/mcp/" + profile.id;
    profile.endpoint.bearer_tokens = security->bearer_tokens;
    profile.endpoint.rate_limit = security->rate_limit;
    if (const auto endpoint_json = item.find("endpoint");
        endpoint_json != item.end()) {
      if (auto error =
              parse_endpoint(*endpoint_json, profile.endpoint,
                             item_path + ".endpoint")) {
        return mcp::core::unexpected(mcp::core::Error{
            static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
            "invalid profile endpoint", *error, "gatewayd.config"});
      }
    }
    if (!allow_non_loopback && !is_loopback_host(profile.endpoint.host)) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "profile endpoint host is not loopback",
          item_path + ".endpoint.host requires security.allowNonLoopback=true",
          "gatewayd.config"});
    }
    if (allow_non_loopback && !is_loopback_host(profile.endpoint.host) &&
        security->bearer_tokens.empty()) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "profile endpoint non-loopback bind requires bearer auth",
          item_path + ".endpoint.host requires security.bearerTokens",
          "gatewayd.config"});
    }

    mcp::gateway::GatewayConfigLoadOptions gateway_load_options;
    gateway_load_options.environment = resolve_environment;
    auto document = mcp::gateway::gateway_config_document_from_json(
        item, gateway_load_options);
    if (!document) {
      return mcp::core::unexpected(document.error());
    }
    profile.config = std::move(document->config);
    profile.config.name = profile.id;
    profile.runtime_config = document->runtime;
    profiles.push_back(std::move(profile));
  }

  return LoadedGatewaydConfig{
      .admin = std::move(admin),
      .security = std::move(*security),
      .profiles = std::move(profiles),
  };
}

void print_config_summary(const AdminEndpoint& admin,
                          const std::vector<ProfileSpec>& profiles) {
  std::cout << "gatewayd config is valid\n"
            << "admin: " << admin_url(admin) << "\n";
  for (const auto& profile : profiles) {
    std::cout << "profile " << profile.id << ": "
              << http_url(profile.endpoint) << ", upstreams="
              << profile.config.upstreams.size() << "\n";
  }
}

bool same_admin_endpoint(const AdminEndpoint& lhs, const AdminEndpoint& rhs) {
  return lhs.host == rhs.host && lhs.port == rhs.port && lhs.path == rhs.path;
}

bool same_security_config(const SecurityConfig& lhs,
                          const SecurityConfig& rhs) {
  if (lhs.allow_non_loopback != rhs.allow_non_loopback ||
      lhs.bearer_tokens.size() != rhs.bearer_tokens.size() ||
      lhs.rate_limit.requests_per_window !=
          rhs.rate_limit.requests_per_window ||
      lhs.rate_limit.window != rhs.rate_limit.window) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.bearer_tokens.size(); ++i) {
    if (lhs.bearer_tokens[i].token != rhs.bearer_tokens[i].token ||
        lhs.bearer_tokens[i].subject != rhs.bearer_tokens[i].subject) {
      return false;
    }
  }
  return true;
}

Json reload_config_json(GatewaydState& state, const Json&) {
  std::lock_guard lock(state.mutex);
  if (state.config_path.empty()) {
    return usage_error_json("missing config path",
                            "reload requires a loaded config file");
  }

  auto loaded = load_gatewayd_config(state.config_path);
  if (!loaded) {
    record_event(state, "config.reload.failed",
                 Json{{"error", error_json(loaded.error())["error"]}});
    return error_json(loaded.error());
  }
  if (!same_admin_endpoint(state.admin, loaded->admin)) {
    auto result = usage_error_json(
        "admin endpoint change requires daemon restart",
        "profile endpoints can reload in-process; admin endpoint cannot");
    record_event(state, "config.reload.failed",
                 Json{{"error", result["error"]}});
    return result;
  }
  if (!same_security_config(state.security, loaded->security)) {
    auto result = usage_error_json(
        "security config change requires daemon restart",
        "admin auth is installed when the admin endpoint starts");
    record_event(state, "config.reload.failed",
                 Json{{"error", result["error"]}});
    return result;
  }

  auto previous = std::move(state.profiles);
  stop_profiles(previous);

  auto started = start_profiles(std::move(loaded->profiles), state.self);
  if (!started) {
    for (const auto& profile : previous) {
      if (profile) {
        (void)restart_profile(*profile, state.self);
      }
    }
    state.profiles = std::move(previous);
    record_event(state, "config.reload.failed",
                 Json{{"error", error_json(started.error())["error"]}});
    return error_json(started.error());
  }

  state.profiles = std::move(*started);
  record_event(state, "config.reloaded",
               Json{{"configPath", state.config_path},
                    {"profileCount", state.profiles.size()}});
  return Json{
      {"ok", true},
      {"configPath", state.config_path},
      {"profileCount", state.profiles.size()},
  };
}

std::string default_admin_url() {
  if (const char* env = std::getenv("CXXMCP_GATEWAYD_ADMIN_URL");
      env != nullptr && *env != '\0') {
    return std::string(env);
  }
  return "http://127.0.0.1:39932/admin";
}

std::optional<std::string> default_admin_bearer_token() {
  if (const char* env = std::getenv("CXXMCP_GATEWAYD_ADMIN_TOKEN");
      env != nullptr && *env != '\0') {
    return std::string(env);
  }
  return std::nullopt;
}

mcp::core::Result<ToolResult> call_admin_tool(
    std::string admin_url,
    std::optional<std::string> bearer_token,
    std::string_view tool_name,
    Json arguments = Json::object()) {
  mcp::client::Client::StreamableHttpEndpoint endpoint;
  endpoint.uri = std::move(admin_url);
  endpoint.auth_header = std::move(bearer_token);

  auto running = mcp::serve(
      mcp::ClientPeer::connect_streamable_http(std::move(endpoint)));
  if (!running) {
    return mcp::core::unexpected(running.error());
  }

  auto initialized =
      running->peer().initialize("cxxmcp-gatewayd-cli", CXXMCP_GATEWAYD_VERSION);
  if (!initialized) {
    (void)running->stop();
    return mcp::core::unexpected(initialized.error());
  }
  auto notified = running->peer().notify_initialized();
  if (!notified) {
    (void)running->stop();
    return mcp::core::unexpected(notified.error());
  }

  auto result = running->peer().call_tool(std::string(tool_name),
                                          std::move(arguments));
  (void)running->stop();
  return result;
}

void print_tool_result(const ToolResult& result) {
  bool printed = false;
  for (const auto& block : result.content) {
    if (block.type == "text") {
      std::cout << block.text;
      if (!block.text.empty() && block.text.back() != '\n') {
        std::cout << '\n';
      }
      printed = true;
    }
  }
  if (!printed && result.structured_content.has_value()) {
    std::cout << result.structured_content->dump(2) << '\n';
  }
}

mcp::core::Result<Json> tool_result_text_json(const ToolResult& result) {
  for (const auto& block : result.content) {
    if (block.type != "text") {
      continue;
    }
    try {
      return Json::parse(block.text);
    } catch (const std::exception& ex) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::ParseError),
          "admin tool returned non-json text", ex.what(), "gatewayd.cli"});
    }
  }
  if (result.structured_content.has_value()) {
    return *result.structured_content;
  }
  return mcp::core::unexpected(mcp::core::Error{
      static_cast<int>(mcp::protocol::ErrorCode::InvalidRequest),
      "admin tool returned no json content", "", "gatewayd.cli"});
}

mcp::core::Result<Json> call_admin_json(
    const std::string& admin_url,
    const std::optional<std::string>& bearer_token,
    std::string_view tool_name,
    Json arguments = Json::object()) {
  auto result = call_admin_tool(admin_url, bearer_token, tool_name,
                                std::move(arguments));
  if (!result) {
    return mcp::core::unexpected(result.error());
  }
  if (result->is_error_result()) {
    return mcp::core::unexpected(mcp::core::Error{
        static_cast<int>(mcp::protocol::ErrorCode::InvalidRequest),
        "admin tool returned an error result", std::string(tool_name),
        "gatewayd.cli"});
  }
  return tool_result_text_json(*result);
}

std::size_t array_size_at(const Json& json, std::string_view key) {
  const auto it = json.find(std::string(key));
  return it != json.end() && it->is_array() ? it->size() : 0;
}

struct DashboardData {
  Json health;
  Json profiles;
  Json upstreams;
  Json tools;
  Json resources;
  Json prompts;
  Json events;
};

mcp::core::Result<DashboardData> load_dashboard_data(
    const std::string& admin_url,
    const std::optional<std::string>& bearer_token) {
  DashboardData data;
  const auto load = [&](std::string_view tool) -> mcp::core::Result<Json> {
    return call_admin_json(admin_url, bearer_token, tool);
  };

  auto health = load("gatewayd.health");
  if (!health) {
    return mcp::core::unexpected(health.error());
  }
  data.health = std::move(*health);
  auto profiles = load("gatewayd.profiles");
  if (!profiles) {
    return mcp::core::unexpected(profiles.error());
  }
  data.profiles = std::move(*profiles);
  auto upstreams = load("gatewayd.upstreams");
  if (!upstreams) {
    return mcp::core::unexpected(upstreams.error());
  }
  data.upstreams = std::move(*upstreams);
  auto tools = load("gatewayd.catalog.tools");
  if (!tools) {
    return mcp::core::unexpected(tools.error());
  }
  data.tools = std::move(*tools);
  auto resources = load("gatewayd.catalog.resources");
  if (!resources) {
    return mcp::core::unexpected(resources.error());
  }
  data.resources = std::move(*resources);
  auto prompts = load("gatewayd.catalog.prompts");
  if (!prompts) {
    return mcp::core::unexpected(prompts.error());
  }
  data.prompts = std::move(*prompts);
  auto events = load("gatewayd.events");
  if (!events) {
    return mcp::core::unexpected(events.error());
  }
  data.events = std::move(*events);
  return data;
}

void print_dashboard_count_line(const Json& catalog,
                                std::string_view field) {
  const auto profiles = catalog.find("profiles");
  if (profiles == catalog.end() || !profiles->is_array()) {
    return;
  }
  for (const auto& profile : *profiles) {
    const auto id = profile.value("id", "");
    if (profile.contains("error")) {
      std::cout << "  " << id << ": error "
                << profile["error"].value("message", "unknown") << "\n";
      continue;
    }
    std::cout << "  " << id << ": "
              << array_size_at(profile, field) << " " << field << "\n";
  }
}

int run_dashboard_cli(const std::string& admin_url,
                      const std::optional<std::string>& bearer_token) {
  const auto data = load_dashboard_data(admin_url, bearer_token);
  if (!data) {
    std::cerr << "dashboard failed: " << data.error().message;
    if (!data.error().detail.empty()) {
      std::cerr << ": " << data.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }

  std::cout << "cxxmcp-gatewayd dashboard\n";
  std::cout << "status: " << data->health.value("status", "unknown") << "\n";
  std::cout << "admin: " << data->health.value("adminUrl", admin_url) << "\n";
  std::cout << "config: " << data->health.value("configPath", "") << "\n\n";

  std::cout << "profiles\n";
  for (const auto& profile : data->profiles.value("profiles", Json::array())) {
    const auto endpoint = profile.value("endpoint", Json::object());
    std::cout << "  " << profile.value("id", "") << "  "
              << endpoint.value("url", "") << "  upstreams="
              << array_size_at(profile, "upstreams") << "\n";
  }

  std::cout << "\nupstreams\n";
  for (const auto& profile : data->upstreams.value("profiles", Json::array())) {
    std::cout << "  [" << profile.value("id", "") << "]\n";
    for (const auto& upstream : profile.value("configured", Json::array())) {
      std::cout << "    " << upstream.value("id", "")
                << " enabled=" << (upstream.value("enabled", false) ? "true"
                                                                    : "false")
                << " transport=" << upstream.value("transport", "") << "\n";
    }
  }

  std::cout << "\ncatalog\n";
  print_dashboard_count_line(data->tools, "tools");
  print_dashboard_count_line(data->resources, "resources");
  print_dashboard_count_line(data->prompts, "prompts");

  std::cout << "\nrecent events\n";
  const auto event_items = data->events.value("events", Json::array());
  const auto start = event_items.size() > 8 ? event_items.size() - 8 : 0;
  for (std::size_t i = start; i < event_items.size(); ++i) {
    const auto& event = event_items[i];
    std::cout << "  #" << event.value("id", 0) << " "
              << event.value("type", "") << "\n";
  }
  return 0;
}

std::string html_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#39;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

void append_catalog_rows(std::string& html,
                         const Json& catalog,
                         std::string_view field) {
  for (const auto& profile : catalog.value("profiles", Json::array())) {
    html += "<tr><td>";
    html += html_escape(profile.value("id", ""));
    html += "</td><td>";
    html += html_escape(field);
    html += "</td><td>";
    if (profile.contains("error")) {
      html += "error: ";
      html += html_escape(profile["error"].value("message", "unknown"));
    } else {
      html += std::to_string(array_size_at(profile, field));
    }
    html += "</td></tr>\n";
  }
}

std::string render_dashboard_html(const DashboardData& data,
                                  const std::string& admin_url) {
  std::string html;
  html += "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>cxxmcp-gatewayd dashboard</title>";
  html += "<style>";
  html += "body{margin:0;font:14px system-ui,Segoe UI,sans-serif;color:#18202a;background:#f6f7f9}";
  html += "header{background:#17202a;color:white;padding:18px 24px}";
  html += "main{max-width:1180px;margin:0 auto;padding:20px 24px}";
  html += "section{background:white;border:1px solid #d8dde5;border-radius:8px;margin:0 0 16px;padding:16px}";
  html += "h1{font-size:22px;margin:0 0 6px}h2{font-size:16px;margin:0 0 12px}";
  html += ".meta{color:#d7dee8}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}";
  html += ".metric{border:1px solid #e1e5eb;border-radius:8px;padding:12px;background:#fbfcfd}";
  html += ".metric b{display:block;font-size:20px;margin-top:4px}";
  html += "table{width:100%;border-collapse:collapse}th,td{text-align:left;border-bottom:1px solid #edf0f4;padding:8px}";
  html += "th{font-size:12px;text-transform:uppercase;color:#536173;background:#fafbfc}";
  html += "code{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}";
  html += "</style></head><body>";
  html += "<header><h1>cxxmcp-gatewayd dashboard</h1><div class=\"meta\">";
  html += html_escape(data.health.value("adminUrl", admin_url));
  html += "</div><div class=\"meta\">";
  html += html_escape(data.health.value("configPath", ""));
  html += "</div></header><main>";

  const auto profile_count = array_size_at(data.profiles, "profiles");
  std::size_t upstream_count = 0;
  for (const auto& profile : data.profiles.value("profiles", Json::array())) {
    upstream_count += array_size_at(profile, "upstreams");
  }
  html += "<section><h2>Summary</h2><div class=\"grid\">";
  html += "<div class=\"metric\">Status<b>";
  html += html_escape(data.health.value("status", "unknown"));
  html += "</b></div><div class=\"metric\">Profiles<b>";
  html += std::to_string(profile_count);
  html += "</b></div><div class=\"metric\">Configured upstreams<b>";
  html += std::to_string(upstream_count);
  html += "</b></div></div></section>";

  html += "<section><h2>Profiles</h2><table><tr><th>Profile</th><th>Endpoint</th><th>Upstreams</th></tr>";
  for (const auto& profile : data.profiles.value("profiles", Json::array())) {
    const auto endpoint = profile.value("endpoint", Json::object());
    html += "<tr><td>";
    html += html_escape(profile.value("id", ""));
    html += "</td><td><code>";
    html += html_escape(endpoint.value("url", ""));
    html += "</code></td><td>";
    html += std::to_string(array_size_at(profile, "upstreams"));
    html += "</td></tr>\n";
  }
  html += "</table></section>";

  html += "<section><h2>Upstreams</h2><table><tr><th>Profile</th><th>Upstream</th><th>Enabled</th><th>Transport</th></tr>";
  for (const auto& profile : data.upstreams.value("profiles", Json::array())) {
    const auto profile_id = profile.value("id", "");
    for (const auto& upstream : profile.value("configured", Json::array())) {
      html += "<tr><td>";
      html += html_escape(profile_id);
      html += "</td><td>";
      html += html_escape(upstream.value("id", ""));
      html += "</td><td>";
      html += upstream.value("enabled", false) ? "true" : "false";
      html += "</td><td>";
      html += html_escape(upstream.value("transport", ""));
      html += "</td></tr>\n";
    }
  }
  html += "</table></section>";

  html += "<section><h2>Catalog</h2><table><tr><th>Profile</th><th>Kind</th><th>Count</th></tr>";
  append_catalog_rows(html, data.tools, "tools");
  append_catalog_rows(html, data.resources, "resources");
  append_catalog_rows(html, data.prompts, "prompts");
  html += "</table></section>";

  html += "<section><h2>Recent Events</h2><table><tr><th>ID</th><th>Time</th><th>Type</th></tr>";
  const auto event_items = data.events.value("events", Json::array());
  const auto start = event_items.size() > 20 ? event_items.size() - 20 : 0;
  for (std::size_t i = start; i < event_items.size(); ++i) {
    const auto& event = event_items[i];
    html += "<tr><td>";
    html += std::to_string(event.value("id", 0));
    html += "</td><td>";
    html += std::to_string(event.value("unixMs", 0));
    html += "</td><td>";
    html += html_escape(event.value("type", ""));
    html += "</td></tr>\n";
  }
  html += "</table></section></main></body></html>\n";
  return html;
}

int run_dashboard_html_cli(const std::string& admin_url,
                           const std::optional<std::string>& bearer_token,
                           const std::string& output_path) {
  const auto data = load_dashboard_data(admin_url, bearer_token);
  if (!data) {
    std::cerr << "dashboard html failed: " << data.error().message;
    if (!data.error().detail.empty()) {
      std::cerr << ": " << data.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }
  try {
    write_text_file(output_path, render_dashboard_html(*data, admin_url));
  } catch (const std::exception& ex) {
    std::cerr << "dashboard html failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "dashboard html written to " << output_path << "\n";
  return 0;
}

bool parse_admin_url(std::vector<std::string_view>& args,
                     std::string& admin_url) {
  for (std::size_t i = 0; i < args.size();) {
    if (args[i] != "--admin-url") {
      ++i;
      continue;
    }
    if (i + 1 >= args.size()) {
      return false;
    }
    admin_url = std::string(args[i + 1]);
    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
               args.begin() + static_cast<std::ptrdiff_t>(i + 2));
  }
  return true;
}

bool parse_admin_bearer_token(std::vector<std::string_view>& args,
                              std::optional<std::string>& bearer_token) {
  for (std::size_t i = 0; i < args.size();) {
    if (args[i] != "--bearer-token") {
      ++i;
      continue;
    }
    if (i + 1 >= args.size()) {
      return false;
    }
    bearer_token = std::string(args[i + 1]);
    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
               args.begin() + static_cast<std::ptrdiff_t>(i + 2));
  }
  return true;
}

int run_admin_cli(std::vector<std::string_view> args) {
  std::string admin_url = default_admin_url();
  std::optional<std::string> bearer_token = default_admin_bearer_token();
  if (!parse_admin_url(args, admin_url)) {
    std::cerr << "--admin-url requires a value\n";
    return 2;
  }
  if (!parse_admin_bearer_token(args, bearer_token)) {
    std::cerr << "--bearer-token requires a value\n";
    return 2;
  }
  if (args.empty()) {
    print_usage(std::cerr);
    return 2;
  }
  if (args[0] == "dashboard" && args.size() == 1) {
    return run_dashboard_cli(admin_url, bearer_token);
  }
  if (args[0] == "dashboard" && args.size() == 3 && args[1] == "--html") {
    return run_dashboard_html_cli(admin_url, bearer_token,
                                  std::string(args[2]));
  }

  std::string tool;
  Json arguments = Json::object();
  std::optional<std::string> restart_profile_after_success;
  if (args[0] == "status") {
    tool = "gatewayd.health";
  } else if (args[0] == "profiles") {
    tool = "gatewayd.profiles";
  } else if (args[0] == "upstreams") {
    tool = "gatewayd.upstreams";
  } else if (args[0] == "catalog" && args.size() == 2) {
    if (args[1] == "tools") {
      tool = "gatewayd.catalog.tools";
    } else if (args[1] == "resources") {
      tool = "gatewayd.catalog.resources";
    } else if (args[1] == "prompts") {
      tool = "gatewayd.catalog.prompts";
    } else {
      print_usage(std::cerr);
      return 2;
    }
  } else if (args[0] == "events") {
    tool = "gatewayd.events";
  } else if (args[0] == "diagnostics") {
    for (const auto& item :
         {std::pair<std::string_view, std::string_view>{"status",
                                                        "gatewayd.health"},
          std::pair<std::string_view, std::string_view>{"profiles",
                                                        "gatewayd.profiles"},
          std::pair<std::string_view, std::string_view>{"upstreams",
                                                        "gatewayd.upstreams"},
          std::pair<std::string_view, std::string_view>{"events",
                                                        "gatewayd.events"}}) {
      std::cout << "== " << item.first << " ==\n";
      auto result = call_admin_tool(admin_url, bearer_token, item.second);
      if (!result) {
        std::cerr << "admin command failed: " << result.error().message;
        if (!result.error().detail.empty()) {
          std::cerr << ": " << result.error().detail;
        }
        std::cerr << "\n";
        return 1;
      }
      print_tool_result(*result);
    }
    return 0;
  } else if (args[0] == "reload") {
    tool = "gatewayd.reload";
  } else if (args[0] == "upstream" && args.size() == 4 &&
             (args[1] == "enable" || args[1] == "disable")) {
    tool = args[1] == "enable" ? "gatewayd.upstream.enable"
                               : "gatewayd.upstream.disable";
    arguments = Json{
        {"profile", std::string(args[2])},
        {"upstream", std::string(args[3])},
    };
  } else if (args[0] == "profile" && args.size() == 3 &&
             args[1] == "restart") {
    tool = "gatewayd.profile.restart";
    arguments = Json{{"profile", std::string(args[2])}};
  } else if (args[0] == "profile" && args.size() >= 4 &&
             args[1] == "runtime" && args[2] == "set") {
    tool = "gatewayd.profile.runtime.set";
    arguments = Json{{"profile", std::string(args[3])}};
    for (std::size_t i = 4; i < args.size();) {
      if (args[i] == "--restart") {
        restart_profile_after_success = std::string(args[3]);
        ++i;
        continue;
      }
      if (i + 1 >= args.size()) {
        print_usage(std::cerr);
        return 2;
      }
      const auto key = args[i];
      const auto value = args[i + 1];
      try {
        if (key == "--session-mode") {
          arguments["upstreamSessionMode"] = std::string(value);
        } else if (key == "--pool-size") {
          arguments["persistentSessionPoolSize"] =
              static_cast<std::uint64_t>(
                  std::stoull(std::string(value)));
        } else if (key == "--session-acquire-timeout-ms") {
          arguments["persistentSessionAcquireTimeoutMs"] =
              static_cast<std::uint64_t>(
                  std::stoull(std::string(value)));
        } else if (key == "--active-call-drain-timeout-ms") {
          arguments["activeCallDrainTimeoutMs"] =
              static_cast<std::uint64_t>(
                  std::stoull(std::string(value)));
        } else if (key == "--prewarm") {
          if (value == "true" || value == "1") {
            arguments["prewarmCapabilities"] = true;
          } else if (value == "false" || value == "0") {
            arguments["prewarmCapabilities"] = false;
          } else {
            std::cerr << "--prewarm requires true or false\n";
            return 2;
          }
        } else {
          print_usage(std::cerr);
          return 2;
        }
      } catch (const std::exception&) {
        std::cerr << key << " requires an unsigned integer\n";
        return 2;
      }
      i += 2;
    }
  } else {
    print_usage(std::cerr);
    return 2;
  }

  auto result = call_admin_tool(admin_url, bearer_token, tool,
                                std::move(arguments));
  if (!result) {
    std::cerr << "admin command failed: " << result.error().message;
    if (!result.error().detail.empty()) {
      std::cerr << ": " << result.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }
  print_tool_result(*result);
  if (result->is_error_result()) {
    return 1;
  }
  if (restart_profile_after_success.has_value()) {
    auto restarted =
        call_admin_tool(std::move(admin_url), std::move(bearer_token),
                        "gatewayd.profile.restart",
                        Json{{"profile", *restart_profile_after_success}});
    if (!restarted) {
      std::cerr << "admin command failed: " << restarted.error().message;
      if (!restarted.error().detail.empty()) {
        std::cerr << ": " << restarted.error().detail;
      }
      std::cerr << "\n";
      return 1;
    }
    print_tool_result(*restarted);
    return restarted->is_error_result() ? 1 : 0;
  }
  return 0;
}

mcp::core::Result<mcp::RunningService<mcp::RoleServer>> start_admin_service(
    const std::shared_ptr<GatewaydState>& state) {
  auto builder = mcp::ServerPeer::builder();
  builder.name("cxxmcp-gatewayd-admin").version(CXXMCP_GATEWAYD_VERSION);
  if (!state->security.bearer_tokens.empty()) {
    builder.auth_provider(make_auth_provider(state->security.bearer_tokens));
  }
  std::shared_ptr<mcp::server::RateLimiter> rate_limiter;
  if (state->security.rate_limit.requests_per_window > 0) {
    rate_limiter = make_rate_limiter(state->security.rate_limit);
  }
  auto peer =
      builder.streamable_http(state->admin.host, state->admin.port,
                              state->admin.path)
          .raw_request([rate_limiter](
                           const mcp::protocol::JsonRpcRequest& request) {
            if (rate_limiter) {
              return rate_limited_response(*rate_limiter, request);
            }
            return std::optional<mcp::protocol::JsonRpcResponse>{};
          })
          .tool<Json, ToolResult>(
              "gatewayd.health",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(state_health_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.profiles",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(state_profiles_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.upstreams",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(state_upstreams_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.catalog.tools",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(
                    state_catalog_tools_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.catalog.resources",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(
                    state_catalog_resources_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.catalog.prompts",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(
                    state_catalog_prompts_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.events",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(state_events_json(*state).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.reload",
              [state](const Json& args) {
                return ToolResult::text(reload_config_json(*state, args).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.upstream.enable",
              [state](const Json& args) {
                return ToolResult::text(
                    set_upstream_enabled_json(*state, args, true).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.upstream.disable",
              [state](const Json& args) {
                return ToolResult::text(
                    set_upstream_enabled_json(*state, args, false).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.profile.restart",
              [state](const Json& args) {
                return ToolResult::text(
                    restart_profile_json(*state, args).dump(2));
              })
          .tool<Json, ToolResult>(
              "gatewayd.profile.runtime.set",
              [state](const Json& args) {
                return ToolResult::text(
                    set_profile_runtime_json(*state, args).dump(2));
              })
          .build();
  if (!peer) {
    return mcp::core::unexpected(peer.error());
  }
  return mcp::serve(std::move(*peer));
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (args.empty() || args[0] == "--help" || args[0] == "-h") {
    print_usage(std::cout);
    return 0;
  }

  if (args[0] == "--version") {
    std::cout << "cxxmcp-gatewayd " << CXXMCP_GATEWAYD_VERSION << "\n";
    return 0;
  }

  if (args[0] == "status" || args[0] == "profiles" ||
      args[0] == "upstreams" || args[0] == "catalog" ||
      args[0] == "dashboard" ||
      args[0] == "events" || args[0] == "diagnostics" ||
      args[0] == "reload" ||
      args[0] == "upstream" || args[0] == "profile") {
    return run_admin_cli(std::move(args));
  }

  enum class Command { run, validate };
  Command command = Command::run;
  std::string config_path;

  if (args.size() == 2 && args[0] == "--config") {
    config_path = std::string(args[1]);
  } else if (args.size() == 1 &&
             (args[0] == "run" || args[0] == "validate")) {
    command = args[0] == "validate" ? Command::validate : Command::run;
    auto discovered = discover_config_path();
    if (!discovered) {
      std::cerr << "failed to discover gatewayd config; pass --config or set "
                   "CXXMCP_GATEWAYD_CONFIG\n";
      return 2;
    }
    config_path = std::move(*discovered);
  } else if (args.size() == 3 &&
             (args[0] == "run" || args[0] == "validate") &&
             args[1] == "--config") {
    command = args[0] == "validate" ? Command::validate : Command::run;
    config_path = std::string(args[2]);
  } else {
    print_usage(std::cerr);
    return 2;
  }

  auto loaded = load_gatewayd_config(config_path);
  if (!loaded) {
    std::cerr << "failed to load gatewayd config: "
              << loaded.error().message;
    if (!loaded.error().detail.empty()) {
      std::cerr << ": " << loaded.error().detail;
    }
    std::cerr << "\n";
    return 2;
  }

  if (command == Command::validate) {
    print_config_summary(loaded->admin, loaded->profiles);
    return 0;
  }

  auto state = std::make_shared<GatewaydState>();
  state->self = state;
  state->admin = std::move(loaded->admin);
  state->security = std::move(loaded->security);
  state->config_path = config_path;

  auto profiles = start_profiles(std::move(loaded->profiles), state->self);
  if (!profiles) {
    std::cerr << "failed to start profiles: " << profiles.error().message;
    if (!profiles.error().detail.empty()) {
      std::cerr << ": " << profiles.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }
  state->profiles = std::move(*profiles);
  for (const auto& profile : state->profiles) {
    std::cout << "profile " << profile->spec.id << " listening on "
              << http_url(profile->spec.endpoint) << std::endl;
  }
  record_event(*state, "daemon.started",
               Json{{"configPath", state->config_path},
                    {"profileCount", state->profiles.size()}});

  auto admin = start_admin_service(state);
  if (!admin) {
    std::cerr << "failed to start admin endpoint: " << admin.error().message;
    if (!admin.error().detail.empty()) {
      std::cerr << ": " << admin.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }
  admin->wait_until_ready();
  std::cout << "admin MCP endpoint listening on " << admin_url(state->admin)
            << std::endl;

  while (!g_stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }

  (void)admin->stop();
  for (auto& profile : state->profiles) {
    if (profile->runtime) {
      (void)profile->runtime->stop();
    }
  }
  return 0;
}

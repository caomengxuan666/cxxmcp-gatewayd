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
#include "cxxmcp/server/authoring.hpp"
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
  std::string config_path;
  std::uint64_t next_event_id = 1;
  mutable std::mutex mutex;
  std::vector<std::shared_ptr<ProfileRuntime>> profiles;
  std::vector<Json> events;
};

void print_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  cxxmcp-gatewayd --help\n"
      << "  cxxmcp-gatewayd --version\n"
      << "  cxxmcp-gatewayd validate [--config <file>]\n"
      << "  cxxmcp-gatewayd run [--config <file>]\n"
      << "  cxxmcp-gatewayd status [--admin-url <url>]\n"
      << "  cxxmcp-gatewayd upstreams [--admin-url <url>]\n"
      << "  cxxmcp-gatewayd events [--admin-url <url>]\n"
      << "  cxxmcp-gatewayd diagnostics [--admin-url <url>]\n"
      << "  cxxmcp-gatewayd reload [--admin-url <url>]\n"
      << "  cxxmcp-gatewayd upstream enable <profile> <upstream> "
         "[--admin-url <url>]\n"
      << "  cxxmcp-gatewayd upstream disable <profile> <upstream> "
         "[--admin-url <url>]\n"
      << "  cxxmcp-gatewayd --config <file>   # legacy alias for run\n\n"
      << "Config discovery without --config:\n"
      << "  1. CXXMCP_GATEWAYD_CONFIG\n"
      << "  2. ./gatewayd.json\n"
      << "  3. ./gatewayd.config.json\n\n"
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

bool allow_non_loopback_bind(const Json& root) {
  const auto security = root.find("security");
  if (security == root.end() || !security->is_object()) {
    return false;
  }
  const auto allow = security->find("allowNonLoopback");
  return allow != security->end() && allow->is_boolean() &&
         allow->get<bool>();
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
    ProfileSpec spec) {
  auto runtime_options =
      mcp::gateway::make_gateway_runtime_options(spec.runtime_config);
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
    std::vector<ProfileSpec> specs) {
  std::vector<std::shared_ptr<ProfileRuntime>> profiles;
  for (auto& spec : specs) {
    auto profile = start_profile(std::move(spec));
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

mcp::core::Result<mcp::core::Unit> restart_profile(ProfileRuntime& profile) {
  if (profile.runtime) {
    (void)profile.runtime->stop();
    profile.runtime.reset();
  }

  auto restarted = start_profile(profile.spec);
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

  auto restarted = restart_profile(*profile);
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

mcp::core::Result<std::pair<AdminEndpoint, std::vector<ProfileSpec>>>
load_gatewayd_config(const std::string& path) {
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

  AdminEndpoint admin;
  const bool allow_non_loopback = allow_non_loopback_bind(root);
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

    auto document = mcp::gateway::gateway_config_document_from_json(item);
    if (!document) {
      return mcp::core::unexpected(document.error());
    }
    profile.config = std::move(document->config);
    profile.config.name = profile.id;
    profile.runtime_config = document->runtime;
    profiles.push_back(std::move(profile));
  }

  return std::pair<AdminEndpoint, std::vector<ProfileSpec>>{
      std::move(admin), std::move(profiles)};
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
  if (!same_admin_endpoint(state.admin, loaded->first)) {
    auto result = usage_error_json(
        "admin endpoint change requires daemon restart",
        "profile endpoints can reload in-process; admin endpoint cannot");
    record_event(state, "config.reload.failed",
                 Json{{"error", result["error"]}});
    return result;
  }

  auto previous = std::move(state.profiles);
  stop_profiles(previous);

  auto started = start_profiles(std::move(loaded->second));
  if (!started) {
    for (const auto& profile : previous) {
      if (profile) {
        (void)restart_profile(*profile);
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

mcp::core::Result<ToolResult> call_admin_tool(
    std::string admin_url,
    std::string_view tool_name,
    Json arguments = Json::object()) {
  mcp::client::Client::StreamableHttpEndpoint endpoint;
  endpoint.uri = std::move(admin_url);

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

int run_admin_cli(std::vector<std::string_view> args) {
  std::string admin_url = default_admin_url();
  if (!parse_admin_url(args, admin_url)) {
    std::cerr << "--admin-url requires a value\n";
    return 2;
  }
  if (args.empty()) {
    print_usage(std::cerr);
    return 2;
  }

  std::string tool;
  Json arguments = Json::object();
  if (args[0] == "status") {
    tool = "gatewayd.health";
  } else if (args[0] == "upstreams") {
    tool = "gatewayd.upstreams";
  } else if (args[0] == "events") {
    tool = "gatewayd.events";
  } else if (args[0] == "diagnostics") {
    for (const auto& item :
         {std::pair<std::string_view, std::string_view>{"status",
                                                        "gatewayd.health"},
          std::pair<std::string_view, std::string_view>{"upstreams",
                                                        "gatewayd.upstreams"},
          std::pair<std::string_view, std::string_view>{"events",
                                                        "gatewayd.events"}}) {
      std::cout << "== " << item.first << " ==\n";
      auto result = call_admin_tool(admin_url, item.second);
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
  } else {
    print_usage(std::cerr);
    return 2;
  }

  auto result = call_admin_tool(std::move(admin_url), tool, std::move(arguments));
  if (!result) {
    std::cerr << "admin command failed: " << result.error().message;
    if (!result.error().detail.empty()) {
      std::cerr << ": " << result.error().detail;
    }
    std::cerr << "\n";
    return 1;
  }
  print_tool_result(*result);
  return result->is_error_result() ? 1 : 0;
}

mcp::core::Result<mcp::RunningService<mcp::RoleServer>> start_admin_service(
    const std::shared_ptr<GatewaydState>& state) {
  auto peer =
      mcp::ServerPeer::builder()
          .name("cxxmcp-gatewayd-admin")
          .version(CXXMCP_GATEWAYD_VERSION)
          .streamable_http(state->admin.host, state->admin.port,
                           state->admin.path)
          .tool<Json, ToolResult>(
              "gatewayd.health",
              [state](const Json&) {
                std::lock_guard lock(state->mutex);
                return ToolResult::text(state_health_json(*state).dump(2));
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

  if (args[0] == "status" || args[0] == "upstreams" ||
      args[0] == "events" || args[0] == "diagnostics" ||
      args[0] == "reload" ||
      args[0] == "upstream") {
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
    print_config_summary(loaded->first, loaded->second);
    return 0;
  }

  auto state = std::make_shared<GatewaydState>();
  state->admin = std::move(loaded->first);
  state->config_path = config_path;

  auto profiles = start_profiles(std::move(loaded->second));
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

// Copyright (c) 2025 [caomengxuan666]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
  mutable std::mutex mutex;
  std::vector<std::shared_ptr<ProfileRuntime>> profiles;
};

void print_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  cxxmcp-gatewayd --help\n"
      << "  cxxmcp-gatewayd --version\n"
      << "  cxxmcp-gatewayd validate --config <file>\n"
      << "  cxxmcp-gatewayd run --config <file>\n"
      << "  cxxmcp-gatewayd --config <file>   # legacy alias for run\n\n"
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
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
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
      {"profiles", std::move(profiles)},
  };
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

mcp::core::Result<mcp::core::Unit> restart_profile(ProfileRuntime& profile) {
  auto runtime_options =
      mcp::gateway::make_gateway_runtime_options(profile.spec.runtime_config);
  if (!runtime_options) {
    return mcp::core::unexpected(runtime_options.error());
  }

  if (profile.runtime) {
    (void)profile.runtime->stop();
    profile.runtime.reset();
  }

  auto next = std::make_unique<mcp::gateway::GatewayRuntime>(
      profile.spec.config, std::move(*runtime_options));
  if (profile.spec.runtime_config.prewarm_capabilities) {
    auto refreshed = next->refresh_upstream_capabilities();
    if (!refreshed) {
      return mcp::core::unexpected(refreshed.error());
    }
  }
  auto started = next->start_http(profile.spec.endpoint);
  if (!started) {
    return mcp::core::unexpected(started.error());
  }
  profile.runtime = std::move(next);
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
    return error_json(restarted.error());
  }
  return Json{
      {"ok", true},
      {"profile", profile->spec.id},
      {"mcpUrl", http_url(profile->spec.endpoint)},
  };
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

  const auto upstream_id = upstream_arg->get<std::string>();
  for (auto& upstream : profile->spec.config.upstreams) {
    if (upstream.id != upstream_id) {
      continue;
    }
    const bool changed = upstream.enabled != enabled;
    upstream.enabled = enabled;
    return Json{
        {"ok", true},
        {"profile", profile->spec.id},
        {"upstream", upstream.id},
        {"enabled", upstream.enabled},
        {"changed", changed},
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
  if (const auto admin_json = root.find("admin");
      admin_json != root.end()) {
    if (auto error = parse_admin(*admin_json, admin)) {
      return mcp::core::unexpected(mcp::core::Error{
          static_cast<int>(mcp::protocol::ErrorCode::InvalidParams),
          "invalid admin endpoint", *error, "gatewayd.config"});
    }
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

  enum class Command { run, validate };
  Command command = Command::run;
  std::string_view config_path;

  if (args.size() == 2 && args[0] == "--config") {
    config_path = args[1];
  } else if (args.size() == 3 &&
             (args[0] == "run" || args[0] == "validate") &&
             args[1] == "--config") {
    command = args[0] == "validate" ? Command::validate : Command::run;
    config_path = args[2];
  } else {
    print_usage(std::cerr);
    return 2;
  }

  auto loaded = load_gatewayd_config(std::string(config_path));
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

  for (auto& spec : loaded->second) {
    auto runtime_options =
        mcp::gateway::make_gateway_runtime_options(spec.runtime_config);
    if (!runtime_options) {
      std::cerr << "invalid runtime config for profile " << spec.id << ": "
                << runtime_options.error().message << "\n";
      return 2;
    }

    auto profile = std::make_shared<ProfileRuntime>();
    profile->spec = std::move(spec);
    profile->runtime = std::make_unique<mcp::gateway::GatewayRuntime>(
        profile->spec.config, std::move(*runtime_options));
    if (profile->spec.runtime_config.prewarm_capabilities) {
      auto refreshed = profile->runtime->refresh_upstream_capabilities();
      if (!refreshed) {
        std::cerr << "failed to prewarm profile " << profile->spec.id << ": "
                  << refreshed.error().message << "\n";
        return 1;
      }
    }
    auto started = profile->runtime->start_http(profile->spec.endpoint);
    if (!started) {
      std::cerr << "failed to start profile " << profile->spec.id << ": "
                << started.error().message;
      if (!started.error().detail.empty()) {
        std::cerr << ": " << started.error().detail;
      }
      std::cerr << "\n";
      return 1;
    }
    std::cout << "profile " << profile->spec.id << " listening on "
              << http_url(profile->spec.endpoint) << std::endl;
    state->profiles.push_back(std::move(profile));
  }

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
    (void)profile->runtime->stop();
  }
  return 0;
}

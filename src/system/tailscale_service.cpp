#include "system/tailscale_service.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("tailscale");
  constexpr auto kPollInterval = std::chrono::seconds(30);
  constexpr auto kCommandTimeout = std::chrono::seconds(60);

  // pkexec/run0 resolve the program themselves, but only reliably for an absolute
  // path — so the binary is looked up once here and reused for every call.
  std::string resolveExecutable(const char* name) {
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      pathEnv = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }
    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::string_view dir = end == std::string_view::npos ? path.substr(start) : path.substr(start, end - start);
      const std::filesystem::path candidate =
          dir.empty() ? std::filesystem::path(name) : (std::filesystem::path(dir) / name);
      std::error_code ec;
      if (::access(candidate.c_str(), X_OK) == 0 && std::filesystem::is_regular_file(candidate, ec)) {
        return candidate.string();
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return {};
  }

  bool looksLikePermissionError(std::string_view message) {
    std::string lower(message);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.contains("access denied")
        || lower.contains("permission denied")
        || lower.contains("must be root")
        || lower.contains("operator")
        || lower.contains("not permitted");
  }

  // "host.tailnet.ts.net." → "host"
  std::string baseName(std::string_view dnsName) {
    const std::size_t dot = dnsName.find('.');
    return std::string(dot == std::string_view::npos ? dnsName : dnsName.substr(0, dot));
  }

  std::string stringField(const nlohmann::json& node, const char* key) {
    const auto it = node.find(key);
    return it != node.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

  // Address of a node on the tailnet, preferring the v4 one since it is the shorter
  // of the two and the one people recognize.
  std::string tailnetAddress(const nlohmann::json& node) {
    const auto ips = node.find("TailscaleIPs");
    if (ips == node.end() || !ips->is_array()) {
      return {};
    }
    std::string fallback;
    for (const auto& ip : *ips) {
      if (!ip.is_string()) {
        continue;
      }
      std::string value = ip.get<std::string>();
      if (!value.contains(':')) {
        return value;
      }
      if (fallback.empty()) {
        fallback = std::move(value);
      }
    }
    return fallback;
  }

  bool boolField(const nlohmann::json& node, const char* key) {
    const auto it = node.find(key);
    return it != node.end() && it->is_boolean() && it->get<bool>();
  }

} // namespace

TailscaleService::TailscaleService() : m_binary(resolveExecutable("tailscale")), m_alive(std::make_shared<bool>(true)) {
  if (m_binary.empty()) {
    return;
  }
  m_escalator = process::resolvePrivilegeEscalator().value_or(std::string{});
}

TailscaleService::~TailscaleService() { *m_alive = false; }

void TailscaleService::start() {
  if (!available()) {
    return;
  }
  kLog.info("tailscale cli found at {}", m_binary);
  refresh();
  m_pollTimer.startRepeating(kPollInterval, [this]() { refresh(); });
}

bool TailscaleService::exitNodeActive() const noexcept {
  return std::ranges::any_of(m_exitNodes, [](const TailscaleExitNode& node) { return node.active; });
}

std::string TailscaleService::activeExitNodeName() const {
  for (const auto& node : m_exitNodes) {
    if (node.active) {
      return node.name;
    }
  }
  return {};
}

void TailscaleService::refresh() {
  if (!available() || m_statusPending) {
    return;
  }
  runStatus();
}

void TailscaleService::runStatus() {
  m_statusPending = true;
  auto alive = m_alive;
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, alive](const process::RunResult& result) {
    DeferredCall::callLater([this, alive, result]() {
      if (!*alive) {
        return;
      }
      m_statusPending = false;
      if (!result) {
        if (!m_exitNodes.empty() || m_running || !m_selfIp.empty()) {
          m_exitNodes.clear();
          m_running = false;
          m_selfIp.clear();
          emitChanged();
        }
        kLog.debug("tailscale status failed: {}", StringUtils::trim(result.err));
        return;
      }
      applyStatus(result.out);
    });
  };
  if (!process::runAsync({m_binary, "status", "--json"}, std::move(callbacks), {.timeout = kCommandTimeout})) {
    m_statusPending = false;
  }
}

void TailscaleService::applyStatus(const std::string& json) {
  std::vector<TailscaleExitNode> nodes;
  bool running = false;
  std::string selfIp;
  try {
    const auto parsed = nlohmann::json::parse(json);
    running = stringField(parsed, "BackendState") == "Running";
    if (const auto self = parsed.find("Self"); self != parsed.end() && self->is_object()) {
      selfIp = tailnetAddress(*self);
    }
    const auto peers = parsed.find("Peer");
    if (peers != parsed.end() && peers->is_object()) {
      for (const auto& peer : *peers) {
        if (!peer.is_object() || !boolField(peer, "ExitNodeOption")) {
          continue;
        }
        TailscaleExitNode node;
        node.id = stringField(peer, "ID");
        node.online = boolField(peer, "Online");
        node.active = boolField(peer, "ExitNode");

        const std::string hostName = stringField(peer, "HostName");
        const std::string dnsName = baseName(stringField(peer, "DNSName"));
        node.name = !dnsName.empty() ? dnsName : hostName;
        node.target = node.name;
        if (node.target.empty()) {
          node.target = tailnetAddress(peer);
          node.name = node.target;
        }
        if (node.target.empty()) {
          continue;
        }
        if (node.id.empty()) {
          node.id = node.target;
        }
        nodes.push_back(std::move(node));
      }
    }
  } catch (const std::exception& e) {
    kLog.debug("failed to parse tailscale status: {}", e.what());
    return;
  }

  // A stopped backend routes nothing, and its peers are not reachable choices —
  // status still lists them, so they are dropped here rather than at every reader.
  if (!running) {
    nodes.clear();
  }

  std::ranges::sort(nodes, [](const TailscaleExitNode& a, const TailscaleExitNode& b) {
    if (a.active != b.active) {
      return a.active;
    }
    if (a.online != b.online) {
      return a.online;
    }
    return a.name < b.name;
  });

  if (nodes == m_exitNodes && running == m_running && selfIp == m_selfIp) {
    return;
  }
  m_exitNodes = std::move(nodes);
  m_running = running;
  m_selfIp = std::move(selfIp);
  emitChanged();
}

void TailscaleService::setEnabled(bool enabled) {
  if (!available() || m_busy) {
    return;
  }
  runCommand({enabled ? "up" : "down"}, i18n::tr("notifications.internal.tailscale-toggle-failed"));
}

void TailscaleService::connectExitNode(const TailscaleExitNode& node) {
  if (!available() || m_busy || node.target.empty()) {
    return;
  }
  runCommand({"set", "--exit-node=" + node.target}, i18n::tr("notifications.internal.tailscale-exit-node-failed"));
}

void TailscaleService::disconnectExitNode() {
  if (!available() || m_busy) {
    return;
  }
  runCommand({"set", "--exit-node="}, i18n::tr("notifications.internal.tailscale-exit-node-failed"));
}

void TailscaleService::runCommand(std::vector<std::string> args, std::string failureMessage) {
  m_busy = true;
  emitChanged();

  auto alive = m_alive;
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, alive, args, failureMessage](const process::RunResult& result) mutable {
    DeferredCall::callLater([this, alive, args = std::move(args), failureMessage = std::move(failureMessage),
                             result]() mutable {
      if (!*alive) {
        return;
      }
      if (result) {
        m_busy = false;
        refresh();
        emitChanged();
        return;
      }
      // Without an operator configured, tailscaled only takes changes from root.
      // Retrying under polkit turns that into the usual password prompt.
      if (!m_escalator.empty() && looksLikePermissionError(result.err)) {
        runCommandEscalated(std::move(args), std::move(failureMessage));
        return;
      }
      m_busy = false;
      kLog.warn("tailscale {} failed: {}", StringUtils::join(args, " "), StringUtils::trim(result.err));
      notifyFailure(failureMessage);
      refresh();
      emitChanged();
    });
  };

  std::vector<std::string> argv{m_binary};
  argv.insert(argv.end(), args.begin(), args.end());
  if (!process::runAsync(argv, std::move(callbacks), {.timeout = kCommandTimeout})) {
    m_busy = false;
    emitChanged();
  }
}

void TailscaleService::runCommandEscalated(std::vector<std::string> args, std::string failureMessage) {
  if (m_privilegedRunCallback) {
    m_privilegedRunCallback();
  }

  auto alive = m_alive;
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, alive, args, failureMessage](const process::RunResult& result) mutable {
    DeferredCall::callLater([this, alive, args = std::move(args), failureMessage = std::move(failureMessage),
                             result]() mutable {
      if (!*alive) {
        return;
      }
      m_busy = false;
      if (!result) {
        kLog.warn(
            "{} tailscale {} failed: {}", m_escalator, StringUtils::join(args, " "), StringUtils::trim(result.err)
        );
        notifyFailure(failureMessage);
      }
      refresh();
      emitChanged();
    });
  };

  std::vector<std::string> argv{m_escalator, m_binary};
  argv.insert(argv.end(), args.begin(), args.end());
  // No timeout: the polkit prompt is on the user's clock.
  if (!process::runAsync(argv, std::move(callbacks))) {
    m_busy = false;
    emitChanged();
  }
}

void TailscaleService::notifyFailure(const std::string& message) {
  notify::error("Noctalia", i18n::tr("notifications.internal.tailscale"), message);
}

void TailscaleService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

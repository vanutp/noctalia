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
        if (!m_exitNodes.empty()) {
          m_exitNodes.clear();
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
  try {
    const auto parsed = nlohmann::json::parse(json);
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
          const auto ips = peer.find("TailscaleIPs");
          if (ips != peer.end() && ips->is_array() && !ips->empty() && ips->front().is_string()) {
            node.target = ips->front().get<std::string>();
            node.name = node.target;
          }
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

  std::ranges::sort(nodes, [](const TailscaleExitNode& a, const TailscaleExitNode& b) {
    if (a.active != b.active) {
      return a.active;
    }
    if (a.online != b.online) {
      return a.online;
    }
    return a.name < b.name;
  });

  if (nodes == m_exitNodes) {
    return;
  }
  m_exitNodes = std::move(nodes);
  emitChanged();
}

void TailscaleService::connectExitNode(const TailscaleExitNode& node) {
  if (!available() || m_busy || node.target.empty()) {
    return;
  }
  runSet(node.target);
}

void TailscaleService::disconnectExitNode() {
  if (!available() || m_busy) {
    return;
  }
  runSet({});
}

void TailscaleService::runSet(const std::string& exitNode) {
  m_busy = true;
  emitChanged();

  auto alive = m_alive;
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, alive, exitNode](const process::RunResult& result) {
    DeferredCall::callLater([this, alive, exitNode, result]() {
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
        runSetEscalated(exitNode);
        return;
      }
      m_busy = false;
      kLog.warn("tailscale set --exit-node={} failed: {}", exitNode, StringUtils::trim(result.err));
      notifyFailure();
      refresh();
      emitChanged();
    });
  };

  const std::string arg = "--exit-node=" + exitNode;
  if (!process::runAsync({m_binary, "set", arg}, std::move(callbacks), {.timeout = kCommandTimeout})) {
    m_busy = false;
    emitChanged();
  }
}

void TailscaleService::runSetEscalated(const std::string& exitNode) {
  if (m_privilegedRunCallback) {
    m_privilegedRunCallback();
  }

  auto alive = m_alive;
  process::RunCallbacks callbacks;
  callbacks.onExit = [this, alive, exitNode](const process::RunResult& result) {
    DeferredCall::callLater([this, alive, exitNode, result]() {
      if (!*alive) {
        return;
      }
      m_busy = false;
      if (!result) {
        kLog.warn("{} tailscale set --exit-node={} failed: {}", m_escalator, exitNode, StringUtils::trim(result.err));
        notifyFailure();
      }
      refresh();
      emitChanged();
    });
  };

  const std::string arg = "--exit-node=" + exitNode;
  // No timeout: the polkit prompt is on the user's clock.
  if (!process::runAsync({m_escalator, m_binary, "set", arg}, std::move(callbacks))) {
    m_busy = false;
    emitChanged();
  }
}

void TailscaleService::notifyFailure() {
  notify::error(
      "Noctalia", i18n::tr("notifications.internal.tailscale"),
      i18n::tr("notifications.internal.tailscale-exit-node-failed")
  );
}

void TailscaleService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// One peer that advertises itself as an exit node.
struct TailscaleExitNode {
  std::string id;     // stable node id — row identity
  std::string name;   // display name
  std::string target; // value handed to `tailscale set --exit-node=`
  bool online = false;
  bool active = false; // currently routing our traffic

  bool operator==(const TailscaleExitNode&) const = default;
};

// Exit node list and connect/disconnect on top of the `tailscale` CLI. Everything
// runs off the main thread and reports back through DeferredCall, so callbacks
// always land on the main loop.
class TailscaleService {
public:
  using ChangeCallback = std::function<void()>;

  TailscaleService();
  ~TailscaleService();

  TailscaleService(const TailscaleService&) = delete;
  TailscaleService& operator=(const TailscaleService&) = delete;

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }
  // Invoked right before a command is re-run under pkexec/run0, so the polkit
  // agent can mark the authentication request as ours.
  void setPrivilegedRunCallback(std::function<void()> callback) { m_privilegedRunCallback = std::move(callback); }

  // Starts periodic polling when the CLI is installed. No-op otherwise.
  void start();

  [[nodiscard]] bool available() const noexcept { return !m_binary.empty(); }
  // The backend is up, i.e. `tailscale status` reports a running state.
  [[nodiscard]] bool running() const noexcept { return m_running; }
  // This machine's own address on the tailnet, empty while it has none.
  [[nodiscard]] const std::string& selfIp() const noexcept { return m_selfIp; }
  [[nodiscard]] const std::vector<TailscaleExitNode>& exitNodes() const noexcept { return m_exitNodes; }
  [[nodiscard]] bool exitNodeActive() const noexcept;
  [[nodiscard]] std::string activeExitNodeName() const;
  // An exit node command is in flight — rows stay disabled until it settles.
  [[nodiscard]] bool busy() const noexcept { return m_busy; }

  void refresh();
  void setEnabled(bool enabled);
  void connectExitNode(const TailscaleExitNode& node);
  void disconnectExitNode();

private:
  void runStatus();
  void applyStatus(const std::string& json);
  // Runs `tailscale <args>`, retrying under polkit when tailscaled refuses the
  // unprivileged call. failureMessage names the notification shown on failure.
  void runCommand(std::vector<std::string> args, std::string failureMessage);
  void runCommandEscalated(std::vector<std::string> args, std::string failureMessage);
  void notifyFailure(const std::string& message);
  void emitChanged();

  std::string m_binary;
  std::string m_escalator;
  std::vector<TailscaleExitNode> m_exitNodes;
  std::string m_selfIp;
  bool m_running = false;
  bool m_busy = false;
  bool m_statusPending = false;
  ChangeCallback m_changeCallback;
  std::function<void()> m_privilegedRunCallback;
  Timer m_pollTimer;
  std::shared_ptr<bool> m_alive;
};

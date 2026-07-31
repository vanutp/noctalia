#include "application.h"
#include "application_internal.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "pipewire/sound_player.h"
#include "render/backend/render_backend.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <unordered_map>

namespace {
  constexpr Logger kLog("app");

  // A reset that outlives this many retries is not a transient GPU hiccup. Back off between
  // attempts so a dead GPU does not spin the loop, and give up rather than retry forever.
  constexpr int kMaxGraphicsRecoveryAttempts = 8;
  constexpr std::chrono::milliseconds kGraphicsRecoveryBaseDelay{250};
  constexpr std::chrono::milliseconds kGraphicsRecoveryMaxDelay{5000};

  std::string_view powerProfileOriginName(PowerProfilesChangeOrigin origin) {
    switch (origin) {
    case PowerProfilesChangeOrigin::Noctalia:
      return "noctalia";
    case PowerProfilesChangeOrigin::External:
      return "external";
    }
    return "external";
  }
} // namespace

void Application::onIconThemeChanged() {
  kLog.info("system icon theme changed; refreshing icon consumers");
  m_bar.reload();
  m_dock.reload();
  m_panelManager.onIconThemeChanged();
  m_notificationToast.requestLayout();
}

void Application::onGraphicsReset(RenderGraphicsResetStatus status) {
  (void)status;
  if (m_graphicsRecoveryScheduled) {
    return;
  }
  m_graphicsRecoveryScheduled = true;
  DeferredCall::callLater([this]() { recoverGraphicsAfterReset(); });
}

void Application::recoverGraphicsAfterReset() {
  m_graphicsRecoveryScheduled = false;
  try {
    // A robust-context reset invalidates the whole share group. Tear down every
    // child before replacing the root, and do it outside the render callback.
    m_lockScreen.prepareForGraphicsReset();
    m_backdrop.prepareForGraphicsReset();
    m_thumbnailService.abandonGpuResources();
    m_sharedTextureCache.abandonGpuResources();
    m_asyncTextureCache.abandonGpuResources();
    m_renderContext.prepareForGraphicsReset();

    m_glShared.recreateRootContext();
    m_renderContext.restoreAfterGraphicsReset(m_glShared);
    m_backdrop.restoreAfterGraphicsReset();

    m_sharedTextureCache.reloadResidentTextures();
    m_asyncTextureCache.reloadResidentTextures();
    m_renderContext.finishGraphicsResetRecovery();
    m_backdrop.finishGraphicsResetRecovery();
    m_thumbnailService.invalidateGpuResources(m_renderContext.backend().textureManager());
    m_wallpaper.onGpuResourcesInvalidated();
    m_backdrop.onGpuResourcesInvalidated();
    m_lockScreen.onGpuResourcesInvalidated();
    m_trayMenu.requestLayout();
    m_settingsWindow.requestRedraw();
    m_screenCorners.requestRedraw();
    requestAllSurfacesRedraw();
    m_graphicsRecoveryAttempts = 0;
    kLog.info("graphics context recovery completed");
  } catch (const std::exception& e) {
    ++m_graphicsRecoveryAttempts;
    if (m_graphicsRecoveryAttempts >= kMaxGraphicsRecoveryAttempts) {
      kLog.error(
          "graphics context recovery failed after {} attempts; rendering stays disabled: {}",
          m_graphicsRecoveryAttempts, e.what()
      );
      return;
    }

    const auto delay =
        std::min(kGraphicsRecoveryBaseDelay * (1 << (m_graphicsRecoveryAttempts - 1)), kGraphicsRecoveryMaxDelay);
    kLog.warn(
        "graphics context recovery failed (attempt {}); retrying in {}ms: {}", m_graphicsRecoveryAttempts,
        delay.count(), e.what()
    );
    m_graphicsRecoveryScheduled = true;
    m_graphicsRecoveryTimer.start(delay, [this]() { recoverGraphicsAfterReset(); });
  }
}

void Application::requestAllSurfacesRedraw() {
  DeferredCall::callLater([this]() {
    m_bar.requestRedraw();
    m_dock.requestRedraw();
    m_desktopWidgetsController.requestRedraw();
    m_panelManager.requestRedraw();
    m_notificationToast.requestRedraw();
    m_osdOverlay.requestRedraw();
    m_lockScreen.requestLayout();
    m_colorPickerDialogPopup.requestRedraw();
    m_glyphPickerDialogPopup.requestRedraw();
    m_fileDialogPopup.requestRedraw();
  });
}

void Application::onUpowerStateChangedForHooks() {
  if (m_upowerService == nullptr) {
    return;
  }
  const auto& state = m_upowerService->state();
  const auto plugged = state.isPresent ? batteryStatePlugged(state.state) : std::nullopt;
  if (plugged.has_value()) {
    if (m_prevBatteryPluggedForEvents.has_value()
        && *m_prevBatteryPluggedForEvents != *plugged
        && m_soundPlayer != nullptr) {
      m_soundPlayer->play(*plugged ? "power-plug" : "power-unplug");
    }
    m_prevBatteryPluggedForEvents = plugged;
  } else if (!state.isPresent) {
    m_prevBatteryPluggedForEvents.reset();
  }
  for (const auto& event : m_batteryHookState.update(state)) {
    if (event.env.empty()) {
      m_hookManager.fire(event.kind);
    } else {
      m_hookManager.fire(event.kind, event.env);
    }
  }
}

void Application::onNetworkStateChangedForEvents(const NetworkState& state, NetworkChangeOrigin origin) {
  if (!m_prevWirelessEnabledForEvents.has_value()) {
    m_prevWirelessEnabledForEvents = state.wirelessEnabled;
    return;
  }
  const bool prev = *m_prevWirelessEnabledForEvents;
  if (prev != state.wirelessEnabled) {
    if (origin != NetworkChangeOrigin::Noctalia) {
      m_osdOverlay.show(wifiOsdContent(state.wirelessEnabled));
    }
    if (state.wirelessEnabled) {
      m_hookManager.fire(HookKind::WifiEnabled);
    } else {
      m_hookManager.fire(HookKind::WifiDisabled);
    }
  }
  m_prevWirelessEnabledForEvents = state.wirelessEnabled;
}

void Application::onBluetoothStateChangedForEvents(const BluetoothState& state, BluetoothStateChangeOrigin origin) {
  if (!m_prevBluetoothPoweredForEvents.has_value()) {
    m_prevBluetoothPoweredForEvents = state.powered;
    return;
  }
  const bool prev = *m_prevBluetoothPoweredForEvents;
  if (prev != state.powered) {
    if (origin != BluetoothStateChangeOrigin::Noctalia) {
      m_osdOverlay.show(bluetoothOsdContent(state.powered));
    }
    if (state.powered) {
      m_hookManager.fire(HookKind::BluetoothEnabled);
    } else {
      m_hookManager.fire(HookKind::BluetoothDisabled);
    }
  }
  m_prevBluetoothPoweredForEvents = state.powered;
}

void Application::onBluetoothDevicesChangedForEvents(const std::vector<BluetoothDeviceInfo>& devices) {
  std::unordered_map<std::string, std::string> connected;
  for (const auto& device : devices) {
    if (device.connected) {
      connected.emplace(device.path, device.alias);
    }
  }

  // The first callback is the initial BlueZ enumeration: seed silently so devices
  // already connected at startup do not notify.
  if (!m_prevBluetoothConnectedForEvents.has_value()) {
    m_prevBluetoothConnectedForEvents = std::move(connected);
    return;
  }
  const auto& prev = *m_prevBluetoothConnectedForEvents;

  auto notify = [this, &devices](const std::string& path, const std::string& alias, bool isConnect) {
    const auto it = std::ranges::find(devices, path, &BluetoothDeviceInfo::path);
    const auto kind = it != devices.end() ? it->kind : BluetoothDeviceKind::Unknown;
    const std::string label = alias.empty() ? i18n::tr("notifications.internal.bluetooth-device") : alias;
    m_notificationManager.addInternal(
        i18n::tr("notifications.internal.bluetooth"),
        i18n::tr(
            isConnect ? "notifications.internal.bluetooth-connected" : "notifications.internal.bluetooth-disconnected"
        ),
        label, Urgency::Low, kDefaultNotificationTimeout, std::string("noctalia-glyph:") + bluetoothDeviceGlyph(kind)
    );
  };

  for (const auto& [path, alias] : connected) {
    if (!prev.contains(path)) {
      notify(path, alias, true);
    }
  }
  for (const auto& [path, alias] : prev) {
    if (!connected.contains(path)) {
      notify(path, alias, false);
    }
  }

  m_prevBluetoothConnectedForEvents = std::move(connected);
}

void Application::onPowerProfileChangedForEvents(const PowerProfilesState& state, PowerProfilesChangeOrigin origin) {
  if (state.activeProfile.empty()) {
    return;
  }
  if (!m_prevPowerProfileActiveForEvents.has_value()) {
    m_prevPowerProfileActiveForEvents = state.activeProfile;
    return;
  }
  const std::string prev = *m_prevPowerProfileActiveForEvents;
  if (prev != state.activeProfile) {
    if (origin != PowerProfilesChangeOrigin::Noctalia) {
      m_osdOverlay.show(powerProfileOsdContent(state.activeProfile));
    }
    m_hookManager.fire(
        HookKind::PowerProfileChanged,
        {{"NOCTALIA_POWER_PROFILE", state.activeProfile},
         {"NOCTALIA_POWER_PROFILE_PREVIOUS", prev},
         {"NOCTALIA_POWER_PROFILE_ORIGIN", std::string(powerProfileOriginName(origin))}}
    );
  }
  m_prevPowerProfileActiveForEvents = state.activeProfile;
}

#include "dbus/dock/dock_badge_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "util/string_utils.h"

#include <algorithm>
#include <glib.h>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("dock-badges");
  const sdbus::ServiceName kUnityName{"com.canonical.Unity"};
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";

  std::optional<std::string> desktopIdFromUri(std::string_view uri) {
    constexpr std::string_view prefix = "application://";
    if (!uri.starts_with(prefix)) {
      return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&g_free)> decoded{
        g_uri_unescape_string(std::string(uri.substr(prefix.size())).c_str(), nullptr), &g_free
    };
    if (!decoded) {
      return std::nullopt;
    }
    std::string id(decoded.get());
    if (id.find_first_of("/\\?#%") != std::string::npos || id.contains('\0')) {
      return std::nullopt;
    }
    id = StringUtils::toLower(id);
    if (!id.ends_with(".desktop")) {
      return std::nullopt;
    }
    id.resize(id.size() - std::string_view(".desktop").size());
    return id.empty() ? std::nullopt : std::optional{std::move(id)};
  }
} // namespace

DockBadgeService::DockBadgeService(SessionBus& bus) : m_bus(bus) {}

void DockBadgeService::start() {
  if (m_started) {
    return;
  }

  auto updateMatch = m_bus.connection().addMatch(
      "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'",
      [this](sdbus::Message message) {
        try {
          std::string uri;
          std::map<std::string, sdbus::Variant> properties;
          message >> uri >> properties;
          update(message.getSender(), uri, properties);
        } catch (const sdbus::Error& error) {
          kLog.debug("ignoring malformed dock badge update: {}", error.what());
        }
      },
      sdbus::return_slot
  );
  auto dbusProxy = sdbus::createProxy(m_bus.connection(), kDbusName, kDbusPath);
  dbusProxy->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
        // Losing a well-known name does not mean the publisher's connection has closed.
        if (name.starts_with(':') && name == oldOwner && newOwner.empty()) {
          removeOwner(oldOwner);
        }
      });
  m_updateMatch = std::move(updateMatch);
  m_dbusProxy = std::move(dbusProxy);
  // libunity clients watch this name and republish their current state when a compatible dock appears.
  // Coexist with another dock without replacing its name ownership.
  try {
    m_bus.connection().requestName(kUnityName);
    m_ownsUnityName = true;
  } catch (const sdbus::Error& error) {
    kLog.debug("Unity badge name unavailable; listening for broadcasts: {}", error.what());
  }
  m_started = true;
}

DockBadgeService::~DockBadgeService() {
  m_dbusProxy.reset();
  m_updateMatch.reset();
  if (m_ownsUnityName) {
    try {
      m_bus.connection().releaseName(kUnityName);
    } catch (const sdbus::Error& error) {
      kLog.debug("could not release Unity badge name: {}", error.what());
    }
  }
}

void DockBadgeService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

std::unordered_map<std::string, std::size_t> DockBadgeService::counts() const {
  std::unordered_map<std::string, std::size_t> result;
  for (const auto& [id, sources] : m_sources) {
    const auto& source = sources.back();
    const auto count =
        std::min<std::uint64_t>(static_cast<std::uint64_t>(source.count), std::numeric_limits<std::size_t>::max());
    result.emplace(id, source.visible ? static_cast<std::size_t>(count) : 0);
  }
  return result;
}

void DockBadgeService::update(
    const std::string& owner, const std::string& appUri, const std::map<std::string, sdbus::Variant>& properties
) {
  const auto id = desktopIdFromUri(appUri);
  if (owner.empty() || !id.has_value()) {
    return;
  }
  std::optional<std::int64_t> count;
  std::optional<bool> visible;
  if (const auto it = properties.find("count");
      it != properties.end() && it->second.containsValueOfType<std::int64_t>()) {
    count = std::max<std::int64_t>(0, it->second.get<std::int64_t>());
  }
  if (const auto it = properties.find("count-visible");
      it != properties.end() && it->second.containsValueOfType<bool>()) {
    visible = it->second.get<bool>();
  }
  // Progress/urgency updates and malformed properties must not disable history fallback.
  if (!count.has_value() && !visible.has_value()) {
    return;
  }
  auto& sources = m_sources[*id];
  const auto it = std::ranges::find(sources, owner, &Source::owner);
  Source source{.owner = owner};
  if (it != sources.end()) {
    source = std::move(*it);
    sources.erase(it);
  }
  if (count.has_value()) {
    source.count = *count;
  }
  if (visible.has_value()) {
    source.visible = *visible;
  }
  sources.push_back(std::move(source));
  emitChanged();
}

void DockBadgeService::removeOwner(const std::string& owner) {
  bool changed = false;
  for (auto& [id, sources] : m_sources) {
    changed = std::erase_if(sources, [&](const auto& source) { return source.owner == owner; }) > 0 || changed;
  }
  std::erase_if(m_sources, [](const auto& entry) { return entry.second.empty(); });
  if (changed) {
    emitChanged();
  }
}

void DockBadgeService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

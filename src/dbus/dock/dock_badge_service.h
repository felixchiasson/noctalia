#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <unordered_map>
#include <vector>

class SessionBus;

// App-reported badge counts from com.canonical.Unity.LauncherEntry.Update.
// A present zero is authoritative: callers must not substitute history for it.
class DockBadgeService {
public:
  using ChangeCallback = std::function<void()>;

  explicit DockBadgeService(SessionBus& bus);
  ~DockBadgeService();
  DockBadgeService(const DockBadgeService&) = delete;
  DockBadgeService& operator=(const DockBadgeService&) = delete;

  void start();
  void setChangeCallback(ChangeCallback callback);
  std::unordered_map<std::string, std::size_t> counts() const;

private:
  struct Source {
    std::string owner;
    std::int64_t count = 0;
    bool visible = false;
  };

  void
  update(const std::string& owner, const std::string& appUri, const std::map<std::string, sdbus::Variant>& properties);
  void removeOwner(const std::string& owner);
  void emitChanged();

  SessionBus& m_bus;
  // Multiple processes may publish for the same app; the most recent count source wins.
  std::unordered_map<std::string, std::vector<Source>> m_sources;
  ChangeCallback m_changeCallback;
  bool m_started = false;
  bool m_ownsUnityName = false;
  sdbus::Slot m_updateMatch;
  std::unique_ptr<sdbus::IProxy> m_dbusProxy;
};

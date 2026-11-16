#include "notification/app_notification_counts.h"

#include "dbus/dock/dock_badge_service.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "util/string_utils.h"

#include <string_view>

std::unordered_map<std::string, std::size_t>
appNotificationCounts(const NotificationManager* notifications, const DockBadgeService* dockBadges) {
  std::unordered_map<std::string, std::size_t> counts;
  if (notifications != nullptr) {
    for (const auto& history : notifications->history()) {
      if (history.seen) {
        continue;
      }
      const auto& notification = history.notification;
      // An explicit desktop-entry hint owns identity; appName is used only when it is absent.
      const std::string_view key = notification.desktopEntry.has_value() && !notification.desktopEntry->empty()
          ? std::string_view(*notification.desktopEntry)
          : std::string_view(notification.appName);
      if (const auto entry = findNotificationDesktopEntry(key)) {
        ++counts[StringUtils::toLower(entry->id)];
      }
    }
  }
  if (dockBadges != nullptr) {
    for (const auto& [id, count] : dockBadges->counts()) {
      counts.insert_or_assign(id, count);
    }
  }
  return counts;
}

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

class DockBadgeService;
class NotificationManager;

// App-reported counts (including zero/hidden) take precedence over unseen history.
std::unordered_map<std::string, std::size_t>
appNotificationCounts(const NotificationManager* notifications, const DockBadgeService* dockBadges);

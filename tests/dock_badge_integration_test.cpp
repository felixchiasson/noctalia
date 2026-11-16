#include "core/deferred_call.h"
#include "dbus/dock/dock_badge_service.h"
#include "dbus/session_bus.h"
#include "notification/app_notification_counts.h"
#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  template <typename Predicate> bool drainUntil(SessionBus& bus, Predicate&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      bus.processPendingEvents();
      if (std::invoke(ready)) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }

  void emitUpdate(
      sdbus::IObject& publisher, const std::string& uri, const std::map<std::string, sdbus::Variant>& properties
  ) {
    publisher.emitSignal("Update").onInterface("com.canonical.Unity.LauncherEntry").withArguments(uri, properties);
  }

  struct DesktopFixture {
    DesktopFixture() {
      char path[] = "/tmp/noctalia-dock-badge-XXXXXX";
      const char* created = ::mkdtemp(path);
      TEST_CHECK(created != nullptr);
      directory = created;
      std::filesystem::create_directories(directory / "applications");
      for (const auto* name : {"org.example.Mail", "org.example.Chat"}) {
        std::ofstream out(directory / "applications" / (std::string(name) + ".desktop"));
        out << "[Desktop Entry]\nType=Application\nName=" << name << "\nExec=true\n";
      }
      TEST_CHECK(::setenv("XDG_DATA_HOME", directory.c_str(), 1) == 0);
      TEST_CHECK(::setenv("XDG_DATA_DIRS", directory.c_str(), 1) == 0);
    }
    ~DesktopFixture() { std::filesystem::remove_all(directory); }
    std::filesystem::path directory;
  };
} // namespace

int main() {
  DesktopFixture fixture;
  SessionBus bus;
  NotificationManager manager;
  const auto first = manager.addOrReplace(
      {.appName = "Mail", .summary = "first", .timeout = 0, .desktopEntry = "org.example.Mail.desktop"}
  );
  manager.addOrReplace({.appName = "Mail", .summary = "second", .timeout = 0, .desktopEntry = "org.example.Mail"});
  manager.addOrReplace({.appName = "org.example.Chat", .summary = "chat", .timeout = 0});
  // An explicit but unknown hint must not be reassigned using the app name.
  manager.addOrReplace(
      {.appName = "org.example.Chat", .summary = "other", .timeout = 0, .desktopEntry = "missing.desktop"}
  );
  auto history = appNotificationCounts(&manager, nullptr);
  TEST_CHECK(history.at("org.example.mail") == 2);
  TEST_CHECK(history.at("org.example.chat") == 1);
  TEST_CHECK(history.size() == 2);

  int historyChanges = 0;
  manager.setHistoryChangeCallback([&] { ++historyChanges; });
  const auto flushHistoryChanges = [] {
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
  };
  flushHistoryChanges();
  TEST_CHECK(historyChanges == 1); // A burst of history updates is coalesced.

  auto service = std::make_unique<DockBadgeService>(bus);
  int changes = 0;
  service->setChangeCallback([&] { ++changes; });
  TEST_CHECK(service->counts().empty());
  service->start();
  service->start(); // Starting twice must not duplicate subscriptions.
  auto client = sdbus::createSessionBusConnection();
  auto publisher = sdbus::createObject(*client, sdbus::ObjectPath{"/arbitrary/dock/path"});
  client->enterEventLoopAsync();
  const std::string mail = "application://org.example.Mail.desktop";
  const auto counts = [&] { return appNotificationCounts(&manager, service.get()); };
  const auto send = [&](const std::map<std::string, sdbus::Variant>& properties) {
    const auto previous = changes;
    emitUpdate(*publisher, mail, properties);
    TEST_CHECK(drainUntil(bus, [&] { return changes > previous; }));
    TEST_CHECK(changes == previous + 1);
  };

  // Progress-only messages do not claim an unread count. A subsequent valid update is a delivery barrier.
  emitUpdate(*publisher, mail, {{"progress", sdbus::Variant{0.5}}, {"progress-visible", sdbus::Variant{true}}});
  emitUpdate(*publisher, "application://barrier.desktop", {{"count-visible", sdbus::Variant{false}}});
  TEST_CHECK(drainUntil(bus, [&] { return service->counts().contains("barrier"); }));
  TEST_CHECK(!service->counts().contains("org.example.mail"));
  TEST_CHECK(counts().at("org.example.mail") == 2);

  send({{"count", sdbus::Variant{std::int64_t{7}}}, {"count-visible", sdbus::Variant{true}}});
  TEST_CHECK(counts().at("org.example.mail") == 7); // App count replaces, rather than adds to, history.
  TEST_CHECK(counts().at("org.example.chat") == 1); // Fallback is decided per app.
  send({{"count", sdbus::Variant{std::int64_t{11}}}});
  TEST_CHECK(counts().at("org.example.mail") == 11); // Partial count update retains visibility.
  send({{"count-visible", sdbus::Variant{false}}});
  TEST_CHECK(counts().at("org.example.mail") == 0); // Hidden counts suppress stale history.
  send({{"count-visible", sdbus::Variant{true}}});
  TEST_CHECK(counts().at("org.example.mail") == 11);
  send({{"count", sdbus::Variant{std::int64_t{0}}}});
  TEST_CHECK(counts().at("org.example.mail") == 0);
  send({{"count", sdbus::Variant{std::int64_t{-4}}}});
  TEST_CHECK(counts().at("org.example.mail") == 0);

  // Malformed signatures, variants, URI schemes and paths do not overwrite a valid app count.
  const auto beforeMalformed = changes;
  publisher->emitSignal("Update").onInterface("com.canonical.Unity.LauncherEntry").withArguments(42);
  emitUpdate(*publisher, mail, {{"count", sdbus::Variant{std::string{"bad"}}}, {"count-visible", sdbus::Variant{42}}});
  for (const auto* uri :
       {"file://org.example.Mail.desktop", "application:///org.example.Mail.desktop", "application://.desktop",
        "application://org.example.Mail.desktop?other"}) {
    emitUpdate(*publisher, uri, {{"count", sdbus::Variant{std::int64_t{99}}}, {"count-visible", sdbus::Variant{true}}});
  }
  emitUpdate(*publisher, "application://barrier.desktop", {{"count", sdbus::Variant{std::int64_t{1}}}});
  TEST_CHECK(drainUntil(bus, [&] { return changes > beforeMalformed; }));
  TEST_CHECK(counts().at("org.example.mail") == 0);
  TEST_CHECK(service->counts().size() == 2);

  // Normalize the desktop suffix, casing, and URI escaping independently of catalog availability.
  const auto previous = changes;
  emitUpdate(*publisher, "application://ORG.EXAMPLE.%4Dail.desktop", {{"count", sdbus::Variant{std::int64_t{5}}}});
  TEST_CHECK(drainUntil(bus, [&] { return changes > previous; }));
  TEST_CHECK(counts().at("org.example.mail") == 5);

  // Losing a well-known name while the connection stays alive must not clear its state.
  const sdbus::ServiceName publisherName{"org.example.BadgePublisher"};
  client->requestName(publisherName);
  client->releaseName(publisherName);
  send({{"count", sdbus::Variant{std::int64_t{6}}}});
  TEST_CHECK(counts().at("org.example.mail") == 6);

  auto secondClient = sdbus::createSessionBusConnection();
  auto secondPublisher = sdbus::createObject(*secondClient, sdbus::ObjectPath{"/another/path"});
  secondClient->enterEventLoopAsync();
  emitUpdate(
      *secondPublisher, mail, {{"count", sdbus::Variant{std::int64_t{12}}}, {"count-visible", sdbus::Variant{true}}}
  );
  TEST_CHECK(drainUntil(bus, [&] { return counts().at("org.example.mail") == 12; }));
  secondPublisher.reset();
  secondClient.reset();
  TEST_CHECK(drainUntil(bus, [&] { return counts().at("org.example.mail") == 6; }));

  // Removal returns to history only when no live publisher remains for this app.
  publisher.reset();
  client.reset();
  TEST_CHECK(drainUntil(bus, [&] { return service->counts().empty(); }));
  TEST_CHECK(counts().at("org.example.mail") == 2);
  const auto beforeRemoval = historyChanges;
  manager.removeHistoryEntry(first);
  flushHistoryChanges();
  TEST_CHECK(historyChanges > beforeRemoval); // Other unread entries remain, but this app's count changed.
  TEST_CHECK(manager.hasUnreadNotificationHistory());
  TEST_CHECK(counts().at("org.example.mail") == 1);
  manager.markNotificationHistorySeen();
  TEST_CHECK(counts().empty());

  // Service teardown releases its name and signal subscriptions on the shared bus.
  service.reset();
  auto nameOwner = sdbus::createSessionBusConnection();
  nameOwner->requestName(sdbus::ServiceName{"com.canonical.Unity"});
  auto listener = std::make_unique<DockBadgeService>(bus); // Coexists with an existing dock.
  listener->start();
  auto thirdClient = sdbus::createSessionBusConnection();
  auto thirdPublisher = sdbus::createObject(*thirdClient, sdbus::ObjectPath{"/third/path"});
  thirdClient->enterEventLoopAsync();
  emitUpdate(
      *thirdPublisher, mail, {{"count", sdbus::Variant{std::int64_t{3}}}, {"count-visible", sdbus::Variant{true}}}
  );
  TEST_CHECK(drainUntil(bus, [&] { return listener->counts().contains("org.example.mail"); }));
  TEST_CHECK(listener->counts().at("org.example.mail") == 3);
  manager.addOrReplace({.appName = "Mail", .summary = "new", .timeout = 0, .desktopEntry = "org.example.Mail"});
  manager.markNotificationHistorySeen();
  TEST_CHECK(appNotificationCounts(&manager, listener.get()).at("org.example.mail") == 3);
  flushHistoryChanges();
  const auto beforeClear = historyChanges;
  manager.clearHistory();
  flushHistoryChanges();
  TEST_CHECK(historyChanges > beforeClear); // Seen-only history still notifies consumers when cleared.
  TEST_CHECK(appNotificationCounts(&manager, listener.get()).at("org.example.mail") == 3);
}

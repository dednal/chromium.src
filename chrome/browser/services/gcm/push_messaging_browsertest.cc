// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <string>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/message_loop/message_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/notifications/notification_test_util.h"
#include "chrome/browser/notifications/platform_notification_service_impl.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/services/gcm/fake_gcm_profile_service.h"
#include "chrome/browser/services/gcm/gcm_profile_service_factory.h"
#include "chrome/browser/services/gcm/push_messaging_application_id.h"
#include "chrome/browser/services/gcm/push_messaging_constants.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/gcm_driver/gcm_client.h"
#include "components/infobars/core/confirm_infobar_delegate.h"
#include "components/infobars/core/infobar.h"
#include "components/infobars/core/infobar_manager.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test_utils.h"
#include "ui/base/window_open_disposition.h"

namespace gcm {

namespace {
// Responds to a confirm infobar by accepting or cancelling it. Responds to at
// most one infobar.
class InfoBarResponder : public infobars::InfoBarManager::Observer {
 public:
  InfoBarResponder(Browser* browser, bool accept)
      : infobar_service_(InfoBarService::FromWebContents(
            browser->tab_strip_model()->GetActiveWebContents())),
        accept_(accept),
        has_observed_(false) {
    infobar_service_->AddObserver(this);
  }

  ~InfoBarResponder() override { infobar_service_->RemoveObserver(this); }

  // infobars::InfoBarManager::Observer
  void OnInfoBarAdded(infobars::InfoBar* infobar) override {
    if (has_observed_)
      return;
    has_observed_ = true;
    ConfirmInfoBarDelegate* delegate =
        infobar->delegate()->AsConfirmInfoBarDelegate();
    DCHECK(delegate);

    // Respond to the infobar asynchronously, like a person.
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(
            &InfoBarResponder::Respond, base::Unretained(this), delegate));
  }

 private:
  void Respond(ConfirmInfoBarDelegate* delegate) {
    if (accept_) {
      delegate->Accept();
    } else {
      delegate->Cancel();
    }
  }

  InfoBarService* infobar_service_;
  bool accept_;
  bool has_observed_;
};

// Class to instantiate on the stack that is meant to be used with
// FakeGCMProfileService. The ::Run() method follows the signature of
// FakeGCMProfileService::UnregisterCallback.
class UnregistrationCallback {
 public:
  UnregistrationCallback() : done_(false) {}

  void Run(const std::string& app_id) {
    app_id_ = app_id;
    done_ = true;
    base::MessageLoop::current()->Quit();
  }

  void WaitUntilSatisfied() {
    while (!done_)
      content::RunMessageLoop();
  }

  const std::string& app_id() {
    return app_id_;
  }

 private:
  bool done_;
  std::string app_id_;
};

// Class to instantiate on the stack that is meant to be used with
// StubNotificationUIManager::SetNotificationAddedCallback. Mind that Run()
// might be invoked prior to WaitUntilSatisfied() being called.
class NotificationAddedCallback {
 public:
  NotificationAddedCallback() : done_(false), waiting_(false) {}

  void Run() {
    done_ = true;
    if (waiting_)
      base::MessageLoop::current()->Quit();
  }

  void WaitUntilSatisfied() {
    if (done_)
      return;

    waiting_ = true;
    while (!done_)
      content::RunMessageLoop();
  }

 private:
  bool done_;
  bool waiting_;
};

}  // namespace

class PushMessagingBrowserTest : public InProcessBrowserTest {
 public:
  PushMessagingBrowserTest() : gcm_service_(nullptr) {}
  ~PushMessagingBrowserTest() override {}

  // InProcessBrowserTest:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    command_line->AppendSwitch(
        switches::kEnableExperimentalWebPlatformFeatures);
    command_line->AppendSwitch(
        switches::kEnablePushMessagePayload);

    InProcessBrowserTest::SetUpCommandLine(command_line);
  }

  // InProcessBrowserTest:
  void SetUp() override {
    https_server_.reset(new net::SpawnedTestServer(
        net::SpawnedTestServer::TYPE_HTTPS,
        net::BaseTestServer::SSLOptions(
            net::BaseTestServer::SSLOptions::CERT_OK),
        base::FilePath(FILE_PATH_LITERAL("chrome/test/data/"))));
    ASSERT_TRUE(https_server_->Start());

#if defined(ENABLE_NOTIFICATIONS)
    notification_manager_.reset(new StubNotificationUIManager);
    notification_service()->SetNotificationUIManagerForTesting(
        notification_manager());
#endif

    InProcessBrowserTest::SetUp();
  }

  // InProcessBrowserTest:
  void SetUpOnMainThread() override {
    gcm_service_ = static_cast<FakeGCMProfileService*>(
        GCMProfileServiceFactory::GetInstance()->SetTestingFactoryAndUse(
            browser()->profile(), &FakeGCMProfileService::Build));
    gcm_service_->set_collect(true);

    LoadTestPage();

    InProcessBrowserTest::SetUpOnMainThread();
  }

  // InProcessBrowserTest:
  void TearDown() override {
#if defined(ENABLE_NOTIFICATIONS)
    notification_service()->SetNotificationUIManagerForTesting(nullptr);
#endif

    InProcessBrowserTest::TearDown();
  }

  void LoadTestPage(const std::string& path) {
    ui_test_utils::NavigateToURL(browser(), https_server_->GetURL(path));
  }

  void LoadTestPage() {
    LoadTestPage(GetTestURL());
  }

  bool RunScript(const std::string& script, std::string* result) {
    return RunScript(script, result, nullptr);
  }

  bool RunScript(const std::string& script, std::string* result,
                 content::WebContents* web_contents) {
    if (!web_contents)
      web_contents = browser()->tab_strip_model()->GetActiveWebContents();
    return content::ExecuteScriptAndExtractString(web_contents->GetMainFrame(),
                                                  script,
                                                  result);
  }

  void TryToRegisterSuccessfully(
      const std::string& expected_push_registration_id);

  PushMessagingApplicationId GetServiceWorkerAppId(
      int64 service_worker_registration_id);

  net::SpawnedTestServer* https_server() const { return https_server_.get(); }

  FakeGCMProfileService* gcm_service() const { return gcm_service_; }

#if defined(ENABLE_NOTIFICATIONS)
  StubNotificationUIManager* notification_manager() const {
    return notification_manager_.get();
  }

  PlatformNotificationServiceImpl* notification_service() const {
    return PlatformNotificationServiceImpl::GetInstance();
  }
#endif

  PushMessagingServiceImpl* push_service() {
    return static_cast<PushMessagingServiceImpl*>(
        gcm_service_->push_messaging_service());
  }

 protected:
  virtual std::string GetTestURL() {
    return "files/push_messaging/test.html";
  }

 private:
  scoped_ptr<net::SpawnedTestServer> https_server_;
  FakeGCMProfileService* gcm_service_;
  scoped_ptr<StubNotificationUIManager> notification_manager_;

  DISALLOW_COPY_AND_ASSIGN(PushMessagingBrowserTest);
};

class PushMessagingBadManifestBrowserTest : public PushMessagingBrowserTest {
  std::string GetTestURL() override {
    return "files/push_messaging/test_bad_manifest.html";
  }
};

IN_PROC_BROWSER_TEST_F(PushMessagingBadManifestBrowserTest,
                       RegisterFailsNotVisibleMessages) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);
  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ("AbortError - Registration failed - permission denied",
            script_result);
}

void PushMessagingBrowserTest::TryToRegisterSuccessfully(
    const std::string& expected_push_registration_id) {
  std::string script_result;

  EXPECT_TRUE(RunScript("registerServiceWorker()", &script_result));
  EXPECT_EQ("ok - service worker registered", script_result);

  InfoBarResponder accepting_responder(browser(), true);
  EXPECT_TRUE(RunScript("requestNotificationPermission()", &script_result));
  EXPECT_EQ("permission status - granted", script_result);

  EXPECT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ(std::string(kPushMessagingEndpoint) + " - "
            + expected_push_registration_id, script_result);
}

PushMessagingApplicationId PushMessagingBrowserTest::GetServiceWorkerAppId(
    int64 service_worker_registration_id) {
  GURL origin = https_server()->GetURL("").GetOrigin();
  PushMessagingApplicationId application_id = PushMessagingApplicationId::Get(
      browser()->profile(), origin, service_worker_registration_id);
  EXPECT_TRUE(application_id.IsValid());
  return application_id;
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest,
                       RegisterSuccessNotificationsGranted) {
  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest,
                       RegisterSuccessNotificationsPrompt) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  InfoBarResponder accepting_responder(browser(), true);
  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ(std::string(kPushMessagingEndpoint) + " - 1-0", script_result);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest,
                       RegisterFailureNotificationsBlocked) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  InfoBarResponder cancelling_responder(browser(), false);
  ASSERT_TRUE(RunScript("requestNotificationPermission();", &script_result));
  ASSERT_EQ("permission status - denied", script_result);

  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ("AbortError - Registration failed - permission denied",
            script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, RegisterFailureNoManifest) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  InfoBarResponder accepting_responder(browser(), true);
  ASSERT_TRUE(RunScript("requestNotificationPermission();", &script_result));
  ASSERT_EQ("permission status - granted", script_result);

  ASSERT_TRUE(RunScript("removeManifest()", &script_result));
  ASSERT_EQ("manifest removed", script_result);

  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ("AbortError - Registration failed - no sender id provided",
            script_result);
}

// TODO(johnme): Test registering from a worker - see https://crbug.com/437298.

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, RegisterPersisted) {
  std::string script_result;

  // First, test that Service Worker registration IDs are assigned in order of
  // registering the Service Workers, and the (fake) push registration ids are
  // assigned in order of push registration (even when these orders are
  // different).

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);
  PushMessagingApplicationId app_id_sw0 = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id_sw0.app_id_guid(), gcm_service()->last_registered_app_id());

  LoadTestPage("files/push_messaging/subscope1/test.html");
  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  LoadTestPage("files/push_messaging/subscope2/test.html");
  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  // Note that we need to reload the page after registering, otherwise
  // navigator.serviceWorker.ready is going to be resolved with the parent
  // Service Worker which still controls the page.
  LoadTestPage("files/push_messaging/subscope2/test.html");
  TryToRegisterSuccessfully("1-1" /* expected_push_registration_id */);
  PushMessagingApplicationId app_id_sw2 = GetServiceWorkerAppId(2LL);
  EXPECT_EQ(app_id_sw2.app_id_guid(), gcm_service()->last_registered_app_id());

  LoadTestPage("files/push_messaging/subscope1/test.html");
  TryToRegisterSuccessfully("1-2" /* expected_push_registration_id */);
  PushMessagingApplicationId app_id_sw1 = GetServiceWorkerAppId(1LL);
  EXPECT_EQ(app_id_sw1.app_id_guid(), gcm_service()->last_registered_app_id());

  // Now test that the Service Worker registration IDs and push registration IDs
  // generated above were persisted to SW storage, by checking that they are
  // unchanged despite requesting them in a different order.
  // TODO(johnme): Ideally we would restart the browser at this point to check
  // they were persisted to disk, but that's not currently possible since the
  // test server uses random port numbers for each test (even PRE_Foo and Foo),
  // so we wouldn't be able to load the test pages with the same origin.

  LoadTestPage("files/push_messaging/subscope1/test.html");
  TryToRegisterSuccessfully("1-2" /* expected_push_registration_id */);
  EXPECT_EQ(app_id_sw1.app_id_guid(), gcm_service()->last_registered_app_id());

  LoadTestPage("files/push_messaging/subscope2/test.html");
  TryToRegisterSuccessfully("1-1" /* expected_push_registration_id */);
  EXPECT_EQ(app_id_sw1.app_id_guid(), gcm_service()->last_registered_app_id());

  LoadTestPage();
  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);
  EXPECT_EQ(app_id_sw1.app_id_guid(), gcm_service()->last_registered_app_id());
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, PushEventSuccess) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("false - is not controlled", script_result);

  LoadTestPage();  // Reload to become controlled.

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("true - is controlled", script_result);

  GCMClient::IncomingMessage message;
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result));
  EXPECT_EQ("testdata", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, PushEventNoServiceWorker) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("false - is not controlled", script_result);

  LoadTestPage();  // Reload to become controlled.

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("true - is controlled", script_result);

  // Unregister service worker. Sending a message should now fail.
  ASSERT_TRUE(RunScript("unregisterServiceWorker()", &script_result));
  ASSERT_EQ("service worker unregistration status: true", script_result);

  // When the push service will receive it next message, given that there is no
  // SW available, it should unregister |app_id|.
  UnregistrationCallback callback;
  gcm_service()->SetUnregisterCallback(base::Bind(&UnregistrationCallback::Run,
                                                  base::Unretained(&callback)));

  GCMClient::IncomingMessage message;
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);

  callback.WaitUntilSatisfied();
  EXPECT_EQ(app_id.app_id_guid(), callback.app_id());

  // No push data should have been received.
  ASSERT_TRUE(RunScript("resultQueue.popImmediately()", &script_result));
  EXPECT_EQ("null", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, PushEventNoPermission) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("false - is not controlled", script_result);

  LoadTestPage();  // Reload to become controlled.

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("true - is controlled", script_result);

  // Revoke Push permission.
  browser()->profile()->GetHostContentSettingsMap()->
      ClearSettingsForOneType(CONTENT_SETTINGS_TYPE_PUSH_MESSAGING);

  // When the push service will receive its next message, given that there is no
  // SW available, it should unregister |app_id|.
  UnregistrationCallback callback;
  gcm_service()->SetUnregisterCallback(base::Bind(&UnregistrationCallback::Run,
                                                  base::Unretained(&callback)));

  GCMClient::IncomingMessage message;
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);

  callback.WaitUntilSatisfied();
  EXPECT_EQ(app_id.app_id_guid(), callback.app_id());

  // No push data should have been received.
  ASSERT_TRUE(RunScript("resultQueue.popImmediately()", &script_result));
  EXPECT_EQ("null", script_result);
}

#if defined(ENABLE_NOTIFICATIONS)
IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest,
                       PushEventEnforcesUserVisibleNotification) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("false - is not controlled", script_result);

  LoadTestPage();  // Reload to become controlled.

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("true - is controlled", script_result);

  notification_manager()->CancelAll();
  ASSERT_EQ(0u, notification_manager()->GetNotificationCount());

  // We'll need to specify the web_contents in which to eval script, since we're
  // going to run script in a background tab.
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  // If the site is visible in an active tab, we should not force a notification
  // to be shown. Try it twice, since we allow one mistake per 10 push events.
  GCMClient::IncomingMessage message;
  for (int n = 0; n < 2; n++) {
    message.data["data"] = "testdata";
    push_service()->OnMessage(app_id.app_id_guid(), message);
    ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result));
    EXPECT_EQ("testdata", script_result);
    EXPECT_EQ(0u, notification_manager()->GetNotificationCount());
  }

  // Open a blank foreground tab so site is no longer visible.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("about:blank"), NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_WAIT_FOR_TAB);

  // If the Service Worker push event handler does not show a notification, we
  // should show a forced one, but only on the 2nd occurrence since we allow one
  // mistake per 10 push events.
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
  EXPECT_EQ("testdata", script_result);
  EXPECT_EQ(0u, notification_manager()->GetNotificationCount());
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
  EXPECT_EQ("testdata", script_result);
  EXPECT_EQ(1u, notification_manager()->GetNotificationCount());
  EXPECT_EQ(base::ASCIIToUTF16(kPushMessagingForcedNotificationTag),
            notification_manager()->GetNotificationAt(0).replace_id());

  // Currently, this notification will stick around until the user or webapp
  // explicitly dismisses it (though we may change this later).
  message.data["data"] = "shownotification";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
  EXPECT_EQ("shownotification", script_result);
  EXPECT_EQ(2u, notification_manager()->GetNotificationCount());

  notification_manager()->CancelAll();
  EXPECT_EQ(0u, notification_manager()->GetNotificationCount());

  // However if the Service Worker push event handler shows a notification, we
  // should not show a forced one.
  message.data["data"] = "shownotification";
  for (int n = 0; n < 9; n++) {
    push_service()->OnMessage(app_id.app_id_guid(), message);
    ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
    EXPECT_EQ("shownotification", script_result);
    EXPECT_EQ(1u, notification_manager()->GetNotificationCount());
    EXPECT_EQ(base::ASCIIToUTF16("push_test_tag"),
              notification_manager()->GetNotificationAt(0).replace_id());
    notification_manager()->CancelAll();
  }

  // Now that 10 push messages in a row have shown notifications, we should
  // allow the next one to mistakenly not show a notification.
  message.data["data"] = "testdata";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
  EXPECT_EQ("testdata", script_result);
  EXPECT_EQ(0u, notification_manager()->GetNotificationCount());
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest,
                       PushEventNotificationWithoutEventWaitUntil) {
  std::string script_result;
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  PushMessagingApplicationId app_id = GetServiceWorkerAppId(0LL);
  EXPECT_EQ(app_id.app_id_guid(), gcm_service()->last_registered_app_id());
  EXPECT_EQ("1234567890", gcm_service()->last_registered_sender_ids()[0]);

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("false - is not controlled", script_result);

  LoadTestPage();  // Reload to become controlled.

  ASSERT_TRUE(RunScript("isControlled()", &script_result));
  ASSERT_EQ("true - is controlled", script_result);

  NotificationAddedCallback callback;
  notification_manager()->SetNotificationAddedCallback(
      base::Bind(&NotificationAddedCallback::Run, base::Unretained(&callback)));

  GCMClient::IncomingMessage message;
  message.data["data"] = "shownotification-without-waituntil";
  push_service()->OnMessage(app_id.app_id_guid(), message);
  ASSERT_TRUE(RunScript("resultQueue.pop()", &script_result, web_contents));
  EXPECT_EQ("immediate:shownotification-without-waituntil", script_result);

  callback.WaitUntilSatisfied();

  ASSERT_EQ(1u, notification_manager()->GetNotificationCount());
  EXPECT_EQ(base::ASCIIToUTF16("push_test_tag"),
            notification_manager()->GetNotificationAt(0).replace_id());

  // Verify that the renderer process hasn't crashed.
  ASSERT_TRUE(RunScript("hasPermission()", &script_result));
  EXPECT_EQ("permission status - granted", script_result);
}
#endif

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, HasPermissionSaysDefault) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  ASSERT_TRUE(RunScript("hasPermission()", &script_result));
  ASSERT_EQ("permission status - default", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, HasPermissionSaysGranted) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  InfoBarResponder accepting_responder(browser(), true);
  ASSERT_TRUE(RunScript("requestNotificationPermission();", &script_result));
  EXPECT_EQ("permission status - granted", script_result);

  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ(std::string(kPushMessagingEndpoint) + " - 1-0", script_result);

  ASSERT_TRUE(RunScript("hasPermission()", &script_result));
  EXPECT_EQ("permission status - granted", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, HasPermissionSaysDenied) {
  std::string script_result;

  ASSERT_TRUE(RunScript("registerServiceWorker()", &script_result));
  ASSERT_EQ("ok - service worker registered", script_result);

  InfoBarResponder cancelling_responder(browser(), false);
  ASSERT_TRUE(RunScript("requestNotificationPermission();", &script_result));
  EXPECT_EQ("permission status - denied", script_result);

  ASSERT_TRUE(RunScript("registerPush()", &script_result));
  EXPECT_EQ("AbortError - Registration failed - permission denied",
            script_result);

  ASSERT_TRUE(RunScript("hasPermission()", &script_result));
  EXPECT_EQ("permission status - denied", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, UnregisterSuccess) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  gcm_service()->AddExpectedUnregisterResponse(GCMClient::SUCCESS);

  ASSERT_TRUE(RunScript("unregister()", &script_result));
  EXPECT_EQ("unregister result: true", script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, UnregisterNetworkError) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  gcm_service()->AddExpectedUnregisterResponse(GCMClient::NETWORK_ERROR);

  ASSERT_TRUE(RunScript("unregister()", &script_result));
  EXPECT_EQ("unregister error: "
            "NetworkError: Failed to connect to the push server.",
            script_result);
}

IN_PROC_BROWSER_TEST_F(PushMessagingBrowserTest, UnregisterUnknownError) {
  std::string script_result;

  TryToRegisterSuccessfully("1-0" /* expected_push_registration_id */);

  gcm_service()->AddExpectedUnregisterResponse(GCMClient::UNKNOWN_ERROR);

  ASSERT_TRUE(RunScript("unregister()", &script_result));
  EXPECT_EQ("unregister error: "
            "UnknownError: Unexpected error while trying to unregister from the"
            " push server.", script_result);
}

}  // namespace gcm

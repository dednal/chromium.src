// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_GUEST_VIEW_BROWSER_GUEST_VIEW_MANAGER_H_
#define COMPONENTS_GUEST_VIEW_BROWSER_GUEST_VIEW_MANAGER_H_

#include <map>

#include "base/bind.h"
#include "base/gtest_prod_util.h"
#include "base/lazy_instance.h"
#include "base/macros.h"
#include "content/public/browser/browser_plugin_guest_manager.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"

class GURL;

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace guest_view {

class GuestViewBase;
class GuestViewManagerDelegate;
class GuestViewManagerFactory;

class GuestViewManager : public content::BrowserPluginGuestManager,
                         public base::SupportsUserData::Data {
 public:
  GuestViewManager(content::BrowserContext* context,
                   scoped_ptr<GuestViewManagerDelegate> delegate);
  ~GuestViewManager() override;

  // Returns the GuestViewManager associated with |context|. If one isn't
  // available, then it is created and returned.
  static GuestViewManager* CreateWithDelegate(
      content::BrowserContext* context,
      scoped_ptr<GuestViewManagerDelegate> delegate);

  // Returns the GuestViewManager associated with |context|. If one isn't
  // available, then nullptr is returned.
  static GuestViewManager* FromBrowserContext(content::BrowserContext* context);

  // Overrides factory for testing. Default (NULL) value indicates regular
  // (non-test) environment.
  static void set_factory_for_testing(GuestViewManagerFactory* factory) {
    GuestViewManager::factory_ = factory;
  }
  // Returns the guest WebContents associated with the given |guest_instance_id|
  // if the provided |embedder_render_process_id| is allowed to access it.
  // If the embedder is not allowed access, the embedder will be killed, and
  // this method will return NULL. If no WebContents exists with the given
  // instance ID, then NULL will also be returned.
  content::WebContents* GetGuestByInstanceIDSafely(
      int guest_instance_id,
      int embedder_render_process_id);

  // Associates the Browser Plugin with |element_instance_id| to a
  // guest that has ID of |guest_instance_id| and sets initialization
  // parameters, |params| for it.
  void AttachGuest(int embedder_process_id,
                   int element_instance_id,
                   int guest_instance_id,
                   const base::DictionaryValue& attach_params);

  // Removes the association between |element_instance_id| and a guest instance
  // ID if one exists.
  void DetachGuest(GuestViewBase* guest);

  // Indicates whether the |guest| is owned by an extension or Chrome App.
  bool IsOwnedByExtension(GuestViewBase* guest);

  int GetNextInstanceID();
  int GetGuestInstanceIDForElementID(
      int owner_process_id,
      int element_instance_id);

  template <typename T>
  void RegisterGuestViewType() {
    // If the GuestView type |T| is already registered, then there is nothing
    // more to do. If an existing entry in the registry was created by this
    // function for type |T|, then registering again would have no effect, and
    // if it was registered elsewhere, then we do not want to overwrite it. Note
    // that it is possible for tests to have special test factory methods
    // registered here.
    if (guest_view_registry_.count(T::Type))
      return;
    guest_view_registry_[T::Type] = base::Bind(&T::Create);
  }

  using WebContentsCreatedCallback =
      base::Callback<void(content::WebContents*)>;
  void CreateGuest(const std::string& view_type,
                   content::WebContents* owner_web_contents,
                   const base::DictionaryValue& create_params,
                   const WebContentsCreatedCallback& callback);

  content::WebContents* CreateGuestWithWebContentsParams(
      const std::string& view_type,
      content::WebContents* owner_web_contents,
      const content::WebContents::CreateParams& create_params);

  content::SiteInstance* GetGuestSiteInstance(
      const GURL& guest_site);

  // BrowserPluginGuestManager implementation.
  content::WebContents* GetGuestByInstanceID(
      int owner_process_id,
      int element_instance_id) override;
  bool ForEachGuest(content::WebContents* owner_web_contents,
                    const GuestCallback& callback) override;
  content::WebContents* GetFullPageGuest(
      content::WebContents* embedder_web_contents) override;

 protected:
  friend class GuestViewBase;
  friend class GuestViewEvent;

  // Can be overriden in tests.
  virtual void AddGuest(int guest_instance_id,
                        content::WebContents* guest_web_contents);

  // Can be overriden in tests.
  virtual void RemoveGuest(int guest_instance_id);

  // Creates a guest of the provided |view_type|.
  GuestViewBase* CreateGuestInternal(content::WebContents* owner_web_contents,
                                     const std::string& view_type);

  // Adds GuestView types to the GuestView registry.
  void RegisterGuestViewTypes();

  // Indicates whether the provided |guest| can be used in the context it has
  // been created.
  bool IsGuestAvailableToContext(GuestViewBase* guest);

  // Dispatches the event with |name| with the provided |args| to the embedder
  // of the given |guest| with |instance_id| for routing.
  void DispatchEvent(const std::string& event_name,
                     scoped_ptr<base::DictionaryValue> args,
                     GuestViewBase* guest,
                     int instance_id);

  content::WebContents* GetGuestByInstanceID(int guest_instance_id);

  bool CanEmbedderAccessInstanceIDMaybeKill(
      int embedder_render_process_id,
      int guest_instance_id);

  bool CanEmbedderAccessInstanceID(int embedder_render_process_id,
                                   int guest_instance_id);

  // Returns true if |guest_instance_id| can be used to add a new guest to this
  // manager.
  // We disallow adding new guest with instance IDs that were previously removed
  // from this manager using RemoveGuest.
  bool CanUseGuestInstanceID(int guest_instance_id);

  static bool GetFullPageGuestHelper(content::WebContents** result,
                                     content::WebContents* guest_web_contents);

  // Static factory instance (always NULL for non-test).
  static GuestViewManagerFactory* factory_;

  // Contains guests' WebContents, mapping from their instance ids.
  using GuestInstanceMap = std::map<int, content::WebContents*>;
  GuestInstanceMap guest_web_contents_by_instance_id_;

  struct ElementInstanceKey {
    int embedder_process_id;
    int element_instance_id;

    ElementInstanceKey();
    ElementInstanceKey(int embedder_process_id,
                       int element_instance_id);

    bool operator<(const ElementInstanceKey& other) const;
    bool operator==(const ElementInstanceKey& other) const;
  };

  using GuestInstanceIDMap = std::map<ElementInstanceKey, int>;
  GuestInstanceIDMap instance_id_map_;

  // The reverse map of GuestInstanceIDMap.
  using GuestInstanceIDReverseMap = std::map<int, ElementInstanceKey>;
  GuestInstanceIDReverseMap reverse_instance_id_map_;

  using GuestCreationCallback =
      base::Callback<GuestViewBase*(content::WebContents*)>;
  using GuestViewCreationMap =
      std::map<std::string, GuestViewManager::GuestCreationCallback>;
  GuestViewCreationMap guest_view_registry_;

  int current_instance_id_;

  // Any instance ID whose number not greater than this was removed via
  // RemoveGuest.
  // This is used so that we don't have store all removed instance IDs in
  // |removed_instance_ids_|.
  int last_instance_id_removed_;
  // The remaining instance IDs that are greater than
  // |last_instance_id_removed_| are kept here.
  std::set<int> removed_instance_ids_;

  content::BrowserContext* context_;

  scoped_ptr<GuestViewManagerDelegate> delegate_;

  DISALLOW_COPY_AND_ASSIGN(GuestViewManager);
};

}  // namespace guest_view

#endif  // COMPONETS_GUEST_VIEW_BROWSER_GUEST_VIEW_MANAGER_H_

// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/autofill/password_generation_manager.h"

#include "base/logging.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebDocument.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebInputElement.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebFormElement.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebFrame.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/WebSecurityOrigin.h"
#include "third_party/WebKit/Source/WebKit/chromium/public/platform/WebVector.h"

namespace autofill {

PasswordGenerationManager::PasswordGenerationManager(
    content::RenderView* render_view)
    : content::RenderViewObserver(render_view) {}
PasswordGenerationManager::~PasswordGenerationManager() {}

void PasswordGenerationManager::DidFinishDocumentLoad(WebKit::WebFrame* frame) {
  if (!ShouldAnalyzeFrame(*frame))
    return;

  WebKit::WebVector<WebKit::WebFormElement> forms;
  frame->document().forms(forms);
  for (size_t i = 0; i < forms.size(); ++i) {
    const WebKit::WebFormElement& web_form = forms[i];
    if (!web_form.autoComplete())
      continue;

    // Grab all of the passwords for each form.
    WebKit::WebVector<WebKit::WebFormControlElement> control_elements;
    web_form.getFormControlElements(control_elements);

    std::vector<WebKit::WebInputElement> passwords;
    for (size_t i = 0; i < control_elements.size(); i++) {
      WebKit::WebInputElement* input_element =
          toWebInputElement(&control_elements[i]);
      if (input_element && input_element->isPasswordField())
        passwords.push_back(*input_element);
    }
    // For now, just assume that if there are two password fields in the
    // form that this is meant for account creation.
    // TODO(gcasto): Determine better heauristics for this.
    if (passwords.size() == 2) {
      account_creation_elements_ = make_pair(passwords[0], passwords);
      break;
    }
  }
}

bool PasswordGenerationManager::ShouldAnalyzeFrame(
    const WebKit::WebFrame& frame) const {
  // Make sure that this security origin is allowed to use password manager.
  // Generating a password that can't be saved is a bad idea.
  WebKit::WebSecurityOrigin origin = frame.document().securityOrigin();
  if (!origin.canAccessPasswordManager()) {
    DVLOG(1) << "No PasswordManager access";
    return false;
  }
  // TODO(gcasto): Query the browser to see if password sync is enabled.
  return true;
}

void PasswordGenerationManager::FocusedNodeChanged(
    const WebKit::WebNode& node) {
  if (account_creation_elements_.first ==
      node.toConst<WebKit::WebInputElement>()) {
    // Eventually we will show UI here and possibly fill the passwords
    // depending on the user interaction. For now, we will just say that the
    // associated passwords fields have been autocompleted to aid in testing.
    std::vector<WebKit::WebInputElement> passwords =
        account_creation_elements_.second;
    for (size_t i = 0; i < passwords.size(); ++i) {
      passwords[i].setAutofilled(true);
    }
  }
}

}  // namespace autofill

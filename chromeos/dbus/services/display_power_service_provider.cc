// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/dbus/services/display_power_service_provider.h"

#include "base/bind.h"
#include "dbus/message.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

namespace chromeos {

namespace {

void RunConfigurationCallback(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender,
    bool status) {
  response_sender.Run(dbus::Response::FromMethodCall(method_call));
}

}  // namespace

DisplayPowerServiceProvider::DisplayPowerServiceProvider(
    scoped_ptr<Delegate> delegate)
    : delegate_(delegate.Pass()),
      weak_ptr_factory_(this) {
}

DisplayPowerServiceProvider::~DisplayPowerServiceProvider() {}

void DisplayPowerServiceProvider::Start(
    scoped_refptr<dbus::ExportedObject> exported_object) {
  exported_object->ExportMethod(
      kLibCrosServiceInterface,
      kSetDisplayPower,
      base::Bind(&DisplayPowerServiceProvider::SetDisplayPower,
                 weak_ptr_factory_.GetWeakPtr()),
      base::Bind(&DisplayPowerServiceProvider::OnExported,
                 weak_ptr_factory_.GetWeakPtr()));
  exported_object->ExportMethod(
      kLibCrosServiceInterface,
      kSetDisplaySoftwareDimming,
      base::Bind(&DisplayPowerServiceProvider::SetDisplaySoftwareDimming,
                 weak_ptr_factory_.GetWeakPtr()),
      base::Bind(&DisplayPowerServiceProvider::OnExported,
                 weak_ptr_factory_.GetWeakPtr()));
}

void DisplayPowerServiceProvider::OnExported(const std::string& interface_name,
                                             const std::string& method_name,
                                             bool success) {
  if (!success) {
    LOG(ERROR) << "Failed to export " << interface_name << "."
               << method_name;
  }
}

void DisplayPowerServiceProvider::SetDisplayPower(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  dbus::MessageReader reader(method_call);
  int int_state = 0;
  Delegate::ResponseCallback callback =
      base::Bind(&RunConfigurationCallback, method_call, response_sender);
  if (reader.PopInt32(&int_state)) {
    DisplayPowerState state = static_cast<DisplayPowerState>(int_state);
    delegate_->SetDisplayPower(state, callback);
  } else {
    LOG(ERROR) << "Unable to parse " << kSetDisplayPower << " request";
    callback.Run(false);
  }
}

void DisplayPowerServiceProvider::SetDisplaySoftwareDimming(
    dbus::MethodCall* method_call,
    dbus::ExportedObject::ResponseSender response_sender) {
  dbus::MessageReader reader(method_call);
  bool dimmed = false;
  if (reader.PopBool(&dimmed)) {
    delegate_->SetDimming(dimmed);
  } else {
    LOG(ERROR) << "Unable to parse " << kSetDisplaySoftwareDimming
               << " request";
  }
  response_sender.Run(dbus::Response::FromMethodCall(method_call));
}

}  // namespace chromeos

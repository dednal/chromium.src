// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/browser_plugin/browser_plugin_compositing_helper.h"

#include "cc/layers/delegated_renderer_layer.h"
#include "cc/layers/solid_color_layer.h"
#include "cc/layers/texture_layer.h"
#include "cc/output/context_provider.h"
#include "content/common/browser_plugin/browser_plugin_messages.h"
#include "content/common/gpu/client/context_provider_command_buffer.h"
#include "content/renderer/browser_plugin/browser_plugin_manager.h"
#include "content/renderer/render_thread_impl.h"
#include "third_party/WebKit/public/platform/WebGraphicsContext3D.h"
#include "third_party/WebKit/public/web/WebPluginContainer.h"
#include "third_party/khronos/GLES2/gl2.h"
#include "ui/gfx/size_conversions.h"
#include "webkit/renderer/compositor_bindings/web_layer_impl.h"

namespace content {

BrowserPluginCompositingHelper::BrowserPluginCompositingHelper(
    WebKit::WebPluginContainer* container,
    BrowserPluginManager* manager,
    int instance_id,
    int host_routing_id)
    : instance_id_(instance_id),
      host_routing_id_(host_routing_id),
      last_route_id_(0),
      last_host_id_(0),
      last_mailbox_valid_(false),
      ack_pending_(true),
      container_(container),
      browser_plugin_manager_(manager) {
}

BrowserPluginCompositingHelper::~BrowserPluginCompositingHelper() {
}

void BrowserPluginCompositingHelper::DidCommitCompositorFrame() {
  if (!delegated_layer_.get() || !ack_pending_)
    return;

  cc::CompositorFrameAck ack;
  delegated_layer_->TakeUnusedResourcesForChildCompositor(&ack.resources);

  browser_plugin_manager_->Send(
      new BrowserPluginHostMsg_CompositorFrameACK(
          host_routing_id_,
          instance_id_,
          last_route_id_,
          last_host_id_,
          ack));

  ack_pending_ = false;
}

void BrowserPluginCompositingHelper::EnableCompositing(bool enable) {
  if (enable && !background_layer_.get()) {
    background_layer_ = cc::SolidColorLayer::Create();
    background_layer_->SetMasksToBounds(true);
    background_layer_->SetBackgroundColor(
        SkColorSetARGBInline(255, 255, 255, 255));
    web_layer_.reset(new webkit::WebLayerImpl(background_layer_));
  }

  container_->setWebLayer(enable ? web_layer_.get() : NULL);
}

void BrowserPluginCompositingHelper::CheckSizeAndAdjustLayerBounds(
    const gfx::Size& new_size,
    float device_scale_factor,
    cc::Layer* layer) {
  if (buffer_size_ != new_size) {
    buffer_size_ = new_size;
    // The container size is in DIP, so is the layer size.
    // Buffer size is in physical pixels, so we need to adjust
    // it by the device scale factor.
    gfx::Size device_scale_adjusted_size = gfx::ToFlooredSize(
        gfx::ScaleSize(buffer_size_, 1.0f / device_scale_factor));
    layer->SetBounds(device_scale_adjusted_size);
  }
}

void BrowserPluginCompositingHelper::MailboxReleased(
    const MailboxSwapInfo& mailbox,
    unsigned sync_point,
    bool lost_resource) {
  if (lost_resource) {
    // Recurse with an empty mailbox if the one being released was lost.
    MailboxSwapInfo empty_info;
    empty_info.type = mailbox.type;
    empty_info.route_id = mailbox.route_id;
    empty_info.host_id = mailbox.host_id;
    MailboxReleased(empty_info, 0, false);
    return;
  }

  // This means the GPU process crashed or guest crashed.
  if (last_host_id_ != mailbox.host_id || last_route_id_ != mailbox.route_id)
    return;

  // We need to send an ACK to for every buffer sent to us.
  // However, if a buffer is freed up from
  // the compositor in cases like switching back to SW mode without a new
  // buffer arriving, no ACK is needed.
  if (!ack_pending_) {
    last_mailbox_valid_ = false;
    return;
  }
  ack_pending_ = false;
  switch (mailbox.type) {
    case TEXTURE_IMAGE_TRANSPORT: {
      std::string mailbox_name(reinterpret_cast<const char*>(mailbox.name.name),
                               sizeof(mailbox.name.name));
      browser_plugin_manager_->Send(
          new BrowserPluginHostMsg_BuffersSwappedACK(
              host_routing_id_,
              instance_id_,
              mailbox.route_id,
              mailbox.host_id,
              mailbox_name,
              sync_point));
      break;
    }
    case COMPOSITOR_FRAME: {
      cc::CompositorFrameAck ack;
      ack.gl_frame_data.reset(new cc::GLFrameData());
      ack.gl_frame_data->mailbox = mailbox.name;
      ack.gl_frame_data->size = mailbox.size;
      ack.gl_frame_data->sync_point = sync_point;

      browser_plugin_manager_->Send(
         new BrowserPluginHostMsg_CompositorFrameACK(
             host_routing_id_,
             instance_id_,
             mailbox.route_id,
             mailbox.host_id,
             ack));
      break;
    }
  }
}

void BrowserPluginCompositingHelper::OnContainerDestroy() {
  if (container_)
    container_->setWebLayer(NULL);
  container_ = NULL;

  texture_layer_ = NULL;
  delegated_layer_ = NULL;
  background_layer_ = NULL;
  web_layer_.reset();
}

void BrowserPluginCompositingHelper::OnBuffersSwappedPrivate(
    const MailboxSwapInfo& mailbox,
    unsigned sync_point,
    float device_scale_factor) {
  DCHECK(!delegated_layer_.get());
  // If these mismatch, we are either just starting up, GPU process crashed or
  // guest renderer crashed.
  // In this case, we are communicating with a new image transport
  // surface and must ACK with the new ID's and an empty mailbox.
  if (last_route_id_ != mailbox.route_id || last_host_id_ != mailbox.host_id)
    last_mailbox_valid_ = false;

  last_route_id_ = mailbox.route_id;
  last_host_id_ = mailbox.host_id;

  ack_pending_ = true;
  // Browser plugin getting destroyed, do a fast ACK.
  if (!background_layer_.get()) {
    MailboxReleased(mailbox, sync_point, false);
    return;
  }

  if (!texture_layer_.get()) {
    texture_layer_ = cc::TextureLayer::CreateForMailbox(NULL);
    texture_layer_->SetIsDrawable(true);
    texture_layer_->SetContentsOpaque(true);

    background_layer_->AddChild(texture_layer_);
  }

  // The size of browser plugin container is not always equal to the size
  // of the buffer that arrives here. This could be for a number of reasons,
  // including autosize and a resize in progress.
  // During resize, the container size changes first and then some time
  // later, a new buffer with updated size will arrive. During this process,
  // we need to make sure that things are still displayed pixel perfect.
  // We accomplish this by modifying bounds of the texture layer only
  // when a new buffer arrives.
  // Visually, this will either display a smaller part of the buffer
  // or introduce a gutter around it.
  CheckSizeAndAdjustLayerBounds(mailbox.size,
                                device_scale_factor,
                                texture_layer_.get());

  bool current_mailbox_valid = !mailbox.name.IsZero();
  if (!last_mailbox_valid_) {
    MailboxSwapInfo empty_info;
    empty_info.type = mailbox.type;
    empty_info.route_id = mailbox.route_id;
    empty_info.host_id = mailbox.host_id;
    MailboxReleased(empty_info, 0, false);
    if (!current_mailbox_valid)
      return;
  }

  cc::TextureMailbox::ReleaseCallback callback;
  if (current_mailbox_valid) {
    callback = base::Bind(&BrowserPluginCompositingHelper::MailboxReleased,
                          scoped_refptr<BrowserPluginCompositingHelper>(this),
                          mailbox);
  }
  texture_layer_->SetTextureMailbox(cc::TextureMailbox(mailbox.name,
                                                       callback,
                                                       sync_point));
  texture_layer_->SetNeedsDisplay();
  last_mailbox_valid_ = current_mailbox_valid;
}

void BrowserPluginCompositingHelper::OnBuffersSwapped(
    const gfx::Size& size,
    const std::string& mailbox_name,
    int gpu_route_id,
    int gpu_host_id,
    float device_scale_factor) {
  MailboxSwapInfo swap_info;
  swap_info.name.SetName(reinterpret_cast<const int8*>(mailbox_name.data()));
  swap_info.type = TEXTURE_IMAGE_TRANSPORT;
  swap_info.size = size;
  swap_info.route_id = gpu_route_id;
  swap_info.host_id = gpu_host_id;
  OnBuffersSwappedPrivate(swap_info, 0, device_scale_factor);
}

void BrowserPluginCompositingHelper::OnCompositorFrameSwapped(
    scoped_ptr<cc::CompositorFrame> frame,
    int route_id,
    int host_id) {
  if (frame->gl_frame_data) {
    MailboxSwapInfo swap_info;
    swap_info.name = frame->gl_frame_data->mailbox;
    swap_info.type = COMPOSITOR_FRAME;
    swap_info.size = frame->gl_frame_data->size;
    swap_info.route_id = route_id;
    swap_info.host_id = host_id;
    OnBuffersSwappedPrivate(swap_info,
                            frame->gl_frame_data->sync_point,
                            frame->metadata.device_scale_factor);
    return;
  }

  DCHECK(!texture_layer_.get());
  if (!delegated_layer_.get()) {
    delegated_layer_ = cc::DelegatedRendererLayer::Create(NULL);
    delegated_layer_->SetIsDrawable(true);
    delegated_layer_->SetContentsOpaque(true);

    background_layer_->AddChild(delegated_layer_);
  }

  cc::DelegatedFrameData *frame_data = frame->delegated_frame_data.get();
  if (!frame_data)
    return;

  CheckSizeAndAdjustLayerBounds(
      frame_data->render_pass_list.back()->output_rect.size(),
      frame->metadata.device_scale_factor,
      delegated_layer_.get());

  delegated_layer_->SetFrameData(frame->delegated_frame_data.Pass());
  last_route_id_ = route_id;
  last_host_id_ = host_id;
  ack_pending_ = true;
}

void BrowserPluginCompositingHelper::UpdateVisibility(bool visible) {
  if (texture_layer_.get())
    texture_layer_->SetIsDrawable(visible);
  if (delegated_layer_.get())
    delegated_layer_->SetIsDrawable(visible);
}

}  // namespace content

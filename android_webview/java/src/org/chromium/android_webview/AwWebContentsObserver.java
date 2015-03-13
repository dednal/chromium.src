// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.android_webview;

import org.chromium.android_webview.AwContents.VisualStateCallback;
import org.chromium.base.ThreadUtils;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContentsObserver;
import org.chromium.net.NetError;

import java.lang.ref.WeakReference;

/**
 * Routes notifications from WebContents to AwContentsClient and other listeners.
 */
public class AwWebContentsObserver extends WebContentsObserver {
    // TODO(tobiasjs) similarly to WebContentsObserver.mWebContents, mAwContents
    // needs to be a WeakReference, which suggests that there exists a strong
    // reference to an AwWebContentsObserver instance. This is not intentional,
    // and should be found and cleaned up.
    private final WeakReference<AwContents> mAwContents;
    private final AwContentsClient mAwContentsClient;
    private boolean mHasStartedAnyProvisionalLoad = false;

    public AwWebContentsObserver(
            WebContents webContents, AwContents awContents, AwContentsClient awContentsClient) {
        super(webContents);
        mAwContents = new WeakReference<AwContents>(awContents);
        mAwContentsClient = awContentsClient;
    }

    boolean hasStartedAnyProvisionalLoad() {
        return mHasStartedAnyProvisionalLoad;
    }

    @Override
    public void didFinishLoad(long frameId, String validatedUrl, boolean isMainFrame) {
        String unreachableWebDataUrl = AwContentsStatics.getUnreachableWebDataUrl();
        boolean isErrorUrl =
                unreachableWebDataUrl != null && unreachableWebDataUrl.equals(validatedUrl);
        if (isMainFrame && !isErrorUrl) {
            mAwContentsClient.onPageFinished(validatedUrl);
        }
    }

    @Override
    public void didFailLoad(boolean isProvisionalLoad,
            boolean isMainFrame, int errorCode, String description, String failingUrl) {
        String unreachableWebDataUrl = AwContentsStatics.getUnreachableWebDataUrl();
        boolean isErrorUrl =
                unreachableWebDataUrl != null && unreachableWebDataUrl.equals(failingUrl);
        if (isErrorUrl) return;
        if (errorCode != NetError.ERR_ABORTED) {
            // This error code is generated for the following reasons:
            // - WebView.stopLoading is called,
            // - the navigation is intercepted by the embedder via shouldOverrideNavigation.
            //
            // The Android WebView does not notify the embedder of these situations using
            // this error code with the WebViewClient.onReceivedError callback.
            AwContentsClient.AwWebResourceRequest request =
                    new AwContentsClient.AwWebResourceRequest();
            request.url = failingUrl;
            request.isMainFrame = isMainFrame;
            // TODO(mnaganov): Fill in the rest of AwWebResourceRequest fields. Probably,
            // we will have to actually invoke the error callback from the network delegate
            // in order to catch load errors for all resources.
            AwContentsClient.AwWebResourceError error = new AwContentsClient.AwWebResourceError();
            error.errorCode = ErrorCodeConversionHelper.convertErrorCode(errorCode);
            error.description = description;
            mAwContentsClient.onReceivedError(request, error);
        }
        if (isMainFrame) {
            // Need to call onPageFinished after onReceivedError (if there is an error) for
            // backwards compatibility with the classic webview.
            mAwContentsClient.onPageFinished(failingUrl);
        }
    }

    @Override
    public void didNavigateMainFrame(final String url, String baseUrl,
            boolean isNavigationToDifferentPage, boolean isFragmentNavigation, int statusCode) {
        // Only invoke the onPageCommitVisible callback when navigating to a different page,
        // but not when navigating to a different fragment within the same page.
        if (isNavigationToDifferentPage) {
            ThreadUtils.postOnUiThread(new Runnable() {
                @Override
                public void run() {
                    AwContents awContents = mAwContents.get();
                    if (awContents != null) {
                        awContents.insertVisualStateCallback(0, new VisualStateCallback() {
                            @Override
                            public void onComplete(long requestId) {
                                mAwContentsClient.onPageCommitVisible(url);
                            }
                        });
                    }
                }
            });
        }
        // This is here to emulate the Classic WebView firing onPageFinished for main frame
        // navigations where only the hash fragment changes.
        if (isFragmentNavigation) {
            mAwContentsClient.onPageFinished(url);
        }
    }

    @Override
    public void didNavigateAnyFrame(String url, String baseUrl, boolean isReload) {
        mAwContentsClient.doUpdateVisitedHistory(url, isReload);
    }

    @Override
    public void didStartProvisionalLoadForFrame(
            long frameId,
            long parentFrameId,
            boolean isMainFrame,
            String validatedUrl,
            boolean isErrorPage,
            boolean isIframeSrcdoc) {
        mHasStartedAnyProvisionalLoad = true;
    }
}

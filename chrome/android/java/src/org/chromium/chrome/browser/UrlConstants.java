// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser;

/**
 * Java side version of chrome/common/url_constants.cc
 */
public class UrlConstants {
    public static final String CHROME_SCHEME = "chrome://";
    public static final String CHROME_NATIVE_SCHEME = "chrome-native://";
    public static final String DOCUMENT_SCHEME = "document";
    public static final String HTTP_SCHEME = "http://";
    public static final String HTTPS_SCHEME = "https://";

    public static final String NTP_URL = "chrome-native://newtab/";
    public static final String NTP_HOST = "newtab";
    public static final String BOOKMARKS_URL = "chrome-native://bookmarks/";
    public static final String BOOKMARKS_FOLDER_URL = "chrome-native://bookmarks/#";
    public static final String MOBILE_BOOKMARKS_URL = "chrome-native://bookmarks/#2";
    public static final String BOOKMARKS_HOST = "bookmarks";
    public static final String RECENT_TABS_URL = "chrome-native://recent-tabs/";
    public static final String RECENT_TABS_HOST = "recent-tabs";
    public static final String HISTORY_URL = "chrome://history/";

    // This is chrome::kCrashReasonURL. Please ensure they stay in sync.
    public static final String CRASH_REASON_URL =
            "https://support.google.com/chrome/?p=mobile_awsnap";
}

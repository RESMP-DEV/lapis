// Copyright (c) 2026 RESMP-DEV. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include "tests/cefsimple/simple_app.h"

#include <cstdlib>
#include <string>

#include "include/cef_browser.h"
#include "include/wrapper/cef_helpers.h"
#include "tests/cefsimple/simple_handler.h"

SimpleApp::SimpleApp() = default;

void SimpleApp::OnContextInitialized() {
    CEF_REQUIRE_UI_THREAD();

    const char* configured_url = std::getenv("LAPIS_W0_URL");
    std::string url = configured_url ? configured_url : "";
    if (url.empty()) {
        url = "about:blank";
    }

    const CefRefPtr<SimpleHandler> handler(new SimpleHandler(true));
    CefBrowserSettings settings;
    settings.windowless_frame_rate = 10;

    CefWindowInfo window_info;
    window_info.SetAsWindowless(nullptr);
    window_info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    CefBrowserHost::CreateBrowser(window_info, handler, url, settings, nullptr, nullptr);
}

CefRefPtr<CefClient> SimpleApp::GetDefaultClient() { return SimpleHandler::GetInstance(); }

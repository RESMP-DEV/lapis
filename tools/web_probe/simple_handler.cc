// Copyright (c) 2026 RESMP-DEV. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include "tests/cefsimple/simple_handler.h"

#include <cstdlib>
#include <fstream>
#include <string>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

namespace {

constexpr int kShutdownInitialGraceMs = 2000;
constexpr int kShutdownRetryMs = 1000;
constexpr int kShutdownRetryLimit = 6;

SimpleHandler* g_instance = nullptr;

std::string OutputPath(const std::string& name) {
    const char* directory = std::getenv("LAPIS_W0_OUT");
    return std::string(directory ? directory : ".") + "/" + name;
}

void WriteText(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

double MillisecondsSince(const std::chrono::steady_clock::time_point& start,
                         const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

SimpleHandler::SimpleHandler(bool is_alloy_style) : is_alloy_style_(is_alloy_style) {
    DCHECK(!g_instance);
    g_instance = this;
    output_dir_ = std::getenv("LAPIS_W0_OUT") ? std::getenv("LAPIS_W0_OUT") : ".";
}

SimpleHandler::~SimpleHandler() { g_instance = nullptr; }

SimpleHandler* SimpleHandler::GetInstance() { return g_instance; }

bool SimpleHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                                     const CefString& message, const CefString& source, int line) {
    CEF_REQUIRE_UI_THREAD();
    const std::string value(message);
    if (value == "lapis-w0:ready" && !interaction_started_) {
        interaction_started_ = true;
        CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::InjectDeterministicProbe, this),
                           50);
    } else if (value == "lapis-w0:click" && !click_observed_) {
        click_observed_ = true;
        CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::FocusInput, this), 50);
    } else if (value == "lapis-w0:type:A" && !keyboard_observed_) {
        keyboard_observed_ = true;
        interaction_done_at_ = std::chrono::steady_clock::now();
        CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::RequestAccessibilityTree, this),
                           100);
    }
    return true;
}

void SimpleHandler::OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    const std::string value(title);
}

void SimpleHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    CHECK_EQ(is_alloy_style_ ? CEF_RUNTIME_STYLE_ALLOY : CEF_RUNTIME_STYLE_CHROME,
             browser->GetHost()->GetRuntimeStyle());
    browser_ = browser;
    created_at_ = std::chrono::steady_clock::now();
    devtools_registration_ = browser->GetHost()->AddDevToolsMessageObserver(this);
}

bool SimpleHandler::DoClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    is_closing_ = true;
    return false;
}

void SimpleHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    browser_ = nullptr;
    devtools_registration_ = nullptr;
    browser_closed_ = true;
    CefQuitMessageLoop();
}

void SimpleHandler::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool is_loading,
                                         bool can_go_back, bool can_go_forward) {
    CEF_REQUIRE_UI_THREAD();
}

void SimpleHandler::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                ErrorCode error_code, const CefString& error_text,
                                const CefString& failed_url) {
    CEF_REQUIRE_UI_THREAD();
    if (error_code != ERR_ABORTED) {
        load_error_ = true;
        WriteText(OutputPath("load-error.txt"),
                  std::string(error_text) + " " + std::string(failed_url));
        browser->GetHost()->CloseBrowser(true);
    }
}

void SimpleHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    rect = CefRect(0, 0, 800, 600);
}

void SimpleHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirty_rects, const void* buffer, int width,
                            int height) {
    if (type != PET_VIEW || width <= 0 || height <= 0) {
        return;
    }
    ++paint_count_;
    if (paint_count_ == 1) {
        first_paint_at_ = std::chrono::steady_clock::now();
        WriteFrame("first.ppm", buffer, width, height);
    }
    WriteFrame("latest.ppm", buffer, width, height);
}

void SimpleHandler::OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id,
                                           bool success, const void* result, size_t result_size) {
    CEF_REQUIRE_UI_THREAD();
    const std::string raw(static_cast<const char*>(result), result_size);
    if (message_id == 1) {
        WriteText(OutputPath("accessibility.json"), raw);
        accessibility_ok_ = success && raw.find("\"role\"") != std::string::npos &&
                            raw.find("button") != std::string::npos &&
                            raw.find("textbox") != std::string::npos;
        CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::RequestDomSnapshot, this));
    } else if (message_id == 2) {
        WriteText(OutputPath("dom.json"), raw);
        dom_ok_ = success && raw.find("\"root\"") != std::string::npos &&
                  raw.find("INPUT") != std::string::npos;
        CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::RequestRuntimeEvaluation, this));
    } else if (message_id == 3) {
        WriteText(OutputPath("runtime-evaluation.json"), raw);
        runtime_ok_ = success && raw.find("lapisProbe") != std::string::npos &&
                      raw.find("injected") != std::string::npos;
        FinishIfReady();
    }
}

void SimpleHandler::InjectDeterministicProbe() {
    if (!browser_) {
        return;
    }
    browser_->GetMainFrame()->ExecuteJavaScript(
        "document.documentElement.dataset.lapisProbe='injected';",
        browser_->GetMainFrame()->GetURL(), 0);
    CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::ClickButton, this), 50);
}

void SimpleHandler::ClickButton() {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = 50;
    event.y = 50;
    event.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
    browser_->GetHost()->SendMouseMoveEvent(event, false);
    browser_->GetHost()->SendMouseClickEvent(event, MBT_LEFT, false, 1);
    browser_->GetHost()->SendMouseClickEvent(event, MBT_LEFT, true, 1);
}

void SimpleHandler::FocusInput() {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = 50;
    event.y = 150;
    event.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
    browser_->GetHost()->SendMouseMoveEvent(event, false);
    browser_->GetHost()->SendMouseClickEvent(event, MBT_LEFT, false, 1);
    browser_->GetHost()->SendMouseClickEvent(event, MBT_LEFT, true, 1);
    CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::SendKeyboardCharacter, this), 50);
}

void SimpleHandler::SendKeyboardCharacter() {
    if (!browser_) {
        return;
    }
    CefKeyEvent event = {};
    event.type = KEYEVENT_CHAR;
    event.character = L'A';
    event.windows_key_code = 'A';
    event.modifiers = 0;
    browser_->GetHost()->SendKeyEvent(event);
}

void SimpleHandler::RequestAccessibilityTree() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->ExecuteDevToolsMethod(1, "Accessibility.getFullAXTree",
                                               CefDictionaryValue::Create());
}

void SimpleHandler::RequestDomSnapshot() {
    if (!browser_) {
        return;
    }
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetInt("depth", -1);
    params->SetBool("pierce", true);
    browser_->GetHost()->ExecuteDevToolsMethod(2, "DOM.getDocument", params);
}

void SimpleHandler::RequestRuntimeEvaluation() {
    if (!browser_) {
        return;
    }
    CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
    params->SetString("expression", "JSON.stringify({lapisProbe:document.documentElement.dataset."
                                    "lapisProbe,button:!!document.querySelector('button'),input:!!"
                                    "document.querySelector('input')})");
    params->SetBool("returnByValue", true);
    browser_->GetHost()->ExecuteDevToolsMethod(3, "Runtime.evaluate", params);
}

void SimpleHandler::FinishIfReady() {
    const bool ready =
        click_observed_ && keyboard_observed_ && accessibility_ok_ && dom_ok_ && runtime_ok_;
    if (ready && !shutdown_started_) {
        shutdown_started_ = true;
        WriteSummary(true);
        CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::BeginShutdown, this));
    } else {
        WriteSummary(false);
    }
}

void SimpleHandler::BeginShutdown() {
    CEF_REQUIRE_UI_THREAD();
    CloseAllBrowsers(true);
    CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::CheckShutdown, this),
                       kShutdownInitialGraceMs);
}

void SimpleHandler::CheckShutdown() {
    CEF_REQUIRE_UI_THREAD();
    if (browser_closed_) {
        return;
    }
    ++shutdown_checks_;
    if (shutdown_checks_ <= kShutdownRetryLimit) {
        CloseAllBrowsers(true);
        CefPostDelayedTask(TID_UI, base::BindOnce(&SimpleHandler::CheckShutdown, this),
                           kShutdownRetryMs);
        return;
    }
    // Never enter CefShutdown with a live browser. The runner verifies that the
    // helpers exit naturally after this abrupt bounded failure. The total retry
    // deadline is eight seconds, below the observed Chromium macOS shutdown
    // watchdog boundary; it is a probe failure deadline, not a close guarantee.
    WriteText(OutputPath("teardown-error.txt"),
              "browser did not reach OnBeforeClose before the bounded deadline");
    std::_Exit(75);
}

void SimpleHandler::WriteSummary(bool passed) {
    std::string json = "{\n";
    json += "  \"schema\": \"lapis.web-engine-w0/1\",\n";
    json += "  \"passed\": " + std::string(passed ? "true" : "false") + ",\n";
    json += "  \"cef\": \"156.0.30+g6028375+chromium-156.0.8078.96\",\n";
    json += "  \"windowless\": true,\n";
    json += "  \"trusted_mouse_click\": " + std::string(click_observed_ ? "true" : "false") + ",\n";
    json +=
        "  \"trusted_keyboard_character\": " + std::string(keyboard_observed_ ? "true" : "false") +
        ",\n";
    json +=
        "  \"accessibility_tree\": " + std::string(accessibility_ok_ ? "true" : "false") + ",\n";
    json += "  \"dom_snapshot\": " + std::string(dom_ok_ ? "true" : "false") + ",\n";
    json += "  \"runtime_evaluation\": " + std::string(runtime_ok_ ? "true" : "false") + ",\n";
    json += "  \"paint_count\": " + std::to_string(paint_count_) + ",\n";
    if (first_paint_at_ != std::chrono::steady_clock::time_point{}) {
        json += "  \"created_to_first_paint_ms\": " +
                std::to_string(MillisecondsSince(created_at_, first_paint_at_)) + ",\n";
    }
    if (interaction_done_at_ != std::chrono::steady_clock::time_point{}) {
        json += "  \"first_paint_to_interaction_ms\": " +
                std::to_string(MillisecondsSince(first_paint_at_, interaction_done_at_)) + ",\n";
    }
    json += "  \"load_error\": " + std::string(load_error_ ? "true" : "false") + "\n}\n";
    WriteText(OutputPath("result.json"), json);
}

void SimpleHandler::WriteFrame(const std::string& name, const void* buffer, int width, int height) {
    const auto* pixels = static_cast<const unsigned char*>(buffer);
    std::ofstream stream(OutputPath(name), std::ios::binary | std::ios::trunc);
    stream << "P6\n" << width << " " << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = pixels + ((y * width) + x) * 4;
            const unsigned char rgb[3] = {pixel[2], pixel[1], pixel[0]};
            stream.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

void SimpleHandler::CloseAllBrowsers(bool force_close) {
    if (!CefCurrentlyOn(TID_UI)) {
        CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::CloseAllBrowsers, this, force_close));
        return;
    }
    if (browser_) {
        browser_->GetHost()->CloseBrowser(force_close);
    }
}

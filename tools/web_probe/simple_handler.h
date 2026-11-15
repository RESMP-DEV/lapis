// Copyright (c) 2026 RESMP-DEV. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#ifndef CEF_TESTS_CEFSIMPLE_SIMPLE_HANDLER_H_
#define CEF_TESTS_CEFSIMPLE_SIMPLE_HANDLER_H_

#include <chrono>
#include <string>

#include "include/cef_client.h"

class SimpleHandler : public CefClient,
                      public CefDevToolsMessageObserver,
                      public CefDisplayHandler,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefRenderHandler {
  public:
    explicit SimpleHandler(bool is_alloy_style);
    ~SimpleHandler() override;

    static SimpleHandler* GetInstance();

    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                          const CefString& message, const CefString& source, int line) override;
    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool is_loading, bool can_go_back,
                              bool can_go_forward) override;
    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error_code,
                     const CefString& error_text, const CefString& failed_url) override;
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirty_rects,
                 const void* buffer, int width, int height) override;
    void OnDevToolsMethodResult(CefRefPtr<CefBrowser> browser, int message_id, bool success,
                                const void* result, size_t result_size) override;

    void CloseAllBrowsers(bool force_close);

  private:
    void InjectDeterministicProbe();
    void ClickButton();
    void FocusInput();
    void SendKeyboardCharacter();
    void RequestAccessibilityTree();
    void RequestDomSnapshot();
    void RequestRuntimeEvaluation();
    void FinishIfReady();
    void BeginShutdown();
    void CheckShutdown();
    void WriteSummary(bool passed);
    void WriteFrame(const std::string& name, const void* buffer, int width, int height);

    const bool is_alloy_style_;
    CefRefPtr<CefBrowser> browser_;
    CefRefPtr<CefRegistration> devtools_registration_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point first_paint_at_;
    std::chrono::steady_clock::time_point interaction_done_at_;
    std::string output_dir_;
    int paint_count_ = 0;
    bool interaction_started_ = false;
    bool click_observed_ = false;
    bool keyboard_observed_ = false;
    bool accessibility_ok_ = false;
    bool dom_ok_ = false;
    bool runtime_ok_ = false;
    bool load_error_ = false;
    bool is_closing_ = false;
    bool shutdown_started_ = false;
    bool browser_closed_ = false;
    int shutdown_checks_ = 0;

    IMPLEMENT_REFCOUNTING(SimpleHandler);
};

#endif // CEF_TESTS_CEFSIMPLE_SIMPLE_HANDLER_H_

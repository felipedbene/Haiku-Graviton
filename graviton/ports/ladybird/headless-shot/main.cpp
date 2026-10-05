/*
 * Copyright (c) 2026, DeBeOS.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * headless-shot: a minimal non-Qt Ladybird chrome for DeBeOS/Haiku arm64.
 * LibWebView::Application's execute() drives HeadlessMode::Screenshot: load one
 * URL, take a screenshot, write a PNG, exit. Usage:
 *   headless-shot --headless=screenshot --screenshot-path out.png https://example.com/
 */

#include <LibCore/EventLoop.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/Options.h>

namespace DeBeOS {

class HeadlessShot : public WebView::Application {
    WEB_VIEW_APPLICATION(HeadlessShot)

public:
    HeadlessShot() = default;
    virtual ~HeadlessShot() override = default;

    // Resolve fonts via fontconfig so a default monospace/sans typeface is found
    // on Haiku; the PathFontProvider finds no UiMonospace here and FontPlugin
    // then fails its VERIFY. Mirrors how the in-tree headless harness sets fonts.
    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        web_content_options.force_fontconfig = WebView::ForceFontconfig::Yes;
    }
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto app = TRY(DeBeOS::HeadlessShot::create(arguments));
    return app->execute();
}

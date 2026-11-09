#pragma once

namespace singularity::app {

/*
 * Window sizing presets
 * ----------------------
 *
 * Shared between application startup (main.cpp) and the native bridge
 * (onboarding.complete), so both always agree on what "onboarding" and
 * "main application" content-area sizes mean.
 *
 * See AppWindow's class comment in app_window.hpp for the sizing semantics
 * and the onboarding -> main workspace transition these values support.
 */

struct WindowSize {
  int width;
  int height;
};

// Equal initial/minimum/maximum sizes temporarily lock the window while
// onboarding has not been completed yet.
inline constexpr WindowSize kOnboardingWindowSize{1080, 720};

// Sizes applied once onboarding has completed, either at startup (when
// settings already record completion) or immediately after
// onboarding.complete transitions the running window.
inline constexpr WindowSize kMainWindowInitialSize{1440, 960};
// The least content size, 5:3: what a Full HD+ 16:10 laptop at Windows'
// default 150% scale (1280 x 800 logical) leaves below the taskbar and the
// title bar is about 1280 x 729, so it fits that laptop, every MacBook and
// every common PC screen.
inline constexpr WindowSize kMainWindowMinimumSize{1200, 720};

} // namespace singularity::app

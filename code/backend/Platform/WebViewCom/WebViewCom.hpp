#pragma once

// WebView2's COM for code that builds with both MSVC and MinGW. The Windows
// package is built with MinGW (deploy/backend, windows-cross), whose headers
// carry neither WRL's event handlers (wrl/event.h, wrl/implements.h) nor the
// interface ids WebView2.h gives MSVC alone (__declspec(uuid) through
// MIDL_INTERFACE). Here:
//
//   - make_handler<Interface>(function): an event or completion handler, as
//     Microsoft::WRL::Callback makes one;
//   - each WebView2 interface Singularity implements or asks for, with its
//     id, so __uuidof names it under MinGW as under MSVC.
//     WebViewCom.test.cpp checks every id against WebView2.h's own (MSVC).

#include <windows.h>

// WebView2.h needs objbase.h's `interface`, which WIN32_LEAN_AND_MEAN leaves out.
#include <objbase.h>

#include <WebView2.h>
#include <wrl/client.h>

#include <atomic>
#include <utility>

// Every interface once, with its id as WebView2.h gives it.
#define SINGULARITY_WEBVIEW2_INTERFACES(INTERFACE)                                                                \
  INTERFACE(ICoreWebView2NavigationStartingEventHandler, 0x9adbe429, 0xf36d, 0x432b, 0x9d, 0xdc, 0xf8, 0x88, 0x1f, \
      0xbd, 0x76, 0xe3)                                                                                            \
  INTERFACE(ICoreWebView2NewWindowRequestedEventHandler, 0xd4c185fe, 0xc81c, 0x4989, 0x97, 0xaf, 0x2d, 0x3f, 0xa7, \
      0xab, 0x56, 0x51)                                                                                            \
  INTERFACE(ICoreWebView2ProcessFailedEventHandler, 0x79e0aea4, 0x990b, 0x42d9, 0xaa, 0x1d, 0x0f, 0xcc, 0x2e, 0x5b, \
      0xc7, 0xf1)                                                                                                  \
  INTERFACE(ICoreWebView2WebMessageReceivedEventHandler, 0x57213f19, 0x00e6, 0x49fa, 0x8e, 0x07, 0x89, 0x8e, 0xa0, \
      0x1e, 0xcb, 0xd2)                                                                                            \
  INTERFACE(ICoreWebView2ExecuteScriptCompletedHandler, 0x49511172, 0xcc67, 0x4bca, 0x99, 0x23, 0x13, 0x71, 0x12,  \
      0xf4, 0xc4, 0xcc)                                                                                            \
  INTERFACE(ICoreWebView2WebMessageReceivedEventArgs2, 0x06fc7ab7, 0xc90c, 0x4297, 0x93, 0x89, 0x33, 0xca, 0x01,   \
      0xcf, 0x6d, 0x5e)                                                                                            \
  INTERFACE(ICoreWebView2File, 0xf2c19559, 0x6bc1, 0x4583, 0xa7, 0x57, 0x90, 0x02, 0x1b, 0xe9, 0xaf, 0xec)         \
  INTERFACE(ICoreWebView2Settings9, 0x0528a73b, 0xe92d, 0x49f4, 0x92, 0x7a, 0xe5, 0x47, 0xdd, 0xda, 0xa3, 0x7d)

#if defined(__MINGW32__)
#define SINGULARITY_DECLARE_WEBVIEW2_ID(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
  __CRT_UUID_DECL(type, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)
SINGULARITY_WEBVIEW2_INTERFACES(SINGULARITY_DECLARE_WEBVIEW2_ID)
#undef SINGULARITY_DECLARE_WEBVIEW2_ID
#endif

namespace singularity::platform::webview_com {

// One handler: its Invoke calls `function` with Invoke's own arguments. It is
// born with one reference; it ends with its last.
template <typename Interface, typename Function, typename... Arguments>
class Handler final : public Interface {
public:
  explicit Handler(Function function) : function_(std::move(function)) {}
  Handler(const Handler &) = delete;
  Handler &operator=(const Handler &) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override {
    if (object == nullptr) return E_POINTER;
    if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, __uuidof(Interface))) {
      *object = static_cast<Interface *>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override { return function_(arguments...); }

private:
  ~Handler() = default;

  std::atomic<ULONG> references_{1};
  Function function_;
};

// Invoke's arguments, read from the interface itself. On x64 every COM
// method has the one calling convention, so the method's type needs none.
template <typename Method>
struct InvokeOf;

template <typename Interface, typename... Arguments>
struct InvokeOf<HRESULT (Interface::*)(Arguments...)> {
  template <typename Function>
  using HandlerFor = Handler<Interface, Function, Arguments...>;
};

// A handler for `Interface`, as WRL's Callback<Interface>(function) makes
// one: hand .Get() to WebView2, which keeps its own reference.
template <typename Interface, typename Function>
Microsoft::WRL::ComPtr<Interface> make_handler(Function function) {
  static_assert(sizeof(void *) == 8, "Singularity's Windows build is x64");
  using HandlerType = typename InvokeOf<decltype(&Interface::Invoke)>::template HandlerFor<Function>;
  Microsoft::WRL::ComPtr<Interface> handler;
  handler.Attach(new HandlerType(std::move(function)));
  return handler;
}

} // namespace singularity::platform::webview_com

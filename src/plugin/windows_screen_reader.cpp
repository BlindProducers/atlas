// SPDX-License-Identifier: GPL-3.0-or-later
// Built on its own, outside the plugin unity build, because it needs
// <windows.h>, the RPC runtime and UI Automation next to each other.
#include "windows_screen_reader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <rpc.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <cstdlib>
#include <mutex>
#include <string>

// The RPC client stub MIDL generates from NVDA's nvdaController.idl.
#include "nvdaController.h"

// The stub allocates and frees through these, and dispatches the SSML mark
// callback of NVDA's second interface to the client. Atlas never calls
// speakSsml, so the callback is never reached.
void* __RPC_USER midl_user_allocate(size_t size) {
  return std::malloc(size);
}

void __RPC_USER midl_user_free(void* pointer) {
  std::free(pointer);
}

error_status_t __stdcall nvdaController_onSsmlMarkReached(const wchar_t*) {
  return ERROR_CALL_NOT_IMPLEMENTED;
}

namespace atlas::windows_screen_reader {
  namespace {
    // NVDA listens on "NvdaCtlr.<session id>.<desktop name>", the name its
    // generateDesktopSpecificNamespace gives (nvdaHelper/common/winIPCUtils.cpp).
    bool bindToNvda() {
      static std::once_flag once;
      static bool bound = false;
      std::call_once(once, [] {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        wchar_t desktop[64] = {};
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                                  desktop, sizeof(desktop) - sizeof(wchar_t), nullptr);
        std::wstring binding = L"ncalrpc:[NvdaCtlr." + std::to_wstring(session) + L"." + desktop + L"]";
        bound = RpcBindingFromStringBindingW(reinterpret_cast<RPC_WSTR>(binding.data()),
                                             &nvdaControllerBindingHandle) == RPC_S_OK;
      });
      return bound;
    }

    using RaiseNotificationEvent = HRESULT (WINAPI*)(IRawElementProviderSimple*, NotificationKind,
                                                     NotificationProcessing, BSTR, BSTR);

    RaiseNotificationEvent raiseNotificationEvent() {
      static const auto function = [] {
        HMODULE module = LoadLibraryExW(L"UIAutomationCore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module != nullptr
            ? reinterpret_cast<RaiseNotificationEvent>(GetProcAddress(module, "UiaRaiseNotificationEvent"))
            : nullptr;
      }();
      return function;
    }
  }

  bool speakWithNvda(const wchar_t* text) {
    if (text == nullptr || *text == L'\0' || !bindToNvda())
      return false;
    // NVDA queues the text behind its own speech; a key press interrupts it
    // as it does NVDA's own messages. Braille shows it as a flash message
    // when NVDA's braille settings report live regions.
    if (nvdaController_speakText(text) != ERROR_SUCCESS)
      return false;
    nvdaController_brailleMessage(text);
    return true;
  }

  bool isNvdaRunning() {
    return bindToNvda() && nvdaController_testIfRunning() == ERROR_SUCCESS;
  }

  bool raiseNotification(void* juceNativeElement, const wchar_t* text, bool important) {
    auto raise = raiseNotificationEvent();
    if (raise == nullptr || juceNativeElement == nullptr || text == nullptr || *text == L'\0')
      return false;

    // JUCE 8's AccessibilityNativeHandle derives first from
    // IRawElementProviderSimple (juce_AccessibilityElement_windows.h), so the
    // handle starts with that interface's IUnknown, which JUCE itself queries.
    IRawElementProviderSimple* provider = nullptr;
    if (FAILED(static_cast<IUnknown*>(juceNativeElement)->QueryInterface(IID_PPV_ARGS(&provider))))
      return false;

    BSTR message = SysAllocString(text);
    BSTR activity = SysAllocString(L"AtlasAnnouncement");
    const HRESULT result = raise(provider, NotificationKind_ActionCompleted,
                                 important ? NotificationProcessing_ImportantAll : NotificationProcessing_All,
                                 message, activity);
    SysFreeString(activity);
    SysFreeString(message);
    provider->Release();
    return SUCCEEDED(result);
  }
}

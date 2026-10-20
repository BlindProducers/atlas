// A UI Automation client in the test process, for AtlasMacroTests
// --focus-events: it listens for focus-changed events on a thread of its own,
// as a screen reader in another process would, and keeps the name of each
// element this process says has the focus.

#include "uia_focus_events.h"

#include <windows.h>
#include <oleauto.h>
#include <UIAutomationClient.h>

#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

namespace atlas::uia_test {
  namespace {
    class Handler final : public IUIAutomationFocusChangedEventHandler {
      public:
        Handler(std::mutex& lock, std::vector<std::wstring>& names) : lock_(lock), names_(names) { }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override {
          const ULONG left = --references_;
          if (left == 0)
            delete this;
          return left;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
          if (id == __uuidof(IUnknown) || id == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *object = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
            AddRef();
            return S_OK;
          }
          *object = nullptr;
          return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* sender) override {
          if (sender == nullptr)
            return S_OK;
          // Focus anywhere on the desktop comes here: keep this process's only.
          int process = 0;
          if (FAILED(sender->get_CachedProcessId(&process)) || process != static_cast<int>(GetCurrentProcessId()))
            return S_OK;
          BSTR name = nullptr;
          if (SUCCEEDED(sender->get_CachedName(&name))) {
            const std::lock_guard<std::mutex> guard(lock_);
            names_.emplace_back(name != nullptr ? name : L"");
          }
          SysFreeString(name);
          return S_OK;
        }

      private:
        std::atomic<ULONG> references_ { 1 };
        std::mutex& lock_;
        std::vector<std::wstring>& names_;
    };
  }

  struct FocusEventRecorder::Impl {
    std::function<void()> pump;
    std::thread thread;
    std::atomic<bool> ready { false };
    std::atomic<bool> listening { false };
    std::atomic<bool> stop { false };
    std::atomic<bool> done { false };
    mutable std::mutex lock;
    std::vector<std::wstring> names;

    void waitFor(const std::atomic<bool>& flag) {
      for (int i = 0; i < 1000 && !flag.load(); ++i)
        pump();
    }

    void run() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      IUIAutomation* automation = nullptr;
      IUIAutomationCacheRequest* cache = nullptr;
      Handler* handler = new Handler(lock, names);
      if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                     __uuidof(IUIAutomation), reinterpret_cast<void**>(&automation))) &&
          SUCCEEDED(automation->CreateCacheRequest(&cache)) &&
          SUCCEEDED(cache->AddProperty(UIA_NamePropertyId)) &&
          SUCCEEDED(cache->AddProperty(UIA_ProcessIdPropertyId)) &&
          SUCCEEDED(automation->AddFocusChangedEventHandler(cache, handler)))
        listening = true;
      ready = true;
      while (!stop.load())
        Sleep(5);
      if (listening.load())
        automation->RemoveFocusChangedEventHandler(handler);
      handler->Release();
      if (cache != nullptr)
        cache->Release();
      if (automation != nullptr)
        automation->Release();
      CoUninitialize();
      done = true;
    }
  };

  FocusEventRecorder::FocusEventRecorder(std::function<void()> pump) : impl_(std::make_unique<Impl>()) {
    impl_->pump = std::move(pump);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    impl_->waitFor(impl_->ready);
  }

  FocusEventRecorder::~FocusEventRecorder() {
    impl_->stop = true;
    impl_->waitFor(impl_->done);
    impl_->thread.join();
  }

  bool FocusEventRecorder::isListening() const {
    return impl_->listening.load();
  }

  std::vector<std::wstring> FocusEventRecorder::names() const {
    const std::lock_guard<std::mutex> guard(impl_->lock);
    return impl_->names;
  }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Samples one thread's call stack about a thousand times a second, for
// AtlasBench --load --profile: where the message thread is while a host waits
// for the plugin. It needs no debug information in the build it watches: it
// writes raw addresses with the modules they fall in, and
// bench/symbolize_profile.py names them from the linker's map file.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace atlas_bench {
  class StackSampler {
  public:
    static constexpr int kMaxFrames = 48;

    ~StackSampler() { stop(); }

    // Call on the thread to watch. Samples are kept in memory until write().
    void start(size_t capacity = 120000) {
      samples_.resize(capacity);
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread_, 0, FALSE,
                      DUPLICATE_SAME_ACCESS);
      origin_ = std::chrono::steady_clock::now();
      running_ = true;
      sampler_ = std::thread([this] { run(); });
      // Looking up a function's unwind data takes a lock of the loader's. A
      // thread stopped while it holds that lock (it was loading a library)
      // would leave the sampler waiting for it for ever, so a stop that lasts
      // is ended from here; that one sample is then worthless, no more.
      watchdog_ = std::thread([this] {
        while (running_) {
          Sleep(20);
          const long long began = capture_began_ms_.load();
          if (began != 0 && nowMs() - began > 100)
            ResumeThread(thread_);
        }
      });
    }

    void stop() {
      if (!running_.exchange(false))
        return;
      sampler_.join();
      watchdog_.join();
      CloseHandle(thread_);
    }

    bool running() const { return running_; }
    void setStage(int stage) { stage_ = stage; }

    // Notes the libraries loaded now. A plugin is unloaded with its last
    // instance, before write() can ask where its addresses fell, so call this
    // while it is loaded.
    void noteModules() {
      HMODULE handles[2048];
      DWORD bytes = 0;
      if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &bytes))
        return;
      const size_t count = std::min<size_t>(bytes / sizeof(HMODULE), sizeof(handles) / sizeof(HMODULE));
      for (size_t i = 0; i < count; ++i) {
        MODULEINFO info = {};
        wchar_t name[1024] = {};
        if (!GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)) ||
            GetModuleFileNameW(handles[i], name, 1023) == 0)
          continue;
        char utf8[3072] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        modules_[reinterpret_cast<uintptr_t>(info.lpBaseOfDll)] = { info.SizeOfImage, utf8 };
      }
    }

    // One line per stage, module and sample; addresses in hexadecimal.
    bool write(const std::wstring& path, const std::vector<std::string>& stage_names) {
      FILE* file = nullptr;
      if (_wfopen_s(&file, path.c_str(), L"w") != 0 || file == nullptr)
        return false;

      noteModules();
      for (size_t i = 0; i < stage_names.size(); ++i)
        std::fprintf(file, "stage %d %s\n", int(i), stage_names[i].c_str());
      for (const auto& entry : modules_)
        std::fprintf(file, "module %llx %lx %s\n", static_cast<unsigned long long>(entry.first),
                     static_cast<unsigned long>(entry.second.size), entry.second.path.c_str());
      for (size_t i = 0; i < count_; ++i) {
        std::fprintf(file, "sample %d %.2f", samples_[i].stage, double(samples_[i].ms));
        for (int frame = 0; frame < samples_[i].count; ++frame)
          std::fprintf(file, " %llx", static_cast<unsigned long long>(samples_[i].frames[frame]));
        std::fputc('\n', file);
      }
      std::fclose(file);
      return true;
    }

  private:
    struct Sample {
      float ms = 0.0f;
      int stage = 0;
      int count = 0;
      DWORD64 frames[kMaxFrames] = {};
    };

    void run() {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      // A timer of its own, so the period does not hang on the system's tick.
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
      while (running_ && count_ < samples_.size()) {
        if (timer != nullptr) {
          LARGE_INTEGER due;
          due.QuadPart = -10000;  // 1 ms, in 100 ns units
          SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
          WaitForSingleObject(timer, 20);
        }
        else {
          Sleep(1);
        }
        Sample& sample = samples_[count_];
        sample.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - origin_).count();
        sample.stage = stage_;
        sample.count = capture(sample.frames);
        if (sample.count > 0)
          ++count_;
      }
      if (timer != nullptr)
        CloseHandle(timer);
    }

    // Nothing here may allocate: the watched thread can be stopped inside the heap.
    int capture(DWORD64* frames) {
      capture_began_ms_ = nowMs();
      if (SuspendThread(thread_) == static_cast<DWORD>(-1)) {
        capture_began_ms_ = 0;
        return 0;
      }
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      int count = 0;
      if (GetThreadContext(thread_, &context))
        count = unwind(context, frames);
      // The watchdog let the thread go: the stack moved under the unwinding.
      const bool lasted = nowMs() - capture_began_ms_.exchange(0) > 100;
      ResumeThread(thread_);
      return lasted ? 0 : count;
    }

    long long nowMs() const {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin_).count() + 1;
    }

    // Guarded by structured exception handling (so no C++ objects in here): a
    // stack caught mid-prologue can point anywhere.
    static int unwind(CONTEXT& context, DWORD64* frames) {
      int count = 0;
      __try {
        while (count < kMaxFrames && context.Rip != 0) {
          frames[count++] = context.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
          if (function == nullptr) {
            // A leaf function: its return address is on top of the stack.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
          }
          else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                             &establisher, nullptr);
          }
        }
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      return count;
    }

    struct Module {
      DWORD size = 0;
      std::string path;
    };

    std::vector<Sample> samples_;
    std::map<uintptr_t, Module> modules_;
    size_t count_ = 0;
    HANDLE thread_ = nullptr;
    std::thread sampler_;
    std::thread watchdog_;
    std::atomic<long long> capture_began_ms_ { 0 };
    std::atomic<bool> running_ { false };
    std::atomic<int> stage_ { 0 };
    std::chrono::steady_clock::time_point origin_;
  };
}

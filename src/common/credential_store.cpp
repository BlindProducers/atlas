// SPDX-License-Identifier: GPL-3.0-or-later
#include "credential_store.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace atlas::credentials {
namespace {
  // A copy of a secret is overwritten before it is freed.
  [[maybe_unused]] void wipe(std::string& text) {
    volatile char* bytes = text.data();
    for (size_t i = 0; i < text.size(); ++i)
      bytes[i] = 0;
    text.clear();
  }

  // Where there is nothing to keep secrets in.
  class UnavailableStore : public Store {
    public:
      explicit UnavailableStore(std::string reason) : reason_(std::move(reason)) { }

      std::string scope() const override { return {}; }
      std::string place() const override { return {}; }
      std::string unavailableReason() const override { return reason_; }

      Status read(const std::string&, std::string&, std::string& error) override {
        error = reason_;
        return Status::unavailable;
      }

      Status write(const std::string&, const std::string&, const std::string&, std::string& error) override {
        error = reason_;
        return Status::unavailable;
      }

      Status remove(const std::string&, std::string& error) override {
        error = reason_;
        return Status::unavailable;
      }

    private:
      std::string reason_;
  };

#if defined(__APPLE__)
  struct CFReleaser {
    void operator()(CFTypeRef ref) const {
      if (ref != nullptr)
        CFRelease(ref);
    }
  };
  template <typename T>
  using CFHolder = std::unique_ptr<std::remove_pointer_t<T>, CFReleaser>;

  CFHolder<CFStringRef> cfString(const std::string& text) {
    return CFHolder<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                                         static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
  }

  std::string utf8(CFStringRef text) {
    if (text == nullptr)
      return {};
    if (const char* direct = CFStringGetCStringPtr(text, kCFStringEncodingUTF8))
      return direct;
    const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<size_t>(size));
    if (!CFStringGetCString(text, buffer.data(), size, kCFStringEncodingUTF8))
      return {};
    return buffer.data();
  }

  // The app the plugin runs in: its bundle identifier, or its executable's
  // name. Logic and GarageBand run Audio Units in Apple's hosting service,
  // which is then the app.
  std::string hostIdentifier() {
    if (CFBundleRef bundle = CFBundleGetMainBundle()) {
      const std::string identifier = utf8(CFBundleGetIdentifier(bundle));
      if (!identifier.empty())
        return identifier;
      CFHolder<CFURLRef> executable(CFBundleCopyExecutableURL(bundle));
      if (executable != nullptr) {
        CFHolder<CFStringRef> name(CFURLCopyLastPathComponent(executable.get()));
        const std::string file = utf8(name.get());
        if (!file.empty())
          return file;
      }
    }
    return "unknown";
  }

  // Whether the app's signature names a developer or Apple, and so stays the
  // same across its updates. An ad hoc signature is a hash of the build: to
  // the keychain each new build is a stranger to the items the last one saved.
  bool hostSignatureIsStable() {
    SecCodeRef running = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &running) != errSecSuccess || running == nullptr)
      return false;
    CFHolder<SecCodeRef> code(running);
    SecStaticCodeRef on_disk = nullptr;
    if (SecCodeCopyStaticCode(running, kSecCSDefaultFlags, &on_disk) != errSecSuccess || on_disk == nullptr)
      return false;
    CFHolder<SecStaticCodeRef> static_code(on_disk);
    CFDictionaryRef information = nullptr;
    if (SecCodeCopySigningInformation(on_disk, kSecCSSigningInformation, &information) != errSecSuccess ||
        information == nullptr)
      return false;
    CFHolder<CFDictionaryRef> info(information);
    uint32_t flags = 0;
    if (const auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(information, kSecCodeInfoFlags)))
      CFNumberGetValue(number, kCFNumberSInt32Type, &flags);
    const auto certificates = static_cast<CFArrayRef>(CFDictionaryGetValue(information, kSecCodeInfoCertificates));
    return (flags & kSecCodeSignatureAdhoc) == 0 && certificates != nullptr && CFArrayGetCount(certificates) > 0;
  }

  std::string keychainError(OSStatus status) {
    switch (status) {
      case errSecUserCanceled:
        return "the keychain's request was cancelled";
      case errSecInteractionNotAllowed:
        return "the keychain is locked";
      case errSecNoSuchKeychain:
      case errSecNoDefaultKeychain:
        return "there is no login keychain";
      case errSecNotAvailable:
        return "no keychain is available";
      case errSecMissingEntitlement:
        return "this DAW may not use the keychain";
      default:
        return "the keychain refused it (error " + std::to_string(status) + ")";
    }
  }

  // Generic passwords in the login keychain, one per account under one service.
  class KeychainStore : public Store {
    public:
      KeychainStore(std::string service, std::string scope) : service_(std::move(service)), scope_(std::move(scope)) { }

      std::string scope() const override { return scope_; }
      std::string place() const override { return "your keychain"; }
      std::string unavailableReason() const override { return {}; }

      Status read(const std::string& account, std::string& secret, std::string& error) override {
        const auto query = itemQuery(account);
        CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef found = nullptr;
        const OSStatus status = SecItemCopyMatching(query.get(), &found);
        const CFHolder<CFTypeRef> result(found);
        if (status == errSecItemNotFound)
          return Status::not_found;
        if (status != errSecSuccess) {
          error = keychainError(status);
          return Status::refused;
        }
        if (found == nullptr || CFGetTypeID(found) != CFDataGetTypeID()) {
          error = "the keychain item holds no password";
          return Status::refused;
        }
        const auto data = static_cast<CFDataRef>(found);
        secret.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<size_t>(CFDataGetLength(data)));
        return Status::ok;
      }

      Status write(const std::string& account, const std::string& label, const std::string& secret,
                   std::string& error) override {
        const CFHolder<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()),
                                                    static_cast<CFIndex>(secret.size())));
        const auto name = cfString(label);
        const auto item = itemQuery(account);
        CFDictionarySetValue(item.get(), kSecValueData, data.get());
        CFDictionarySetValue(item.get(), kSecAttrDescription, CFSTR("Atlas API token"));
        if (name != nullptr)
          CFDictionarySetValue(item.get(), kSecAttrLabel, name.get());
        OSStatus status = SecItemAdd(item.get(), nullptr);
        if (status == errSecDuplicateItem) {
          // This app saved one before: its item takes the new password.
          const CFHolder<CFMutableDictionaryRef> changes(CFDictionaryCreateMutable(
              kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
          CFDictionarySetValue(changes.get(), kSecValueData, data.get());
          if (name != nullptr)
            CFDictionarySetValue(changes.get(), kSecAttrLabel, name.get());
          status = SecItemUpdate(itemQuery(account).get(), changes.get());
        }
        if (status != errSecSuccess) {
          error = keychainError(status);
          return Status::refused;
        }
        return Status::ok;
      }

      Status remove(const std::string& account, std::string& error) override {
        const OSStatus status = SecItemDelete(itemQuery(account).get());
        if (status == errSecItemNotFound)
          return Status::not_found;
        if (status != errSecSuccess) {
          error = keychainError(status);
          return Status::refused;
        }
        return Status::ok;
      }

    private:
      CFHolder<CFMutableDictionaryRef> itemQuery(const std::string& account) const {
        CFHolder<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                         &kCFTypeDictionaryKeyCallBacks,
                                                                         &kCFTypeDictionaryValueCallBacks));
        CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
        if (const auto service = cfString(service_))
          CFDictionarySetValue(query.get(), kSecAttrService, service.get());
        if (const auto name = cfString(account))
          CFDictionarySetValue(query.get(), kSecAttrAccount, name.get());
        // The login keychain. The data protection keychain keeps items in
        // access groups that come from the host app's entitlements, which most
        // DAWs do not have (errSecMissingEntitlement).
        CFDictionarySetValue(query.get(), kSecUseDataProtectionKeychain, kCFBooleanFalse);
        return query;
      }

      std::string service_;
      std::string scope_;
  };

#elif defined(_WIN32)
  std::wstring wide(const std::string& text) {
    if (text.empty())
      return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0)
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
  }

  std::string credentialError(DWORD code) {
    if (code == ERROR_NO_SUCH_LOGON_SESSION)
      return "Windows Credential Manager is not available in this session";
    return "Windows Credential Manager refused it (error " + std::to_string(code) + ")";
  }

  Status failure(DWORD code, std::string& error) {
    error = credentialError(code);
    return code == ERROR_NO_SUCH_LOGON_SESSION ? Status::unavailable : Status::refused;
  }

  // Generic credentials, kept on this computer for every app the user runs.
  class CredentialManagerStore : public Store {
    public:
      explicit CredentialManagerStore(std::string service) : service_(std::move(service)) { }

      std::string scope() const override { return {}; }
      std::string place() const override { return "Windows Credential Manager"; }
      std::string unavailableReason() const override { return {}; }

      Status read(const std::string& account, std::string& secret, std::string& error) override {
        const std::wstring target = targetName(account);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
          const DWORD code = GetLastError();
          return code == ERROR_NOT_FOUND ? Status::not_found : failure(code, error);
        }
        secret.assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
        return Status::ok;
      }

      Status write(const std::string& account, const std::string& label, const std::string& secret,
                   std::string& error) override {
        if (secret.size() > static_cast<size_t>(CRED_MAX_CREDENTIAL_BLOB_SIZE)) {
          error = "it is too long for Windows Credential Manager";
          return Status::refused;
        }
        std::wstring target = targetName(account);
        std::wstring comment = wide(label);
        std::string blob = secret;   // CredWriteW takes it through a pointer to non-const
        CREDENTIALW credential {};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = target.data();
        credential.Comment = comment.empty() ? nullptr : comment.data();
        credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
        // On this computer only: CRED_PERSIST_ENTERPRISE would roam with the profile.
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        const BOOL written = CredWriteW(&credential, 0);
        const DWORD code = written ? ERROR_SUCCESS : GetLastError();
        wipe(blob);
        return written ? Status::ok : failure(code, error);
      }

      Status remove(const std::string& account, std::string& error) override {
        const std::wstring target = targetName(account);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
          const DWORD code = GetLastError();
          return code == ERROR_NOT_FOUND ? Status::not_found : failure(code, error);
        }
        return Status::ok;
      }

    private:
      std::wstring targetName(const std::string& account) const {
        return wide(account.empty() ? service_ : service_ + "/" + account);
      }

      std::string service_;
  };
#endif
}

std::shared_ptr<Store> systemStore(const std::string& service, [[maybe_unused]] bool require_stable_host) {
#if defined(__APPLE__)
  if (require_stable_host && !hostSignatureIsStable())
    return std::make_shared<UnavailableStore>("this DAW has no developer signature, so the keychain would ask for "
                                              "your password after every update of it");
  return std::make_shared<KeychainStore>(service, hostIdentifier());
#elif defined(_WIN32)
  return std::make_shared<CredentialManagerStore>(service);
#else
  (void) service;
  return std::make_shared<UnavailableStore>("Atlas has no credential store on this system");
#endif
}

void disallowInteractionForTesting() {
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  // Process-wide, which is why only the tests do it: a plugin would be turning
  // the host's own keychain dialogs off too.
  SecKeychainSetUserInteractionAllowed(false);
#pragma clang diagnostic pop
#endif
}

MemoryStore::MemoryStore(std::string scope, std::string place) : scope_(std::move(scope)), place_(std::move(place)) { }

std::string MemoryStore::scope() const {
  std::lock_guard<std::mutex> hold(lock_);
  return unavailable_reason_.empty() ? scope_ : std::string();
}

std::string MemoryStore::place() const {
  std::lock_guard<std::mutex> hold(lock_);
  return unavailable_reason_.empty() ? place_ : std::string();
}

std::string MemoryStore::unavailableReason() const {
  std::lock_guard<std::mutex> hold(lock_);
  return unavailable_reason_;
}

Status MemoryStore::read(const std::string& account, std::string& secret, std::string& error) {
  std::lock_guard<std::mutex> hold(lock_);
  ++reads_;
  if (!unavailable_reason_.empty()) {
    error = unavailable_reason_;
    return Status::unavailable;
  }
  const auto found = secrets_.find(account);
  if (found == secrets_.end())
    return Status::not_found;
  secret = found->second.second;
  return Status::ok;
}

Status MemoryStore::write(const std::string& account, const std::string& label, const std::string& secret,
                          std::string& error) {
  std::lock_guard<std::mutex> hold(lock_);
  if (!unavailable_reason_.empty()) {
    error = unavailable_reason_;
    return Status::unavailable;
  }
  if (refusing_writes_) {
    error = "the test store refused it";
    return Status::refused;
  }
  secrets_[account] = { label, secret };
  return Status::ok;
}

Status MemoryStore::remove(const std::string& account, std::string& error) {
  std::lock_guard<std::mutex> hold(lock_);
  if (!unavailable_reason_.empty()) {
    error = unavailable_reason_;
    return Status::unavailable;
  }
  return secrets_.erase(account) > 0 ? Status::ok : Status::not_found;
}

void MemoryStore::setScope(std::string scope) {
  std::lock_guard<std::mutex> hold(lock_);
  scope_ = std::move(scope);
}

void MemoryStore::setRefusingWrites(bool refusing) {
  std::lock_guard<std::mutex> hold(lock_);
  refusing_writes_ = refusing;
}

void MemoryStore::setUnavailable(std::string reason) {
  std::lock_guard<std::mutex> hold(lock_);
  unavailable_reason_ = std::move(reason);
}

bool MemoryStore::holds(const std::string& account) const {
  std::lock_guard<std::mutex> hold(lock_);
  return secrets_.count(account) > 0;
}

std::string MemoryStore::secretOf(const std::string& account) const {
  std::lock_guard<std::mutex> hold(lock_);
  const auto found = secrets_.find(account);
  return found != secrets_.end() ? found->second.second : std::string();
}

std::string MemoryStore::labelOf(const std::string& account) const {
  std::lock_guard<std::mutex> hold(lock_);
  const auto found = secrets_.find(account);
  return found != secrets_.end() ? found->second.first : std::string();
}

int MemoryStore::reads() const {
  std::lock_guard<std::mutex> hold(lock_);
  return reads_;
}

void MemoryStore::erase(const std::string& account) {
  std::lock_guard<std::mutex> hold(lock_);
  secrets_.erase(account);
}
}

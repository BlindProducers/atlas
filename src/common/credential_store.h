// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

// Secrets in the system's credential store: generic passwords in the macOS
// login keychain, generic credentials in Windows Credential Manager. Plain C++
// on purpose: Security.h (which brings Carbon's Component) and <windows.h> stay
// in credential_store.cpp, outside the unity files, away from JUCE's names.
namespace atlas::credentials {
  enum class Status {
    ok,
    not_found,
    refused,       // the store said no: locked, cancelled, or not this app's item
    unavailable    // there is no store to ask
  };

  class Store {
    public:
      virtual ~Store() = default;

      // Whose secrets these are. On macOS each app a plugin runs in keeps its
      // own, named by the app's bundle identifier ("com.cockos.reaper"): the
      // keychain ties an item to the app that saved it and asks the user, with
      // their password, before another app reads it. Empty where every app
      // shares one set (Windows).
      virtual std::string scope() const = 0;
      // Where the secrets go, as Settings says it: "your keychain",
      // "Windows Credential Manager". Empty when there is no store.
      virtual std::string place() const = 0;
      // Why nothing can be kept, for a store that is unavailable: a clause that
      // follows "because", such as "Atlas has no credential store on this system".
      virtual std::string unavailableReason() const = 0;

      // `account` names the secret within the store's service. Each call fills
      // `error`, when it fails, with why: again a clause that follows "because".
      virtual Status read(const std::string& account, std::string& secret, std::string& error) = 0;
      // Saves `secret`, replacing what `account` held. `label` is the name the
      // keychain shows for the item.
      virtual Status write(const std::string& account, const std::string& label, const std::string& secret,
                           std::string& error) = 0;
      virtual Status remove(const std::string& account, std::string& error) = 0;
  };

  // The system's store for `service` ("org.blindproducers.atlas.community"),
  // or one that is unavailable on systems without one. On macOS it is also
  // unavailable inside an app signed ad hoc, whose signature changes with every
  // build, so the keychain would ask for the user's password after each one;
  // `require_stable_host` false lifts that, for the round-trip test.
  std::shared_ptr<Store> systemStore(const std::string& service, bool require_stable_host = true);

  // Tests: keychain calls in this process fail rather than show a dialog.
  void disallowInteractionForTesting();

  // Secrets in memory, for tests, so that none reach the real keychain. It can
  // stand for another app (setScope), for a store that refuses to keep
  // anything, or for none at all.
  class MemoryStore : public Store {
    public:
      explicit MemoryStore(std::string scope = "com.example.daw", std::string place = "the test store");

      std::string scope() const override;
      std::string place() const override;
      std::string unavailableReason() const override;
      Status read(const std::string& account, std::string& secret, std::string& error) override;
      Status write(const std::string& account, const std::string& label, const std::string& secret,
                   std::string& error) override;
      Status remove(const std::string& account, std::string& error) override;

      void setScope(std::string scope);
      // Writes fail as a locked or refusing keychain makes them fail; reads and
      // removals still work.
      void setRefusingWrites(bool refusing);
      // Every call fails as on a system with no store, for this reason.
      void setUnavailable(std::string reason);

      bool holds(const std::string& account) const;
      std::string secretOf(const std::string& account) const;
      std::string labelOf(const std::string& account) const;
      int reads() const;
      // Forgets a secret behind the plugin's back, as Keychain Access can.
      void erase(const std::string& account);

    private:
      mutable std::mutex lock_;
      std::string scope_;
      std::string place_;
      std::string unavailable_reason_;
      bool refusing_writes_ = false;
      int reads_ = 0;
      std::map<std::string, std::pair<std::string, std::string>> secrets_;   // account: label, secret
  };
}

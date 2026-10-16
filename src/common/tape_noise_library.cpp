// SPDX-License-Identifier: GPL-3.0-or-later
#include "tape_noise_library.h"
#include "tape_asset_key.h"
#include "load_save.h"
#include <mutex>

#if JUCE_MAC
 #include <CommonCrypto/CommonCryptor.h>
 #include <CommonCrypto/CommonHMAC.h>
 #include <dlfcn.h>
#elif JUCE_WINDOWS
 #include <windows.h>
 #include <bcrypt.h>
#else
 #include <openssl/evp.h>
 #include <openssl/hmac.h>
 #include <dlfcn.h>
#endif

namespace vital {
namespace {
juce::File bundledDirectory() {
  static int anchor;
 #if JUCE_WINDOWS
  HMODULE module = nullptr;
  wchar_t path[32768] {};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&anchor), &module) ||
      !GetModuleFileNameW(module, path, 32768)) return {};
  const juce::File binary { juce::String(path) };
 #else
  Dl_info info {};
  if (dladdr(&anchor, &info) == 0 || !info.dli_fname) return {};
  const juce::File binary { juce::String(info.dli_fname) };
 #endif
  return binary.getParentDirectory().getSiblingFile("Resources").getChildFile("Tape");
}

bool decrypt(const juce::MemoryBlock& encrypted, juce::MemoryBlock& plain) {
  const auto size = encrypted.getSize();
  const auto* data = static_cast<const uint8_t*>(encrypted.getData());
  if (size < 72 || size > 128 * 1024 * 1024 || (size - 56) % 16 != 0 ||
      std::memcmp(data, "ATLASTP1", 8) != 0) return false;
  uint8_t tag[32] {};
  const size_t authenticated_size = size - 32, ciphertext_size = size - 56;
 #if JUCE_MAC
  CCHmac(kCCHmacAlgSHA256, tape::asset_keys + 32, 32, data, authenticated_size, tag);
 #elif JUCE_WINDOWS
  BCRYPT_ALG_HANDLE hash_algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&hash_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
  const auto hash_status = BCryptHash(hash_algorithm, const_cast<PUCHAR>(tape::asset_keys + 32), 32,
                                    const_cast<PUCHAR>(data), static_cast<ULONG>(authenticated_size), tag, 32);
  BCryptCloseAlgorithmProvider(hash_algorithm, 0);
  if (hash_status < 0) return false;
 #else
  unsigned int tag_size = 32;
  if (!HMAC(EVP_sha256(), tape::asset_keys + 32, 32, data, authenticated_size, tag, &tag_size)) return false;
 #endif
  // Authenticate the header, IV and ciphertext before decrypting or parsing.
  unsigned int difference = 0;
  for (int i = 0; i < 32; ++i) difference |= tag[i] ^ data[authenticated_size + i];
  if (difference != 0) return false;
  plain.setSize(ciphertext_size);
  size_t decoded_size = 0;
 #if JUCE_MAC
  if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, tape::asset_keys, 32, data + 8,
              data + 24, ciphertext_size, plain.getData(), plain.getSize(), &decoded_size) != kCCSuccess) return false;
 #elif JUCE_WINDOWS
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE key = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return false;
  auto result = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
  if (result >= 0) result = BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0, const_cast<PUCHAR>(tape::asset_keys), 32, 0);
  uint8_t iv[16]; std::memcpy(iv, data + 8, 16);
  ULONG length = 0;
  if (result >= 0) result = BCryptDecrypt(key, const_cast<PUCHAR>(data + 24), static_cast<ULONG>(ciphertext_size), nullptr,
                                         iv, 16, static_cast<PUCHAR>(plain.getData()), static_cast<ULONG>(plain.getSize()), &length, BCRYPT_BLOCK_PADDING);
  if (key) BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (result < 0) return false;
  decoded_size = length;
 #else
  auto* context = EVP_CIPHER_CTX_new();
  if (!context) return false;
  int length = 0, tail = 0;
  auto* out = static_cast<unsigned char*>(plain.getData());
  const bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr, tape::asset_keys, data + 8) == 1 &&
      EVP_DecryptUpdate(context, out, &length, data + 24, static_cast<int>(ciphertext_size)) == 1 &&
      EVP_DecryptFinal_ex(context, out + length, &tail) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!ok) return false;
  decoded_size = length + tail;
 #endif
  plain.setSize(decoded_size);
  return true;
}
}

std::shared_ptr<TapeNoiseLibrary> TapeNoiseLibrary::shared() {
  static std::mutex mutex;
  static std::weak_ptr<TapeNoiseLibrary> resource;
  std::lock_guard<std::mutex> lock(mutex);
  auto result = resource.lock();
  if (!result) { result = std::make_shared<TapeNoiseLibrary>(); resource = result; }
  return result;
}

TapeNoiseLibrary::TapeNoiseLibrary(juce::File directory) :
    juce::Thread("Atlas Tape noise loader"), directory_(directory), explicit_directory_(directory != juce::File()) {
  for (auto& pointer : published_) pointer.store(nullptr);
  for (auto& state : status_) state.store(0);
  startThread(juce::Thread::Priority::low);
}
TapeNoiseLibrary::~TapeNoiseLibrary() {
  signalThreadShouldExit(); notify(); stopThread(-1);
}
juce::File TapeNoiseLibrary::factoryDirectory() {
  return LoadSave::getDataDirectory().getChildFile("Factory").getChildFile("Samples").getChildFile("Tape");
}
const TapeNoiseLibrary::Clip* TapeNoiseLibrary::request(int index) noexcept {
  index = std::clamp(index, 0, kNumClips - 1);
  const auto* clip = published_[index].load(std::memory_order_acquire);
  if (!clip) requested_.fetch_or(uint32_t(1) << index, std::memory_order_relaxed);
  return clip;
}
int TapeNoiseLibrary::status(int index) const noexcept {
  return status_[std::clamp(index, 0, kNumClips - 1)].load(std::memory_order_acquire);
}
std::unique_ptr<TapeNoiseLibrary::Clip> TapeNoiseLibrary::decode(const juce::File& file) {
  if (file.getSize() < 72 || file.getSize() > 128 * 1024 * 1024) return {};
  juce::MemoryBlock encrypted, plain;
  if (!file.loadFileAsData(encrypted) || !decrypt(encrypted, plain)) return {};
  juce::WavAudioFormat format;
  auto stream = std::make_unique<juce::MemoryInputStream>(plain.getData(), plain.getSize(), false);
  std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(stream.release(), true));
  if (!reader || reader->lengthInSamples < 2 || reader->lengthInSamples > 32 * 1024 * 1024 ||
      reader->numChannels < 1 || reader->numChannels > 2 || reader->sampleRate < 8000 || reader->sampleRate > 384000) return {};
  auto clip = std::make_unique<Clip>();
  clip->sample_rate = reader->sampleRate;
  clip->audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
  if (!reader->read(&clip->audio, 0, clip->audio.getNumSamples(), 0, true, true)) return {};
  return clip;
}
void TapeNoiseLibrary::run() {
  bool located = false;
  // The clips inside the plug-in bundle. They are read directly when the
  // factory folder cannot be written (a sandboxed host, or a Documents folder
  // the host was refused) or a copy there is missing or damaged.
  juce::File bundled;
  std::array<double, kNumClips> attempted {};
  while (!threadShouldExit()) {
    const uint32_t requested = requested_.exchange(0, std::memory_order_relaxed);
    if (requested && !located) {
      if (!explicit_directory_) {
        directory_ = factoryDirectory();
        bundled = bundledDirectory();
        // Ship encrypted files only. The factory folder is the playback source
        // when it can be written; otherwise the bundle is.
        if (bundled.isDirectory() && directory_.createDirectory().wasOk()) {
          for (int index = 1; index <= kNumClips && !threadShouldExit(); ++index) {
            const auto name = "Vinyl " + juce::String(index) + ".atape";
            const auto source = bundled.getChildFile(name), destination = directory_.getChildFile(name);
            if (source.existsAsFile() && !source.hasIdenticalContentTo(destination)) {
              juce::TemporaryFile replacement(destination);
              if (source.copyFileTo(replacement.getFile())) replacement.overwriteTargetFileWithTemporary();
            }
          }
        }
      }
      located = true;
    }
    for (int i = 0; i < kNumClips && !threadShouldExit(); ++i) {
      if (!(requested & (uint32_t(1) << i)) || clips_[i]) continue;
      const double now = juce::Time::getMillisecondCounterHiRes();
      if (attempted[i] != 0 && now - attempted[i] < 1000) continue;
      attempted[i] = now;
      const auto name = "Vinyl " + juce::String(i + 1) + ".atape";
      clips_[i] = decode(directory_.getChildFile(name));
      if (!clips_[i] && bundled.isDirectory())
        clips_[i] = decode(bundled.getChildFile(name));
      if (clips_[i]) {
        published_[i].store(clips_[i].get(), std::memory_order_release);
        status_[i].store(1, std::memory_order_release);
      }
      else if (status_[i].exchange(-1) != -1)
        juce::Logger::writeToLog("Atlas Tape: missing or invalid encrypted noise Vinyl " + juce::String(i + 1));
    }
    wait(50);
  }
}
}

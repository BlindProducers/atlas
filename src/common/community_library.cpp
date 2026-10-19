// SPDX-License-Identifier: GPL-3.0-or-later
#include "community_library.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace atlas::community {
namespace {
  using json = nlohmann::json;

  constexpr const char* kDefaultLibraryUrl = "https://atlas.blindproducers.org";
  constexpr int kConnectionTimeoutMs = 15000;
  constexpr int kRedirectsToFollow = 5;
  // A page of JSON, and a preset: the library refuses larger uploads.
  constexpr juce::int64 kMaxPageBytes = 16 * 1024 * 1024;
  constexpr juce::int64 kMaxPresetBytes = 64 * 1024 * 1024;
  // 100 pages of 200 is far more than the library holds; a `next` chain that
  // never ends stops here.
  constexpr int kMaxPages = 100;
  constexpr int kChunkBytes = 64 * 1024;

  juce::String& testingLibraryUrl() {
    static juce::String url;
    return url;
  }

  juce::String userAgent() {
    return "Atlas/" + juce::String(ProjectInfo::versionString);
  }

  // SHA-256 (FIPS 180-4), a few lines rather than another JUCE module.
  class Sha256 {
    public:
      void update(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        total_ += size;
        while (size > 0) {
          const size_t take = std::min(size, sizeof(buffer_) - buffered_);
          std::memcpy(buffer_ + buffered_, bytes, take);
          buffered_ += take;
          bytes += take;
          size -= take;
          if (buffered_ == sizeof(buffer_)) {
            block(buffer_);
            buffered_ = 0;
          }
        }
      }

      // Pads and returns the digest as lower-case hex. Call once.
      juce::String hex() {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80;
        const uint8_t zero = 0;
        update(&one, 1);
        while (buffered_ != 56)
          update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; ++i)
          length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(length, 8);

        static constexpr char digits[] = "0123456789abcdef";
        juce::String out;
        out.preallocateBytes(64);
        for (const uint32_t word : state_) {
          for (int shift = 28; shift >= 0; shift -= 4)
            out += digits[(word >> shift) & 0xf];
        }
        return out;
      }

    private:
      static uint32_t rotr(uint32_t x, unsigned bits) { return (x >> bits) | (x << (32 - bits)); }

      void block(const uint8_t* p) {
        static constexpr uint32_t kRound[64] = {
          0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
          0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
          0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
          0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
          0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
          0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
          0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
          0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
          w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                 (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
          const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
          const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
          w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
          const uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
          const uint32_t choose = (e & f) ^ (~e & g);
          const uint32_t t1 = h + big_s1 + choose + kRound[i] + w[i];
          const uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
          const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
          const uint32_t t2 = big_s0 + majority;
          h = g;
          g = f;
          f = e;
          e = d + t1;
          d = c;
          c = b;
          b = a;
          a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
      }

      uint32_t state_[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
      uint8_t buffer_[64] = {};
      size_t buffered_ = 0;
      uint64_t total_ = 0;
  };

  juce::String jsonString(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string())
      return {};
    return juce::String(found->get_ref<const std::string&>());
  }

  juce::int64 jsonInteger(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end())
      return 0;
    if (found->is_number_integer())
      return found->get<juce::int64>();
    if (found->is_number_float())
      return static_cast<juce::int64>(found->get<double>());
    return 0;
  }

  const json& jsonObject(const json& object, const char* key) {
    static const json empty = json::object();
    const auto found = object.find(key);
    return found != object.end() && found->is_object() ? *found : empty;
  }

  juce::String httpStatusText(int status) {
    switch (status) {
      case 0: return "did not answer";
      case 401: case 403: return "refused the request (" + juce::String(status) + ")";
      case 404: return "has no such page (404)";
      case 429: return "asked to slow down (429)";
      case 500: case 502: case 503: case 504: return "is having trouble (" + juce::String(status) + ")";
      default: return "answered " + juce::String(status);
    }
  }

  bool isWebUrl(const juce::URL& url) {
    const juce::String scheme = url.getScheme();
    return url.isWellFormed() && (scheme == "https" || scheme == "http");
  }

  // The file a file: URL names, without its query string (a download URL
  // carries ?v=<version>, which is not part of a saved file's name).
  juce::File localFileOf(const juce::URL& url) {
    return juce::URL(url.toString(false)).getLocalFile();
  }

  std::unique_ptr<juce::InputStream> openStream(const juce::URL& url, const juce::String& accept,
                                                juce::String& error) {
    if (url.isLocalFile()) {
      const juce::File file = localFileOf(url);
      auto stream = file.createInputStream();
      if (stream == nullptr)
        error = "The file " + file.getFullPathName() + " could not be read";
      return stream;
    }
    if (!isWebUrl(url)) {
      error = url.toString(true) + " is not a web address";
      return nullptr;
    }

    int status = 0;
    auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                            .withConnectionTimeoutMs(kConnectionTimeoutMs)
                                            .withExtraHeaders("User-Agent: " + userAgent() + "\r\nAccept: " + accept + "\r\n")
                                            .withStatusCode(&status)
                                            .withNumRedirectsToFollow(kRedirectsToFollow));
    if (stream == nullptr) {
      error = "Could not connect to " + url.getDomain();
      return nullptr;
    }
    if (status < 200 || status >= 300) {
      error = url.getDomain() + " " + httpStatusText(status);
      return nullptr;
    }
    return stream;
  }

  // Copies `stream` to `out` (and into `hash`), at most `max_bytes`.
  juce::String readAll(juce::InputStream& stream, juce::OutputStream& out, Sha256* hash, juce::int64 max_bytes,
                       const std::atomic<bool>* cancelled) {
    juce::HeapBlock<char> chunk(static_cast<size_t>(kChunkBytes));
    juce::int64 total = 0;
    while (!stream.isExhausted()) {
      if (cancelled != nullptr && cancelled->load())
        return "Cancelled";
      const int read = stream.read(chunk, kChunkBytes);
      if (read < 0)
        return "The connection was lost";
      if (read == 0)
        break;
      total += read;
      if (total > max_bytes)
        return "The download is larger than expected";
      if (!out.write(chunk, static_cast<size_t>(read)))
        return "Could not write the download";
      if (hash != nullptr)
        hash->update(chunk, static_cast<size_t>(read));
    }
    return {};
  }

  // One part of a path: a file or folder name that stays inside its parent.
  juce::String pathPart(juce::String text, const juce::String& fallback) {
    text = juce::File::createLegalFileName(text.trim());
    while (text.contains(".."))
      text = text.replace("..", ".");
    // Not hidden: the library scan skips folders and files that start with a dot.
    text = text.trimCharactersAtStart(".").trim();
    if (text.isEmpty())
      text = fallback;
    return text;
  }
}

juce::String libraryUrl() {
  if (testingLibraryUrl().isNotEmpty())
    return testingLibraryUrl();
  const juce::String environment = juce::SystemStats::getEnvironmentVariable("ATLAS_COMMUNITY_URL", {}).trim();
  if (environment.isNotEmpty())
    return environment.trimCharactersAtEnd("/");
  return kDefaultLibraryUrl;
}

juce::String libraryHost() {
  const juce::String url = libraryUrl();
  if (url.startsWith("file:"))
    return "a saved copy of the library";
  const juce::String host = juce::URL(url).getDomain();
  return host.isNotEmpty() ? host : url;
}

juce::String presetsPageUrl(const juce::String& library_url, int page) {
  if (library_url.startsWith("file:"))
    return library_url;
  return library_url.trimCharactersAtEnd("/") + "/api/v1/presets/?kind=patch&page_size=" + juce::String(kPageSize) +
         "&page=" + juce::String(std::max(1, page));
}

bool isLibraryUrl(const juce::String& url, const juce::String& library_url) {
  if (library_url.startsWith("file:"))
    return url.startsWith("file:");
  const juce::URL candidate(url);
  return isWebUrl(candidate) && candidate.getDomain().equalsIgnoreCase(juce::URL(library_url).getDomain());
}

void setLibraryUrlForTesting(const juce::String& url) {
  testingLibraryUrl() = url;
}

juce::String parsePresetsPage(const std::string& text, std::vector<RemotePreset>& presets, juce::String& next_url) {
  next_url.clear();
  json page;
  try {
    page = json::parse(text, nullptr, false);
  }
  catch (const json::exception&) {
    page = json();
  }
  if (page.is_discarded() || !page.is_object())
    return "The library sent a page that is not JSON";
  const auto results = page.find("results");
  if (results == page.end() || !results->is_array()) {
    const juce::String error = jsonString(page, "error");
    return error.isNotEmpty() ? "The library said: " + error : juce::String("The library's page has no presets in it");
  }

  next_url = jsonString(page, "next");
  for (const auto& entry : *results) {
    if (!entry.is_object())
      continue;
    RemotePreset preset;
    preset.id = jsonInteger(entry, "id");
    preset.kind = jsonString(entry, "kind");
    preset.name = jsonString(entry, "name").trim();
    preset.style = jsonString(entry, "style");
    preset.style_label = jsonString(entry, "style_label");
    preset.description = jsonString(entry, "description");
    preset.author = jsonString(entry, "author").trim();
    preset.downloads = static_cast<int>(jsonInteger(entry, "downloads"));
    preset.page_url = jsonString(entry, "url");
    const juce::String updated = jsonString(entry, "updated_at");
    if (updated.isNotEmpty())
      preset.updated_ms = juce::Time::fromISO8601(updated).toMilliseconds();
    const auto tags = entry.find("tags");
    if (tags != entry.end() && tags->is_array()) {
      for (const auto& tag : *tags) {
        if (tag.is_string()) {
          const juce::String name = juce::String(tag.get_ref<const std::string&>()).trim();
          if (name.isNotEmpty())
            preset.tags.addIfNotAlreadyThere(name);
        }
      }
    }
    const json& uploader = jsonObject(entry, "uploader");
    preset.uploader = jsonString(uploader, "username").trim();
    preset.uploader_name = jsonString(uploader, "display_name").trim();
    if (preset.uploader_name.isEmpty())
      preset.uploader_name = preset.uploader;
    const json& file = jsonObject(entry, "file");
    preset.download_url = jsonString(file, "url");
    preset.file_name = jsonString(file, "name").trim();
    preset.extension = jsonString(file, "extension").trimCharactersAtStart(".").toLowerCase();
    preset.file_size = jsonInteger(file, "size");
    preset.sha256 = jsonString(file, "sha256").trim().toLowerCase();
    // Without a name or a file there is nothing to show or fetch.
    if (preset.name.isEmpty() || preset.download_url.isEmpty())
      continue;
    presets.push_back(std::move(preset));
  }
  return {};
}

juce::File communityDirectory(const juce::File& data_directory) {
  return data_directory.getChildFile("Community").getChildFile("Presets");
}

juce::File localFile(const RemotePreset& preset, const juce::File& data_directory) {
  const juce::String extension = preset.extension.isNotEmpty() ? preset.extension : juce::String("atp");
  juce::String name = preset.file_name;
  if (name.isEmpty())
    name = preset.name + "." + extension;
  if (!name.endsWithIgnoreCase("." + extension))
    name += "." + extension;
  const juce::String uploader = preset.uploader.isNotEmpty() ? preset.uploader : preset.uploader_name;
  const juce::String style = preset.style_label.isNotEmpty() ? preset.style_label : juce::String("Other");
  return communityDirectory(data_directory)
      .getChildFile(pathPart(uploader, "Unknown uploader"))
      .getChildFile(pathPart(style, "Other"))
      .getChildFile(pathPart(name, "Preset." + extension));
}

juce::String sha256Hex(const void* data, size_t size) {
  Sha256 hash;
  hash.update(data, size);
  return hash.hex();
}

juce::String sha256Hex(juce::InputStream& stream, const std::function<bool()>& keep_going) {
  Sha256 hash;
  juce::HeapBlock<char> chunk(static_cast<size_t>(kChunkBytes));
  while (!stream.isExhausted()) {
    if (keep_going && !keep_going())
      return {};
    const int read = stream.read(chunk, kChunkBytes);
    if (read <= 0)
      break;
    hash.update(chunk, static_cast<size_t>(read));
  }
  return hash.hex();
}

juce::String fetchText(const juce::URL& url, std::string& text, const std::atomic<bool>* cancelled) {
  text.clear();
  juce::String error;
  auto stream = openStream(url, "application/json", error);
  if (stream == nullptr)
    return error;
  juce::MemoryOutputStream out;
  error = readAll(*stream, out, nullptr, kMaxPageBytes, cancelled);
  if (error.isNotEmpty())
    return error;
  text.assign(static_cast<const char*>(out.getData()), out.getDataSize());
  return {};
}

juce::String fetchAllPresets(const juce::String& library_url, std::vector<RemotePreset>& presets,
                             const std::atomic<bool>* cancelled) {
  presets.clear();
  juce::String url = presetsPageUrl(library_url, 1);
  for (int page = 0; page < kMaxPages && url.isNotEmpty(); ++page) {
    std::string text;
    const juce::String error = fetchText(juce::URL(url), text, cancelled);
    if (error.isNotEmpty())
      return error;
    juce::String next;
    const juce::String parse_error = parsePresetsPage(text, presets, next);
    if (parse_error.isNotEmpty())
      return parse_error;
    // Only the library's own pages, and never the same one again.
    if (next == url || !isLibraryUrl(next, library_url))
      break;
    url = next;
  }
  return {};
}

juce::String downloadFile(const juce::URL& url, const juce::File& target, const juce::String& expected_sha256,
                          juce::int64 expected_size, const std::atomic<bool>* cancelled) {
  const juce::File folder = target.getParentDirectory();
  if (!folder.createDirectory())
    return "Could not create the folder " + folder.getFullPathName();

  juce::String error;
  auto stream = openStream(url, "*/*", error);
  if (stream == nullptr)
    return error;

  const juce::File part = target.getSiblingFile(target.getFileName() + ".part");
  part.deleteFile();
  {
    juce::FileOutputStream out(part);
    if (out.failedToOpen())
      return "Could not write " + part.getFullPathName();
    Sha256 hash;
    error = readAll(*stream, out, &hash, expected_size > 0 ? expected_size : kMaxPresetBytes, cancelled);
    out.flush();
    if (error.isEmpty()) {
      if (expected_size > 0 && out.getPosition() != expected_size) {
        error = "The download is " + juce::String(out.getPosition()) + " bytes, not the " +
                juce::String(expected_size) + " the library lists";
      }
      else if (expected_sha256.isNotEmpty() && !hash.hex().equalsIgnoreCase(expected_sha256)) {
        error = "The download does not match the library's checksum";
      }
    }
  }
  if (error.isNotEmpty()) {
    part.deleteFile();
    return error;
  }
  if (target.existsAsFile() && !target.deleteFile()) {
    part.deleteFile();
    return "Could not replace " + target.getFullPathName();
  }
  if (!part.moveFileTo(target)) {
    part.deleteFile();
    return "Could not move the download to " + target.getFullPathName();
  }
  return {};
}
}

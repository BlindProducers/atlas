// SPDX-License-Identifier: GPL-3.0-or-later
#include "community_library.h"
#include "credential_store.h"
#include "load_save.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

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
  // An upload may be a bank of hundreds of megabytes.
  constexpr int kUploadTimeoutMs = 120000;
  constexpr const char* kTokenConfigKey = "community_api_token";
  constexpr const char* kAccountUsernameKey = "community_account_username";
  constexpr const char* kAccountNameKey = "community_account_name";
  constexpr const char* kBrowserShownKey = "community_browser";

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

  // Whether the API token may go to `url`: over https, or over http only to
  // this computer (the tests' server). Anywhere else plain http would carry
  // the token across the network unencrypted.
  bool mayCarryToken(const juce::URL& url) {
    if (!isWebUrl(url))
      return false;
    if (url.getScheme() == "https")
      return true;
    const juce::String host = url.getDomain().toLowerCase();
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
  }

  juce::String insecureTokenError() {
    return "unsecure address, host not using HTTPS.";
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

#if JUCE_WINDOWS
  // Whether Windows keeps `name` for a device (CON, NUL, COM1 and the like),
  // as it does whatever extension follows: no file or folder can have it.
  bool isWindowsDeviceName(const juce::String& name) {
    const juce::String stem = name.upToFirstOccurrenceOf(".", false, false).trimEnd().toUpperCase();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$")
      return true;
    if (stem.length() != 4 || !(stem.startsWith("COM") || stem.startsWith("LPT")))
      return false;
    // COM0 to COM9 and LPT0 to LPT9, with superscript 1, 2 and 3 too.
    const juce_wchar digit = stem[3];
    return (digit >= '0' && digit <= '9') || digit == 0xb9 || digit == 0xb2 || digit == 0xb3;
  }
#endif

  // One part of a path: a file or folder name that stays inside its parent.
  juce::String pathPart(juce::String text, const juce::String& fallback) {
    text = juce::File::createLegalFileName(text.trim());
    while (text.contains(".."))
      text = text.replace("..", ".");
    // Not hidden: the library scan skips folders and files that start with a dot.
    text = systemFileName(text.trimCharactersAtStart(".").trim());
    if (text.isEmpty())
      text = fallback;
    return text;
  }

  // The `preview` object of a preset or bank; none when it is null.
  RemotePreview parsePreview(const json& entry) {
    RemotePreview preview;
    const json& object = jsonObject(entry, "preview");
    preview.url = jsonString(object, "url");
    preview.file_name = jsonString(object, "name").trim();
    preview.content_type = jsonString(object, "content_type").trim().toLowerCase();
    preview.size = jsonInteger(object, "size");
    const auto duration = object.find("duration");
    if (duration != object.end() && duration->is_number())
      preview.duration_seconds = std::max(0.0, duration->get<double>());
    return preview;
  }

  // The extension a preview's content type implies, for a name without one.
  juce::String extensionForContentType(const juce::String& content_type) {
    static const std::pair<const char*, const char*> kExtensions[] = {
      { "audio/mpeg", "mp3" }, { "audio/mp3", "mp3" }, { "audio/ogg", "ogg" }, { "audio/wav", "wav" },
      { "audio/x-wav", "wav" }, { "audio/flac", "flac" }, { "audio/mp4", "m4a" }, { "audio/aac", "aac" },
      { "audio/aiff", "aiff" }, { "audio/x-aiff", "aiff" } };
    for (const auto& [type, extension] : kExtensions) {
      if (content_type == type)
        return extension;
    }
    return "mp3";
  }

  // The file name a cached preview gets: the library's, which names the
  // format Atlas decodes it with, or the item's name with the type's extension.
  juce::String previewFileName(const RemotePreview& preview, const juce::String& item_name) {
    juce::String name = preview.file_name;
    if (name.isEmpty())
      name = item_name + " preview";
    const juce::String extension = name.contains(".") ? name.fromLastOccurrenceOf(".", false, false) : juce::String();
    if (extension.isEmpty() || extension.length() > 5 || extension.containsAnyOf(" /\\"))
      name += "." + extensionForContentType(preview.content_type);
    return name;
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

juce::String banksPageUrl(const juce::String& library_url, int page) {
  if (library_url.startsWith("file:"))
    return juce::URL(localFileOf(juce::URL(library_url)).getSiblingFile("banks.json")).toString(false);
  return library_url.trimCharactersAtEnd("/") + "/api/v1/banks/?page_size=" + juce::String(kPageSize) +
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

namespace {
  // Parses a page of results, returning what went wrong or an empty string
  // and leaving the results array in `results`.
  juce::String parsePage(const std::string& text, const char* what, json& page, const json*& results,
                         juce::String& next_url) {
    next_url.clear();
    results = nullptr;
    try {
      page = json::parse(text, nullptr, false);
    }
    catch (const json::exception&) {
      page = json();
    }
    if (page.is_discarded() || !page.is_object())
      return "The library sent a page that is not JSON";
    const auto found = page.find("results");
    if (found == page.end() || !found->is_array()) {
      const juce::String error = jsonString(page, "error");
      return error.isNotEmpty() ? "The library said: " + error : "The library's page has no " + juce::String(what) + " in it";
    }
    next_url = jsonString(page, "next");
    results = &*found;
    return {};
  }

  juce::StringArray jsonTags(const json& entry) {
    juce::StringArray result;
    const auto tags = entry.find("tags");
    if (tags != entry.end() && tags->is_array()) {
      for (const auto& tag : *tags) {
        if (tag.is_string()) {
          const juce::String name = juce::String(tag.get_ref<const std::string&>()).trim();
          if (name.isNotEmpty())
            result.addIfNotAlreadyThere(name);
        }
      }
    }
    return result;
  }

  // Page after page from `first_url`, each read by `parse_page`, following
  // `next` only within the library and never to the same page again.
  juce::String fetchPages(const juce::String& first_url, const juce::String& library_url,
                          const std::function<juce::String(const std::string&, juce::String&)>& parse_page,
                          const std::atomic<bool>* cancelled) {
    juce::String url = first_url;
    for (int page = 0; page < kMaxPages && url.isNotEmpty(); ++page) {
      std::string text;
      const juce::String error = fetchText(juce::URL(url), text, cancelled);
      if (error.isNotEmpty())
        return error;
      juce::String next;
      const juce::String parse_error = parse_page(text, next);
      if (parse_error.isNotEmpty())
        return parse_error;
      if (next == url || !isLibraryUrl(next, library_url))
        break;
      url = next;
    }
    return {};
  }
}

juce::String parsePresetsPage(const std::string& text, std::vector<RemotePreset>& presets, juce::String& next_url) {
  json page;
  const json* results = nullptr;
  const juce::String error = parsePage(text, "presets", page, results, next_url);
  if (error.isNotEmpty())
    return error;

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
    preset.tags = jsonTags(entry);
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
    preset.preview = parsePreview(entry);
    // Without a name or a file there is nothing to show or fetch.
    if (preset.name.isEmpty() || preset.download_url.isEmpty())
      continue;
    presets.push_back(std::move(preset));
  }
  return {};
}

juce::String parseBanksPage(const std::string& text, std::vector<RemoteBank>& banks, juce::String& next_url) {
  json page;
  const json* results = nullptr;
  const juce::String error = parsePage(text, "banks", page, results, next_url);
  if (error.isNotEmpty())
    return error;

  for (const auto& entry : *results) {
    if (!entry.is_object())
      continue;
    RemoteBank bank;
    bank.id = jsonInteger(entry, "id");
    bank.name = jsonString(entry, "name").trim();
    const json& genre = jsonObject(entry, "genre");
    bank.genre = jsonString(genre, "name").trim();
    bank.genre_slug = jsonString(genre, "slug").trim();
    bank.tags = jsonTags(entry);
    bank.description = jsonString(entry, "description");
    const json& uploader = jsonObject(entry, "uploader");
    bank.uploader = jsonString(uploader, "username").trim();
    bank.uploader_name = jsonString(uploader, "display_name").trim();
    if (bank.uploader_name.isEmpty())
      bank.uploader_name = bank.uploader;
    const json& file = jsonObject(entry, "file");
    bank.download_url = jsonString(file, "url");
    bank.file_name = jsonString(file, "name").trim();
    bank.extension = jsonString(file, "extension").trimCharactersAtStart(".").toLowerCase();
    bank.file_size = jsonInteger(file, "size");
    bank.sha256 = jsonString(file, "sha256").trim().toLowerCase();
    bank.preview = parsePreview(entry);
    const json& counts = jsonObject(entry, "counts");
    bank.presets = static_cast<int>(jsonInteger(counts, "preset"));
    bank.wavetables = static_cast<int>(jsonInteger(counts, "wavetable"));
    bank.lfos = static_cast<int>(jsonInteger(counts, "lfo"));
    bank.fx = static_cast<int>(jsonInteger(counts, "fx"));
    bank.samples = static_cast<int>(jsonInteger(counts, "sample"));
    bank.total_files = static_cast<int>(jsonInteger(entry, "total_files"));
    bank.unpacked_bytes = jsonInteger(entry, "unpacked_bytes");
    bank.downloads = static_cast<int>(jsonInteger(entry, "downloads"));
    bank.page_url = jsonString(entry, "url");
    const juce::String updated = jsonString(entry, "updated_at");
    if (updated.isNotEmpty())
      bank.updated_ms = juce::Time::fromISO8601(updated).toMilliseconds();
    if (bank.name.isEmpty() || bank.download_url.isEmpty())
      continue;
    banks.push_back(std::move(bank));
  }
  return {};
}

juce::String bankContentsText(const RemoteBank& bank) {
  juce::StringArray parts;
  auto add = [&parts](int count, const char* singular, const char* plural) {
    if (count > 0)
      parts.add(juce::String(count) + " " + (count == 1 ? singular : plural));
  };
  add(bank.presets, "preset", "presets");
  add(bank.wavetables, "wavetable", "wavetables");
  add(bank.lfos, "LFO shape", "LFO shapes");
  add(bank.fx, "effect preset", "effect presets");
  add(bank.samples, "sample", "samples");
  if (parts.isEmpty())
    return "no Atlas files";
  if (parts.size() == 1)
    return parts[0];
  const juce::String last = parts[parts.size() - 1];
  parts.remove(parts.size() - 1);
  return parts.joinIntoString(", ") + " and " + last;
}

juce::File communityDirectory(const juce::File& data_directory) {
  return data_directory.getChildFile("Community");
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
      .getChildFile("Banks")
      .getChildFile(pathPart(uploader, "Unknown uploader"))
      .getChildFile(pathPart(style, "Other"))
      .getChildFile(pathPart(name, "Preset." + extension));
}

juce::File banksDirectory(const juce::File& data_directory) {
  return data_directory.getChildFile("Community").getChildFile("Archives");
}

juce::File localFile(const RemoteBank& bank, const juce::File& data_directory) {
  const juce::String extension = bank.extension.isNotEmpty() ? bank.extension : juce::String("atb");
  juce::String name = bank.file_name;
  if (name.isEmpty())
    name = bank.name + "." + extension;
  if (!name.endsWithIgnoreCase("." + extension))
    name += "." + extension;
  const juce::String uploader = bank.uploader.isNotEmpty() ? bank.uploader : bank.uploader_name;
  return banksDirectory(data_directory)
      .getChildFile(pathPart(uploader, "Unknown uploader"))
      .getChildFile(pathPart(name, "Bank." + extension));
}

juce::File previewDirectory(const juce::File& data_directory) {
  return data_directory.getChildFile("Community").getChildFile("Previews");
}

juce::File localPreviewFile(const RemotePreset& preset, const juce::File& data_directory) {
  const juce::String uploader = preset.uploader.isNotEmpty() ? preset.uploader : preset.uploader_name;
  return previewDirectory(data_directory)
      .getChildFile("Presets")
      .getChildFile(pathPart(uploader, "Unknown uploader"))
      .getChildFile(pathPart(previewFileName(preset.preview, preset.name), "Preview.mp3"));
}

juce::File localPreviewFile(const RemoteBank& bank, const juce::File& data_directory) {
  const juce::String uploader = bank.uploader.isNotEmpty() ? bank.uploader : bank.uploader_name;
  return previewDirectory(data_directory)
      .getChildFile("Banks")
      .getChildFile(pathPart(uploader, "Unknown uploader"))
      .getChildFile(pathPart(previewFileName(bank.preview, bank.name), "Preview.mp3"));
}

juce::String systemFileName(const juce::String& name) {
#if JUCE_WINDOWS
  juce::String printable;
  for (const juce_wchar character : name)
    printable += (character < 0x20 ? juce_wchar(' ') : character);
  juce::String text = printable.trimCharactersAtEnd(". ").trim();
  if (isWindowsDeviceName(text)) {
    const int dot = text.indexOfChar('.');
    text = dot < 0 ? text + "_" : text.substring(0, dot) + "_" + text.substring(dot);
  }
  return text;
#else
  return name;
#endif
}

juce::String durationText(double seconds) {
  if (!(seconds > 0.0))
    return {};
  const int whole = static_cast<int>(std::lround(seconds));
  if (whole < 1)
    return "under a second";
  const int minutes = whole / 60;
  const int rest = whole % 60;
  juce::String text;
  if (minutes > 0)
    text = juce::String(minutes) + (minutes == 1 ? " minute" : " minutes");
  if (rest > 0 || minutes == 0)
    text += (text.isEmpty() ? juce::String() : juce::String(" ")) + juce::String(rest) + (rest == 1 ? " second" : " seconds");
  return text;
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
  return fetchPages(presetsPageUrl(library_url, 1), library_url,
                    [&presets](const std::string& text, juce::String& next) {
                      return parsePresetsPage(text, presets, next);
                    },
                    cancelled);
}

juce::String fetchAllBanks(const juce::String& library_url, std::vector<RemoteBank>& banks,
                           const std::atomic<bool>* cancelled) {
  banks.clear();
  return fetchPages(banksPageUrl(library_url, 1), library_url,
                    [&banks](const std::string& text, juce::String& next) {
                      return parseBanksPage(text, banks, next);
                    },
                    cancelled);
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

const std::vector<Style>& styles() {
  // The library's own list (library/models.py, Style), in its order.
  static const std::vector<Style> kStyles = {
    { "atmosphere", "Atmosphere" }, { "bass", "Bass" }, { "bell", "Bells" }, { "brass", "Brass" },
    { "chord", "Chords" }, { "drums", "Drums" }, { "experiment", "Experiment" }, { "guitar", "Guitar" },
    { "keys", "Keys" }, { "lead", "Lead" }, { "organ", "Organ" }, { "pad", "Pad" },
    { "percussion", "Percussion" }, { "piano", "Piano" }, { "pluck", "Pluck" }, { "sequence", "Sequence" },
    { "sfx", "SFX" }, { "strings", "Strings" }, { "template", "Template" }, { "vocal", "Vocal" },
    { "woodwind", "Woodwind" }, { "world", "World" }, { "other", "Other" } };
  return kStyles;
}

juce::String styleIdForText(const juce::String& text) {
  // The library's own reading (library/models.py, style_from_text): an id or
  // label, then the other names a style goes by, then anything that starts
  // with an id ("Bass drop", "Leads").
  const juce::String value = text.trim().toLowerCase();
  if (value.isEmpty())
    return {};
  for (const auto& style : styles()) {
    if (value == style.id || value == juce::String(style.label).toLowerCase())
      return style.id;
  }
  static const std::pair<const char*, const char*> kAliases[] = {
    { "atmos", "atmosphere" }, { "soundscape", "atmosphere" }, { "soundscapes", "atmosphere" },
    { "texture", "atmosphere" }, { "textures", "atmosphere" }, { "drone", "atmosphere" }, { "drones", "atmosphere" },
    { "ambient", "atmosphere" },
    { "basses", "bass" }, { "sub", "bass" }, { "sub bass", "bass" },
    { "mallet", "bell" }, { "mallets", "bell" }, { "chime", "bell" }, { "chimes", "bell" },
    { "horn", "brass" }, { "horns", "brass" }, { "trumpet", "brass" }, { "trombone", "brass" },
    { "stab", "chord" }, { "stabs", "chord" },
    { "drum", "drums" }, { "drum kit", "drums" }, { "drumkit", "drums" }, { "kit", "drums" },
    { "experimental", "experiment" },
    { "key", "keys" }, { "keyboard", "keys" }, { "keyboards", "keys" }, { "electric piano", "keys" },
    { "e-piano", "keys" }, { "epiano", "keys" }, { "rhodes", "keys" }, { "wurlitzer", "keys" }, { "wurli", "keys" },
    { "leads", "lead" },
    { "pads", "pad" },
    { "perc", "percussion" }, { "percussive", "percussion" },
    { "seq", "sequence" }, { "sequences", "sequence" }, { "arp", "sequence" }, { "arps", "sequence" },
    { "arpeggio", "sequence" }, { "arpeggios", "sequence" }, { "arpeggiated", "sequence" },
    { "fx", "sfx" }, { "sound effects", "sfx" }, { "sound effect", "sfx" }, { "effects", "sfx" },
    { "string", "strings" }, { "violin", "strings" }, { "violins", "strings" }, { "cello", "strings" },
    { "init", "template" },
    { "voice", "vocal" }, { "voices", "vocal" }, { "vox", "vocal" }, { "choir", "vocal" }, { "choirs", "vocal" },
    { "wind", "woodwind" }, { "winds", "woodwind" }, { "flute", "woodwind" }, { "flutes", "woodwind" },
    { "reed", "woodwind" }, { "reeds", "woodwind" }, { "sax", "woodwind" }, { "saxophone", "woodwind" },
    { "clarinet", "woodwind" },
    { "ethnic", "world" } };
  for (const auto& [alias, id] : kAliases) {
    if (value == alias)
      return id;
  }
  for (const auto& style : styles()) {
    if (juce::String(style.id) != "other" && value.startsWith(style.id))
      return style.id;
  }
  return {};
}

juce::String tokensPageUrl(const juce::String& library_url) {
  return library_url.trimCharactersAtEnd("/") + "/accounts/tokens/";
}

juce::String genresPageUrl(const juce::String& library_url) {
  if (library_url.startsWith("file:"))
    return juce::URL(localFileOf(juce::URL(library_url)).getSiblingFile("genres.json")).toString(false);
  return library_url.trimCharactersAtEnd("/") + "/api/v1/genres/";
}

juce::String parseGenresPage(const std::string& text, std::vector<Genre>& genres) {
  json page;
  const json* results = nullptr;
  juce::String next;
  const juce::String error = parsePage(text, "genres", page, results, next);
  if (error.isNotEmpty())
    return error;
  for (const auto& entry : *results) {
    if (!entry.is_object())
      continue;
    Genre genre;
    genre.name = jsonString(entry, "name").trim();
    genre.slug = jsonString(entry, "slug").trim();
    if (genre.name.isNotEmpty() && genre.slug.isNotEmpty())
      genres.push_back(std::move(genre));
  }
  return {};
}

juce::String fetchGenres(const juce::String& library_url, std::vector<Genre>& genres,
                         const std::atomic<bool>* cancelled) {
  genres.clear();
  std::string text;
  const juce::String error = fetchText(juce::URL(genresPageUrl(library_url)), text, cancelled);
  if (error.isNotEmpty())
    return error;
  return parseGenresPage(text, genres);
}

juce::String presetsEndpoint(const juce::String& library_url) {
  return library_url.trimCharactersAtEnd("/") + "/api/v1/presets/";
}

juce::String banksEndpoint(const juce::String& library_url) {
  return library_url.trimCharactersAtEnd("/") + "/api/v1/banks/";
}

namespace {
  UploadRequest buildUpload(const juce::String& endpoint, const juce::File& file, const juce::String& mime_type,
                            const ShareFields& fields, bool with_style, bool with_genre, const juce::String& token) {
    UploadRequest request;
    const juce::URL target(endpoint);
    if (!isWebUrl(target)) {
      request.error = "Sharing needs the library's web address, not " + endpoint;
      return request;
    }
    if (!mayCarryToken(target)) {
      request.error = insecureTokenError();
      return request;
    }
    if (!file.existsAsFile()) {
      request.error = "The file to share, " + file.getFileName() + ", is missing";
      return request;
    }
    const juce::String key = token.trim();
    if (key.isEmpty()) {
      request.error = "No API token. Make one on your account's API tokens page on " + target.getDomain();
      return request;
    }
    juce::URL url = target;
    auto add = [&url](const char* name, const juce::String& value) {
      if (value.trim().isNotEmpty())
        url = url.withParameter(name, value.trim());
    };
    add("name", fields.name);
    if (with_style)
      add("style", fields.style);
    if (with_genre)
      add("genre", fields.genre);
    add("tags", fields.tags);
    add("description", fields.description);
    request.url = url.withFileToUpload("file", file, mime_type);
    // No line breaks may reach the headers from the token.
    request.headers = "User-Agent: " + userAgent() + "\r\nAccept: application/json\r\nAuthorization: Bearer " +
                      key.removeCharacters("\r\n") + "\r\n";
    return request;
  }

  juce::String fieldLabel(const juce::String& field) {
    if (field == "__all__")
      return {};
    if (field == "file")
      return "File";
    return field.substring(0, 1).toUpperCase() + field.substring(1).replaceCharacter('_', ' ');
  }
}

UploadRequest buildPresetUpload(const juce::String& library_url, const juce::File& preset, const ShareFields& fields,
                                const juce::String& token) {
  return buildUpload(presetsEndpoint(library_url), preset, "application/octet-stream", fields, true, true, token);
}

UploadRequest buildBankUpload(const juce::String& library_url, const juce::File& bank, const ShareFields& fields,
                              const juce::String& token) {
  return buildUpload(banksEndpoint(library_url), bank, "application/zip", fields, false, true, token);
}

UploadResult parseUploadAnswer(int status, const std::string& body) {
  UploadResult result;
  result.status = status;
  json answer;
  try {
    answer = json::parse(body, nullptr, false);
  }
  catch (const json::exception&) {
    answer = json();
  }
  const bool is_object = !answer.is_discarded() && answer.is_object();
  if (status == 201) {
    if (!is_object) {
      result.error = "The library's answer is not JSON";
      return result;
    }
    result.id = jsonInteger(answer, "id");
    result.name = jsonString(answer, "name").trim();
    result.page_url = jsonString(answer, "url");
    const json& uploader = jsonObject(answer, "uploader");
    result.uploader.username = jsonString(uploader, "username").trim();
    result.uploader.display_name = jsonString(uploader, "display_name").trim();
    return result;
  }

  const juce::String message = is_object ? jsonString(answer, "error").trim() : juce::String();
  if (status == 401) {
    result.error = "The library refused the API token. Check it on your account's API tokens page";
    return result;
  }
  if (is_object) {
    const json& errors = jsonObject(answer, "errors");
    for (const auto& item : errors.items()) {
      const std::string field = item.key();
      const json& messages = item.value();
      juce::StringArray texts;
      if (messages.is_array()) {
        for (const auto& text : messages) {
          if (text.is_string())
            texts.add(juce::String(text.get_ref<const std::string&>()).trim());
        }
      }
      else if (messages.is_string()) {
        texts.add(juce::String(messages.get_ref<const std::string&>()).trim());
      }
      texts.removeEmptyStrings();
      if (texts.isEmpty())
        continue;
      const juce::String label = fieldLabel(juce::String(field));
      result.field_errors.add((label.isNotEmpty() ? label + ": " : juce::String()) + texts.joinIntoString(" "));
    }
  }
  if (status == 400) {
    result.error = message.isNotEmpty() ? message : juce::String("The upload was not accepted.");
    if (!result.field_errors.isEmpty())
      result.error = result.error.trimCharactersAtEnd(".") + ". " + result.field_errors.joinIntoString(". ");
    return result;
  }
  if (status == 413) {
    result.error = "The file is larger than the library accepts";
    return result;
  }
  result.error = "The library " + httpStatusText(status) + (message.isNotEmpty() ? ": " + message : juce::String());
  return result;
}

UploadResult upload(const UploadRequest& request, const std::atomic<bool>* cancelled, const UploadProgress& progress) {
  UploadResult result;
  if (request.error.isNotEmpty()) {
    result.error = request.error;
    return result;
  }
  int status = 0;
  // JUCE tells how much of the body has gone out as it goes (on macOS about
  // every millisecond, on Windows after each chunk); answering false breaks
  // the connection off, which is how a cancelled share stops.
  auto stream = request.url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                                  .withHttpRequestCmd("POST")
                                                  .withExtraHeaders(request.headers)
                                                  .withConnectionTimeoutMs(kUploadTimeoutMs)
                                                  .withStatusCode(&status)
                                                  .withNumRedirectsToFollow(0)
                                                  .withProgressCallback([cancelled, progress](int sent, int total) {
                                                    if (cancelled != nullptr && cancelled->load())
                                                      return false;
                                                    return !progress || progress(sent, total);
                                                  }));
  if (stream == nullptr) {
    result.status = status;
    result.error = status != 0 ? "The library " + httpStatusText(status)
                               : "Could not connect to " + request.url.getDomain();
    return result;
  }
  juce::MemoryOutputStream out;
  const juce::String error = readAll(*stream, out, nullptr, kMaxPageBytes, cancelled);
  if (error.isNotEmpty()) {
    result.status = status;
    result.error = error;
    return result;
  }
  return parseUploadAnswer(status, std::string(static_cast<const char*>(out.getData()), out.getDataSize()));
}

UploadResult uploadPreset(const juce::String& library_url, const juce::String& token, const juce::File& preset,
                          const ShareFields& fields, const std::atomic<bool>* cancelled, const UploadProgress& progress) {
  return upload(buildPresetUpload(library_url, preset, fields, token), cancelled, progress);
}

UploadResult uploadBank(const juce::String& library_url, const juce::String& token, const juce::File& bank,
                        const ShareFields& fields, const std::atomic<bool>* cancelled, const UploadProgress& progress) {
  return upload(buildBankUpload(library_url, bank, fields, token), cancelled, progress);
}

namespace {
  // -1 until the config has been read, then 0 or 1.
  std::atomic<int>& browserShownState() {
    static std::atomic<int> state { -1 };
    return state;
  }
}

juce::String meEndpoint(const juce::String& library_url) {
  return library_url.trimCharactersAtEnd("/") + "/api/v1/me/";
}

juce::String parseAccount(int status, const std::string& body, Account& account) {
  account = {};
  if (status == 401 || status == 403)
    return "The library refused the API token";
  json answer;
  try {
    answer = json::parse(body, nullptr, false);
  }
  catch (const json::exception&) {
    answer = json();
  }
  if (status < 200 || status >= 300)
    return "The library " + httpStatusText(status);
  if (answer.is_discarded() || !answer.is_object())
    return "The library's answer is not JSON";
  account.username = jsonString(answer, "username").trim();
  account.display_name = jsonString(answer, "display_name").trim();
  account.presets = static_cast<int>(jsonInteger(answer, "presets"));
  account.banks = static_cast<int>(jsonInteger(answer, "banks"));
  if (!account.known())
    return "The library's answer names no account";
  return {};
}

juce::String fetchAccount(const juce::String& library_url, const juce::String& token, Account& account,
                          const std::atomic<bool>* cancelled) {
  account = {};
  const juce::URL url(meEndpoint(library_url));
  if (!isWebUrl(url))
    return "Logging in needs the library's web address";
  if (!mayCarryToken(url))
    return insecureTokenError();
  const juce::String key = token.trim().removeCharacters("\r\n");
  if (key.isEmpty())
    return "No API token";
  int status = 0;
  auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                          .withConnectionTimeoutMs(kConnectionTimeoutMs)
                                          .withExtraHeaders("User-Agent: " + userAgent() +
                                                            "\r\nAccept: application/json\r\nAuthorization: Bearer " +
                                                            key + "\r\n")
                                          .withStatusCode(&status)
                                          .withNumRedirectsToFollow(0));
  if (stream == nullptr)
    return status != 0 ? parseAccount(status, {}, account) : "Could not connect to " + url.getDomain();
  juce::MemoryOutputStream out;
  const juce::String error = readAll(*stream, out, nullptr, kMaxPageBytes, cancelled);
  if (error.isNotEmpty())
    return error;
  return parseAccount(status, std::string(static_cast<const char*>(out.getData()), out.getDataSize()), account);
}

juce::String accountName(const Account& account) {
  if (account.display_name.isNotEmpty() && account.display_name != account.username)
    return account.display_name + " (" + account.username + ")";
  return account.username;
}

namespace {
  constexpr const char* kCredentialService = "org.blindproducers.atlas.community";
  // Which DAWs are logged in, as whom, by login key:
  // {"com.cockos.reaper": {"username": "jay", "name": "Jay Ladder", "saved": 1792000000000}}.
  // "saved" marks each token saved, so that another process holding the last
  // one knows to read the store again. The tokens are in the store.
  constexpr const char* kLoginsKey = "community_logins";
  constexpr const char* kLoginUsernameKey = "username";
  constexpr const char* kLoginNameKey = "name";
  constexpr const char* kLoginSavedKey = "saved";
  // The login key where every DAW shares one token (Windows).
  constexpr const char* kSharedLoginKey = "all";

  // What this process holds of the token, for every Atlas in it.
  struct TokenMemory {
    std::mutex lock;
    std::shared_ptr<credentials::Store> store;   // the system's, until a test sets one
    juce::String token;                          // as read or saved; empty when none is held
    juce::int64 saved = 0;                       // the login's "saved" mark `token` belongs to
    bool session_only = false;                   // the store would not keep it: memory alone holds it
    Account session_account;                     // its account, then
    juce::String session_reason;                 // why the store would not keep it
  };

  TokenMemory& tokenMemory() {
    static TokenMemory memory;
    return memory;
  }

  // The rest take the memory's lock first.
  credentials::Store& tokenStore(TokenMemory& memory) {
    if (memory.store == nullptr)
      memory.store = credentials::systemStore(kCredentialService);
    return *memory.store;
  }

  std::string loginKey(credentials::Store& store) {
    const std::string scope = store.scope();
    return scope.empty() ? std::string(kSharedLoginKey) : scope;
  }

  // The DAW the plugin runs in, as Settings names it: "Reaper", "Apple Logic".
  juce::String dawName() {
    const juce::String described = juce::PluginHostType().getHostDescription();
    if (described.isNotEmpty() && described != "Unknown")
      return described;
    const juce::String file = juce::File::getSpecialLocation(juce::File::hostApplicationPath)
                                  .getFileNameWithoutExtension();
    return file.isNotEmpty() ? file : juce::String("this DAW");
  }

  void forgetHeldToken(TokenMemory& memory) {
    memory.token = {};
    memory.saved = 0;
    memory.session_only = false;
    memory.session_account = {};
    memory.session_reason = {};
  }

  json* findLogin(json& config, const std::string& key) {
    if (!config.is_object())
      return nullptr;
    const auto logins = config.find(kLoginsKey);
    if (logins == config.end() || !logins->is_object())
      return nullptr;
    const auto login = logins->find(key);
    return login != logins->end() && login->is_object() ? &*login : nullptr;
  }

  void eraseLogin(json& config, const std::string& key) {
    const auto logins = config.find(kLoginsKey);
    if (logins == config.end() || !logins->is_object())
      return;
    logins->erase(key);
    if (logins->empty())
      config.erase(kLoginsKey);
  }

  Account loginAccount(const json& login) {
    Account account;
    account.username = jsonString(login, kLoginUsernameKey).trim();
    account.display_name = jsonString(login, kLoginNameKey).trim();
    return account;
  }

  void setLoginAccount(json& login, const Account& account) {
    if (account.known()) {
      login[kLoginUsernameKey] = account.username.toStdString();
      login[kLoginNameKey] = account.display_name.toStdString();
    }
    else {
      login.erase(kLoginUsernameKey);
      login.erase(kLoginNameKey);
    }
  }

  // Saves `token` for this DAW in the store, with `account`, and holds it; or,
  // where the store will not keep it, holds it in memory alone. The caller
  // saves `config`.
  void keepToken(TokenMemory& memory, json& config, const juce::String& token, const Account& account) {
    auto& store = tokenStore(memory);
    const std::string key = loginKey(store);
    const juce::String label = store.scope().empty() ? juce::String("Atlas community library token")
                                                     : "Atlas community library token for " + dawName();
    std::string error;
    const auto status = store.write(key, label.toStdString(), token.toStdString(), error);
    const json* last = findLogin(config, key);
    const juce::int64 last_saved = last != nullptr ? jsonInteger(*last, kLoginSavedKey) : 0;
    forgetHeldToken(memory);
    memory.token = token;
    if (status == credentials::Status::ok) {
      json login = json::object();
      setLoginAccount(login, account);
      memory.saved = std::max(juce::Time::currentTimeMillis(), last_saved + 1);
      login[kLoginSavedKey] = memory.saved;
      if (!config[kLoginsKey].is_object())
        config[kLoginsKey] = json::object();
      config[kLoginsKey][key] = login;
      return;
    }
    // Not in the store, so not logged in by the config either. Nothing more is
    // asked of the store: a locked keychain would only ask again.
    eraseLogin(config, key);
    memory.session_only = true;
    memory.session_account = account;
    memory.session_reason = error.empty() ? juce::String("the credential store would not keep it") : juce::String(error);
  }

  // An older Atlas kept the token in its config: it moves into the store, or
  // into memory alone where the store will not keep it, and leaves the config
  // either way.
  void moveConfigToken(TokenMemory& memory) {
    json config = LoadSave::getConfigJson();
    if (!config.is_object() || (config.find(kTokenConfigKey) == config.end() &&
                                config.find(kAccountUsernameKey) == config.end() &&
                                config.find(kAccountNameKey) == config.end()))
      return;
    const juce::String token = jsonString(config, kTokenConfigKey).trim();
    Account account;
    account.username = jsonString(config, kAccountUsernameKey).trim();
    account.display_name = jsonString(config, kAccountNameKey).trim();
    config.erase(kTokenConfigKey);
    config.erase(kAccountUsernameKey);
    config.erase(kAccountNameKey);
    if (token.isNotEmpty())
      keepToken(memory, config, token, account);
    LoadSave::saveJsonToConfig(config);
  }
}

Account storedAccount() {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  moveConfigToken(memory);
  if (memory.session_only)
    return memory.session_account;
  json config = LoadSave::getConfigJson();
  const json* login = findLogin(config, loginKey(tokenStore(memory)));
  return login != nullptr ? loginAccount(*login) : Account {};
}

void storeAccount(const Account& account) {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  moveConfigToken(memory);
  if (memory.session_only) {
    memory.session_account = account.known() ? account : Account {};
    return;
  }
  json config = LoadSave::getConfigJson();
  json* login = findLogin(config, loginKey(tokenStore(memory)));
  if (login == nullptr)
    return;
  setLoginAccount(*login, account);
  LoadSave::saveJsonToConfig(config);
}

bool browserShown() {
  auto& state = browserShownState();
  int shown = state.load(std::memory_order_relaxed);
  if (shown < 0) {
    const json config = LoadSave::getConfigJson();
    const auto found = config.is_object() ? config.find(kBrowserShownKey) : config.end();
    shown = found != config.end() && found->is_boolean() && !found->get<bool>() ? 0 : 1;
    state.store(shown, std::memory_order_relaxed);
  }
  return shown != 0;
}

void setBrowserShown(bool shown) {
  browserShownState().store(shown ? 1 : 0, std::memory_order_relaxed);
  json config = LoadSave::getConfigJson();
  if (!config.is_object())
    config = json::object();
  config[kBrowserShownKey] = shown;
  LoadSave::saveJsonToConfig(config);
}

void forgetBrowserShownForTesting() {
  browserShownState().store(-1, std::memory_order_relaxed);
}

juce::String storedApiToken(juce::String* problem) {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  if (problem != nullptr)
    problem->clear();
  moveConfigToken(memory);
  if (memory.session_only)
    return memory.token;
  json config = LoadSave::getConfigJson();
  auto& store = tokenStore(memory);
  const std::string key = loginKey(store);
  const json* login = findLogin(config, key);
  if (login == nullptr) {
    // Logged out, perhaps by another process.
    forgetHeldToken(memory);
    return {};
  }
  const juce::int64 saved = jsonInteger(*login, kLoginSavedKey);
  if (memory.token.isNotEmpty() && memory.saved == saved)
    return memory.token;

  std::string secret;
  std::string error;
  const auto status = store.read(key, secret, error);
  forgetHeldToken(memory);
  if (status == credentials::Status::ok) {
    memory.token = juce::String::fromUTF8(secret.data(), static_cast<int>(secret.size())).trim();
    memory.saved = saved;
    return memory.token;
  }
  if (status == credentials::Status::not_found) {
    // Taken out of the store behind Atlas's back (in Keychain Access, say):
    // this DAW is logged out.
    eraseLogin(config, key);
    LoadSave::saveJsonToConfig(config);
    error = "it is no longer in " + store.place();
  }
  if (problem != nullptr)
    *problem = error.empty() ? juce::String("the credential store would not give it") : juce::String(error);
  return {};
}

void storeApiToken(const juce::String& token) {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  moveConfigToken(memory);
  json config = LoadSave::getConfigJson();
  if (!config.is_object())
    config = json::object();
  auto& store = tokenStore(memory);
  const std::string key = loginKey(store);
  const juce::String trimmed = token.trim();
  if (trimmed.isEmpty()) {
    // Logging out. The store is asked only when it holds this DAW's token.
    if (findLogin(config, key) != nullptr) {
      std::string error;
      store.remove(key, error);
      eraseLogin(config, key);
      LoadSave::saveJsonToConfig(config);
    }
    forgetHeldToken(memory);
    return;
  }
  // An account belongs to the token it was seen with.
  Account account;
  if (memory.token == trimmed) {
    if (memory.session_only)
      account = memory.session_account;
    else if (const json* login = findLogin(config, key))
      account = loginAccount(*login);
  }
  keepToken(memory, config, trimmed, account);
  LoadSave::saveJsonToConfig(config);
}

bool hasApiToken() {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  moveConfigToken(memory);
  if (memory.session_only)
    return memory.token.isNotEmpty();
  json config = LoadSave::getConfigJson();
  if (findLogin(config, loginKey(tokenStore(memory))) != nullptr)
    return true;
  forgetHeldToken(memory);
  return false;
}

juce::String tokenStorageText(bool logged_in) {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  auto& store = tokenStore(memory);
  const juce::String daw = dawName();
  const juce::String place = store.place();
  const juce::String start = logged_in ? "Your API token is kept in " : "Logging in keeps your API token in ";
  if ((logged_in && memory.session_only) || place.isEmpty()) {
    const juce::String reason = memory.session_only ? memory.session_reason : juce::String(store.unavailableReason());
    return start + "memory until " + daw + " closes" + (reason.isNotEmpty() ? ", because " + reason : juce::String()) +
           ".";
  }
  if (store.scope().empty())
    return start + place + ", for every DAW on this computer.";
  return start + place + ", for " + daw + " only. Each DAW logs in on its own.";
}

juce::String tokenPlace() {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  const juce::String place = tokenStore(memory).place();
  return memory.session_only || place.isEmpty() ? juce::String("memory") : place;
}

bool tokenInMemoryOnly() {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  return memory.session_only && memory.token.isNotEmpty();
}

void setCredentialStoreForTesting(std::shared_ptr<credentials::Store> store) {
  auto& memory = tokenMemory();
  const std::lock_guard<std::mutex> hold(memory.lock);
  memory.store = std::move(store);
  forgetHeldToken(memory);
}

juce::String tokenHint(const juce::String& token) {
  const juce::String key = token.trim();
  if (key.isEmpty())
    return "no token";
  if (key.length() < 12)
    return "a token";
  return "the token ending in " + key.getLastCharacters(4);
}

bool looksLikeApiToken(const juce::String& token) {
  const juce::String key = token.trim();
  return key.startsWith("bpa_") && key.length() > 12 && key.substring(4).containsOnly("0123456789abcdefABCDEF");
}
}

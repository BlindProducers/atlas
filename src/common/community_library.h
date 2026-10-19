// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// The community preset library at https://atlas.blindproducers.org, read
// through its JSON API (/api/v1/; reading needs no account). Nothing here
// knows the editor: it runs on a background thread, the editor turns each
// RemotePreset into a row of its preset browser, and every function works
// as well on a saved page or file (a file: URL) as on the live server, which
// is how the tests run without a network.
namespace atlas::community {

// The audio preview of a shared preset or bank: a recording the uploader
// attached, served by the library like the files. The library lists no
// checksum for it, only its size.
struct RemotePreview {
  juce::String url;
  juce::String file_name;           // "Glow Lead 01 preview.mp3"
  juce::String content_type;        // "audio/mpeg"
  juce::int64 size = 0;
  double duration_seconds = 0.0;    // 0 when the library does not know
  bool exists() const { return url.isNotEmpty(); }
};

// A published patch, as the API's /presets/ page describes it.
struct RemotePreset {
  juce::int64 id = 0;
  juce::String kind;                // "patch"; the API also lists fx, wavetable and lfo files
  juce::String name;
  juce::String style;               // the API's id, "bass"
  juce::String style_label;         // what the site shows, "Bass"
  juce::StringArray tags;
  juce::String description;
  juce::String author;              // the preset's author as the site shows it
  juce::String uploader;            // the account that shared it
  juce::String uploader_name;       // that account's display name
  juce::String download_url;
  juce::String file_name;           // "Glow Lead 01.atp"
  juce::String extension;           // "atp"
  juce::int64 file_size = 0;
  juce::String sha256;              // hex, what the download must hash to
  RemotePreview preview;            // none unless the uploader added one
  int downloads = 0;
  juce::String page_url;            // the preset's page on the site
  juce::int64 updated_ms = 0;       // when it was last changed, in Unix milliseconds
};

// A shared bank (.atb), as the API's /banks/ page describes it. The library
// also lists every preset inside a bank on its /presets/ page.
struct RemoteBank {
  juce::int64 id = 0;
  juce::String name;
  juce::String genre;               // "Speed Garage"; empty for none
  juce::String genre_slug;          // "speed-garage"
  juce::StringArray tags;
  juce::String description;
  juce::String uploader;
  juce::String uploader_name;
  juce::String download_url;
  juce::String file_name;           // "Hamo Speed Garage Bank.atb"
  juce::String extension;           // "atb"
  juce::int64 file_size = 0;
  juce::String sha256;
  RemotePreview preview;
  int presets = 0;                  // what the archive holds, by kind
  int wavetables = 0;
  int lfos = 0;
  int fx = 0;
  int samples = 0;
  int total_files = 0;
  juce::int64 unpacked_bytes = 0;
  int downloads = 0;
  juce::String page_url;
  juce::int64 updated_ms = 0;
};

// Presets per page; the API serves at most this many.
constexpr int kPageSize = 200;

// https://atlas.blindproducers.org, or the ATLAS_COMMUNITY_URL environment
// variable while developing against a local server, or the test URL below.
juce::String libraryUrl();
// The library's host name, for what the editor says: "atlas.blindproducers.org".
juce::String libraryHost();
// Page `page` of the published patches at `library_url`, kPageSize at a time.
// A file: URL is a saved page and is returned as it is.
juce::String presetsPageUrl(const juce::String& library_url, int page);
// Page `page` of the shared banks. For a saved library (a file: URL naming
// the presets page) it is the file banks.json beside that page.
juce::String banksPageUrl(const juce::String& library_url, int page);
// Whether `url` belongs to the library (its host, or any file when the library
// is a saved page): pages and downloads are followed nowhere else.
bool isLibraryUrl(const juce::String& url, const juce::String& library_url);
// Makes libraryUrl() answer `url`; empty puts the real library back.
void setLibraryUrlForTesting(const juce::String& url);

// Reads one page of /api/v1/presets/, adding its patches to `presets` and
// naming the next page in `next_url` (empty on the last). Returns what went
// wrong, or an empty string.
juce::String parsePresetsPage(const std::string& text, std::vector<RemotePreset>& presets, juce::String& next_url);
// The same for a page of /api/v1/banks/.
juce::String parseBanksPage(const std::string& text, std::vector<RemoteBank>& banks, juce::String& next_url);
// What a bank holds, as it is said: "62 presets", "24 presets, 3 wavetables
// and 2 samples", or "no Atlas files".
juce::String bankContentsText(const RemoteBank& bank);

// Where downloads live: <data>/Community/Presets, which the preset browser
// indexes like the Factory and User folders.
juce::File communityDirectory(const juce::File& data_directory);
// Where `preset` lands: <data>/Community/Presets/<uploader>/<style>/<file name>,
// so the browser shows the uploader as its bank and the style as its category.
juce::File localFile(const RemotePreset& preset, const juce::File& data_directory);
// Where downloaded bank archives are kept: <data>/Community/Banks/<uploader>/
// <file name>. The archive itself unpacks into the Atlas folder, as Import
// preset bank does, and stays here as the record of the import.
juce::File banksDirectory(const juce::File& data_directory);
juce::File localFile(const RemoteBank& bank, const juce::File& data_directory);
// Where fetched previews are kept: <data>/Community/Previews. A preset's lands
// at Presets/<uploader>/<preview file name> below it, a bank's at Banks/<uploader>/.
juce::File previewDirectory(const juce::File& data_directory);
juce::File localPreviewFile(const RemotePreset& preset, const juce::File& data_directory);
juce::File localPreviewFile(const RemoteBank& bank, const juce::File& data_directory);
// A length as it is said: "12 seconds", "1 minute 5 seconds"; empty for none.
juce::String durationText(double seconds);

// Lower-case hex SHA-256.
juce::String sha256Hex(const void* data, size_t size);
// Hashes `stream` to its end; returns an empty string when `keep_going` says to stop.
juce::String sha256Hex(juce::InputStream& stream, const std::function<bool()>& keep_going = {});

// Networking, for a background thread. Each returns what went wrong, or an
// empty string; setting `cancelled` makes them stop early with an error.
juce::String fetchText(const juce::URL& url, std::string& text, const std::atomic<bool>* cancelled = nullptr);
// Every published patch, page after page.
juce::String fetchAllPresets(const juce::String& library_url, std::vector<RemotePreset>& presets,
                             const std::atomic<bool>* cancelled = nullptr);
// Every shared bank, page after page.
juce::String fetchAllBanks(const juce::String& library_url, std::vector<RemoteBank>& banks,
                           const std::atomic<bool>* cancelled = nullptr);
// Downloads `url` to `target`, through a .part file beside it, and keeps it
// only when it is `expected_size` bytes (when known) hashing to `expected_sha256`.
juce::String downloadFile(const juce::URL& url, const juce::File& target, const juce::String& expected_sha256,
                          juce::int64 expected_size, const std::atomic<bool>* cancelled = nullptr);
}

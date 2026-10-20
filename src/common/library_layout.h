// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "JuceHeader.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vital::multisample {
  struct FileInfo;
}

// The Atlas folder (LoadSave::getDataDirectory) holds four libraries, and
// each library holds its banks and one folder per kind of file:
//
//   <Atlas folder>/<Library>/Banks/<Bank>/<category folders>/<preset>
//   <Atlas folder>/<Library>/Wavetables/<pack>/<folders>/<file>
//   ... and the same for Multisamples, Samples, LFO Presets, FX Presets and
//   Impulse Responses.
//
// The libraries are Factory (content Atlas ships), User (what the user saves
// and keeps), Third Party (banks and packs imported from files or dropped in
// by hand) and Community (downloads from atlas.blindproducers.org). A bank is
// a set of presets, and the folders inside it are their categories. Every
// other file lives in its library's kind folder: a pack is a folder there,
// and the wavetables, samples, LFO and FX presets a bank ships with are a pack
// named after the bank. A multisample instrument lives in its library's
// Multisamples folder once, however many presets play it. Exporting a bank
// gathers its presets, its packs and the instruments its presets play.
// docs/library-layout-plan.md has the whole design; this is the model, the
// migration from the older layouts, the bank archives and the instruments.
namespace atlas::library {

enum class Library { kFactory, kUser, kThirdParty, kCommunity };
// In the order the browser lists them.
const std::vector<Library>& libraries();
// "Factory", "User", "Third Party", "Community": the folder and what is said.
juce::String libraryName(Library library);
bool libraryFromName(const juce::String& name, Library& library);

enum class Kind { kPresets, kWavetables, kMultisamples, kSamples, kLfos, kFx, kImpulseResponses };
// In the order a library lists its folders.
const std::vector<Kind>& kinds();
// The kind's folder in a library: "Banks", "Wavetables", "Multisamples",
// "Samples", "LFO Presets", "FX Presets", "Impulse Responses".
juce::String kindFolderName(Kind kind);
// The kind's folder inside a bank archive, and inside a bank in the layouts
// before 2.0.0 beta 3: "Presets", "Wavetables", "Multisamples", "Samples",
// "LFOs", "FX", "Impulse Responses". The community library reads archives
// laid out this way.
juce::String archiveFolderName(Kind kind);
// The kind a folder of either name holds.
bool kindFromFolderName(const juce::String& name, Kind& kind);

// Where a path sits in the Atlas folder.
struct Location {
  bool in_library = false;         // under one of the four libraries
  Library library = Library::kUser;
  bool in_kind = false;            // under one of the library's kind folders (Banks, for presets)
  Kind kind = Kind::kPresets;
  juce::String bank;               // presets: the bank; empty for anything else
  juce::String pack;               // other kinds: the folder directly in the kind folder; empty for a file there
  juce::StringArray folders;       // the folders between the bank or pack and the file
  // The folders joined with " / ", which is a preset's category; empty for none.
  juce::String category() const;
};
Location locate(const juce::File& path, const juce::File& data_directory);

juce::File libraryDirectory(Library library, const juce::File& data_directory);
// <Library>/<kind folder>; for presets, <Library>/Banks.
juce::File kindDirectory(Library library, Kind kind, const juce::File& data_directory);
juce::File bankDirectory(Library library, const juce::String& bank, const juce::File& data_directory);
juce::File packDirectory(Library library, Kind kind, const juce::String& pack, const juce::File& data_directory);
// The bank folders of a library in natural order, and every library's, library by library.
std::vector<juce::File> bankFolders(Library library, const juce::File& data_directory);
std::vector<juce::File> bankFolders(const juce::File& data_directory);
juce::StringArray bankNames(Library library, const juce::File& data_directory);
// Every library's folder of `kind` that exists (its Banks folder for
// presets), library by library; or one library's.
std::vector<juce::File> kindFolders(Kind kind, const juce::File& data_directory);
std::vector<juce::File> kindFolders(Kind kind, Library library, const juce::File& data_directory);
// `name` as a folder or file name: no separators or other illegal characters,
// no leading dots; `fallback` when nothing is left.
juce::String legalName(const juce::String& name, const juce::String& fallback);

// The default bank, in User: the bank last saved into, kept in the config, and
// "My Bank" while there is none. Scan Downloads and a first Save patch use it.
juce::String defaultBankName();
void setDefaultBankName(const juce::String& name);
juce::File defaultBankDirectory(const juce::File& data_directory);
// Where the user's own files of `kind` are saved, created if need be: the
// User library's kind folder, and for presets the default bank.
juce::File userKindDirectory(Kind kind, const juce::File& data_directory);

// Community's own folders beside its kind folders: the bank archives it
// downloaded (Archives) and the audio previews it fetched (Previews).
juce::File communityArchiveDirectory(const juce::File& data_directory);
juce::File communityPreviewDirectory(const juce::File& data_directory);

// One rename done while reorganising.
struct Move {
  juce::File from;
  juce::File to;
};
// Maps `path` through `moves`: a path at or below a moved folder follows it.
juce::String movedPath(const juce::String& path, const std::vector<Move>& moves);
// Rewrites the favourites that `moves` carried elsewhere; returns how many.
int rewriteFavourites(const std::vector<Move>& moves);

// A folder dropped at the top of the Atlas folder by hand in the shape of a
// bank archive (Presets, Wavetables, LFOs and so on inside; the Vital habit)
// is filed into Third Party: its presets become a bank and the rest packs
// named after it. Favourites are rewritten and the moves logged, so the four
// libraries are the only things at the top level.
std::vector<Move> adoptStrayBanks(const juce::File& data_directory);

// The migration from the layouts before 2.0.0 beta 3: the first, where each
// library held kind folders (User/Presets/<bank>) and imported banks sat at
// the top of the Atlas folder, and the second, where each library held bank
// folders of kinds (User/<bank>/Presets). Needed while the config's
// library_layout is below kLayoutVersion.
constexpr int kLayoutVersion = 3;
bool needsMigration();
struct MigrationReport {
  bool ran = false;                // false when there was nothing to migrate
  int moves = 0;
  int favourites = 0;              // favourites rewritten
  juce::String default_bank;       // the User bank the loose presets went to
  juce::StringArray lines;         // what was done, as Migration.txt records it
  juce::StringArray problems;      // what could not be done
};
// Migrates `data_directory` and records library_layout in the config. Writes
// Migration.txt in the Atlas folder.
MigrationReport migrate(const juce::File& data_directory);
// Migrates the Atlas folder once per process when needsMigration(); the
// report of that run, or an empty one, each time after.
MigrationReport migrateIfNeeded();
// The log the migration and adoptStrayBanks append to: <Atlas folder>/Migration.txt.
juce::File migrationLogFile(const juce::File& data_directory);

// ---- Bank archives (.atb, and Vital's .vitalbank) ----

// The bank folder the archive unpacks to: the one top folder every entry sits
// in. Empty when the entries sit at the root or under several folders, or the
// top folder is a kind (an archive of Presets/... has no bank folder).
juce::String archiveBankName(juce::ZipFile& zip);
struct ImportResult {
  juce::String name;               // the bank's name
  juce::File bank;                 // the bank folder its presets went to; none for an archive without presets
  juce::StringArray packs;         // the kind folders that got a pack of its name ("Wavetables")
  int files = 0;                   // entries unpacked
  int replaced = 0;                // files that were there already
  juce::String error;              // empty once imported
  bool ok() const { return error.isEmpty(); }
};
// Files `archive` into `library`: its presets become the bank, its other
// files packs named after it, its instruments the library's (one already
// there by hash is not added again). The bank is named after the archive's
// top folder, or `fallback_name`. Files already there are replaced and the
// rest stay. Entries that would land outside are refused, and macOS resource
// forks are skipped.
ImportResult importBankArchive(const juce::File& archive, Library library, const juce::File& data_directory,
                               const juce::String& fallback_name);

struct ArchiveResult {
  int added = 0;                   // files in the archive
  juce::String error;              // why there is none
  juce::String spoken;             // what is said for it
  bool ok() const { return error.isEmpty(); }
};
// Writes `source` into a bank archive at `destination`, named after the
// archive's file name, laid out <name>/Presets/..., Wavetables/... and so on
// (archiveFolderName). A bank brings its presets, the packs of its name in
// its library and the instruments its presets play; a pack brings its files;
// any other folder has its Atlas files sorted into kinds by extension.
// Audio, SF2 and container presets are stored uncompressed. Runs on any
// thread; the archive is written beside its target and replaces it only when
// complete.
ArchiveResult writeBankArchive(const juce::File& source, const juce::File& destination);
// The most a bank archive may hold: JUCE's zip writer has no ZIP64.
constexpr juce::int64 kMaxArchiveBytes = 0xF0000000ll;

// ---- Multisample instruments ----

// An instrument Atlas puts in a library is a folder in its Multisamples
// holding the instrument's files in their own layout (the layout the cache
// uses) beside this manifest, which names the entry file, the content hash,
// every file with its size and CRC, and the SFZ paths outside the
// instrument that were mapped under "_external/". An instrument the user put
// there needs no manifest.
constexpr const char* kInstrumentManifestName = "atlas-multisample.json";
struct InstrumentManifest {
  juce::String hash;
  juce::String entry;              // relative to the folder
  juce::String format;             // "sfz", "dspreset" or "sf2"
  juce::String name;
  // Each file at its path relative to the folder, as the loader records it.
  struct File {
    std::string relative;
    juce::int64 size = 0;
    juce::uint32 crc = 0;
    bool text = false;
  };
  std::vector<File> files;
  std::map<std::string, std::string> path_overrides;
  bool valid = false;
};
// <Library>/Multisamples for a library's folder.
juce::File instrumentsDirectory(const juce::File& library_directory);
InstrumentManifest readInstrumentManifest(const juce::File& folder);
bool writeInstrumentManifest(const juce::File& folder, const InstrumentManifest& manifest);
// The folder directly in a library's Multisamples that `file` sits in; none
// for a file directly in Multisamples or outside it.
juce::File instrumentFolderOf(const juce::File& file, const juce::File& data_directory);
// The entry file of the instrument with `hash` that Atlas placed in a
// library, or none; and the same across every library.
juce::File findInstrumentByHash(const std::string& hash, const juce::File& library_directory);
juce::File findInstrumentAnywhere(const std::string& hash, const juce::File& data_directory);
// How a preset names an instrument of its library: the entry file's path
// relative to the library's Multisamples, "Piano/Programs/main.sfz".
juce::String libraryInstrumentPath(const juce::File& entry, const juce::File& library_directory);
// Makes sure the library holds an instrument and returns its entry file there:
// the instrument itself when its files are in the library's Multisamples
// already, the copy Atlas placed before (same hash), or a new folder
// Multisamples/<name> (a numbered sibling when the name is taken) filled by
// hard link where the volume allows and by copy otherwise, with the manifest
// written last. None, with `error`, when it cannot.
juce::File placeInstrument(const juce::File& library_directory, const juce::String& name, const std::string& hash,
                           const std::string& entry, const std::string& format,
                           const std::vector<vital::multisample::FileInfo>& files,
                           const std::map<std::string, std::string>& path_overrides,
                           const std::function<bool()>& cancelled, juce::String& error);
// A preset carrying its instruments inside (a zip container) that sits in a
// bank: the instruments go into the library's Multisamples once and the
// preset is rewritten as plain JSON referring to them. True when it was
// converted; false with an empty `error` when the preset is no container.
bool adoptContainerPreset(const juce::File& preset, const juce::File& data_directory, juce::String& error);

} // namespace atlas::library

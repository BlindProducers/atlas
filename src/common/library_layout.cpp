// SPDX-License-Identifier: GPL-3.0-or-later
#include "library_layout.h"
#include "community_library.h"
#include "load_save.h"
#include "multisample_container.h"
#include "multisample_loader.h"
#include "multisample_manager.h"
#include "synth_constants.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace atlas::library {

namespace {
  const char* const kLibraryNames[] = { "Factory", "User", "Third Party", "Community" };
  // By Kind: the folder in a library, and the folder in a bank archive.
  const char* const kKindFolderNames[] = { "Banks", "Wavetables", "Multisamples", "Samples", "LFO Presets",
                                           "FX Presets", "Impulse Responses" };
  const char* const kArchiveFolderNames[] = { "Presets", "Wavetables", "Multisamples", "Samples", "LFOs", "FX",
                                              "Impulse Responses" };
  constexpr const char* kCommunityArchives = "Archives";
  constexpr const char* kCommunityPreviews = "Previews";
  constexpr const char* kDefaultBankFallback = "My Bank";
  constexpr const char* kLayoutConfigKey = "library_layout";
  constexpr const char* kDefaultBankConfigKey = "default_bank";
  constexpr const char* kMigrationLogName = "Migration.txt";
  // The Factory content in the Atlas repository; Vital's content, which users
  // copied in by hand, is the Vital bank.
  constexpr const char* kAtlasFactoryBank = "Atlas";
  constexpr const char* kAtlasFactoryPresets = "Oscillator Slots";
  constexpr const char* kVitalFactoryBank = "Vital";

  juce::StringArray pathParts(const juce::File& path, const juce::File& base) {
    juce::StringArray parts;
    parts.addTokens(path.getRelativePathFrom(base), juce::File::getSeparatorString(), "");
    parts.removeEmptyStrings();
    return parts;
  }

  bool naturalLess(const juce::File& a, const juce::File& b) {
    return a.getFileName().compareNatural(b.getFileName()) < 0;
  }

  bool naturalPathLess(const juce::File& a, const juce::File& b) {
    return a.getFullPathName().compareNatural(b.getFullPathName()) < 0;
  }

  // Names the library never treats as content: dot files (.DS_Store, a
  // staging folder) and macOS resource forks.
  bool isHiddenName(const juce::String& name) {
    return name.startsWithChar('.') || name == "__MACOSX";
  }

  // The children of a folder that are content, in natural order.
  juce::Array<juce::File> contentOf(const juce::File& folder, int what = juce::File::findFilesAndDirectories) {
    juce::Array<juce::File> children;
    if (folder.isDirectory())
      folder.findChildFiles(children, what | juce::File::ignoreHiddenFiles, false);
    for (int i = children.size() - 1; i >= 0; --i) {
      if (isHiddenName(children[i].getFileName()))
        children.remove(i);
    }
    std::sort(children.begin(), children.end(), naturalLess);
    return children;
  }

  bool isPresetFile(const juce::File& file) {
    return file.hasFileExtension(vital::kPresetExtension) || file.hasFileExtension(vital::kLegacyPresetExtension);
  }

  // Everything under `from` goes to `to`, merging folders that are both
  // there. Two files of one name: `replace` puts the new one in place of the
  // old (and counts it), otherwise both are kept. Each rename is recorded.
  bool mergeMove(const juce::File& from, const juce::File& to, std::vector<Move>& moves, juce::StringArray& problems,
                 bool replace = false, int* replaced = nullptr) {
    if (from == to)
      return true;
    if (!to.exists()) {
      if (!to.getParentDirectory().createDirectory()) {
        problems.add("Could not create " + to.getParentDirectory().getFullPathName());
        return false;
      }
      if (from.moveFileTo(to)) {
        moves.push_back({ from, to });
        return true;
      }
      problems.add("Could not move " + from.getFullPathName() + " to " + to.getFullPathName());
      return false;
    }
    if (from.isDirectory() && to.isDirectory()) {
      bool ok = true;
      for (const auto& child : contentOf(from))
        ok = mergeMove(child, to.getChildFile(child.getFileName()), moves, problems, replace, replaced) && ok;
      // What is left is hidden (.DS_Store); the folder goes with it.
      juce::Array<juce::File> left;
      from.findChildFiles(left, juce::File::findFilesAndDirectories, false);
      bool only_hidden_files = true;
      for (const auto& file : left)
        only_hidden_files = only_hidden_files && !file.isDirectory() &&
                            (file.isHidden() || isHiddenName(file.getFileName()));
      if (ok && only_hidden_files)
        from.deleteRecursively();
      return ok;
    }
    if (replace && !from.isDirectory() && !to.isDirectory()) {
      if (to.deleteFile() && from.moveFileTo(to)) {
        moves.push_back({ from, to });
        if (replaced != nullptr)
          ++*replaced;
        return true;
      }
      problems.add("Could not replace " + to.getFullPathName());
      return false;
    }
    const juce::File alternative = to.getNonexistentSibling(false);
    if (from.moveFileTo(alternative)) {
      moves.push_back({ from, alternative });
      problems.add(to.getFullPathName() + " was there already, so " + from.getFileName() + " became " +
                   alternative.getFileName());
      return true;
    }
    problems.add("Could not move " + from.getFullPathName() + " to " + alternative.getFullPathName());
    return false;
  }

  // Deletes a folder that holds nothing but hidden files.
  void removeIfEmpty(const juce::File& folder) {
    if (!folder.isDirectory())
      return;
    juce::Array<juce::File> left;
    folder.findChildFiles(left, juce::File::findFilesAndDirectories, false);
    for (const auto& file : left) {
      if (file.isDirectory() || !(file.isHidden() || isHiddenName(file.getFileName())))
        return;
    }
    folder.deleteRecursively();
  }

  int layoutVersion() {
    const json config = LoadSave::getConfigJson();
    if (config.is_object() && config.count(kLayoutConfigKey) && config[kLayoutConfigKey].is_number_integer())
      return config[kLayoutConfigKey].get<int>();
    return 0;
  }

  void setLayoutVersion(int version) {
    json config = LoadSave::getConfigJson();
    if (!config.is_object())
      config = json::object();
    config[kLayoutConfigKey] = version;
    LoadSave::saveJsonToConfig(config);
  }

  // Whether the community library's downloaded archives hold one called `name`.
  bool cameFromTheCommunity(const juce::File& data_directory, const juce::String& name) {
    const juce::File community = libraryDirectory(Library::kCommunity, data_directory);
    for (const char* folder_name : { kCommunityArchives, "Banks" }) {
      const juce::File archives = community.getChildFile(folder_name);
      if (!archives.isDirectory())
        continue;
      juce::Array<juce::File> found;
      archives.findChildFiles(found, juce::File::findFiles | juce::File::ignoreHiddenFiles, true, vital::kBankExtensionsList);
      for (const auto& archive : found) {
        if (archive.getFileNameWithoutExtension().equalsIgnoreCase(name))
          return true;
      }
    }
    return false;
  }

  void appendLog(const juce::File& data_directory, const juce::String& title, const juce::StringArray& lines,
                 const juce::StringArray& problems) {
    if (lines.isEmpty() && problems.isEmpty())
      return;
    juce::String text = title + ", " + juce::Time::getCurrentTime().toString(true, true, false, true) + "\n";
    for (const auto& line : lines)
      text += "  " + line + "\n";
    for (const auto& problem : problems)
      text += "  Problem: " + problem + "\n";
    migrationLogFile(data_directory).appendText(text + "\n");
  }

  // The author the loose presets in `folder` agree on, as a bank name; empty
  // when they name none.
  juce::String commonAuthor(const juce::File& folder) {
    juce::Array<juce::File> presets;
    folder.findChildFiles(presets, juce::File::findFiles | juce::File::ignoreHiddenFiles, false,
                          vital::kPresetExtensionsList);
    std::map<juce::String, int> counts;
    for (const auto& preset : presets) {
      const juce::String author = legalName(LoadSave::getAuthorFromFile(preset), {});
      if (author.isNotEmpty())
        ++counts[author];
    }
    juce::String best;
    int best_count = 0;
    for (const auto& [author, count] : counts) {
      if (count > best_count) {
        best = author;
        best_count = count;
      }
    }
    return best;
  }

  bool isTextFile(const juce::File& file) {
    static const char* const kTextExtensions[] = { "atp", "vital", "att", "vitaltable", "ats", "vitallfo", "atf",
                                                   "atfx", "sfz", "sfzh", "dspreset", "txt", "json", "xml", "md" };
    for (const char* extension : kTextExtensions) {
      if (file.hasFileExtension(extension))
        return true;
    }
    return false;
  }

  int compressionFor(const juce::File& file) {
    // Presets carrying multi-sample instruments are zips of audio already.
    if (!isTextFile(file) || vital::multisample::container::isContainer(file))
      return 0;
    return 9;
  }

  // An archive entry's path, with directory entries, resource forks and
  // hidden files marked as nothing to unpack.
  juce::StringArray entryParts(const juce::ZipFile::ZipEntry& entry, bool& skip) {
    juce::StringArray parts;
    parts.addTokens(entry.filename.replaceCharacter('\\', '/'), "/", "");
    parts.removeEmptyStrings();
    skip = parts.isEmpty() || entry.filename.endsWithChar('/');
    for (const auto& part : parts) {
      if (isHiddenName(part))
        skip = true;
    }
    return parts;
  }

  // Whether `folder` is in the shape of a bank archive: a kind folder of
  // either naming inside it.
  bool hasKindFolders(const juce::File& folder) {
    for (const auto& child : contentOf(folder, juce::File::findDirectories)) {
      Kind kind;
      if (kindFromFolderName(child.getFileName(), kind))
        return true;
    }
    return false;
  }

  // The folders a library holds in the current layout.
  bool isLayoutFolder(Library library, const juce::String& name) {
    for (Kind kind : kinds()) {
      if (name == kindFolderName(kind))
        return true;
    }
    return library == Library::kCommunity && (name == kCommunityArchives || name == kCommunityPreviews);
  }

  bool isInstrumentText(const std::string& relative) {
    const juce::String name(relative);
    return name.endsWithIgnoreCase(".sfz") || name.endsWithIgnoreCase(".sfzh") || name.endsWithIgnoreCase(".dspreset") ||
           name.endsWithIgnoreCase(".txt") || name.endsWithIgnoreCase(".json");
  }

  // A free folder in `instruments` for an instrument called `name`: the name,
  // or a numbered sibling when it is taken.
  juce::File freeInstrumentFolder(const juce::File& instruments, const juce::String& name) {
    const juce::String legal = legalName(name, "Instrument");
    juce::File folder = instruments.getChildFile(legal);
    for (int n = 2; folder.exists() || folder.getSiblingFile("." + folder.getFileName() + ".placing").exists(); ++n)
      folder = instruments.getChildFile(legal + " " + juce::String(n));
    return folder;
  }

  // The hidden folder an instrument is filled in before it takes its name.
  juce::File placingFolder(const juce::File& folder) {
    return folder.getSiblingFile("." + folder.getFileName() + ".placing");
  }

  // Writes the manifest into the folder being filled and moves it into place.
  bool completeInstrumentFolder(const juce::File& partial, const juce::File& folder, const InstrumentManifest& manifest,
                                juce::String& error) {
    if (!writeInstrumentManifest(partial, manifest)) {
      error = "Could not write " + partial.getChildFile(kInstrumentManifestName).getFullPathName();
      partial.deleteRecursively();
      return false;
    }
    if (!partial.moveFileTo(folder)) {
      error = "Could not move the instrument into " + folder.getFullPathName();
      partial.deleteRecursively();
      return false;
    }
    return true;
  }

  InstrumentManifest manifestFor(const juce::String& name, const std::string& hash, const std::string& entry,
                                 const std::string& format, const std::vector<vital::multisample::FileInfo>& files,
                                 const std::map<std::string, std::string>& path_overrides) {
    InstrumentManifest manifest;
    manifest.hash = juce::String(hash);
    manifest.entry = juce::String(entry);
    manifest.format = juce::String(format);
    manifest.name = name;
    for (const auto& info : files)
      manifest.files.push_back({ info.relative, info.size, info.crc, info.text });
    manifest.path_overrides = path_overrides;
    manifest.valid = true;
    return manifest;
  }

  // What dissolving a bank-shaped folder came to.
  struct Dissolved {
    bool presets = false;
    juce::StringArray packs;
    int replaced = 0;
  };

  // Files `root`, a folder in the shape of a bank archive, into `library`:
  // Presets becomes the bank `name`, every other kind folder a pack of that
  // name (or its contents go straight into the kind folder when `flat`), and
  // each instrument in Multisamples the library's. `importing` replaces files
  // already there and leaves out an instrument the library has by hash;
  // otherwise both are kept, numbered. Loose presets join the bank; anything
  // else stays where it is.
  void dissolve(const juce::File& root, const juce::String& name, Library library, const juce::File& data_directory,
                bool flat, bool importing, std::vector<Move>& moves, juce::StringArray& lines,
                juce::StringArray& problems, Dissolved& result) {
    auto move = [&](const juce::File& from, const juce::File& to) {
      const size_t before = moves.size();
      if (mergeMove(from, to, moves, problems, importing, &result.replaced) && moves.size() > before && !importing)
        lines.add("Moved " + from.getRelativePathFrom(data_directory) + " to " + to.getRelativePathFrom(data_directory));
    };
    const juce::File bank = bankDirectory(library, name, data_directory);
    for (const auto& child : contentOf(root)) {
      Kind kind;
      if (child.isDirectory() && kindFromFolderName(child.getFileName(), kind)) {
        if (kind == Kind::kPresets) {
          move(child, bank);
          result.presets = true;
        }
        else if (kind == Kind::kMultisamples) {
          const juce::File instruments = kindDirectory(library, Kind::kMultisamples, data_directory);
          for (const auto& instrument : contentOf(child)) {
            if (importing && instrument.isDirectory()) {
              const auto incoming = readInstrumentManifest(instrument);
              if (incoming.valid && findInstrumentByHash(incoming.hash.toStdString(),
                                                         libraryDirectory(library, data_directory)) != juce::File())
                continue;   // the library has it; the copy goes with the staging folder
            }
            juce::File target = instruments.getChildFile(instrument.getFileName());
            if (target.exists()) {
              // Two instruments are never merged into one folder.
              target = instrument.isDirectory() ? freeInstrumentFolder(instruments, instrument.getFileName())
                                                : target.getNonexistentSibling(false);
            }
            move(instrument, target);
          }
          removeIfEmpty(child);
          result.packs.addIfNotAlreadyThere(kindFolderName(kind));
        }
        else {
          move(child, flat ? kindDirectory(library, kind, data_directory) : packDirectory(library, kind, name, data_directory));
          result.packs.addIfNotAlreadyThere(kindFolderName(kind));
        }
      }
      else if (!child.isDirectory() && isPresetFile(child)) {
        move(child, bank.getChildFile(child.getFileName()));
        result.presets = true;
      }
      else if (!importing) {
        lines.add("Left " + child.getRelativePathFrom(data_directory) + " where it is");
      }
    }
    removeIfEmpty(root);
  }

  // Files every bank-shaped folder at the top of the Atlas folder into Third
  // Party, or Community when its archive was downloaded from there.
  void moveStrayBanks(const juce::File& data_directory, std::vector<Move>& moves, juce::StringArray& lines,
                      juce::StringArray& problems) {
    for (const auto& folder : contentOf(data_directory, juce::File::findDirectories)) {
      Library library;
      if (libraryFromName(folder.getFileName(), library) || !hasKindFolders(folder))
        continue;
      const Library target = cameFromTheCommunity(data_directory, folder.getFileName()) ? Library::kCommunity
                                                                                        : Library::kThirdParty;
      lines.add("Filed " + folder.getFileName() + " into " + libraryName(target));
      Dissolved dissolved;
      dissolve(folder, folder.getFileName(), target, data_directory, false, false, moves, lines, problems, dissolved);
    }
  }
}

const std::vector<Library>& libraries() {
  static const std::vector<Library> all { Library::kFactory, Library::kUser, Library::kThirdParty, Library::kCommunity };
  return all;
}

juce::String libraryName(Library library) {
  return kLibraryNames[static_cast<int>(library)];
}

bool libraryFromName(const juce::String& name, Library& library) {
  for (Library candidate : libraries()) {
    if (name == libraryName(candidate)) {
      library = candidate;
      return true;
    }
  }
  return false;
}

const std::vector<Kind>& kinds() {
  static const std::vector<Kind> all { Kind::kPresets, Kind::kWavetables, Kind::kMultisamples, Kind::kSamples,
                                       Kind::kLfos, Kind::kFx, Kind::kImpulseResponses };
  return all;
}

juce::String kindFolderName(Kind kind) {
  return kKindFolderNames[static_cast<int>(kind)];
}

juce::String archiveFolderName(Kind kind) {
  return kArchiveFolderNames[static_cast<int>(kind)];
}

bool kindFromFolderName(const juce::String& name, Kind& kind) {
  for (Kind candidate : kinds()) {
    if (name == kindFolderName(candidate) || name == archiveFolderName(candidate)) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

juce::String Location::category() const {
  return folders.joinIntoString(" / ");
}

Location locate(const juce::File& path, const juce::File& data_directory) {
  Location location;
  if (data_directory == juce::File() || !path.isAChildOf(data_directory))
    return location;
  const juce::StringArray parts = pathParts(path, data_directory);
  if (parts.isEmpty() || !libraryFromName(parts[0], location.library))
    return location;
  location.in_library = true;
  // A path that is no folder (a file, or a download not made yet) ends in its name.
  const int folder_parts = path.isDirectory() ? parts.size() : parts.size() - 1;
  if (folder_parts < 2)
    return location;
  bool found = false;
  for (Kind kind : kinds()) {
    if (parts[1] == kindFolderName(kind)) {
      location.kind = kind;
      found = true;
    }
  }
  if (!found)
    return location;
  location.in_kind = true;
  if (folder_parts < 3)
    return location;
  (location.kind == Kind::kPresets ? location.bank : location.pack) = parts[2];
  for (int i = 3; i < folder_parts; ++i)
    location.folders.add(parts[i]);
  return location;
}

juce::File libraryDirectory(Library library, const juce::File& data_directory) {
  return data_directory.getChildFile(libraryName(library));
}

juce::File defaultPatchFile(const juce::File& data_directory) {
  return libraryDirectory(Library::kUser, data_directory).getChildFile("Default")
      .withFileExtension(vital::kPresetExtension);
}

juce::File kindDirectory(Library library, Kind kind, const juce::File& data_directory) {
  return libraryDirectory(library, data_directory).getChildFile(kindFolderName(kind));
}

juce::File bankDirectory(Library library, const juce::String& bank, const juce::File& data_directory) {
  return kindDirectory(library, Kind::kPresets, data_directory).getChildFile(legalName(bank, kDefaultBankFallback));
}

juce::File packDirectory(Library library, Kind kind, const juce::String& pack, const juce::File& data_directory) {
  return kindDirectory(library, kind, data_directory).getChildFile(legalName(pack, "Pack"));
}

std::vector<juce::File> bankFolders(Library library, const juce::File& data_directory) {
  std::vector<juce::File> banks;
  for (const auto& folder : contentOf(kindDirectory(library, Kind::kPresets, data_directory), juce::File::findDirectories))
    banks.push_back(folder);
  return banks;
}

std::vector<juce::File> bankFolders(const juce::File& data_directory) {
  std::vector<juce::File> banks;
  for (Library library : libraries()) {
    const auto found = bankFolders(library, data_directory);
    banks.insert(banks.end(), found.begin(), found.end());
  }
  return banks;
}

juce::StringArray bankNames(Library library, const juce::File& data_directory) {
  juce::StringArray names;
  for (const auto& bank : bankFolders(library, data_directory))
    names.add(bank.getFileName());
  return names;
}

std::vector<juce::File> kindFolders(Kind kind, Library library, const juce::File& data_directory) {
  const juce::File folder = kindDirectory(library, kind, data_directory);
  if (folder.isDirectory())
    return { folder };
  return {};
}

std::vector<juce::File> kindFolders(Kind kind, const juce::File& data_directory) {
  std::vector<juce::File> folders;
  for (Library library : libraries()) {
    const auto found = kindFolders(kind, library, data_directory);
    folders.insert(folders.end(), found.begin(), found.end());
  }
  return folders;
}

juce::String legalName(const juce::String& name, const juce::String& fallback) {
  juce::String legal = juce::File::createLegalFileName(name.trim()).removeCharacters("\\/:*?\"<>|");
  while (legal.contains(".."))
    legal = legal.replace("..", ".");
  legal = legal.trimCharactersAtStart(". ").trimCharactersAtEnd(". ");
  // As Windows takes a name: control characters become spaces and a device
  // name (CON, AUX, NUL, COM1 and the like) gets an underscore, so a bank
  // typed as "Aux" is the folder Aux_. Nothing changes elsewhere.
  legal = atlas::community::systemFileName(legal).trimCharactersAtStart(". ").trimCharactersAtEnd(". ");
  return legal.isNotEmpty() ? legal : fallback;
}

juce::String defaultBankName() {
  const json config = LoadSave::getConfigJson();
  if (config.is_object() && config.count(kDefaultBankConfigKey) && config[kDefaultBankConfigKey].is_string())
    return legalName(juce::String(config[kDefaultBankConfigKey].get<std::string>()), kDefaultBankFallback);
  return kDefaultBankFallback;
}

void setDefaultBankName(const juce::String& name) {
  json config = LoadSave::getConfigJson();
  if (!config.is_object())
    config = json::object();
  config[kDefaultBankConfigKey] = legalName(name, kDefaultBankFallback).toStdString();
  LoadSave::saveJsonToConfig(config);
}

juce::File defaultBankDirectory(const juce::File& data_directory) {
  return bankDirectory(Library::kUser, defaultBankName(), data_directory);
}

juce::File userKindDirectory(Kind kind, const juce::File& data_directory) {
  const juce::File directory = kind == Kind::kPresets ? defaultBankDirectory(data_directory)
                                                      : kindDirectory(Library::kUser, kind, data_directory);
  if (!directory.exists())
    directory.createDirectory();
  return directory;
}

juce::File communityArchiveDirectory(const juce::File& data_directory) {
  return libraryDirectory(Library::kCommunity, data_directory).getChildFile(kCommunityArchives);
}

juce::File communityPreviewDirectory(const juce::File& data_directory) {
  return libraryDirectory(Library::kCommunity, data_directory).getChildFile(kCommunityPreviews);
}

namespace {
  // Whether `path` is `folder` or below it. Windows compares file names
  // without case, so a favourite saved as ...\HAMO Bank\x.atp is the preset
  // in ...\Hamo Bank and must follow that folder when it moves.
  bool isAtOrBelow(const juce::String& path, const juce::String& folder) {
    const juce::String below = folder + juce::File::getSeparatorString();
#if JUCE_WINDOWS
    return path.equalsIgnoreCase(folder) || path.startsWithIgnoreCase(below);
#else
    return path == folder || path.startsWith(below);
#endif
  }
}

juce::String movedPath(const juce::String& path, const std::vector<Move>& moves) {
  const Move* best = nullptr;
  for (const auto& move : moves) {
    const juce::String from = move.from.getFullPathName();
    if (!isAtOrBelow(path, from))
      continue;
    if (best == nullptr || from.length() > best->from.getFullPathName().length())
      best = &move;
  }
  if (best == nullptr)
    return path;
  return best->to.getFullPathName() + path.substring(best->from.getFullPathName().length());
}

int rewriteFavourites(const std::vector<Move>& moves) {
  if (moves.empty())
    return 0;
  const json favourites = LoadSave::getFavoritesJson();
  if (!favourites.is_object())
    return 0;
  json rewritten = json::object();
  int changed = 0;
  for (const auto& entry : favourites.items()) {
    const juce::String key(entry.key());
    const juce::String moved = movedPath(key, moves);
    if (moved != key)
      ++changed;
    rewritten[moved.toStdString()] = entry.value();
  }
  if (changed > 0)
    LoadSave::saveJsonToFavorites(rewritten);
  return changed;
}

juce::File migrationLogFile(const juce::File& data_directory) {
  return data_directory.getChildFile(kMigrationLogName);
}

std::vector<Move> adoptStrayBanks(const juce::File& data_directory) {
  std::vector<Move> moves;
  if (!data_directory.isDirectory())
    return moves;
  juce::StringArray lines, problems;
  moveStrayBanks(data_directory, moves, lines, problems);
  if (!moves.empty()) {
    const int favourites = rewriteFavourites(moves);
    if (favourites > 0)
      lines.add(juce::String(favourites) + (favourites == 1 ? " favourite" : " favourites") + " rewritten");
  }
  appendLog(data_directory, "Banks filed into the library", lines, problems);
  return moves;
}

bool needsMigration() {
  return layoutVersion() < kLayoutVersion;
}

MigrationReport migrate(const juce::File& data_directory) {
  MigrationReport report;
  if (!data_directory.isDirectory()) {
    setLayoutVersion(kLayoutVersion);
    return report;
  }
  const int from_version = layoutVersion();
  std::vector<Move> moves;
  juce::StringArray& lines = report.lines;
  juce::StringArray& problems = report.problems;
  auto move = [&](const juce::File& from, const juce::File& to) {
    const size_t before = moves.size();
    if (mergeMove(from, to, moves, problems) && moves.size() > before)
      lines.add("Moved " + from.getRelativePathFrom(data_directory) + " to " + to.getRelativePathFrom(data_directory));
  };

  // The first layout's loose user presets name the default bank, when they agree.
  const juce::File first_user_presets = libraryDirectory(Library::kUser, data_directory).getChildFile("Presets");
  juce::String default_bank = from_version < 2 && first_user_presets.isDirectory() ? commonAuthor(first_user_presets)
                                                                                   : juce::String();
  if (default_bank.isEmpty())
    default_bank = defaultBankName();
  setDefaultBankName(default_bank);
  report.default_bank = default_bank;

  // Community kept its downloaded archives in Community/Banks, where its banks go now.
  if (from_version < 3) {
    const juce::File old_archives = libraryDirectory(Library::kCommunity, data_directory).getChildFile("Banks");
    if (old_archives.isDirectory())
      move(old_archives, communityArchiveDirectory(data_directory));
  }

  // Banks at the top of the Atlas folder.
  moveStrayBanks(data_directory, moves, lines, problems);

  for (Library library : libraries()) {
    const juce::File root = libraryDirectory(library, data_directory);
    for (const auto& folder : contentOf(root, juce::File::findDirectories)) {
      const juce::String name = folder.getFileName();
      if (isLayoutFolder(library, name))
        continue;
      if (name == "Presets") {
        // The first layout: a library's Presets held its banks, and loose presets.
        if (library == Library::kUser) {
          const juce::File default_patch = folder.getChildFile("Default").withFileExtension(vital::kPresetExtension);
          if (default_patch.existsAsFile())
            move(default_patch, root.getChildFile(default_patch.getFileName()));
        }
        for (const auto& entry : contentOf(folder)) {
          juce::File target;
          if (entry.isDirectory()) {
            if (library == Library::kFactory) {
              if (entry.getFileName() == kAtlasFactoryPresets)
                target = bankDirectory(library, kAtlasFactoryBank, data_directory).getChildFile(entry.getFileName());
              else if (entry.getFileName() == kVitalFactoryBank)
                target = bankDirectory(library, kVitalFactoryBank, data_directory);
              else
                target = bankDirectory(library, kVitalFactoryBank, data_directory).getChildFile(entry.getFileName());
            }
            else
              target = bankDirectory(library, entry.getFileName(), data_directory);
          }
          else if (isPresetFile(entry)) {
            const juce::String bank = library == Library::kUser ? default_bank
                                    : library == Library::kFactory ? juce::String(kVitalFactoryBank)
                                    : library == Library::kCommunity ? juce::String("Unknown uploader")
                                                                     : juce::String("Imported");
            target = bankDirectory(library, bank, data_directory).getChildFile(entry.getFileName());
          }
          else {
            lines.add("Left " + entry.getRelativePathFrom(data_directory) + " where it is");
            continue;
          }
          move(entry, target);
        }
        removeIfEmpty(folder);
      }
      else if (name == "LFOs")
        move(folder, kindDirectory(library, Kind::kLfos, data_directory));
      else if (name == "FX")
        move(folder, kindDirectory(library, Kind::kFx, data_directory));
      else if (name == "Skins")
        removeIfEmpty(folder);   // Vital's skins, which Atlas never used
      else if (hasKindFolders(folder)) {
        // The second layout: a bank folder of kinds. Its presets stay a bank,
        // the rest become packs of its name; Factory's banks and the default
        // User bank held loose files, which go straight into the kind folders.
        const bool flat = library == Library::kFactory || (library == Library::kUser && name == default_bank);
        Dissolved dissolved;
        dissolve(folder, name, library, data_directory, flat, false, moves, lines, problems, dissolved);
      }
      else
        lines.add("Left " + folder.getRelativePathFrom(data_directory) + " where it is");
    }
  }

  report.ran = true;
  report.moves = static_cast<int>(moves.size());
  report.favourites = rewriteFavourites(moves);
  if (report.favourites > 0)
    lines.add(juce::String(report.favourites) + (report.favourites == 1 ? " favourite" : " favourites") + " rewritten");
  lines.add("Default bank: " + default_bank);
  appendLog(data_directory, "Library reorganised: each library's Banks, Wavetables, Multisamples, Samples, "
                            "LFO Presets, FX Presets and Impulse Responses",
            lines, problems);
  setLayoutVersion(kLayoutVersion);
  return report;
}

MigrationReport migrateIfNeeded() {
  static std::once_flag once;
  static MigrationReport first_report;
  std::call_once(once, [] {
    if (needsMigration())
      first_report = migrate(LoadSave::getDataDirectory());
  });
  return first_report.ran ? std::exchange(first_report, MigrationReport()) : MigrationReport();
}

juce::String archiveBankName(juce::ZipFile& zip) {
  juce::String top;
  for (int i = 0; i < zip.getNumEntries(); ++i) {
    bool skip = false;
    const juce::StringArray parts = entryParts(*zip.getEntry(i), skip);
    if (skip)
      continue;
    if (parts.size() < 2)
      return {};
    if (top.isEmpty())
      top = parts[0];
    else if (top != parts[0])
      return {};
  }
  Kind kind;
  if (kindFromFolderName(top, kind))
    return {};
  return top;
}

ImportResult importBankArchive(const juce::File& archive, Library library, const juce::File& data_directory,
                               const juce::String& fallback_name) {
  ImportResult result;
  juce::FileInputStream input(archive);
  if (!input.openedOk()) {
    result.error = "The bank file could not be opened";
    return result;
  }
  juce::ZipFile zip(input);
  if (zip.getNumEntries() == 0) {
    result.error = "The bank file holds no files";
    return result;
  }

  // Unpacked beside the libraries first, so that filing it is a rename.
  const juce::File staging = data_directory.getChildFile(".Atlas import " + juce::Uuid().toString());
  if (!staging.createDirectory()) {
    result.error = "The folder " + staging.getFullPathName() + " could not be created";
    return result;
  }
  for (int i = 0; i < zip.getNumEntries(); ++i) {
    bool skip = false;
    const juce::StringArray parts = entryParts(*zip.getEntry(i), skip);
    if (skip)
      continue;
    const juce::File destination = staging.getChildFile(parts.joinIntoString("/"));
    if (!destination.isAChildOf(staging)) {
      result.error = "The bank file holds an entry outside its folder: " + zip.getEntry(i)->filename;
      staging.deleteRecursively();
      return result;
    }
    const juce::Result unpacked = zip.uncompressEntry(i, staging, true);
    if (unpacked.failed()) {
      result.error = unpacked.getErrorMessage();
      staging.deleteRecursively();
      return result;
    }
    ++result.files;
  }
  if (result.files == 0) {
    result.error = "The bank file holds no files";
    staging.deleteRecursively();
    return result;
  }

  const juce::String top = archiveBankName(zip);
  const juce::File root = top.isNotEmpty() ? staging.getChildFile(top) : staging;
  result.name = legalName(top.isNotEmpty() ? top : fallback_name, "Imported bank");
  std::vector<Move> moves;
  juce::StringArray lines, problems;
  Dissolved dissolved;
  if (hasKindFolders(root))
    dissolve(root, result.name, library, data_directory, false, true, moves, lines, problems, dissolved);
  else {
    // A folder of presets with no Presets folder: the whole of it is the bank.
    mergeMove(root, bankDirectory(library, result.name, data_directory), moves, problems, true, &dissolved.replaced);
    dissolved.presets = true;
  }
  staging.deleteRecursively();
  result.replaced = dissolved.replaced;
  // In the order a library lists its folders.
  for (Kind kind : kinds()) {
    if (dissolved.packs.contains(kindFolderName(kind)))
      result.packs.add(kindFolderName(kind));
  }
  if (dissolved.presets)
    result.bank = bankDirectory(library, result.name, data_directory);
  if (!problems.isEmpty())
    result.error = problems[0];
  return result;
}

namespace {
  // The instruments the presets of a bank play, as folders (or single files)
  // in a library's Multisamples.
  std::vector<juce::File> instrumentsOfBank(const juce::File& bank, const juce::File& library_directory,
                                            const juce::File& data_directory) {
    std::vector<juce::File> found;
    std::set<juce::String> seen;
    juce::Array<juce::File> presets;
    bank.findChildFiles(presets, juce::File::findFiles | juce::File::ignoreHiddenFiles, true, vital::kPresetExtensionsList);
    std::sort(presets.begin(), presets.end(), naturalPathLess);
    for (const auto& preset : presets) {
      if (vital::multisample::container::isContainer(preset))
        continue;   // it carries its instruments
      std::string text;
      if (!vital::multisample::container::readPresetText(preset, text) || text.find("_multisample\"") == std::string::npos)
        continue;
      json data;
      try {
        data = json::parse(text, nullptr, false);
      }
      catch (const json::exception&) {
        continue;
      }
      if (data.is_discarded() || !data.is_object() || !data.count("settings") || !data["settings"].is_object())
        continue;
      for (const auto& item : data["settings"].items()) {
        const std::string& key = item.key();
        if (key.size() < 12 || key.compare(key.size() - 12, 12, "_multisample") != 0 || !item.value().is_object())
          continue;
        const auto spec = vital::multisample::InstrumentSpec::fromJson(item.value());
        juce::File entry;
        if (!spec.library_instrument.empty())
          entry = instrumentsDirectory(library_directory).getChildFile(juce::String(spec.library_instrument));
        if (!entry.existsAsFile())
          entry = findInstrumentAnywhere(spec.hash, data_directory);
        if (!entry.existsAsFile())
          continue;
        juce::File instrument = instrumentFolderOf(entry, data_directory);
        if (instrument == juce::File())
          instrument = entry;
        if (seen.insert(instrument.getFullPathName()).second)
          found.push_back(instrument);
      }
    }
    return found;
  }
}

ArchiveResult writeBankArchive(const juce::File& source, const juce::File& destination) {
  ArchiveResult result;
  if (!source.isDirectory()) {
    result.error = "The selected source is not a folder.";
    result.spoken = "Unable to export bank";
    return result;
  }
  const juce::String bank_name = legalName(destination.getFileNameWithoutExtension(), "Atlas Bank");
  juce::ZipFile::Builder zip;
  juce::int64 total_bytes = 0;
  auto add = [&](const juce::File& file, const juce::String& stored_path) {
    zip.addFile(file, compressionFor(file), bank_name + "/" + stored_path);
    total_bytes += file.getSize();
    ++result.added;
  };
  // Every file in `folder`, stored under `prefix` at its path below it.
  auto addFolder = [&](const juce::File& folder, const juce::String& prefix) {
    juce::Array<juce::File> files;
    folder.findChildFiles(files, juce::File::findFiles | juce::File::ignoreHiddenFiles, true);
    std::sort(files.begin(), files.end(), naturalPathLess);
    for (const auto& file : files) {
      const juce::StringArray parts = pathParts(file, folder);
      bool hidden = false;
      for (const auto& part : parts)
        hidden = hidden || isHiddenName(part);
      if (!hidden)
        add(file, prefix + "/" + parts.joinIntoString("/"));
    }
  };

  const juce::File data_directory = LoadSave::getDataDirectory();
  const Location location = locate(source, data_directory);
  if (location.in_kind && location.kind == Kind::kPresets && location.bank.isNotEmpty() && location.folders.isEmpty()) {
    // A bank: its presets, the packs of its name and the instruments its presets play.
    addFolder(source, archiveFolderName(Kind::kPresets));
    for (Kind kind : { Kind::kWavetables, Kind::kSamples, Kind::kLfos, Kind::kFx, Kind::kImpulseResponses }) {
      const juce::File pack = kindDirectory(location.library, kind, data_directory).getChildFile(location.bank);
      if (pack.isDirectory())
        addFolder(pack, archiveFolderName(kind));
    }
    const juce::File library_directory = libraryDirectory(location.library, data_directory);
    for (const auto& instrument : instrumentsOfBank(source, library_directory, data_directory)) {
      const juce::String prefix = archiveFolderName(Kind::kMultisamples);
      if (instrument.isDirectory())
        addFolder(instrument, prefix + "/" + instrument.getFileName());
      else
        add(instrument, prefix + "/" + instrument.getFileName());
    }
  }
  else if (location.in_kind && location.kind != Kind::kPresets && location.pack.isNotEmpty() &&
           location.folders.isEmpty()) {
    // A pack: its files, under its kind.
    addFolder(source, archiveFolderName(location.kind));
  }
  else {
    // Any other folder: its Atlas files sorted into kinds by extension, below
    // the kind folder on their path when there is one.
    const juce::String atlas_files = vital::kPresetExtensionsList + ";*." + vital::kWavetableExtension +
                                     ";*." + vital::kLegacyWavetableExtension + ";" + vital::kSampleExtensionsList +
                                     ";" + vital::kLfoExtensionsList + ";*." + vital::kFxExtension +
                                     ";*." + vital::kFxBusExtension;
    juce::Array<juce::File> files;
    source.findChildFiles(files, juce::File::findFiles | juce::File::ignoreHiddenFiles, true, atlas_files);
    std::sort(files.begin(), files.end(), naturalPathLess);
    for (const auto& file : files) {
      const juce::StringArray parts = pathParts(file, source);
      auto indexOfKind = [&parts](Kind kind) {
        const int library_index = parts.indexOf(kindFolderName(kind));
        return library_index >= 0 ? library_index : parts.indexOf(archiveFolderName(kind));
      };
      Kind kind;
      if (isPresetFile(file))
        kind = Kind::kPresets;
      else if (file.hasFileExtension(vital::kWavetableExtension) || file.hasFileExtension(vital::kLegacyWavetableExtension))
        kind = Kind::kWavetables;
      else if (file.hasFileExtension(vital::kLfoExtension) || file.hasFileExtension(vital::kLegacyLfoExtension))
        kind = Kind::kLfos;
      else if (file.hasFileExtension(vital::kFxExtension) || file.hasFileExtension(vital::kFxBusExtension))
        kind = Kind::kFx;
      else if (indexOfKind(Kind::kWavetables) >= 0)
        kind = Kind::kWavetables;
      else if (indexOfKind(Kind::kImpulseResponses) >= 0)
        kind = Kind::kImpulseResponses;
      else
        kind = Kind::kSamples;
      const int kind_index = indexOfKind(kind);
      juce::StringArray stored;
      for (int i = kind_index >= 0 ? kind_index + 1 : 0; i < parts.size(); ++i)
        stored.add(parts[i]);
      add(file, archiveFolderName(kind) + "/" + stored.joinIntoString("/"));
    }
  }

  if (result.added == 0) {
    result.error = "No Atlas presets, tables, samples, LFOs, FX presets or instruments were found.";
    result.spoken = "No bank content found";
    return result;
  }
  if (total_bytes > kMaxArchiveBytes) {
    result.error = "The bank is " + juce::File::descriptionOfSizeInBytes(total_bytes) +
                   ", more than the 4 GB a bank can hold.";
    result.spoken = "Bank too large to export";
    return result;
  }

  // FileOutputStream appends to a file that is there already, which made a
  // broken archive of an export over an older one.
  juce::TemporaryFile temporary(destination);
  bool written = false;
  {
    juce::FileOutputStream output(temporary.getFile());
    written = output.openedOk() && zip.writeToStream(output, nullptr);
    output.flush();
    written = written && !output.getStatus().failed();
  }
  if (!written || !temporary.overwriteTargetFileWithTemporary()) {
    result.error = "The bank file could not be written.";
    result.spoken = "Unable to export bank";
  }
  return result;
}

// ---- Multisample instruments ----

juce::File instrumentsDirectory(const juce::File& library_directory) {
  return library_directory.getChildFile(kindFolderName(Kind::kMultisamples));
}

InstrumentManifest readInstrumentManifest(const juce::File& folder) {
  InstrumentManifest manifest;
  const juce::File file = folder.getChildFile(kInstrumentManifestName);
  if (!file.existsAsFile())
    return manifest;
  json data;
  try {
    data = json::parse(file.loadFileAsString().toStdString(), nullptr, false);
  }
  catch (const json::exception&) {
    return manifest;
  }
  if (data.is_discarded() || !data.is_object())
    return manifest;
  manifest.hash = juce::String(data.value("hash", std::string()));
  manifest.entry = juce::String(data.value("entry", std::string()));
  manifest.format = juce::String(data.value("format", std::string()));
  manifest.name = juce::String(data.value("name", std::string()));
  if (data.count("files") && data["files"].is_array()) {
    for (const auto& item : data["files"]) {
      if (!item.is_object() || !item.count("path") || !item["path"].is_string())
        continue;
      InstrumentManifest::File info;
      info.relative = item["path"].get<std::string>();
      info.size = item.value("size", static_cast<juce::int64>(0));
      info.crc = item.value("crc", static_cast<juce::uint32>(0));
      info.text = item.value("text", isInstrumentText(info.relative));
      manifest.files.push_back(info);
    }
  }
  if (data.count("path_overrides") && data["path_overrides"].is_object()) {
    for (const auto& entry : data["path_overrides"].items()) {
      if (entry.value().is_string())
        manifest.path_overrides[entry.key()] = entry.value().get<std::string>();
    }
  }
  manifest.valid = manifest.hash.isNotEmpty() && manifest.entry.isNotEmpty();
  return manifest;
}

bool writeInstrumentManifest(const juce::File& folder, const InstrumentManifest& manifest) {
  json data;
  data["hash"] = manifest.hash.toStdString();
  data["entry"] = manifest.entry.toStdString();
  data["format"] = manifest.format.toStdString();
  data["name"] = manifest.name.toStdString();
  json files = json::array();
  for (const auto& info : manifest.files)
    files.push_back({ { "path", info.relative }, { "size", info.size }, { "crc", info.crc }, { "text", info.text } });
  data["files"] = files;
  json overrides = json::object();
  for (const auto& entry : manifest.path_overrides)
    overrides[entry.first] = entry.second;
  data["path_overrides"] = overrides;
  return folder.getChildFile(kInstrumentManifestName).replaceWithText(juce::String(data.dump(2)));
}

juce::File instrumentFolderOf(const juce::File& file, const juce::File& data_directory) {
  const Location location = locate(file, data_directory);
  if (!location.in_kind || location.kind != Kind::kMultisamples || location.pack.isEmpty())
    return {};
  return kindDirectory(location.library, Kind::kMultisamples, data_directory).getChildFile(location.pack);
}

juce::File findInstrumentByHash(const std::string& hash, const juce::File& library_directory) {
  if (hash.empty())
    return {};
  for (const auto& folder : contentOf(instrumentsDirectory(library_directory), juce::File::findDirectories)) {
    const auto manifest = readInstrumentManifest(folder);
    if (manifest.valid && manifest.hash == juce::String(hash)) {
      const juce::File entry = folder.getChildFile(manifest.entry);
      if (entry.existsAsFile())
        return entry;
    }
  }
  return {};
}

juce::File findInstrumentAnywhere(const std::string& hash, const juce::File& data_directory) {
  if (hash.empty())
    return {};
  for (Library library : libraries()) {
    const juce::File found = findInstrumentByHash(hash, libraryDirectory(library, data_directory));
    if (found != juce::File())
      return found;
  }
  return {};
}

juce::String libraryInstrumentPath(const juce::File& entry, const juce::File& library_directory) {
  return entry.getRelativePathFrom(instrumentsDirectory(library_directory)).replaceCharacter('\\', '/');
}

juce::File placeInstrument(const juce::File& library_directory, const juce::String& name, const std::string& hash,
                           const std::string& entry, const std::string& format,
                           const std::vector<vital::multisample::FileInfo>& files,
                           const std::map<std::string, std::string>& path_overrides,
                           const std::function<bool()>& cancelled, juce::String& error) {
  if (hash.empty() || entry.empty() || files.empty()) {
    error = "The instrument has no files to keep.";
    return {};
  }
  const juce::File instruments = instrumentsDirectory(library_directory);

  // Already in the library's Multisamples: referred to where it is. A folder
  // Atlas placed it in has its manifest brought up to date (an edited SFZ).
  bool in_place = true;
  juce::File entry_file;
  for (const auto& info : files) {
    in_place = in_place && info.file.isAChildOf(instruments);
    if (info.relative == entry)
      entry_file = info.file;
  }
  if (in_place && entry_file.existsAsFile()) {
    const juce::File folder = instrumentFolderOf(entry_file, library_directory.getParentDirectory());
    const auto manifest = folder != juce::File() ? readInstrumentManifest(folder) : InstrumentManifest();
    if (manifest.valid && folder.getChildFile(manifest.entry) == entry_file && manifest.hash != juce::String(hash)) {
      std::vector<vital::multisample::FileInfo> relative_to_folder = files;
      for (auto& info : relative_to_folder)
        info.relative = info.file.getRelativePathFrom(folder).replaceCharacter('\\', '/').toStdString();
      writeInstrumentManifest(folder, manifestFor(manifest.name, hash, manifest.entry.toStdString(), format,
                                                  relative_to_folder, manifest.path_overrides));
    }
    return entry_file;
  }

  // Placed before.
  const juce::File existing = findInstrumentByHash(hash, library_directory);
  if (existing != juce::File())
    return existing;

  const juce::File folder = freeInstrumentFolder(instruments, name);
  const juce::File partial = placingFolder(folder);
  partial.deleteRecursively();
  std::string place_error;
  if (!vital::multisample::container::placeFiles(files, partial, -1, cancelled, place_error)) {
    error = juce::String(place_error);
    partial.deleteRecursively();
    return {};
  }
  if (!completeInstrumentFolder(partial, folder, manifestFor(name, hash, entry, format, files, path_overrides), error))
    return {};
  return folder.getChildFile(juce::String(entry));
}

bool adoptContainerPreset(const juce::File& preset, const juce::File& data_directory, juce::String& error) {
  namespace ms = vital::multisample;
  if (!ms::container::isContainer(preset))
    return false;
  const Location location = locate(preset, data_directory);
  if (!location.in_kind || location.kind != Kind::kPresets || location.bank.isEmpty()) {
    error = "The preset is not in a bank.";
    return false;
  }
  const juce::File library_directory = libraryDirectory(location.library, data_directory);
  std::string text;
  if (!ms::container::readPresetText(preset, text)) {
    error = "The preset could not be read.";
    return false;
  }
  json data;
  try {
    data = json::parse(text, nullptr, false);
  }
  catch (const json::exception&) {
    data = json();
  }
  if (data.is_discarded() || !data.is_object() || !data.count("settings") || !data["settings"].is_object()) {
    error = "The preset is not valid.";
    return false;
  }
  json& settings = data["settings"];
  for (auto& item : settings.items()) {
    const std::string& key = item.key();
    json& value = item.value();
    if (key.size() < 12 || key.compare(key.size() - 12, 12, "_multisample") != 0 || !value.is_object())
      continue;
    ms::InstrumentSpec spec = ms::InstrumentSpec::fromJson(value);
    if (spec.hash.empty() || spec.files.empty() || spec.entry.empty())
      continue;
    juce::File entry = findInstrumentByHash(spec.hash, library_directory);
    if (entry == juce::File()) {
      const juce::File folder = freeInstrumentFolder(instrumentsDirectory(library_directory), juce::String(spec.name));
      const juce::File partial = placingFolder(folder);
      std::string extract_error;
      if (!ms::container::extractTo(preset, spec.hash, spec.files, partial, {}, extract_error)) {
        error = juce::String(extract_error);
        return false;
      }
      std::vector<ms::FileInfo> files = spec.files;
      for (auto& info : files)
        info.text = isInstrumentText(info.relative);
      if (!completeInstrumentFolder(partial, folder,
                                    manifestFor(juce::String(spec.name), spec.hash, spec.entry, spec.format, files,
                                                spec.path_overrides),
                                    error))
        return false;
      entry = folder.getChildFile(juce::String(spec.entry));
    }
    spec.library_instrument = libraryInstrumentPath(entry, library_directory).toStdString();
    spec.path = entry.getFullPathName().toStdString();
    spec.files.clear();
    spec.path_overrides.clear();
    value = spec.toJson(false);
  }
  // Plain JSON takes the container's place.
  juce::TemporaryFile temporary(preset);
  if (!temporary.getFile().replaceWithText(juce::String(LoadSave::dumpState(data))) ||
      !temporary.overwriteTargetFileWithTemporary()) {
    error = "Could not rewrite " + preset.getFullPathName();
    return false;
  }
  return true;
}

} // namespace atlas::library

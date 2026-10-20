#pragma once

// The library layout (library_layout.h): where a path sits, the banks and
// kind folders a library holds, the migration from both older layouts, stray
// banks filed into Third Party, and bank archives written, imported and
// merged. Everything runs in a temporary Atlas folder with a config and
// favourites file of its own.
// Included after editor_accessibility_tests.h, whose helpers it uses.

#include "library_layout.h"
#include "load_save.h"
#include "line_generator.h"

namespace {
  namespace lib = atlas::library;

  struct LayoutTestLibrary {
    File folder = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("AtlasLayoutTest", "", false);
    TemporaryFile config { ".config" };
    TemporaryFile favourites { ".favorites" };

    explicit LayoutTestLibrary(int layout_version = -1, const String& default_bank = {}) {
      folder.createDirectory();
      json contents = json::object();
      contents["data_directory"] = folder.getFullPathName().toStdString();
      if (layout_version >= 0)
        contents["library_layout"] = layout_version;
      if (default_bank.isNotEmpty())
        contents["default_bank"] = default_bank.toStdString();
      config.getFile().replaceWithText(contents.dump());
      LoadSave::setConfigFileForTesting(config.getFile());
      LoadSave::setFavoritesFileForTesting(favourites.getFile());
    }
    ~LayoutTestLibrary() {
      LoadSave::setConfigFileForTesting({});
      LoadSave::setFavoritesFileForTesting({});
      folder.deleteRecursively();
    }
    bool isolated() const { return LoadSave::getDataDirectory() == folder; }
  };

  // A preset file as Atlas writes one: sorted keys, so "author" comes first
  // and the metadata readers find it, padded past their minimum size.
  bool writeLayoutPreset(const File& file, const String& author, const String& style = "Bass",
                         const json& settings = json::object()) {
    json data;
    data["author"] = author.toStdString();
    data["comments"] = "A preset written by the layout test, padded past the metadata reader's minimum size.";
    data["preset_name"] = file.getFileNameWithoutExtension().toStdString();
    data["preset_style"] = style.toStdString();
    data["settings"] = settings;
    return file.getParentDirectory().createDirectory() && file.replaceWithText(String(data.dump()));
  }

  bool writeLayoutFile(const File& file, const String& text = "Atlas layout test file") {
    return file.getParentDirectory().createDirectory() && file.replaceWithText(text);
  }

  // Checks that each path exists, or with a leading '!' that it does not.
  void expectLayout(const File& data, const StringArray& paths, const std::string& when) {
    for (const auto& path : paths) {
      const bool absent = path.startsWithChar('!');
      const String relative = absent ? path.substring(1) : path;
      const bool there = data.getChildFile(relative).exists();
      expect(there != absent, when + (absent ? " there is no " : " there is ") + relative.toStdString());
    }
  }

  void testLocateFindsLibraryBankKindAndCategory() {
    LayoutTestLibrary library;
    expect(library.isolated(), "the test's Atlas folder is its own temporary folder");
    if (!library.isolated())
      return;
    const File data = library.folder;

    expect(lib::libraryName(lib::Library::kThirdParty) == "Third Party" &&
               lib::libraryName(lib::Library::kFactory) == "Factory" &&
               lib::libraryName(lib::Library::kUser) == "User" &&
               lib::libraryName(lib::Library::kCommunity) == "Community",
           "the four library names");
    lib::Library parsed;
    expect(lib::libraryFromName("Community", parsed) && parsed == lib::Library::kCommunity &&
               !lib::libraryFromName("Other", parsed),
           "a library is found by name, and Other is none");
    StringArray folder_names;
    for (lib::Kind kind : lib::kinds())
      folder_names.add(lib::kindFolderName(kind));
    expect(folder_names == StringArray { "Banks", "Wavetables", "Multisamples", "Samples", "LFO Presets", "FX Presets",
                                         "Impulse Responses" },
           "a library's folders, in order: " + folder_names.joinIntoString(", ").toStdString());
    lib::Kind kind;
    expect(lib::archiveFolderName(lib::Kind::kLfos) == "LFOs" && lib::archiveFolderName(lib::Kind::kPresets) == "Presets" &&
               lib::kindFromFolderName("LFOs", kind) && kind == lib::Kind::kLfos &&
               lib::kindFromFolderName("FX Presets", kind) && kind == lib::Kind::kFx &&
               lib::kindFromFolderName("Banks", kind) && kind == lib::Kind::kPresets &&
               lib::kindFromFolderName("Presets", kind) && kind == lib::Kind::kPresets &&
               !lib::kindFromFolderName("Skins", kind),
           "a kind is found by its library or archive folder name, and Skins is none");

    const lib::Location preset = lib::locate(data.getChildFile("Third Party/Banks/VR Vol 1/Bass/Sub/x.atp"), data);
    expect(preset.in_library && preset.library == lib::Library::kThirdParty && preset.in_kind &&
               preset.kind == lib::Kind::kPresets && preset.bank == "VR Vol 1" && preset.pack.isEmpty() &&
               preset.category() == "Bass / Sub",
           "a third-party preset: library, bank and category from its folders");
    const lib::Location community = lib::locate(data.getChildFile("Community/Banks/jay/Lead/g.atp"), data);
    expect(community.library == lib::Library::kCommunity && community.bank == "jay" && community.category() == "Lead",
           "a community preset: the uploader is the bank, the style the category");
    const lib::Location loose_preset = lib::locate(data.getChildFile("User/Banks/x.atp"), data);
    expect(loose_preset.in_kind && loose_preset.bank.isEmpty(), "a preset directly in Banks has no bank");
    const lib::Location table = lib::locate(data.getChildFile("User/Wavetables/Serum/Bass/w.wav"), data);
    expect(table.in_kind && table.kind == lib::Kind::kWavetables && table.pack == "Serum" && table.bank.isEmpty() &&
               table.folders == StringArray { "Bass" },
           "a wavetable in a pack: the pack and its folders");
    const lib::Location loose_table = lib::locate(data.getChildFile("User/Wavetables/w.att"), data);
    expect(loose_table.in_kind && loose_table.pack.isEmpty(), "a wavetable directly in Wavetables has no pack");
    const lib::Location default_patch = lib::locate(data.getChildFile("User/Default.atp"), data);
    expect(default_patch.in_library && default_patch.library == lib::Library::kUser && !default_patch.in_kind,
           "a file at the library level is in no kind folder");
    expect(data.getChildFile("User/Banks/My Bank").createDirectory().wasOk(), "a bank folder is made");
    const lib::Location bank_folder = lib::locate(data.getChildFile("User/Banks/My Bank"), data);
    expect(bank_folder.in_kind && bank_folder.bank == "My Bank" && bank_folder.folders.isEmpty(),
           "a bank folder locates as its bank");
    expect(!lib::locate(data.getChildFile("User/Skins/x"), data).in_kind, "a folder that is no kind leaves the kind unset");
    expect(!lib::locate(File::getSpecialLocation(File::tempDirectory).getChildFile("elsewhere.atp"), data).in_library,
           "a file outside the Atlas folder is in no library");
    expect(!lib::locate(data.getChildFile("Other/x.atp"), data).in_library, "a top-level folder that is no library");

    const String legal = lib::legalName("../Sneaky: Bass?", "x");
    expect(legal.isNotEmpty() && !legal.containsAnyOf("/\\:?") && !legal.startsWithChar('.'),
           "a legal name has no separators, colons or leading dots: " + legal.toStdString());
    expect(lib::legalName("   ", "Fallback") == "Fallback" && lib::legalName("..", "Fallback") == "Fallback",
           "nothing left gives the fallback");
    expect(lib::bankDirectory(lib::Library::kUser, "a/b", data).getParentDirectory() == data.getChildFile("User/Banks"),
           "a bank name with a separator stays one folder");
    expect(lib::packDirectory(lib::Library::kCommunity, lib::Kind::kMultisamples, "jay", data) ==
               data.getChildFile("Community/Multisamples/jay") &&
               lib::kindDirectory(lib::Library::kFactory, lib::Kind::kFx, data) == data.getChildFile("Factory/FX Presets"),
           "kind and pack folders");
  }

  void testBanksAndKindFoldersAreListed() {
    LayoutTestLibrary library;
    if (!library.isolated())
      return;
    const File data = library.folder;
    for (const char* folder : { "User/Banks/Alpha", "User/Banks/beta", "User/Banks/Gamma 10", "User/Banks/Gamma 2",
                                "User/Wavetables/Serum", "Community/Banks/jay", "Community/Archives/jay",
                                "Community/Previews/Presets/jay", "Third Party/Samples/VR", "Factory/Banks/Atlas" })
      expect(data.getChildFile(folder).createDirectory().wasOk(), std::string("made ") + folder);
    writeLayoutPreset(data.getChildFile("User/Default.atp"), "");

    const StringArray user_banks = lib::bankNames(lib::Library::kUser, data);
    expect(user_banks == StringArray { "Alpha", "beta", "Gamma 2", "Gamma 10" },
           "User banks in natural order: " + user_banks.joinIntoString(", ").toStdString());
    expect(lib::bankNames(lib::Library::kCommunity, data) == StringArray { "jay" },
           "Community's banks are its Banks folder's, not its archives or previews");
    expect(lib::bankFolders(data).size() == 6, "every library's banks, library by library");

    const auto presets = lib::kindFolders(lib::Kind::kPresets, data);
    expect(presets.size() == 3 && presets[0] == data.getChildFile("Factory/Banks") &&
               presets[1] == data.getChildFile("User/Banks") && presets[2] == data.getChildFile("Community/Banks"),
           "the Banks folders of every library, Factory first and Community last");
    expect(LoadSave::getPresetDirectories() == presets, "LoadSave scans the same folders for presets");
    expect(LoadSave::getWavetableDirectories() == std::vector<File> { data.getChildFile("User/Wavetables") } &&
               LoadSave::getSampleDirectories() == std::vector<File> { data.getChildFile("Third Party/Samples") } &&
               LoadSave::getSkinDirectories().empty(),
           "the other kinds are each library's kind folder, and there are no skins");

    expect(lib::defaultBankName() == "My Bank", "the default bank is My Bank until one is saved into");
    lib::setDefaultBankName("Alessio");
    expect(lib::defaultBankName() == "Alessio", "the default bank is remembered in the config");
    expect(LoadSave::getUserPresetDirectory() == data.getChildFile("User/Banks/Alessio") &&
               data.getChildFile("User/Banks/Alessio").isDirectory(),
           "the user preset directory is the default bank, made on demand");
    expect(LoadSave::getUserLfoDirectory() == data.getChildFile("User/LFO Presets") &&
               LoadSave::getUserFxDirectory() == data.getChildFile("User/FX Presets") &&
               LoadSave::getUserWavetableDirectory() == data.getChildFile("User/Wavetables"),
           "LFO, FX and wavetable saves go to the User library's kind folders");
    lib::setDefaultBankName("../ /");
    expect(lib::defaultBankName() == "My Bank", "an illegal default bank name falls back");
  }

  // The first layout: libraries holding kind folders, imported banks at the
  // top of the Atlas folder, community presets under Community/Presets and
  // its archives under Community/Banks, loose user files and a hand-made
  // Factory.
  void testMigrationFromTheFirstLayout() {
    LayoutTestLibrary library;
    if (!library.isolated())
      return;
    const File data = library.folder;
    auto preset = [&](const char* relative, const String& author = "Alessio") {
      expect(writeLayoutPreset(data.getChildFile(relative), author), std::string("wrote ") + relative);
    };
    auto file = [&](const char* relative) {
      expect(writeLayoutFile(data.getChildFile(relative)), std::string("wrote ") + relative);
    };
    preset("Avant/Presets/Drop Bass/a.atp", "Avant");
    file("Avant/Wavetables/w.wav");
    preset("Hamo/Presets/h.atp", "Hamo");
    file("Community/Banks/jay/Hamo.atb");
    preset("Community/Presets/jay/Lead/g.atp", "Jay");
    file("Community/Previews/Presets/jay/g preview.mp3");
    preset("User/Presets/loose1.atp");
    preset("User/Presets/loose2.atp");
    preset("User/Presets/loose3.atp", "Someone else");
    preset("User/Presets/Default.atp", "");
    preset("User/Presets/IDK/x.atp");
    preset("User/Presets/IDK/Sub/y.atp");
    file("User/Presets/.DS_Store");
    file("User/Wavetables/Serum/Bass/w.wav");
    file("User/Wavetables/loose.att");
    file("User/LFOs/l.ats");
    expect(data.getChildFile("User/Skins").createDirectory().wasOk(), "an empty skins folder");
    preset("Factory/Presets/Vital/v.atp", "Vital");
    preset("Factory/Presets/Oscillator Slots/o.atp", "Atlas");
    preset("Factory/Presets/stray.atp", "Vital");
    file("Factory/LFOs/Basic/b.ats");
    file("Factory/Wavetables/Vital/t.wav");
    file("Factory/Samples/Tape/Vinyl 1.wav");
    file("Factory/FX/Analogue/a.atf");
    file("Notes/readme.txt");
    file("packs.json");
    json favourites = json::object();
    favourites[data.getChildFile("User/Presets/loose1.atp").getFullPathName().toStdString()] = 1;
    favourites[data.getChildFile("Avant/Presets/Drop Bass/a.atp").getFullPathName().toStdString()] = 1;
    favourites["/Elsewhere/x.atp"] = 1;
    LoadSave::saveJsonToFavorites(favourites);
    expect(lib::needsMigration(), "a config without a layout version needs the migration");

    const lib::MigrationReport report = lib::migrate(data);
    expect(report.ran && report.problems.isEmpty(),
           "the migration ran without problems: " + report.problems.joinIntoString("; ").toStdString());
    expect(report.default_bank == "Alessio", "the loose presets' author names the default bank: " +
                                                 report.default_bank.toStdString());
    expectLayout(data, {
      "Third Party/Banks/Avant/Drop Bass/a.atp", "Third Party/Wavetables/Avant/w.wav", "!Avant",
      "Community/Banks/Hamo/h.atp", "!Hamo", "Community/Archives/jay/Hamo.atb",
      "Community/Banks/jay/Lead/g.atp", "!Community/Presets", "Community/Previews/Presets/jay/g preview.mp3",
      "User/Banks/Alessio/loose1.atp", "User/Banks/Alessio/loose2.atp", "User/Banks/Alessio/loose3.atp",
      "User/Default.atp", "User/Banks/IDK/x.atp", "User/Banks/IDK/Sub/y.atp",
      "User/Wavetables/Serum/Bass/w.wav", "User/Wavetables/loose.att", "User/LFO Presets/l.ats",
      "!User/Presets", "!User/LFOs", "!User/Skins",
      "Factory/Banks/Vital/v.atp", "Factory/Banks/Atlas/Oscillator Slots/o.atp", "Factory/Banks/Vital/stray.atp",
      "Factory/LFO Presets/Basic/b.ats", "Factory/Wavetables/Vital/t.wav", "Factory/Samples/Tape/Vinyl 1.wav",
      "Factory/FX Presets/Analogue/a.atf", "!Factory/Presets", "!Factory/LFOs", "!Factory/FX",
      "Notes/readme.txt", "packs.json"
    }, "after migrating the first layout");
    expect(lib::migrationLogFile(data).existsAsFile() &&
               lib::migrationLogFile(data).loadFileAsString().contains("Moved User/Presets/IDK to User/Banks/IDK"),
           "Migration.txt lists the moves");

    const json rewritten = LoadSave::getFavoritesJson();
    expect(rewritten.count(data.getChildFile("User/Banks/Alessio/loose1.atp").getFullPathName().toStdString()) == 1 &&
               rewritten.count(data.getChildFile("Third Party/Banks/Avant/Drop Bass/a.atp").getFullPathName().toStdString()) == 1 &&
               rewritten.count("/Elsewhere/x.atp") == 1 && rewritten.size() == 3,
           "favourites follow their files and others stay");
    expect(report.favourites == 2, "two favourites were rewritten");
    expect(!lib::needsMigration(), "the config records the layout version");

    const lib::MigrationReport again = lib::migrate(data);
    expect(again.ran && again.moves == 0 && again.problems.isEmpty(), "a second run finds nothing to move");
    expect(lib::defaultBankName() == "Alessio", "the default bank stays");

    Array<File> presets;
    LoadSave::getAllPresets(presets);
    expect(presets.size() == 11, "eleven presets are listed, the default patch not among them: " +
                                     std::to_string(presets.size()));
  }

  // The second layout: libraries holding bank folders of kinds.
  void testMigrationFromTheSecondLayout() {
    LayoutTestLibrary library(2, "JayLadder");
    if (!library.isolated())
      return;
    const File data = library.folder;
    auto preset = [&](const char* relative, const String& author = "JayLadder") {
      expect(writeLayoutPreset(data.getChildFile(relative), author), std::string("wrote ") + relative);
    };
    auto file = [&](const char* relative) {
      expect(writeLayoutFile(data.getChildFile(relative)), std::string("wrote ") + relative);
    };
    preset("User/JayLadder/Presets/a.atp");
    file("User/JayLadder/Wavetables/Monkey.vitaltable");
    file("User/JayLadder/FX/Bus.atfx");
    preset("User/IDK/Presets/x.atp");
    preset("User/IDK/Presets/Sub/y.atp");
    file("User/IDK/Multisamples/Piano/p.sfz");
    file("User/Serum/Wavetables/Bass/w.wav");
    file("User/Serum/Samples/s.wav");
    preset("User/Default.atp", "");
    preset("Third Party/VR/Presets/v.atp", "VR");
    file("Third Party/VR/Wavetables/t.wav");
    preset("Third Party/KLING/Presets/k.atp", "Kling");
    file("Third Party/KLING/LFOs/l.ats");
    preset("Community/jay/Presets/Bass/b.atp", "Jay");
    file("Community/Banks/jay/Hamo.atb");
    file("Community/Previews/Presets/jay/p.mp3");
    preset("Factory/Atlas/Presets/Oscillator Slots/o.atp", "Atlas");
    file("Factory/Atlas/Wavetables/Filter Wavetables/f.wav");
    file("Factory/Atlas/Samples/Tape/Vinyl 1.wav");
    file("Factory/Atlas/FX/Analogue/a.atf");
    preset("Factory/Vital/Presets/v.atp", "Vital");
    file("Factory/Vital/LFOs/Basic/b.ats");
    file("Migration.txt");
    json favourites = json::object();
    favourites[data.getChildFile("User/JayLadder/Presets/a.atp").getFullPathName().toStdString()] = 1;
    LoadSave::saveJsonToFavorites(favourites);
    expect(lib::needsMigration(), "the second layout needs the migration");

    const lib::MigrationReport report = lib::migrate(data);
    expect(report.ran && report.problems.isEmpty(),
           "the migration ran without problems: " + report.problems.joinIntoString("; ").toStdString());
    expect(report.default_bank == "JayLadder", "the default bank stays JayLadder");
    expectLayout(data, {
      "User/Banks/JayLadder/a.atp", "User/Wavetables/Monkey.vitaltable", "User/FX Presets/Bus.atfx",
      "User/Banks/IDK/x.atp", "User/Banks/IDK/Sub/y.atp", "User/Multisamples/Piano/p.sfz",
      "User/Wavetables/Serum/Bass/w.wav", "User/Samples/Serum/s.wav", "User/Default.atp",
      "!User/JayLadder", "!User/IDK", "!User/Serum",
      "Third Party/Banks/VR/v.atp", "Third Party/Wavetables/VR/t.wav", "Third Party/Banks/KLING/k.atp",
      "Third Party/LFO Presets/KLING/l.ats", "!Third Party/VR", "!Third Party/KLING",
      "Community/Banks/jay/Bass/b.atp", "Community/Archives/jay/Hamo.atb", "Community/Previews/Presets/jay/p.mp3",
      "!Community/jay",
      "Factory/Banks/Atlas/Oscillator Slots/o.atp", "Factory/Wavetables/Filter Wavetables/f.wav",
      "Factory/Samples/Tape/Vinyl 1.wav", "Factory/FX Presets/Analogue/a.atf", "Factory/Banks/Vital/v.atp",
      "Factory/LFO Presets/Basic/b.ats", "!Factory/Atlas", "!Factory/Vital"
    }, "after migrating the second layout");
    expect(LoadSave::getFavoritesJson().count(
               data.getChildFile("User/Banks/JayLadder/a.atp").getFullPathName().toStdString()) == 1,
           "the favourite follows its preset");
    expect(lib::migrationLogFile(data).loadFileAsString().contains("Moved User/Serum/Wavetables to User/Wavetables/Serum"),
           "the log appends to the earlier one");
    expect(lib::bankNames(lib::Library::kUser, data) == StringArray { "IDK", "JayLadder" },
           "the packs are no banks: " + lib::bankNames(lib::Library::kUser, data).joinIntoString(", ").toStdString());
  }

  void testStrayBankIsAdopted() {
    LayoutTestLibrary library;
    if (!library.isolated())
      return;
    const File data = library.folder;
    lib::migrate(data);
    expect(writeLayoutPreset(data.getChildFile("Dropped/Presets/d.atp"), "Someone") &&
               writeLayoutFile(data.getChildFile("Dropped/Wavetables/w.att")) &&
               writeLayoutFile(data.getChildFile("Loose/readme.txt")),
           "a bank dropped at the top of the Atlas folder, and a folder that is no bank");
    json favourites = json::object();
    favourites[data.getChildFile("Dropped/Presets/d.atp").getFullPathName().toStdString()] = 1;
    LoadSave::saveJsonToFavorites(favourites);

    const auto moves = lib::adoptStrayBanks(data);
    expectLayout(data, { "Third Party/Banks/Dropped/d.atp", "Third Party/Wavetables/Dropped/w.att", "!Dropped",
                         "Loose/readme.txt" },
                 "after filing the dropped bank");
    expect(moves.size() == 2, "two folders moved");
    expect(LoadSave::getFavoritesJson().count(
               data.getChildFile("Third Party/Banks/Dropped/d.atp").getFullPathName().toStdString()) == 1,
           "its favourite followed it");
    expect(lib::adoptStrayBanks(data).empty(), "a second look finds nothing");
    expect(lib::movedPath("/a/b/c.atp", { { File("/a/b"), File("/x/y") }, { File("/a"), File("/z") } }) == "/x/y/c.atp" &&
               lib::movedPath("/a/other.atp", { { File("/a/b"), File("/x/y") } }) == "/a/other.atp",
           "a path follows the longest moved folder it sits in");
  }

  void testBankArchivesRoundTrip() {
    LayoutTestLibrary library;
    if (!library.isolated())
      return;
    const File data = library.folder;
    lib::migrate(data);
    const File bank = data.getChildFile("User/Banks/Alpha");
    const File piano = data.getChildFile("User/Multisamples/Piano");
    json instrument_settings = json::object();
    instrument_settings["osc_1_multisample"] = { { "format", "sfz" }, { "name", "piano" },
                                                 { "path", piano.getChildFile("piano.sfz").getFullPathName().toStdString() },
                                                 { "hash", "0123456789abcdef0123456789abcdef" }, { "entry", "piano.sfz" },
                                                 { "library_instrument", "Piano/piano.sfz" } };
    lib::InstrumentManifest manifest;
    manifest.hash = "0123456789abcdef0123456789abcdef";
    manifest.entry = "piano.sfz";
    manifest.format = "sfz";
    manifest.name = "piano";
    manifest.valid = true;
    expect(writeLayoutPreset(bank.getChildFile("Bass/a.atp"), "Alessio") &&
               writeLayoutPreset(bank.getChildFile("b.atp"), "Alessio", "Keys", instrument_settings) &&
               writeLayoutFile(bank.getChildFile(".DS_Store")) &&
               writeLayoutFile(data.getChildFile("User/Wavetables/Alpha/w.att")) &&
               writeLayoutFile(data.getChildFile("User/Samples/Other/x.wav")) &&
               writeLayoutFile(piano.getChildFile("piano.sfz"), "<region> sample=Samples/c4.wav") &&
               writeLayoutFile(piano.getChildFile("Samples/c4.wav"), "RIFF") &&
               lib::writeInstrumentManifest(piano, manifest),
           "a bank, a pack of its name, another pack and the instrument one of its presets plays");
    expect(lib::findInstrumentByHash(manifest.hash.toStdString(), data.getChildFile("User")) ==
               piano.getChildFile("piano.sfz") &&
               lib::instrumentFolderOf(piano.getChildFile("Samples/c4.wav"), data) == piano,
           "the instrument is found by its hash, and its folder from any of its files");

    const File archive = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("Alpha", ".atb", false);
    const lib::ArchiveResult written = lib::writeBankArchive(bank, archive);
    expect(written.ok() && written.added == 6, "the bank's presets, its pack and the instrument are archived: " +
                                                   std::to_string(written.added) + " " + written.error.toStdString());
    {
      FileInputStream input(archive);
      ZipFile zip(input);
      expect(lib::archiveBankName(zip) == "Alpha", "the archive's bank folder is its name");
      StringArray names;
      for (int i = 0; i < zip.getNumEntries(); ++i)
        names.add(zip.getEntry(i)->filename);
      expect(names.contains("Alpha/Presets/Bass/a.atp") && names.contains("Alpha/Wavetables/w.att") &&
                 names.contains("Alpha/Multisamples/Piano/Samples/c4.wav") &&
                 names.contains("Alpha/Multisamples/Piano/atlas-multisample.json") &&
                 !names.contains("Alpha/Samples/x.wav"),
             "laid out by kind, with the pack of its name and no other: " + names.joinIntoString(", ").toStdString());
    }

    const File other = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("AtlasLayoutOther", "", false);
    other.createDirectory();
    lib::ImportResult imported = lib::importBankArchive(archive, lib::Library::kThirdParty, other, "Fallback");
    expect(imported.ok() && imported.files == 6 && imported.replaced == 0 && imported.name == "Alpha" &&
               imported.bank == other.getChildFile("Third Party/Banks/Alpha") &&
               imported.packs == StringArray { "Wavetables", "Multisamples" },
           "the archive files its presets as a Third Party bank and the rest as its packs: " +
               imported.error.toStdString() + " " + imported.packs.joinIntoString(", ").toStdString());
    expectLayout(other, { "Third Party/Banks/Alpha/Bass/a.atp", "Third Party/Banks/Alpha/b.atp",
                          "Third Party/Wavetables/Alpha/w.att", "Third Party/Multisamples/Piano/Samples/c4.wav" },
                 "after the import");
    imported = lib::importBankArchive(archive, lib::Library::kThirdParty, other, "Fallback");
    expect(imported.ok() && imported.files == 6 && imported.replaced == 3 &&
               !other.getChildFile("Third Party/Multisamples/Piano 2").exists(),
           "importing again replaces its files and keeps the instrument once: " + std::to_string(imported.replaced));
    Array<File> left;
    other.findChildFiles(left, File::findDirectories, false);
    expect(left.size() == 1, "no staging folder is left behind");

    // An archive with the kind folders at its root, as some hand-made banks are.
    const File rootless = archive.getSiblingFile("Rootless.vitalbank");
    {
      ZipFile::Builder builder;
      builder.addFile(bank.getChildFile("b.atp"), 9, "Presets/p.atp");
      builder.addFile(bank.getChildFile("b.atp"), 9, "__MACOSX/Presets/._p.atp");
      builder.addFile(bank.getChildFile("b.atp"), 9, "Presets/.DS_Store");
      builder.addFile(data.getChildFile("User/Wavetables/Alpha/w.att"), 9, "Wavetables/w.att");
      FileOutputStream output(rootless);
      expect(output.openedOk() && builder.writeToStream(output, nullptr), "a rootless archive is written");
    }
    imported = lib::importBankArchive(rootless, lib::Library::kCommunity, other, "Hamo Speed Garage Bank");
    expect(imported.ok() && imported.files == 2 &&
               imported.bank == other.getChildFile("Community/Banks/Hamo Speed Garage Bank"),
           "it is named after the fallback: " + imported.error.toStdString());
    expectLayout(other, { "Community/Banks/Hamo Speed Garage Bank/p.atp", "Community/Wavetables/Hamo Speed Garage Bank/w.att",
                          "!Community/Banks/Hamo Speed Garage Bank/__MACOSX", "!Community/Banks/Hamo Speed Garage Bank/.DS_Store" },
                 "after the rootless import");

    // An archive that tries to leave its folder.
    const File evil = archive.getSiblingFile("Evil.atb");
    {
      ZipFile::Builder builder;
      builder.addFile(bank.getChildFile("b.atp"), 9, "Evil/Presets/ok.atp");
      builder.addFile(bank.getChildFile("b.atp"), 9, "Evil/../../escaped.atp");
      FileOutputStream output(evil);
      expect(output.openedOk() && builder.writeToStream(output, nullptr), "an escaping archive is written");
    }
    imported = lib::importBankArchive(evil, lib::Library::kThirdParty, other, "Evil");
    expect(!other.getParentDirectory().getChildFile("escaped.atp").exists() && !other.getChildFile("escaped.atp").exists(),
           "nothing lands outside the library");

    // A pack exports and imports as a pack.
    const File tables_archive = archive.getSiblingFile("Alpha Tables.atb");
    const lib::ArchiveResult tables = lib::writeBankArchive(data.getChildFile("User/Wavetables/Alpha"), tables_archive);
    expect(tables.ok() && tables.added == 1, "a pack is archived");
    imported = lib::importBankArchive(tables_archive, lib::Library::kThirdParty, other, "Fallback");
    expect(imported.ok() && imported.bank == File() && imported.packs == StringArray { "Wavetables" } &&
               other.getChildFile("Third Party/Wavetables/Alpha Tables/w.att").existsAsFile(),
           "and comes back as a pack, with no bank: " + imported.error.toStdString());

    // Any folder exports with its files sorted into kinds.
    const File loose = other.getChildFile("Loose");
    expect(writeLayoutPreset(loose.getChildFile("x.atp"), "Alessio") && writeLayoutFile(loose.getChildFile("Wavetables/w.att")) &&
               writeLayoutFile(loose.getChildFile("notes.txt")),
           "a folder that is no bank");
    const File loose_archive = archive.getSiblingFile("Loose.atb");
    const lib::ArchiveResult loose_written = lib::writeBankArchive(loose, loose_archive);
    expect(loose_written.ok() && loose_written.added == 2, "its two Atlas files are archived");
    {
      FileInputStream input(loose_archive);
      ZipFile zip(input);
      StringArray names;
      for (int i = 0; i < zip.getNumEntries(); ++i)
        names.add(zip.getEntry(i)->filename);
      expect(names.contains("Loose/Presets/x.atp") && names.contains("Loose/Wavetables/w.att"),
             "sorted into kind folders: " + names.joinIntoString(", ").toStdString());
    }
    const lib::ArchiveResult empty = lib::writeBankArchive(other.getChildFile("Nothing"), archive.getSiblingFile("Nothing.atb"));
    expect(!empty.ok(), "a missing folder is refused");

    for (const auto& made : { archive, rootless, evil, tables_archive, loose_archive })
      made.deleteFile();
    other.deleteRecursively();
  }
  // The Browse menus' lists: FX presets of one effect from every library,
  // loaded into an effect slot's own parameters and stepped with Previous
  // and Next; LFO presets the same way; FX bus presets apart; and the FX and
  // LFO presets groups a screen reader enters.
  void testPresetBrowsersFilterAndLoad() {
    LayoutTestLibrary library;
    if (!library.isolated())
      return;
    const File data = library.folder;
    lib::migrate(data);
    auto writeFx = [&](const char* relative, const char* effect, std::initializer_list<std::pair<const char*, float>> values) {
      json preset;
      preset["type"] = "Atlas FX Preset";
      preset["version"] = 1;
      preset["effect"] = effect;
      preset["name"] = File(relative).getFileNameWithoutExtension().toStdString();
      json parameters = json::object();
      for (const auto& value : values)
        parameters[value.first] = value.second;
      preset["parameters"] = parameters;
      expect(writeLayoutFile(data.getChildFile(relative), String(preset.dump())), std::string("wrote ") + relative);
    };
    writeFx("Factory/FX Presets/Analogue/Wide.atf", "analoguechorus", { { "analoguechorus_rate", 0.25f }, { "analoguechorus_mix", 0.4f } });
    writeFx("User/FX Presets/Mine.atf", "analoguechorus", { { "analoguechorus_rate", 0.75f }, { "analoguechorus_mix", 0.6f } });
    writeFx("User/FX Presets/Echo.atf", "delay", { { "delay_feedback", 0.3f } });
    expect(writeLayoutFile(data.getChildFile("User/FX Presets/Bus.atfx"), "{}"), "an FX bus preset");
    LineGenerator saw(16);
    saw.initSawUp();
    LineGenerator triangle(16);
    triangle.initTriangle();
    const File saw_file = data.getChildFile("Factory/LFO Presets/Basic/Saw.ats");
    const File triangle_file = data.getChildFile("User/LFO Presets/Triangle.ats");
    expect(writeLayoutFile(saw_file, String(saw.stateToJson().dump())) &&
               writeLayoutFile(triangle_file, String(triangle.stateToJson().dump())),
           "two LFO presets in two libraries");

    SynthPlugin synth;
    synth.parameterChanged("fx_slot_1_type", vital::constants::kAnalogueChorus + 1);
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    pumpEditorEvents();

    const auto chorus = editor.effectPresetFiles("analoguechorus");
    expect(chorus.size() == 2 && chorus[0] == data.getChildFile("Factory/FX Presets/Analogue/Wide.atf") &&
               chorus[1] == data.getChildFile("User/FX Presets/Mine.atf"),
           "the chorus presets of every library, Factory first: " + std::to_string(chorus.size()));
    expect(editor.effectPresetFiles("delay").size() == 1 && editor.effectPresetFiles("reverb").empty(),
           "each effect lists only its own presets");
    expect(editor.presetFiles(SynthEditor::PresetFileList::kFxBusPresets).size() == 1 &&
               editor.presetFiles(SynthEditor::PresetFileList::kLfoPresets).size() == 2,
           "FX bus presets and LFO presets are lists of their own");

    const String chain = "Chain and routing";
    auto rate = [&] {
      const auto found = synth.getControls().find("fx_slot_1_analoguechorus_rate");
      return found != synth.getControls().end() && found->second != nullptr ? found->second->value() : -1.0f;
    };
    expect(editor.loadEffectPresetIntoSlot(chain, 0, chorus[1]) && std::abs(rate() - 0.75f) < 1.0e-4f,
           "a preset loads into the slot's own parameters: " + std::to_string(rate()));
    editor.loadShiftedEffectPreset(chain, 0, 1);
    expect(std::abs(rate() - 0.25f) < 1.0e-4f, "Next wraps to the first preset: " + std::to_string(rate()));
    editor.loadShiftedEffectPreset(chain, 0, -1);
    expect(std::abs(rate() - 0.75f) < 1.0e-4f, "Previous goes back: " + std::to_string(rate()));

    LineGenerator* lfo = synth.getLfoSource(0);
    editor.loadShiftedLfoPreset(0, 1);
    expect(lfo != nullptr && File(String(lfo->getLastBrowsedFile())) == saw_file,
           "Next LFO preset starts with the first in the list");
    editor.loadShiftedLfoPreset(0, 1);
    expect(lfo != nullptr && File(String(lfo->getLastBrowsedFile())) == triangle_file, "and steps to the next");

    auto groupTitles = [&](const String& title) {
      StringArray titles;
      if (auto* group = titled(editor, title)) {
        if (auto* handler = group->getAccessibilityHandler()) {
          expect(handler->getRole() == AccessibilityRole::group, (title + " is a group a screen reader enters").toStdString());
          for (auto* child : handler->getChildren())
            titles.add(child->getTitle());
        }
      }
      return titles;
    };
    expect(groupTitles("FX presets") == StringArray { "Browse FX presets", "Previous FX preset", "Next FX preset",
                                                      "Import FX preset", "Save FX preset" },
           "the slot's FX presets group: " + groupTitles("FX presets").joinIntoString(", ").toStdString());
    expect(groupTitles("LFO presets") == StringArray { "Browse LFO presets", "Previous LFO preset", "Next LFO preset",
                                                       "Import LFO preset", "Save LFO preset" },
           "an LFO's presets group: " + groupTitles("LFO presets").joinIntoString(", ").toStdString());
  }
}

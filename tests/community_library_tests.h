#pragma once

// The community library: the preset browser's Community collection lists the
// presets shared on atlas.blindproducers.org and Return downloads one, checks
// it against the library's checksum and loads it. A saved page and a saved
// preset file stand in for the library, so none of this needs a network.
// Included after editor_accessibility_tests.h, editor_speech_tests.h and
// undo_tests.h, whose helpers it uses.

#include "community_library.h"
#include "credential_store.h"
#include "library_layout.h"

namespace {
  // Two patches as /api/v1/presets/ describes them: one complete, one with
  // every optional field missing and names that try to leave the folder.
  const char* const kCommunityPage = R"json({
    "count": 2, "page": 1, "pages": 1, "page_size": 200, "next": null, "previous": null,
    "results": [
      {"id": 7, "type": "preset", "kind": "patch", "kind_label": "Patch", "name": "Glow Lead",
       "style": "lead", "style_label": "Lead", "tags": ["glow", "test"], "description": "A soft lead.",
       "author": "Jay", "file_author": "Jay", "synth_version": "2.0.0",
       "uploader": {"username": "jay", "display_name": "Jay Ladder", "url": "https://atlas.blindproducers.org/users/jay/"},
       "file": {"url": "https://atlas.blindproducers.org/presets/7/download/?v=abc", "name": "Glow Lead.atp",
                "extension": "atp", "size": 1234, "sha256": "ABCDEF"},
       "preview": {"url": "https://atlas.blindproducers.org/presets/7/preview/?v=abc", "name": "Glow Lead preview.mp3",
                   "content_type": "audio/mpeg", "size": 4321, "duration": 65.4},
       "downloads": 3, "created_at": "2026-10-19T03:03:42.102Z",
       "updated_at": "2026-10-19T04:05:06.007Z",
       "url": "https://atlas.blindproducers.org/presets/7/", "api_url": "https://atlas.blindproducers.org/api/v1/presets/7/"},
      {"id": 8, "type": "preset", "kind": "patch", "kind_label": "Patch", "name": "../Sneaky: Bass?",
       "style": null, "style_label": null, "tags": [], "description": "",
       "author": "", "file_author": "", "synth_version": "2.0.0",
       "uploader": {"username": "../root", "display_name": "", "url": ""},
       "file": {"url": "https://elsewhere.example/evil.atp", "name": "", "extension": ".atp", "size": 0, "sha256": ""},
       "preview": null, "downloads": 0, "created_at": "2026-10-19T03:03:42.102Z", "updated_at": "",
       "url": "", "api_url": ""}
    ],
    "filters": {"sort": "newest", "kind": "patch"}
  })json";

  void testSha256MatchesTheStandardVectors() {
    using atlas::community::sha256Hex;
    expect(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "SHA-256 of nothing");
    expect(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA-256 of abc");
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect(sha256Hex(two_blocks.data(), two_blocks.size()) ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "SHA-256 across a block boundary");
    MemoryBlock million(1000000);
    million.fillWith('a');
    MemoryInputStream stream(million, false);
    expect(sha256Hex(stream) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
           "SHA-256 of a million a's read as a stream");
  }

  void testPresetsPageParses() {
    std::vector<atlas::community::RemotePreset> presets;
    String next;
    const String error = atlas::community::parsePresetsPage(kCommunityPage, presets, next);
    expect(error.isEmpty(), "the saved page parses: " + error.toStdString());
    expect(next.isEmpty(), "the only page names no next page");
    expect(presets.size() == 2, "both presets are read");
    if (presets.size() != 2)
      return;

    const auto& glow = presets[0];
    expect(glow.id == 7 && glow.name == "Glow Lead" && glow.kind == "patch", "id, name and kind");
    expect(glow.style == "lead" && glow.style_label == "Lead", "the style and its label");
    expect(glow.tags.size() == 2 && glow.tags[0] == "glow" && glow.tags[1] == "test", "the tags");
    expect(glow.author == "Jay" && glow.uploader == "jay" && glow.uploader_name == "Jay Ladder",
           "the author and the uploader");
    expect(glow.download_url == "https://atlas.blindproducers.org/presets/7/download/?v=abc" &&
               glow.file_name == "Glow Lead.atp" && glow.extension == "atp" && glow.file_size == 1234 &&
               glow.sha256 == "abcdef",
           "the file's url, name, extension, size and lower-case checksum");
    expect(glow.downloads == 3 && glow.description == "A soft lead." &&
               glow.page_url == "https://atlas.blindproducers.org/presets/7/",
           "downloads, description and page");
    expect(glow.updated_ms > 0 && glow.updated_ms == Time::fromISO8601("2026-10-19T04:05:06.007Z").toMilliseconds(),
           "the time of the last change");
    expect(glow.preview.exists() && glow.preview.url == "https://atlas.blindproducers.org/presets/7/preview/?v=abc" &&
               glow.preview.file_name == "Glow Lead preview.mp3" && glow.preview.content_type == "audio/mpeg" &&
               glow.preview.size == 4321 && std::abs(glow.preview.duration_seconds - 65.4) < 1.0e-9,
           "the preview's url, name, type, size and length");

    const auto& sneaky = presets[1];
    expect(sneaky.style_label.isEmpty() && sneaky.tags.isEmpty() && sneaky.description.isEmpty(),
           "missing fields stay empty");
    expect(!sneaky.preview.exists() && sneaky.preview.duration_seconds == 0.0, "a null preview is none");
    expect(sneaky.uploader_name == "../root", "the display name falls back to the account name");
    expect(sneaky.extension == "atp", "a dotted extension loses its dot");

    std::vector<atlas::community::RemotePreset> none;
    expect(atlas::community::parsePresetsPage("<html>", none, next).isNotEmpty(), "an HTML page is refused");
    expect(atlas::community::parsePresetsPage(R"({"error": "Slow down"})", none, next).contains("Slow down"),
           "the library's own error is passed on");
    expect(none.empty(), "nothing is read from either");
  }

  void testDownloadsLandInTheCommunityFolder() {
    std::vector<atlas::community::RemotePreset> presets;
    String next;
    atlas::community::parsePresetsPage(kCommunityPage, presets, next);
    expect(presets.size() == 2, "the saved page parses");
    if (presets.size() != 2)
      return;
    const File data = File::getSpecialLocation(File::tempDirectory).getChildFile("AtlasCommunityData");
    const File community = atlas::community::communityDirectory(data);
    expect(community == data.getChildFile("Community"),
           "downloads live in the Community library under the data folder");
    const File glow = atlas::community::localFile(presets[0], data);
    expect(glow == community.getChildFile("Banks").getChildFile("jay").getChildFile("Lead").getChildFile("Glow Lead.atp"),
           "a preset lands in Banks under its uploader, the bank, and its style: " + glow.getFullPathName().toStdString());
    const File sneaky = atlas::community::localFile(presets[1], data);
    expect(sneaky.isAChildOf(community) &&
               sneaky.getParentDirectory().getParentDirectory().getParentDirectory().getParentDirectory() == community,
           "dots and slashes in the names stay inside the folder: " + sneaky.getFullPathName().toStdString());
    expect(sneaky.getParentDirectory().getFileName() == "Other" &&
               sneaky.getParentDirectory().getParentDirectory().getFileName() == "root" &&
               sneaky.getParentDirectory().getParentDirectory().getParentDirectory().getFileName() == "Banks",
           "no style files under Other, and the uploader's folder is not hidden: " +
               sneaky.getFullPathName().toStdString());
    expect(sneaky.hasFileExtension("atp") && sneaky.getFileName() == "Sneaky Bass.atp",
           "the file keeps a legal name with the preset extension: " + sneaky.getFileName().toStdString());
    expect(atlas::community::previewDirectory(data) == data.getChildFile("Community").getChildFile("Previews"),
           "previews are kept in Community/Previews under the data folder");
    const File glow_preview = atlas::community::localPreviewFile(presets[0], data);
    expect(glow_preview == atlas::community::previewDirectory(data).getChildFile("Presets").getChildFile("jay")
                               .getChildFile("Glow Lead preview.mp3"),
           "a preview lands under Presets and its uploader with the library's file name: " +
               glow_preview.getFullPathName().toStdString());
    atlas::community::RemotePreset unnamed = presets[0];
    unnamed.preview.file_name.clear();
    unnamed.preview.content_type = "audio/ogg";
    expect(atlas::community::localPreviewFile(unnamed, data).getFileName() == "Glow Lead preview.ogg",
           "a preview without a name is named after its preset with the type's extension: " +
               atlas::community::localPreviewFile(unnamed, data).getFileName().toStdString());
    using atlas::community::durationText;
    expect(durationText(65.4) == "1 minute 5 seconds" && durationText(12.0) == "12 seconds" &&
               durationText(120.0) == "2 minutes" && durationText(1.0) == "1 second" && durationText(0.2) == "under a second" &&
               durationText(0.0).isEmpty(),
           "lengths are said in minutes and seconds: " + durationText(65.4).toStdString());

    expect(atlas::community::isLibraryUrl("https://atlas.blindproducers.org/presets/7/download/", "https://atlas.blindproducers.org") &&
               !atlas::community::isLibraryUrl("https://elsewhere.example/evil.atp", "https://atlas.blindproducers.org") &&
               !atlas::community::isLibraryUrl("file:///etc/passwd", "https://atlas.blindproducers.org"),
           "only the library's own addresses are followed");
    expect(atlas::community::presetsPageUrl("https://atlas.blindproducers.org/", 2) ==
               "https://atlas.blindproducers.org/api/v1/presets/?kind=patch&page_size=200&page=2",
           "a page of patches is asked for in full pages");

#if JUCE_WINDOWS
    // Windows has no file or folder called CON, AUX, NUL, COM1 and so on, with
    // any extension, and drops a name's trailing dots and spaces.
    atlas::community::RemotePreset device = presets[0];
    device.uploader = "aux";
    device.style_label = "Lead.";
    device.file_name = "nul.atp";
    const File device_file = atlas::community::localFile(device, data);
    const File banks = community.getChildFile("Banks");
    expect(device_file == banks.getChildFile("aux_").getChildFile("Lead").getChildFile("nul_.atp"),
           "device names get an underscore and a trailing dot goes: " + device_file.getFullPathName().toStdString());
    device.uploader = "COM1 sounds";
    device.style_label = "Lead";
    device.file_name = "Lpt9.Bass.atp";
    expect(atlas::community::localFile(device, data) ==
               banks.getChildFile("COM1 sounds").getChildFile("Lead").getChildFile("Lpt9_.Bass.atp"),
           "only a name that is a device name changes: " +
               atlas::community::localFile(device, data).getFullPathName().toStdString());
    device.file_name = String("Two") + String::charToString('\t') + "Lines.atp";
    expect(atlas::community::localFile(device, data).getFileName() == "Two Lines.atp",
           "control characters become spaces: " + atlas::community::localFile(device, data).getFileName().toStdString());
    // The Share form names its temporary file the same way.
    expect(atlas::community::systemFileName("Aux") == "Aux_" && atlas::community::systemFileName("Aux Bass") == "Aux Bass" &&
               atlas::community::systemFileName("Bass. ") == "Bass",
           "a patch named Aux is shared from Aux_");
#endif
  }

  // One shared preset as the library's page would list it, pointing at a saved file.
  json communityPresetJson(int id, const String& name, const File& file, const String& sha256, const String& style,
                           const String& style_label, const StringArray& tags) {
    json entry;
    entry["id"] = id;
    entry["kind"] = "patch";
    entry["name"] = name.toStdString();
    entry["style"] = style.toStdString();
    entry["style_label"] = style_label.toStdString();
    entry["tags"] = json::array();
    for (const auto& tag : tags)
      entry["tags"].push_back(tag.toStdString());
    entry["description"] = "Shared for the test";
    entry["author"] = "Tester";
    entry["uploader"] = { { "username", "tester" }, { "display_name", "Test Account" }, { "url", "" } };
    entry["file"] = { { "url", (URL(file).toString(false) + "?v=1").toStdString() },
                      { "name", file.getFileName().toStdString() },
                      { "extension", "atp" },
                      { "size", file.getSize() },
                      { "sha256", sha256.toStdString() } };
    entry["downloads"] = 1;
    entry["updated_at"] = "2026-10-19T04:05:06.007Z";
    entry["url"] = "";
    return entry;
  }

  String sha256OfFile(const File& file) {
    MemoryBlock bytes;
    file.loadFileAsData(bytes);
    return atlas::community::sha256Hex(bytes.getData(), bytes.getSize());
  }

  // The networking against a saved page and file: the list is fetched, a
  // download is kept when it checks out and refused when it does not.
  // A download carrying its instruments is rewritten once they are unpacked:
  // the record of its checksum still says it is the library's copy, so
  // Return loads it rather than fetching it again.
  void testDownloadRecords() {
    const File data = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("AtlasDownloadRecords", "");
    const File preset = atlas::community::communityDirectory(data).getChildFile("Banks").getChildFile("jay")
                            .getChildFile("Keys").getChildFile("Piano.atp");
    preset.getParentDirectory().createDirectory();
    const String container(String::repeatedString("x", 4000));
    preset.replaceWithText(container);
    const String sha = atlas::community::sha256Hex(container.toRawUTF8(), container.getNumBytesAsUTF8());
    const int64 size = preset.getSize();
    expect(atlas::community::isDownloadCurrent(preset, sha, size, data),
           "a download from before records is current while its size matches");
    preset.replaceWithText("{}");
    expect(!atlas::community::isDownloadCurrent(preset, sha, size, data),
           "and fetched again once its size differs, with no record to say otherwise");
    expect(!atlas::community::isDownloadRecorded(preset, sha, data), "a file with no record is not recorded");
    atlas::community::recordDownload(preset, sha, data);
    expect(atlas::community::isDownloadRecorded(preset, sha.toUpperCase(), data) &&
               !atlas::community::isDownloadRecorded(preset, String::repeatedString("0", 64), data),
           "the record names the library's copy it was");
    expect(atlas::community::communityDirectory(data).getChildFile("Downloads.json").existsAsFile(),
           "the record is kept in the Community library");
    expect(atlas::community::isDownloadCurrent(preset, sha.toUpperCase(), size, data),
           "a recorded download stays current after its instruments are unpacked");
    expect(!atlas::community::isDownloadCurrent(preset, String::repeatedString("0", 64), size, data),
           "a new copy on the library is fetched again");
    preset.deleteFile();
    expect(!atlas::community::isDownloadCurrent(preset, sha, size, data), "a download that is gone is fetched");
    data.deleteRecursively();
  }

  void testSavedLibraryRoundTrip() {
    const File folder = File::getSpecialLocation(File::tempDirectory)
                            .getNonexistentChildFile("AtlasCommunityRoundTrip", "", false);
    folder.createDirectory();
    SynthPlugin synth;
    const File shared = folder.getChildFile("Shared.atp");
    expect(synth.saveCopyToFile(shared), "a preset is saved to stand in for the library's file");
    json page;
    page["count"] = 2;
    page["next"] = nullptr;
    page["results"] = json::array();
    page["results"].push_back(communityPresetJson(1, "Shared", shared, sha256OfFile(shared), "lead", "Lead", { "test" }));
    page["results"].push_back(communityPresetJson(2, "Broken", shared, String::repeatedString("0", 64), "bass", "Bass", {}));
    const File page_file = folder.getChildFile("presets.json");
    page_file.replaceWithText(String(page.dump()));

    std::vector<atlas::community::RemotePreset> presets;
    String error = atlas::community::fetchAllPresets(URL(page_file).toString(false), presets);
    expect(error.isEmpty() && presets.size() == 2, "the saved page is fetched and read: " + error.toStdString());
    if (presets.size() == 2) {
      const File target = folder.getChildFile("downloads").getChildFile("Shared.atp");
      error = atlas::community::downloadFile(URL(presets[0].download_url), target, presets[0].sha256,
                                             presets[0].file_size);
      expect(error.isEmpty() && target.existsAsFile() && target.getSize() == shared.getSize() &&
                 sha256OfFile(target) == sha256OfFile(shared),
             "a download that checks out is kept: " + error.toStdString());
      const File refused = folder.getChildFile("downloads").getChildFile("Broken.atp");
      error = atlas::community::downloadFile(URL(presets[1].download_url), refused, presets[1].sha256,
                                             presets[1].file_size);
      expect(error.contains("checksum") && !refused.existsAsFile() &&
                 !refused.getSiblingFile("Broken.atp.part").existsAsFile(),
             "a download with the wrong checksum is refused and leaves nothing behind: " + error.toStdString());
      error = atlas::community::downloadFile(URL(presets[0].download_url), target, presets[0].sha256,
                                             presets[0].file_size + 1);
      expect(error.contains("bytes") && target.existsAsFile(),
             "a download of the wrong size is refused and the file already there is kept: " + error.toStdString());
    }
    expect(atlas::community::fetchAllPresets(URL(folder.getChildFile("missing.json")).toString(false), presets).isNotEmpty(),
           "a page that cannot be read is an error");
    folder.deleteRecursively();
  }

  // The browser: the Community row lists the library's presets with the
  // uploader as the bank and the style as the category, the Category and Tags
  // lists and the search narrow them, Return downloads and loads one, a bad
  // download is refused, and the download then shows among all presets.
  void testCommunityCollectionListsAndDownloads() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };

    // The saved library: two preset files, one listed with the wrong checksum.
    const File saved = library.folder.getChildFile("SavedLibrary");
    saved.createDirectory();
    SynthPlugin synth;
    const File shared = saved.getChildFile("Shared Lead.atp");
    const File broken = saved.getChildFile("Broken Bass.atp");
    const bool made = synth.saveCopyToFile(shared) && synth.saveCopyToFile(broken);
    expect(made, "presets are saved to stand in for the library's files");
    json page;
    page["count"] = 2;
    page["next"] = nullptr;
    page["results"] = json::array();
    page["results"].push_back(communityPresetJson(1, "Shared Lead", shared, sha256OfFile(shared), "lead", "Lead",
                                                  { "glow", "test" }));
    page["results"].push_back(communityPresetJson(2, "Broken Bass", broken, String::repeatedString("0", 64), "bass",
                                                  "Bass", { "test" }));
    const File page_file = saved.getChildFile("presets.json");
    page_file.replaceWithText(String(page.dump()));
    atlas::community::setLibraryUrlForTesting(URL(page_file).toString(false));
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;

    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    auto* libraries = dynamic_cast<TableListBox*>(titled(editor, "Library"));
    auto* authors = dynamic_cast<TableListBox*>(titled(editor, "Bank"));
    auto* categories = dynamic_cast<TableListBox*>(titled(editor, "Category"));
    auto* tags = dynamic_cast<TableListBox*>(titled(editor, "Tags"));
    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* search = dynamic_cast<TextEditor*>(titled(editor, "Preset search"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    const bool browser = made && collection != nullptr && libraries != nullptr && authors != nullptr &&
                         categories != nullptr && tags != nullptr && results != nullptr && search != nullptr &&
                         autoload != nullptr;
    expect(browser, "the editor has its preset browser");
    if (!browser) {
      editor.removeFromDesktop();
      return;
    }
    const bool autoload_was_on = autoload->getToggleState();
    autoload->setToggleState(false, dontSendNotification);
    auto finish = [&] {
      autoload->setToggleState(autoload_was_on, dontSendNotification);
      editor.removeFromDesktop();
    };
    auto cell = [&](int row, int column) { return results->getTableListBoxModel()->getCellTooltip(row, column); };
    auto facetRows = [](TableListBox& table) {
      StringArray rows;
      for (int row = 0; row < table.getNumRows(); ++row)
        rows.add(table.getTableListBoxModel()->getCellTooltip(row, 1));
      return rows;
    };
    enum { kName = 1, kAuthor, kDownloaded, kTags, kCategory, kBank, kLibrary };

    // Into the Community presets collection: the list is fetched and shown.
    expect(collection->getNumRows() == 4 &&
               collection->getTableListBoxModel()->getCellTooltip(2, 1) == "Community presets" &&
               collection->getTableListBoxModel()->getCellTooltip(3, 1) == "Community banks",
           "the Collection list ends with Community presets and Community banks");
    collection->selectRow(2);
    pumpEditorEvents();
    pumpUntil([&] { return !editor.isCommunityLibraryBusy(); }, 10000);
    expect(editor.isCommunityLibraryLoaded(),
           "the community list arrives: " + editor.communityLibraryError().toStdString());
    expect(synth.getEditorMemory().preset_collection == "Community presets",
           "the collection is remembered for the instance");
    expect(said("Community presets loaded, 2 found"), "the list's arrival is said");
    expect(editor.presetRowCount() == 2, "both shared presets are listed, got " + std::to_string(editor.presetRowCount()));
    if (editor.presetRowCount() != 2) {
      finish();
      return;
    }
    const File expected = library.folder.getChildFile("Community").getChildFile("Banks").getChildFile("tester")
                              .getChildFile("Lead").getChildFile("Shared Lead.atp");
    // Sorted by name: Broken Bass, then Shared Lead.
    expect(editor.presetFileAtRow(1) == expected,
           "a row is where its download lands: " + editor.presetFileAtRow(1).getFullPathName().toStdString());
    expect(cell(1, kName) == "Shared Lead" && cell(1, kAuthor) == "tester" && cell(1, kDownloaded) == "Not downloaded" &&
               cell(1, kTags) == "glow, test" && cell(1, kCategory) == "Lead" && cell(1, kBank) == "tester" &&
               cell(1, kLibrary) == "Community",
           "the row shows the name, uploader as its author, download state, tags, style, uploader and library: " +
               (cell(1, kName) + " | " + cell(1, kAuthor) + " | " + cell(1, kDownloaded) + " | " + cell(1, kTags) + " | " +
                cell(1, kCategory) + " | " + cell(1, kBank) + " | " + cell(1, kLibrary)).toStdString());
    expect(results->getHeader().getColumnName(kDownloaded) == "Downloaded", "the Favourite column is named Downloaded");
    expect(results->getHeader().getColumnName(kCategory) == "Style" && !results->getHeader().isColumnVisible(kBank) &&
               !results->getHeader().isColumnVisible(kLibrary),
           "the Category column is Style, and the Bank and Library columns are left out");
    expect(!libraries->isVisible() && authors->getTitle() == "Author" && categories->getTitle() == "Style",
           "the lists are Collection, Author, Style and Tags, with no Library list: " +
               (authors->getTitle() + ", " + categories->getTitle()).toStdString());
    expect(facetRows(*authors) == StringArray { "All authors", "tester" } &&
               facetRows(*categories) == StringArray { "All styles", "Bass", "Lead" } &&
               facetRows(*tags) == StringArray { "All tags", "glow", "test" },
           "the Author, Style and Tags lists come from the library's uploaders, styles and tags: " +
               (facetRows(*authors).joinIntoString(", ") + " / " + facetRows(*categories).joinIntoString(", ") + " / " +
                facetRows(*tags).joinIntoString(", ")).toStdString());

    // The Category list, the Tags list and the search narrow the rows.
    categories->selectRow(2);
    pumpEditorEvents();
    expect(editor.presetRowCount() == 1 && editor.presetFileAtRow(0) == expected,
           "choosing the Lead category leaves the lead, got " + std::to_string(editor.presetRowCount()));
    categories->selectRow(0);
    pumpEditorEvents();
    tags->selectRow(1);
    pumpEditorEvents();
    expect(editor.presetRowCount() == 1 && editor.presetFileAtRow(0) == expected,
           "choosing the glow tag leaves the lead, got " + std::to_string(editor.presetRowCount()));
    tags->selectRow(0);
    pumpEditorEvents();
    search->setText("broken", true);
    pumpEditorEvents();
    pumpEditorEvents();
    expect(editor.presetRowCount() == 1 && editor.presetFileAtRow(0).getFileNameWithoutExtension() == "Broken Bass",
           "searching narrows the rows, got " + std::to_string(editor.presetRowCount()));
    search->setText("", true);
    pumpEditorEvents();
    pumpEditorEvents();
    expect(editor.presetRowCount() == 2, "clearing the search shows both again");
    if (editor.presetRowCount() != 2) {
      finish();
      return;
    }

    // Return downloads the lead into the Community library and loads it.
    results->selectRow(1);
    pumpEditorEvents();
    editor.ensureComponentVisible(results);
    results->grabKeyboardFocus();
    pumpEditorEvents();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the row is handled");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && synth.getActiveFile() == expected; }, 10000);
    expect(expected.existsAsFile() && sha256OfFile(expected) == sha256OfFile(shared),
           "the preset is downloaded into Community/Banks/tester/Lead");
    expect(synth.getActiveFile() == expected, "and loaded: " + synth.getActiveFile().getFullPathName().toStdString());
    // "Loaded preset" belongs to the optional Presets announcements, off by default.
    expect(said("Downloading Shared Lead") && said("Downloaded Shared Lead"),
           "the download is said when it starts and when it lands: " + spoken.messages.joinIntoString(" | ").toStdString());
    expect(cell(1, kDownloaded) == "Downloaded", "the row says Downloaded");
    expect(!expected.getSiblingFile("Shared Lead.atp.part").existsAsFile(), "no part file is left behind");

    // A download that does not match the library's checksum is refused.
    results->selectRow(0);
    pumpEditorEvents();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the broken row is handled");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && said("Unable to download"); }, 10000);
    const File refused = library.folder.getChildFile("Community").getChildFile("Banks").getChildFile("tester")
                             .getChildFile("Bass").getChildFile("Broken Bass.atp");
    expect(!refused.existsAsFile() && said("Unable to download Broken Bass") && said("checksum"),
           "the download is refused and the reason said: " + spoken.messages.joinIntoString(" | ").toStdString());
    expect(cell(0, kDownloaded) == "Not downloaded", "its row still says Not downloaded");

    // Back among all presets, the download is a preset of the Community library.
    collection->selectRow(0);
    pumpEditorEvents();
    expect(synth.getEditorMemory().preset_collection == "All presets", "the collection is All presets again");
    expect(editor.presetRowCount() == 1 && editor.presetFileAtRow(0) == expected,
           "the downloaded preset is among all presets, got " + std::to_string(editor.presetRowCount()));
    expect(facetRows(*libraries).contains("Community") && cell(0, kLibrary) == "Community" && cell(0, kBank) == "tester" &&
               cell(0, kCategory) == "Lead" && cell(0, kDownloaded) == "Not favourite",
           "and files under the Community library, its uploader and style: " +
               (facetRows(*libraries).joinIntoString(", ") + " | " + cell(0, kLibrary) + " | " + cell(0, kBank) + " | " +
                cell(0, kCategory) + " | " + cell(0, kDownloaded)).toStdString());
    expect(results->getHeader().getColumnName(kDownloaded) == "Favourite", "the column is named Favourite again");
    expect(libraries->isVisible() && authors->getTitle() == "Bank" && categories->getTitle() == "Category" &&
               results->getHeader().isColumnVisible(kBank) && results->getHeader().isColumnVisible(kLibrary) &&
               results->getHeader().getColumnName(kCategory) == "Category",
           "and the Library, Bank and Category lists and columns are back");
    finish();
  }

  // Two banks as /api/v1/banks/ describes them: one complete, with a contents
  // list the plugin leaves alone, and one with no genre, preview or tags.
  const char* const kCommunityBanksPage = R"json({
    "count": 2, "page": 1, "pages": 1, "page_size": 200, "next": null, "previous": null,
    "results": [
      {"id": 1, "type": "bank", "name": "Hamo Speed Garage Bank", "genre": {"name": "Speed Garage", "slug": "speed-garage"},
       "tags": ["dance", "speed garage"], "description": "Test bank",
       "uploader": {"username": "jayladdermusic", "display_name": "Jay Ladder", "url": "https://atlas.blindproducers.org/users/jayladdermusic/"},
       "file": {"url": "https://atlas.blindproducers.org/banks/1/download/?v=1792387344", "name": "Hamo Speed Garage Bank.atb",
                "extension": ".atb", "size": 50066794, "sha256": "5F981598D51D96C2CD98397EE40583AA42AB9A7ACF464BB7A0F7B641910FBB2F"},
       "preview": {"url": "https://atlas.blindproducers.org/banks/1/preview/?v=1792387344", "name": "Hamo Speed Garage Bank preview.wav",
                   "content_type": "audio/wav", "size": 3908628, "duration": null},
       "counts": {"preset": 62, "wavetable": 3, "lfo": 0, "fx": 0, "sample": 2}, "total_files": 67, "unpacked_bytes": 92330836,
       "downloads": 1, "created_at": "2026-10-19T04:22:29.397Z", "updated_at": "2026-10-19T05:22:24.312Z",
       "url": "https://atlas.blindproducers.org/banks/1/", "api_url": "https://atlas.blindproducers.org/api/v1/banks/1/",
       "contents": [{"path": "Hamo Speed Garage Bank/Presets/Bass Alloy.atp", "type": "preset", "name": "Bass Alloy", "size": 759307}]},
      {"id": 2, "type": "bank", "name": "Plain", "genre": null, "tags": [], "description": "",
       "uploader": {"username": "someone", "display_name": "", "url": ""},
       "file": {"url": "https://atlas.blindproducers.org/banks/2/download/?v=1", "name": "", "extension": "atb", "size": 10, "sha256": ""},
       "preview": null, "counts": {"preset": 1, "wavetable": 0, "lfo": 0, "fx": 0, "sample": 0}, "total_files": 1,
       "unpacked_bytes": 10, "downloads": 0, "created_at": "", "updated_at": "", "url": "", "api_url": ""}
    ],
    "filters": {"sort": "newest"}
  })json";

  void testBanksPageParses() {
    std::vector<atlas::community::RemoteBank> banks;
    String next;
    const String error = atlas::community::parseBanksPage(kCommunityBanksPage, banks, next);
    expect(error.isEmpty() && next.isEmpty() && banks.size() == 2,
           "the saved banks page parses, both banks read: " + error.toStdString());
    if (banks.size() != 2)
      return;
    const auto& hamo = banks[0];
    expect(hamo.id == 1 && hamo.name == "Hamo Speed Garage Bank" && hamo.genre == "Speed Garage" &&
               hamo.genre_slug == "speed-garage",
           "id, name and genre");
    expect(hamo.tags == StringArray { "dance", "speed garage" } && hamo.description == "Test bank", "tags and description");
    expect(hamo.uploader == "jayladdermusic" && hamo.uploader_name == "Jay Ladder", "the uploader");
    expect(hamo.download_url == "https://atlas.blindproducers.org/banks/1/download/?v=1792387344" &&
               hamo.file_name == "Hamo Speed Garage Bank.atb" && hamo.extension == "atb" && hamo.file_size == 50066794 &&
               hamo.sha256 == "5f981598d51d96c2cd98397ee40583aa42ab9a7acf464bb7a0f7b641910fbb2f",
           "the archive's url, name, extension, size and lower-case checksum");
    expect(hamo.preview.exists() && hamo.preview.file_name == "Hamo Speed Garage Bank preview.wav" &&
               hamo.preview.content_type == "audio/wav" && hamo.preview.size == 3908628 && hamo.preview.duration_seconds == 0.0,
           "the preview, whose length the library does not know");
    expect(hamo.presets == 62 && hamo.wavetables == 3 && hamo.samples == 2 && hamo.lfos == 0 && hamo.fx == 0 &&
               hamo.total_files == 67 && hamo.unpacked_bytes == 92330836 && hamo.downloads == 1,
           "the counts, the sizes and the downloads");
    expect(hamo.updated_ms == Time::fromISO8601("2026-10-19T05:22:24.312Z").toMilliseconds() &&
               hamo.page_url == "https://atlas.blindproducers.org/banks/1/",
           "the time of the last change and the page");
    expect(atlas::community::bankContentsText(hamo) == "62 presets, 3 wavetables and 2 samples",
           "the contents are said by kind: " + atlas::community::bankContentsText(hamo).toStdString());
    const auto& plain = banks[1];
    expect(plain.genre.isEmpty() && plain.tags.isEmpty() && !plain.preview.exists() && plain.uploader_name == "someone" &&
               plain.updated_ms == 0,
           "missing fields stay empty and the display name falls back to the account name");
    expect(atlas::community::bankContentsText(plain) == "1 preset", "one preset is singular");
    expect(atlas::community::bankContentsText(atlas::community::RemoteBank()) == "no Atlas files", "an empty bank says so");

    const File data = File::getSpecialLocation(File::tempDirectory).getChildFile("AtlasCommunityData");
    expect(atlas::community::banksDirectory(data) == data.getChildFile("Community").getChildFile("Archives"),
           "bank archives live in Community/Archives under the data folder");
    expect(atlas::community::localFile(hamo, data) ==
               atlas::community::banksDirectory(data).getChildFile("jayladdermusic").getChildFile("Hamo Speed Garage Bank.atb"),
           "an archive lands under its uploader: " + atlas::community::localFile(hamo, data).getFullPathName().toStdString());
    expect(atlas::community::localFile(plain, data).getFileName() == "Plain.atb",
           "an archive without a file name is named after the bank");
    expect(atlas::community::localPreviewFile(hamo, data) ==
               atlas::community::previewDirectory(data).getChildFile("Banks").getChildFile("jayladdermusic")
                   .getChildFile("Hamo Speed Garage Bank preview.wav"),
           "a bank's preview lands under Banks and its uploader");
    expect(atlas::community::banksPageUrl("https://atlas.blindproducers.org/", 3) ==
               "https://atlas.blindproducers.org/api/v1/banks/?page_size=200&page=3",
           "a page of banks is asked for in full pages");
    const File saved_page = data.getChildFile("saved").getChildFile("presets.json");
    expect(atlas::community::banksPageUrl(URL(saved_page).toString(false), 1) ==
               URL(saved_page.getSiblingFile("banks.json")).toString(false),
           "a saved library's banks are the banks.json beside its presets page");
  }

  // A bank archive as Export folder as bank writes it: the presets saved at
  // `preset_paths` under <bank name>/Presets/.
  bool writeTestBank(const File& archive, const String& bank_name, SynthPlugin& synth, const StringArray& preset_paths) {
    const File source = archive.getSiblingFile(bank_name + " source");
    source.deleteRecursively();
    ZipFile::Builder builder;
    for (const auto& path : preset_paths) {
      const File preset = source.getChildFile(path);
      if (!synth.saveCopyToFile(preset))
        return false;
      builder.addFile(preset, 9, bank_name + "/" + LoadSave::kPresetFolderName + "/" + path);
    }
    archive.deleteFile();
    FileOutputStream out(archive);
    return out.openedOk() && builder.writeToStream(out, nullptr);
  }

  // One shared bank as the library's page would list it, pointing at a saved archive.
  json communityBankJson(int id, const String& name, const File& archive, const String& sha256, const String& genre,
                         const StringArray& tags, int presets) {
    json entry;
    entry["id"] = id;
    entry["name"] = name.toStdString();
    entry["genre"] = { { "name", genre.toStdString() },
                       { "slug", genre.toLowerCase().replaceCharacter(' ', '-').toStdString() } };
    entry["tags"] = json::array();
    for (const auto& tag : tags)
      entry["tags"].push_back(tag.toStdString());
    entry["description"] = "A bank shared for the test";
    entry["uploader"] = { { "username", "tester" }, { "display_name", "Test Account" }, { "url", "" } };
    entry["file"] = { { "url", (URL(archive).toString(false) + "?v=1").toStdString() },
                      { "name", archive.getFileName().toStdString() },
                      { "extension", ".atb" },
                      { "size", archive.getSize() },
                      { "sha256", sha256.toStdString() } };
    entry["preview"] = nullptr;
    entry["counts"] = { { "preset", presets }, { "wavetable", 0 }, { "lfo", 0 }, { "fx", 0 }, { "sample", 0 } };
    entry["total_files"] = presets;
    entry["unpacked_bytes"] = 2048;
    entry["downloads"] = 4;
    entry["updated_at"] = "2026-10-19T04:05:06.007Z";
    entry["url"] = "";
    return entry;
  }

  // The browser: the Community banks row lists the library's banks with the
  // uploader as the bank and the genre as the category, Return downloads one,
  // checks it and unpacks it into the Atlas folder, after which its presets
  // show among all presets; a bank with the wrong checksum is refused.
  void testCommunityBanksListAndImport() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };
    auto heard = [&] { return spoken.messages.joinIntoString(" | ").toStdString(); };

    // The saved library: two bank archives, one listed with the wrong checksum.
    const File saved = library.folder.getChildFile("SavedLibrary");
    saved.createDirectory();
    SynthPlugin synth;
    const File good = saved.getChildFile("Test Bank.atb");
    const File broken = saved.getChildFile("Broken Bank.atb");
    const bool made = writeTestBank(good, "Test Bank", synth, { "Lead/Shared One.atp", "Shared Two.atp" }) &&
                      writeTestBank(broken, "Broken Bank", synth, { "Broken One.atp" });
    expect(made, "bank archives are written to stand in for the library's files");
    json banks;
    banks["count"] = 2;
    banks["next"] = nullptr;
    banks["results"] = json::array();
    banks["results"].push_back(communityBankJson(1, "Test Bank", good, sha256OfFile(good), "Speed Garage",
                                                 { "test", "garage" }, 2));
    banks["results"].push_back(communityBankJson(2, "Broken Bank", broken, String::repeatedString("0", 64), "Ambient",
                                                 {}, 1));
    saved.getChildFile("banks.json").replaceWithText(String(banks.dump()));
    saved.getChildFile("presets.json").replaceWithText(R"({"count": 0, "next": null, "results": []})");
    atlas::community::setLibraryUrlForTesting(URL(saved.getChildFile("presets.json")).toString(false));
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;

    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    auto* libraries = dynamic_cast<TableListBox*>(titled(editor, "Library"));
    auto* uploaders = dynamic_cast<TableListBox*>(titled(editor, "Bank"));
    auto* categories = dynamic_cast<TableListBox*>(titled(editor, "Category"));
    auto* tags = dynamic_cast<TableListBox*>(titled(editor, "Tags"));
    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* details = dynamic_cast<Label*>(titled(editor, "Selected preset details"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    const bool browser = made && collection != nullptr && libraries != nullptr && uploaders != nullptr &&
                         categories != nullptr && tags != nullptr && results != nullptr && details != nullptr &&
                         autoload != nullptr;
    expect(browser, "the editor has its preset browser");
    if (!browser) {
      editor.removeFromDesktop();
      return;
    }
    const bool autoload_was_on = autoload->getToggleState();
    autoload->setToggleState(true, dontSendNotification);   // a bank never autoloads, even with autoload on
    auto finish = [&] {
      autoload->setToggleState(autoload_was_on, dontSendNotification);
      editor.removeFromDesktop();
    };
    auto cell = [&](int row, int column) { return results->getTableListBoxModel()->getCellTooltip(row, column); };
    auto facetRows = [](TableListBox& table) {
      StringArray rows;
      for (int row = 0; row < table.getNumRows(); ++row)
        rows.add(table.getTableListBoxModel()->getCellTooltip(row, 1));
      return rows;
    };
    enum { kName = 1, kAuthor, kImported, kTags, kCategory, kBank, kLibrary };

    // Into the Community banks collection: the list is fetched and shown.
    expect(collection->getNumRows() == 4 && collection->getTableListBoxModel()->getCellTooltip(3, 1) == "Community banks",
           "the Collection list ends with Community banks");
    collection->selectRow(3);
    pumpEditorEvents();
    pumpUntil([&] { return !editor.isCommunityBankLibraryBusy(); }, 10000);
    expect(editor.isCommunityBankLibraryLoaded(),
           "the community bank list arrives: " + editor.communityBankLibraryError().toStdString());
    expect(synth.getEditorMemory().preset_collection == "Community banks", "the collection is remembered for the instance");
    expect(said("Community banks loaded, 2 found"), "the list's arrival is said: " + heard());
    expect(editor.presetRowCount() == 2, "both shared banks are listed, got " + std::to_string(editor.presetRowCount()));
    if (editor.presetRowCount() != 2) {
      finish();
      return;
    }
    // Sorted by name: Broken Bank, then Test Bank.
    const File archive = atlas::community::banksDirectory(library.folder).getChildFile("tester").getChildFile("Test Bank.atb");
    expect(editor.presetFileAtRow(1) == archive,
           "a row is where its archive lands: " + editor.presetFileAtRow(1).getFullPathName().toStdString());
    expect(cell(1, kName) == "Test Bank" && cell(1, kAuthor) == "tester" && cell(1, kImported) == "Not imported" &&
               cell(1, kTags) == "test, garage" && cell(1, kCategory) == "Speed Garage" && cell(1, kBank) == "tester" &&
               cell(1, kLibrary) == "Community",
           "the row shows the name, uploader, import state, tags, genre, uploader and library: " +
               (cell(1, kName) + " | " + cell(1, kAuthor) + " | " + cell(1, kImported) + " | " + cell(1, kTags) + " | " +
                cell(1, kCategory) + " | " + cell(1, kBank) + " | " + cell(1, kLibrary)).toStdString());
    expect(results->getHeader().getColumnName(kImported) == "Imported", "the Favourite column is named Imported");
    expect(results->getHeader().getColumnName(kCategory) == "Genre" && !results->getHeader().isColumnVisible(kBank) &&
               !results->getHeader().isColumnVisible(kLibrary),
           "the Category column is Genre, and the Bank and Library columns are left out");
    expect(!libraries->isVisible() && uploaders->getTitle() == "Author" && categories->getTitle() == "Genre",
           "the lists are Collection, Author, Genre and Tags, with no Library list");
    expect(facetRows(*uploaders) == StringArray { "All authors", "tester" } &&
               facetRows(*categories) == StringArray { "All genres", "Ambient", "Speed Garage" } &&
               facetRows(*tags) == StringArray { "All tags", "garage", "test" },
           "the Author, Genre and Tags lists come from the banks' uploaders, genres and tags: " +
               (facetRows(*uploaders).joinIntoString(", ") + " / " + facetRows(*categories).joinIntoString(", ") + " / " +
                facetRows(*tags).joinIntoString(", ")).toStdString());
    results->selectRow(1);
    pumpEditorEvents();
    const String before = details->getText();
    expect(before.contains("Test Bank. Not imported, Return downloads and imports it. Community bank shared by Test Account") &&
               before.contains("Genre: Speed Garage. Contents: 2 presets. Download: ") && before.contains("2.0 KB unpacked") &&
               before.contains("Tags: test, garage. Downloads: 4. No preview. Description: A bank shared for the test"),
           "the details say what the bank is and holds: " + before.toStdString());
    expect(synth.getActiveFile() == File(), "selecting a bank loads nothing, autoload or not");

    // Return downloads the archive, checks it and unpacks it into the Atlas folder.
    editor.ensureComponentVisible(results);
    results->grabKeyboardFocus();
    pumpEditorEvents();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the bank row is handled");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && said("Imported bank Test Bank"); }, 15000);
    const File unpacked = library.folder.getChildFile("Community").getChildFile("Banks").getChildFile("Test Bank");
    expect(unpacked.getChildFile("Lead").getChildFile("Shared One.atp").existsAsFile() &&
               unpacked.getChildFile("Shared Two.atp").existsAsFile(),
           "the bank's presets are filed into the Community library's Banks under its own name");
    expect(archive.existsAsFile() && sha256OfFile(archive) == sha256OfFile(good),
           "the archive is kept in Community/Archives/tester");
    expect(said("Downloading bank Test Bank from a saved copy of the library") && said("Imported bank Test Bank, 2 files"),
           "the download and the import are said: " + heard());
    expect(cell(1, kImported) == "Imported" && details->getText().contains("Test Bank. Imported. Community bank shared by"),
           "the row and the details say Imported: " + details->getText().toStdString());
    expect(synth.getActiveFile() == File(), "importing a bank loads nothing");

    // Return again neither fetches nor unpacks it: its packs go in once.
    const Time imported_time = unpacked.getChildFile("Shared Two.atp").getLastModificationTime();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the imported bank row is handled");
    pumpEditorEvents();
    expect(said("Bank Test Bank is already imported, as the Community bank Test Bank") && !said("Downloading") &&
               !said("Importing") && !editor.isCommunityDownloadBusy() &&
               unpacked.getChildFile("Shared Two.atp").getLastModificationTime() == imported_time,
           "an imported bank is not imported again: " + heard());

    // A bank whose folder is gone is unpacked again from the archive, not fetched.
    unpacked.deleteRecursively();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the bank whose folder is gone is handled");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && said("Imported bank Test Bank"); }, 15000);
    expect(said("Importing bank Test Bank again") && !said("Downloading") &&
               unpacked.getChildFile("Shared Two.atp").existsAsFile(),
           "a bank whose folder is gone is imported again from its archive: " + heard());

    // A bank whose archive does not match the library's checksum is refused.
    results->selectRow(0);
    pumpEditorEvents();
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the broken bank row is handled");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && said("Unable to import bank"); }, 15000);
    expect(!library.folder.getChildFile("Broken Bank").exists() && said("Unable to import bank Broken Bank") &&
               said("checksum") && cell(0, kImported) == "Not imported",
           "the bank is refused, the reason said, and nothing unpacked: " + heard());

    // Back among all presets, the bank's presets are there under its name.
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);
    collection->selectRow(0);
    pumpEditorEvents();
    expect(editor.presetRowCount() == 2 && cell(0, kBank) == "Test Bank" && cell(1, kBank) == "Test Bank" &&
               facetRows(*uploaders).contains("Test Bank"),
           "the unpacked presets are among all presets as the bank Test Bank, got " +
               std::to_string(editor.presetRowCount()) + ": " + (cell(0, kBank) + " | " + cell(1, kBank)).toStdString());
    expect(results->getHeader().getColumnName(kImported) == "Favourite", "the column is named Favourite again");
    finish();
  }

  // A short recording, to stand in for a preview the library serves.
  bool writeTestPreview(const File& file, double seconds) {
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    constexpr double kRate = 48000.0;
    const int frames = static_cast<int>(kRate * seconds);
    AudioBuffer<float> audio(1, frames);
    for (int i = 0; i < frames; ++i) {
      audio.setSample(0, i, 0.5f * std::sin(MathConstants<float>::twoPi * 440.0f * static_cast<float>(i) /
                                            static_cast<float>(kRate)));
    }
    WavAudioFormat wav;
    std::unique_ptr<FileOutputStream> stream(file.createOutputStream());
    if (stream == nullptr)
      return false;
    std::unique_ptr<AudioFormatWriter> writer(wav.createWriterFor(stream.get(), kRate, 1, 16, {}, 0));
    if (writer == nullptr)
      return false;
    stream.release();   // the writer owns the stream now
    return writer->writeFromAudioSampleBuffer(audio, 0, frames);
  }

  // A community preset's preview: the details say it is there and how long
  // it is, P fetches it into Community/Previews and plays it through the
  // plugin's output, P again stops it, and the end of the recording is said.
  void testCommunityPreviewPlays() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };
    auto heard = [&] { return spoken.messages.joinIntoString(" | ").toStdString(); };

    // The saved library: two presets, one with a two second preview.
    const File saved = library.folder.getChildFile("SavedLibrary");
    saved.createDirectory();
    SynthPlugin synth;
    synth.setPlayConfigDetails(0, 2, 48000.0, 128);
    synth.prepareToPlay(48000.0, 128);
    const File shared = saved.getChildFile("Shared Lead.atp");
    const File quiet = saved.getChildFile("Quiet Bass.atp");
    const File preview = saved.getChildFile("Shared Lead preview.wav");
    const bool made = synth.saveCopyToFile(shared) && synth.saveCopyToFile(quiet) && writeTestPreview(preview, 2.0);
    expect(made, "presets and a recording are saved to stand in for the library's files");
    json lead = communityPresetJson(1, "Shared Lead", shared, sha256OfFile(shared), "lead", "Lead", { "glow" });
    lead["preview"] = { { "url", (URL(preview).toString(false) + "?v=1").toStdString() },
                        { "name", "Shared Lead preview.wav" },
                        { "content_type", "audio/wav" },
                        { "size", preview.getSize() },
                        { "duration", 2.0 } };
    json page;
    page["count"] = 2;
    page["next"] = nullptr;
    page["results"] = json::array();
    page["results"].push_back(lead);
    page["results"].push_back(communityPresetJson(2, "Quiet Bass", quiet, sha256OfFile(quiet), "bass", "Bass", {}));
    const File page_file = saved.getChildFile("presets.json");
    page_file.replaceWithText(String(page.dump()));
    atlas::community::setLibraryUrlForTesting(URL(page_file).toString(false));
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;

    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* details = dynamic_cast<Label*>(titled(editor, "Selected preset details"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    const bool browser = made && collection != nullptr && results != nullptr && details != nullptr && autoload != nullptr;
    expect(browser, "the editor has its preset browser");
    if (!browser) {
      editor.removeFromDesktop();
      return;
    }
    const bool autoload_was_on = autoload->getToggleState();
    autoload->setToggleState(false, dontSendNotification);
    auto finish = [&] {
      autoload->setToggleState(autoload_was_on, dontSendNotification);
      editor.removeFromDesktop();
    };
    // Renders the plugin's output until a block carries sound, or `blocks` have gone by.
    auto renderedSound = [&](int blocks) {
      AudioBuffer<float> out(2, 128);
      MidiBuffer midi;
      for (int block = 0; block < blocks; ++block) {
        out.clear();
        synth.processBlock(out, midi);
        if (out.getMagnitude(0, 128) > 0.05f)
          return true;
        Thread::sleep(2);
      }
      return false;
    };

    collection->selectRow(2);
    pumpEditorEvents();
    pumpUntil([&] { return !editor.isCommunityLibraryBusy(); }, 10000);
    expect(editor.isCommunityLibraryLoaded() && editor.presetRowCount() == 2,
           "the community list arrives with both presets: " + editor.communityLibraryError().toStdString());
    if (editor.presetRowCount() != 2) {
      finish();
      return;
    }
    editor.ensureComponentVisible(results);
    results->grabKeyboardFocus();
    pumpEditorEvents();

    // Sorted by name: Quiet Bass, then Shared Lead. The bass has no preview.
    results->selectRow(0);
    pumpEditorEvents();
    expect(details->getText().contains(". No preview"),
           "a row without a preview says so: " + details->getText().toStdString());
    spoken.messages.clear();
    expect(results->keyPressed(characterKey('p')), "P on the row without a preview is handled");
    pumpEditorEvents();
    expect(said("No preview for Quiet Bass") && !editor.isCommunityPreviewPlaying(),
           "P on a row without a preview says so: " + heard());

    // The lead's preview is fetched and plays through the plugin.
    results->selectRow(1);
    pumpEditorEvents();
    expect(details->getText().contains("Preview: 2 seconds, P plays it"),
           "the details say the preview is there and how long it is: " + details->getText().toStdString());
    spoken.messages.clear();
    expect(results->keyPressed(characterKey('p')), "P on the row with a preview is handled");
    pumpUntil([&] { return editor.isCommunityPreviewPlaying(); }, 10000);
    const File cached = atlas::community::previewDirectory(library.folder).getChildFile("Presets").getChildFile("tester")
                            .getChildFile("Shared Lead preview.wav");
    expect(cached.existsAsFile() && cached.getSize() == preview.getSize(),
           "the preview is fetched into Community/Previews/Presets/tester: " + cached.getFullPathName().toStdString());
    expect(editor.isCommunityPreviewPlaying() && editor.communityPreviewFile() == cached, "and plays");
    expect(said("Fetching the preview of Shared Lead") && said("Playing preview of Shared Lead"),
           "the fetch and the start are said: " + heard());
    expect(renderedSound(400), "the plugin's output carries the preview");
    expect(details->getText().contains("Preview: 2 seconds, playing, P stops it"),
           "the details say it is playing: " + details->getText().toStdString());

    // P again stops it.
    spoken.messages.clear();
    expect(results->keyPressed(characterKey('p')), "P again is handled");
    pumpEditorEvents();
    expect(!editor.isCommunityPreviewPlaying() && said("Stopped"), "P again stops the preview and says so: " + heard());
    expect(!renderedSound(20), "the output is quiet once the preview has stopped");
    expect(details->getText().contains("Preview: 2 seconds, P plays it"),
           "the details no longer say playing: " + details->getText().toStdString());

    // Played again, from the fetched copy, it runs to its end, which is said.
    spoken.messages.clear();
    expect(results->keyPressed(characterKey('p')), "P plays the fetched copy");
    pumpUntil([&] { return editor.isCommunityPreviewPlaying(); }, 10000);
    expect(editor.isCommunityPreviewPlaying() && !said("Fetching"), "the fetched copy plays without another fetch: " + heard());
    // Rendered a little faster than real time, with the message loop running
    // between blocks so the read-ahead thread keeps up and the timer sees the end.
    AudioBuffer<float> out(2, 128);
    MidiBuffer midi;
    const uint32 started = Time::getMillisecondCounter();
    while (editor.isCommunityPreviewPlaying() && Time::getMillisecondCounter() - started < 15000) {
      out.clear();
      synth.processBlock(out, midi);
      MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    pumpUntil([&] { return said("finished"); }, 5000);
    expect(!editor.isCommunityPreviewPlaying() && editor.communityPreviewFile() == File() &&
               said("Preview of Shared Lead finished"),
           "the end of the recording is said: " + heard());
    finish();
  }

  // A made-up token in the library's format; never a real one.
  const String kTestToken = "bpa_" + String::repeatedString("0123456789abcdef", 2) + "feedface";

  // The library's styles and genres, and its answers to an upload, as the API
  // sends them.
  void testShareRequestsAndAnswers() {
    using namespace atlas::community;
    expect(styleIdForText("Basses") == "bass" && styleIdForText("Lead") == "lead" && styleIdForText("drums") == "drums" &&
               styleIdForText("perc") == "percussion" && styleIdForText("pads") == "pad" &&
               styleIdForText("Keys and things") == "keys" && styleIdForText("Electric Piano") == "keys" &&
               styleIdForText("Piano") == "piano" && styleIdForText("Brass section") == "brass" &&
               styleIdForText("Choir") == "vocal" && styleIdForText("Arp") == "sequence" &&
               styleIdForText("Soundscape") == "atmosphere" && styleIdForText("").isEmpty() &&
               styleIdForText("Wobble").isEmpty(),
           "a patch's style reads as the library reads it");
    expect(styles().size() == 23 && String(styles().front().id) == "atmosphere" &&
               String(styles()[13].label) == "Piano" && String(styles().back().label) == "Other",
           "the library's twenty-three styles, Other last");
    expect(tokensPageUrl("https://atlas.blindproducers.org/") == "https://atlas.blindproducers.org/accounts/tokens/",
           "the site's API tokens page");

    std::vector<Genre> genres;
    expect(parseGenresPage(R"({"results": [{"name": "Acid House", "slug": "acid-house", "presets": 0, "banks": 0},
                                           {"name": "", "slug": "nameless"}, {"name": "Ambient", "slug": "ambient"}]})",
                           genres).isEmpty() &&
               genres.size() == 2 && genres[0].name == "Acid House" && genres[0].slug == "acid-house" &&
               genres[1].slug == "ambient",
           "the genres page is read, without nameless entries");
    expect(genresPageUrl("https://atlas.blindproducers.org") == "https://atlas.blindproducers.org/api/v1/genres/",
           "the genres page's address");

    // The request: the endpoint, the fields that are not empty, the file and the token.
    const File folder = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("AtlasShareRequest", "", false);
    folder.createDirectory();
    const File preset = folder.getChildFile("Glow Lead.atp");
    preset.replaceWithText("{}");
    ShareFields fields;
    fields.name = " Glow Lead ";
    fields.style = "lead";
    fields.genre = "ambient";
    fields.tags = "glow, soft";
    UploadRequest request = buildPresetUpload("https://atlas.blindproducers.org/", preset, fields, kTestToken);
    expect(request.error.isEmpty() && request.url.toString(false) == "https://atlas.blindproducers.org/api/v1/presets/",
           "a patch is posted to /api/v1/presets/: " + request.url.toString(false).toStdString());
    expect(request.url.getParameterNames() == StringArray { "name", "style", "genre", "tags" } &&
               request.url.getParameterValues() == StringArray { "Glow Lead", "lead", "ambient", "glow, soft" },
           "the fields go with it, trimmed, and empty ones are left out: " +
               request.url.getParameterNames().joinIntoString(", ").toStdString());
    expect(request.headers.contains("Authorization: Bearer " + kTestToken + "\r\n") &&
               request.headers.contains("Accept: application/json"),
           "the token is sent as a Bearer header");
    request = buildBankUpload("https://atlas.blindproducers.org", preset, fields, kTestToken + "\r\nX-Evil: 1");
    expect(request.url.toString(false) == "https://atlas.blindproducers.org/api/v1/banks/" &&
               !request.url.getParameterNames().contains("style") && request.url.getParameterNames().contains("genre"),
           "a bank is posted to /api/v1/banks/ with a genre and no style");
    expect(!request.headers.contains("\r\nX-Evil") && request.headers.contains(kTestToken + "X-Evil: 1\r\n"),
           "line breaks in a token never make headers of their own");
    expect(buildPresetUpload("file:///saved/presets.json", preset, fields, kTestToken).error.contains("web address") &&
               buildPresetUpload("https://atlas.blindproducers.org", preset, fields, " ").error.contains("API token") &&
               buildPresetUpload("https://atlas.blindproducers.org", folder.getChildFile("gone.atp"), fields,
                                 kTestToken).error.contains("missing"),
           "a saved library, a missing token and a missing file stop the upload before it is sent");
    expect(buildPresetUpload("http://atlas.blindproducers.org", preset, fields, kTestToken).error.contains("not using HTTPS") &&
               buildBankUpload("http://example.com", preset, fields, kTestToken).headers.isEmpty() &&
               buildPresetUpload("http://127.0.0.1:8000", preset, fields, kTestToken).error.isEmpty() &&
               buildPresetUpload("http://localhost:8000", preset, fields, kTestToken).error.isEmpty(),
           "the token goes over plain http only to this computer");
    expect(upload(buildPresetUpload("file:///saved/presets.json", preset, fields, kTestToken)).error.contains("web address"),
           "and are what the upload reports");
    folder.deleteRecursively();

    // The answers.
    UploadResult result = parseUploadAnswer(201, R"({"id": 63, "type": "preset", "name": "Glow Lead",
        "url": "https://atlas.blindproducers.org/presets/63/", "file": {"size": 2}})");
    expect(result.shared() && result.id == 63 && result.name == "Glow Lead" &&
               result.page_url == "https://atlas.blindproducers.org/presets/63/",
           "201 is shared, with the new item's id, name and page");
    result = parseUploadAnswer(400, R"({"error": "The upload was not accepted.",
        "errors": {"file": ["This patch has no settings section, so Atlas could not load it."],
                   "genre": ["This field is required.", "Choose a genre."], "__all__": ["Try again later."]}})");
    expect(!result.shared() && result.field_errors.size() == 3 &&
               result.field_errors.contains("File: This patch has no settings section, so Atlas could not load it.") &&
               result.field_errors.contains("Genre: This field is required. Choose a genre.") &&
               result.field_errors.contains("Try again later.") &&
               result.error.startsWith("The upload was not accepted. ") && result.error.contains("Genre: This field is required."),
           "400 names each field the library refused: " + result.error.toStdString());
    result = parseUploadAnswer(401, R"({"error": "A valid API token is required."})");
    expect(!result.shared() && result.tokenRefused() && result.error.contains("refused the API token"),
           "401 says the token was refused: " + result.error.toStdString());
    result = parseUploadAnswer(413, "<html>Too large</html>");
    expect(!result.shared() && result.error.contains("larger than the library accepts"), "413 says the file is too large");
    result = parseUploadAnswer(502, "<html>Bad gateway</html>");
    expect(!result.shared() && result.error.contains("is having trouble (502)"), "other answers say the status");
    result = parseUploadAnswer(201, "<html>");
    expect(!result.shared() && result.error.contains("not JSON"), "a 201 that is not JSON is not taken as shared");
  }

  // A fresh store in memory for API tokens, standing for the keychain as the
  // DAW `scope` sees it.
  std::shared_ptr<atlas::credentials::MemoryStore> useTestCredentialStore(const std::string& scope = "com.example.daw") {
    auto store = std::make_shared<atlas::credentials::MemoryStore>(scope);
    atlas::community::setCredentialStoreForTesting(store);
    return store;
  }

  // The DAWs the config says are logged in.
  StringArray loggedInDaws() {
    StringArray daws;
    const json config = LoadSave::getConfigJson();
    const auto logins = config.is_object() ? config.find("community_logins") : config.end();
    if (logins != config.end() && logins->is_object())
      for (const auto& login : logins->items())
        daws.add(login.key());
    return daws;
  }

  // The token lives in the credential store (here one in memory standing for
  // the keychain) and never in a file: the config says only which DAW is
  // logged in. Each DAW keeps its own; one the store will not keep is held in
  // memory until the DAW closes; and a token an older Atlas left in the config
  // moves into the store.
  void testApiTokenIsKeptInTheCredentialStore() {
    using namespace atlas::community;
    TemporaryUndoLibrary library;   // its own config file
    expect(library.isolated(), "the test's config is its own");
    if (!library.isolated())
      return;
    auto store = useTestCredentialStore("com.example.first-daw");
    const auto configText = [] { return LoadSave::getConfigFile().loadFileAsString(); };
    expect(!hasApiToken() && storedApiToken().isEmpty(), "no token is saved at first");
    storeApiToken(" " + kTestToken + " ");
    expect(hasApiToken() && storedApiToken() == kTestToken, "a token is saved trimmed");
    expect(store->secretOf("com.example.first-daw") == kTestToken.toStdString() &&
               store->labelOf("com.example.first-daw").rfind("Atlas community library token for ", 0) == 0,
           "in the store, under this DAW's name: " + store->labelOf("com.example.first-daw"));
    expect(!configText().contains(kTestToken) && !configText().contains("bpa_") &&
               loggedInDaws() == StringArray { "com.example.first-daw" },
           "the config says only that this DAW is logged in: " + configText().toStdString());
    expect(tokenStorageText(true).startsWith("Your API token is kept in the test store, for ") &&
               tokenStorageText(true).endsWith(" only. Each DAW logs in on its own.") && tokenPlace() == "the test store",
           "Settings can say where it is kept: " + tokenStorageText(true).toStdString());
    expect(tokenHint(kTestToken) == "the token ending in face" && !tokenHint(kTestToken).contains("bpa_"),
           "only its last four characters are ever said: " + tokenHint(kTestToken).toStdString());
    expect(looksLikeApiToken(kTestToken) && !looksLikeApiToken("bpa_xyz") && !looksLikeApiToken("hello") &&
               !looksLikeApiToken(""),
           "a token looks like one the library issues");

    // Read from the store once a process, then from memory; whether there is
    // one is known without reading it.
    setCredentialStoreForTesting(store);   // as a new process starts
    const int reads = store->reads();
    expect(hasApiToken() && store->reads() == reads, "whether a token is saved is known without reading it");
    expect(storedApiToken() == kTestToken && storedApiToken() == kTestToken && store->reads() == reads + 1,
           "it is read once, then held in memory");

    // Another DAW keeps its own.
    store->setScope("com.example.second-daw");
    setCredentialStoreForTesting(store);
    expect(!hasApiToken() && storedApiToken().isEmpty() &&
               tokenStorageText(false).startsWith("Logging in keeps your API token in the test store, for ") &&
               tokenStorageText(false).endsWith("Each DAW logs in on its own."),
           "another DAW is not logged in by the first one's token: " + tokenStorageText(false).toStdString());
    const String other = "bpa_" + String::repeatedString("cd", 20);
    storeApiToken(other);
    expect(storedApiToken() == other && store->secretOf("com.example.first-daw") == kTestToken.toStdString() &&
               loggedInDaws().size() == 2,
           "it logs in on its own, and the first keeps its token");
    storeApiToken({});
    expect(!hasApiToken() && !store->holds("com.example.second-daw") && store->holds("com.example.first-daw") &&
               loggedInDaws() == StringArray { "com.example.first-daw" },
           "logging out there leaves the first logged in");
    store->setScope("com.example.first-daw");
    setCredentialStoreForTesting(store);
    expect(hasApiToken() && storedApiToken() == kTestToken, "the first DAW still has its token");

    SynthPlugin synth;
    MemoryBlock state;
    synth.getStateInformation(state);
    const File preset = library.folder.getChildFile("Token check.atp");
    expect(synth.saveCopyToFile(preset), "a preset is saved");
    expect(!state.toString().contains(kTestToken) && !state.toString().contains("bpa_") &&
               !preset.loadFileAsString().contains(kTestToken),
           "the token is in neither the host's state nor a preset");

    // Another process of the same DAW logs out, then saves a new token: the
    // config says so, and what memory holds follows.
    json config = LoadSave::getConfigJson();
    const json login = config["community_logins"]["com.example.first-daw"];
    config.erase("community_logins");
    LoadSave::saveJsonToConfig(config);
    expect(!hasApiToken() && storedApiToken().isEmpty(), "a logout in another process is seen");
    std::string error;
    store->write("com.example.first-daw", "Atlas community library token", other.toStdString(), error);
    config = LoadSave::getConfigJson();
    config["community_logins"]["com.example.first-daw"] = login;
    config["community_logins"]["com.example.first-daw"]["saved"] = login.at("saved").get<int64>() + 1;
    LoadSave::saveJsonToConfig(config);
    expect(storedApiToken() == other, "a token another process saved is read again");
    storeApiToken(kTestToken);

    // Taken out of the store behind Atlas's back (in Keychain Access, say).
    store->erase("com.example.first-daw");
    setCredentialStoreForTesting(store);
    String problem;
    expect(hasApiToken() && storedApiToken(&problem).isEmpty() && problem == "it is no longer in the test store" &&
               !hasApiToken() && loggedInDaws().isEmpty(),
           "a token gone from the store logs this DAW out, and says why: " + problem.toStdString());

    // A store that will not keep it: memory alone holds it, until the DAW closes.
    store->setRefusingWrites(true);
    storeApiToken(kTestToken);
    expect(hasApiToken() && storedApiToken() == kTestToken && tokenInMemoryOnly() &&
               !store->holds("com.example.first-daw") && loggedInDaws().isEmpty() && !configText().contains(kTestToken),
           "a token the store refuses is held in memory alone, and never written to the config");
    expect(tokenStorageText(true).startsWith("Your API token is kept in memory until ") &&
               tokenStorageText(true).endsWith(" closes, because the test store refused it.") && tokenPlace() == "memory",
           "Settings says so: " + tokenStorageText(true).toStdString());
    setCredentialStoreForTesting(store);   // the DAW closes
    expect(!hasApiToken() && storedApiToken().isEmpty(), "and it is gone once the DAW closes");
    store->setRefusingWrites(false);

    // No store at all, as on Linux.
    store->setUnavailable("Atlas has no credential store on this system");
    setCredentialStoreForTesting(store);
    expect(tokenStorageText(false).startsWith("Logging in keeps your API token in memory until ") &&
               tokenStorageText(false).endsWith(" closes, because Atlas has no credential store on this system."),
           "with no store, Settings says the token would be held in memory: " + tokenStorageText(false).toStdString());
    storeApiToken(kTestToken);
    expect(hasApiToken() && storedApiToken() == kTestToken && tokenInMemoryOnly() && loggedInDaws().isEmpty() &&
               !configText().contains(kTestToken),
           "memory alone holds it");
    storeApiToken({});
    expect(!hasApiToken() && storedApiToken().isEmpty(), "until logging out");
    store->setUnavailable({});

    // A token an older Atlas left in the config moves into the store, with
    // its account, and leaves the config.
    const auto leaveOldToken = [&] {
      json old = LoadSave::getConfigJson();
      old["community_api_token"] = kTestToken.toStdString();
      old["community_account_username"] = "jayladdermusic";
      old["community_account_name"] = "Jay Ladder";
      LoadSave::saveJsonToConfig(old);
    };
    leaveOldToken();
    setCredentialStoreForTesting(store);
    expect(hasApiToken() && store->secretOf("com.example.first-daw") == kTestToken.toStdString() &&
               !configText().contains(kTestToken) && !configText().contains("community_api_token") &&
               !configText().contains("community_account_") && storedAccount().username == "jayladdermusic" &&
               storedAccount().display_name == "Jay Ladder",
           "an older Atlas's token moves from the config into the store, with its account: " + configText().toStdString());
    expect(storedApiToken() == kTestToken, "and is used from there");
    storeApiToken({});
    store->setRefusingWrites(true);
    leaveOldToken();
    setCredentialStoreForTesting(store);
    expect(hasApiToken() && tokenInMemoryOnly() && storedApiToken() == kTestToken && !configText().contains(kTestToken) &&
               !configText().contains("community_api_token") && storedAccount().username == "jayladdermusic",
           "where the store refuses it, memory holds it and it still leaves the config");
    store->setRefusingWrites(false);

    storeApiToken({});
    expect(!hasApiToken() && storedApiToken().isEmpty() && !store->holds("com.example.first-daw") &&
               loggedInDaws().isEmpty() && !configText().contains(kTestToken),
           "storing nothing logs out: out of the store, memory and the config");
  }

  // A web server on 127.0.0.1 standing in for the library: it answers each
  // request with the next answer queued for its method and path (the last one
  // stays) and records what it was sent.
  class TestLibraryServer : private Thread {
    public:
      struct Request {
        String method, path;
        StringPairArray headers;
        MemoryBlock body;
        String bodyText() const { return String::fromUTF8(static_cast<const char*>(body.getData()), static_cast<int>(body.getSize())); }
        // For a body with binary parts (a zip), which is not text.
        bool bodyContains(const String& text) const {
          const std::string needle = text.toStdString();
          const auto* begin = static_cast<const char*>(body.getData());
          const auto* end = begin + body.getSize();
          return std::search(begin, end, needle.begin(), needle.end()) != end;
        }
      };

      TestLibraryServer() : Thread("Atlas test library") {
        listening_ = listener_.createListener(0, "127.0.0.1");
        if (listening_)
          startThread();
      }
      ~TestLibraryServer() override {
        signalThreadShouldExit();
        listener_.close();
        stopThread(5000);
      }

      bool isListening() const { return listening_; }
      String url() const { return "http://127.0.0.1:" + String(listener_.getBoundPort()); }

      // An answer sent in `chunk_bytes` pieces `chunk_delay_ms` apart, when
      // both are given, stands in for a large file on a slow connection.
      void answer(const String& method, const String& path, int status, const String& body, int chunk_bytes = 0,
                  int chunk_delay_ms = 0) {
        const ScopedLock lock(lock_);
        auto& queue = answers_[method + " " + path];
        // The last answer stays for later requests until another is queued.
        if (queue.size() == 1 && queue.front().served)
          queue.clear();
        queue.push_back({ status, body, false, chunk_bytes, chunk_delay_ms });
      }
      std::vector<Request> requests() const {
        const ScopedLock lock(lock_);
        return requests_;
      }

    private:
      struct Answer {
        int status;
        String body;
        bool served = false;
        int chunk_bytes = 0;
        int chunk_delay_ms = 0;
      };

      void run() override {
        while (!threadShouldExit()) {
          std::unique_ptr<StreamingSocket> client(listener_.waitForNextConnection());
          if (client != nullptr)
            handle(*client);
        }
      }

      static bool readSome(StreamingSocket& socket, MemoryBlock& into) {
        if (socket.waitUntilReady(true, 5000) <= 0)
          return false;
        char chunk[65536];
        const int read = socket.read(chunk, sizeof(chunk), false);
        if (read <= 0)
          return false;
        into.append(chunk, static_cast<size_t>(read));
        return true;
      }

      static int headerEnd(const MemoryBlock& data) {
        const auto* bytes = static_cast<const char*>(data.getData());
        for (size_t i = 0; i + 3 < data.getSize(); ++i) {
          if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' && bytes[i + 3] == '\n')
            return static_cast<int>(i);
        }
        return -1;
      }

      void handle(StreamingSocket& socket) {
        MemoryBlock data;
        int end = -1;
        while ((end = headerEnd(data)) < 0) {
          if (!readSome(socket, data))
            return;
        }
        StringArray lines;
        lines.addLines(String::fromUTF8(static_cast<const char*>(data.getData()), end));
        Request request;
        StringArray first;
        first.addTokens(lines[0], " ", "");
        request.method = first[0];
        request.path = first[1];
        for (int i = 1; i < lines.size(); ++i) {
          const String line = lines[i];
          if (line.contains(":"))
            request.headers.set(line.upToFirstOccurrenceOf(":", false, false).trim().toLowerCase(),
                                line.fromFirstOccurrenceOf(":", false, false).trim());
        }
        const int64 length = request.headers["content-length"].getLargeIntValue();
        const size_t body_start = static_cast<size_t>(end) + 4;
        while (static_cast<int64>(data.getSize() - body_start) < length) {
          if (!readSome(socket, data))
            break;
        }
        if (data.getSize() > body_start)
          request.body.append(static_cast<const char*>(data.getData()) + body_start, data.getSize() - body_start);

        Answer reply { 404, R"({"error": "No such page."})" };
        {
          const ScopedLock lock(lock_);
          requests_.push_back(request);
          auto found = answers_.find(request.method + " " + request.path);
          if (found != answers_.end() && !found->second.empty()) {
            found->second.front().served = true;
            reply = found->second.front();
            if (found->second.size() > 1)
              found->second.erase(found->second.begin());
          }
        }
        const std::string body = reply.body.toStdString();
        const String head = "HTTP/1.1 " + String(reply.status) + (reply.status < 300 ? " OK" : " Error") +
                            "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.size()) +
                            "\r\nConnection: close\r\n\r\n";
        if (reply.chunk_bytes <= 0 || reply.chunk_delay_ms <= 0) {
          const std::string text = head.toStdString() + body;
          socket.write(text.data(), static_cast<int>(text.size()));
          return;
        }
        // Slowly: the head, then the body a chunk at a time, until the client
        // has gone (a cancelled download closes its connection).
        const std::string head_text = head.toStdString();
        if (socket.write(head_text.data(), static_cast<int>(head_text.size())) < 0)
          return;
        for (size_t sent = 0; sent < body.size() && !threadShouldExit(); sent += static_cast<size_t>(reply.chunk_bytes)) {
          const int piece = static_cast<int>(std::min<size_t>(static_cast<size_t>(reply.chunk_bytes), body.size() - sent));
          if (socket.write(body.data() + sent, piece) < 0)
            return;
          Thread::sleep(reply.chunk_delay_ms);
        }
      }

      StreamingSocket listener_;
      bool listening_ = false;
      CriticalSection lock_;
      std::map<String, std::vector<Answer>> answers_;
      std::vector<Request> requests_;
  };

  String presetAnswer(int id, const String& name) {
    json answer;
    answer["id"] = id;
    answer["type"] = "preset";
    answer["name"] = name.toStdString();
    answer["url"] = ("https://atlas.blindproducers.org/presets/" + String(id) + "/").toStdString();
    return String(answer.dump());
  }

  // A real multipart POST, to the server above: the fields, the file and the
  // token arrive, and the answer is read.
  void testUploadReachesTheLibrary() {
    TestLibraryServer server;
    expect(server.isListening(), "the test library listens on 127.0.0.1");
    if (!server.isListening())
      return;
    server.answer("POST", "/api/v1/presets/", 201, presetAnswer(64, "Glow Lead"));
    const File folder = File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("AtlasShareUpload", "", false);
    folder.createDirectory();
    const File preset = folder.getChildFile("Glow Lead.atp");
    preset.replaceWithText(R"({"preset_name": "Glow Lead", "settings": {}})");
    atlas::community::ShareFields fields;
    fields.name = "Glow Lead";
    fields.style = "lead";
    fields.tags = "glow";
    auto result = atlas::community::uploadPreset(server.url(), kTestToken, preset, fields);
    expect(result.shared() && result.id == 64 && result.name == "Glow Lead",
           "the library's 201 is read: " + std::to_string(result.status) + " " + result.error.toStdString());
    const auto requests = server.requests();
    expect(requests.size() == 1, "one request reached the library");
    if (requests.size() == 1) {
      const auto& request = requests[0];
      const String body = request.bodyText();
      expect(request.method == "POST" && request.path == "/api/v1/presets/", "it is a POST to /api/v1/presets/");
      expect(request.headers["authorization"] == "Bearer " + kTestToken, "with the token as a Bearer header");
      expect(request.headers["content-type"].startsWith("multipart/form-data; boundary="), "as multipart form data");
      expect(body.contains("name=\"name\"\r\n\r\nGlow Lead\r\n") && body.contains("name=\"style\"\r\n\r\nlead\r\n") &&
                 body.contains("name=\"tags\"\r\n\r\nglow\r\n") &&
                 body.contains("name=\"file\"; filename=\"Glow Lead.atp\"") &&
                 body.contains(R"({"preset_name": "Glow Lead", "settings": {}})"),
             "carrying the fields and the file: " + body.substring(0, 400).toStdString());
    }

    server.answer("POST", "/api/v1/presets/", 401, R"({"error": "A valid API token is required."})");
    result = atlas::community::uploadPreset(server.url(), kTestToken, preset, fields);
    expect(result.tokenRefused() && !result.shared(), "a 401 from the library is a refused token: " +
                                                          std::to_string(result.status) + " " + result.error.toStdString());
    folder.deleteRecursively();
  }

  // The Preset menu's Share patch and Share bank, against the test library.
  // Logged out, either opens the login dialog first: it says no token was
  // found, refuses what is not a token, says what the library answers and
  // keeps the token that works; then the form opens, with no token box or
  // Log out of its own. The patch form passes on the library's field errors
  // and stays open, shares with the focus on the upload's progress bar, says
  // so and gives the focus back; the bank form exports the chosen folder,
  // needs a genre and shares with the saved token; a token the library
  // refuses brings the dialog back and Share goes on; Escape closes.
  void testShareFormsUploadAndSpeak() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    const auto credentials = useTestCredentialStore();
    TestLibraryServer server;
    expect(server.isListening(), "the test library listens on 127.0.0.1");
    if (!server.isListening())
      return;
    const String account_answer = R"({"username": "jayladdermusic", "display_name": "Jay Ladder", "presets": 3, "banks": 1})";
    server.answer("GET", "/api/v1/me/", 401, R"({"error": "A valid API token is required."})");
    server.answer("GET", "/api/v1/me/", 200, account_answer);
    server.answer("GET", "/api/v1/me/", 200, account_answer);
    server.answer("GET", "/api/v1/genres/", 200,
                  R"({"results": [{"name": "Ambient", "slug": "ambient"}, {"name": "Speed Garage", "slug": "speed-garage"}]})");
    server.answer("POST", "/api/v1/presets/", 400,
                  R"({"error": "The upload was not accepted.", "errors": {"name": ["Ensure this value has at most 120 characters."]}})");
    server.answer("POST", "/api/v1/presets/", 201, presetAnswer(70, "Shared Patch"));
    server.answer("POST", "/api/v1/banks/", 401, R"({"error": "A valid API token is required."})");
    server.answer("POST", "/api/v1/banks/", 201, R"({"id": 3, "type": "bank", "name": "My Bank", "url": "https://atlas.blindproducers.org/banks/3/"})");
    atlas::community::setLibraryUrlForTesting(server.url());
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;

    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };
    auto heard = [&] { return spoken.messages.joinIntoString(" | ").toStdString(); };

    SynthPlugin synth;
    // A bank folder in the user library to share.
    const File bank_folder = atlas::library::bankDirectory(atlas::library::Library::kUser, "My Bank",
                                                           LoadSave::getDataDirectory());
    expect(synth.saveCopyToFile(bank_folder.getChildFile("Bank Patch.atp")), "a preset is saved in the user library's My Bank");
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();

    auto* preset_menu = titled(editor, "Preset menu");
    expect(preset_menu != nullptr, "the editor has its Preset menu");
    if (preset_menu == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    editor.ensureComponentVisible(preset_menu);
    preset_menu->grabKeyboardFocus();
    pumpEditorEvents();

    // Logged out: the login dialog, not the form.
    spoken.messages.clear();
    editor.sharePatchForTesting();
    pumpEditorEvents();
    auto* dialog = editor.communityLoginWindowForTesting();
    auto* content = dialog != nullptr ? dialog->getContentComponent() : nullptr;
    expect(dialog != nullptr && !editor.isShareFormVisible(), "Share patch while logged out opens the login dialog, not the form");
    if (content == nullptr) {
      editor.removeFromDesktop();
      return;
    }
    auto* message = dynamic_cast<Label*>(titled(*content, "Login message"));
    auto* token = dynamic_cast<TextEditor*>(titled(*content, "API token"));
    auto* website = dynamic_cast<Button*>(titled(*content, "Get your API token on the website"));
    auto* log_in = dynamic_cast<Button*>(titled(*content, "Log in"));
    auto* cancel = dynamic_cast<Button*>(titled(*content, "Cancel"));
    const bool dialog_controls = message != nullptr && token != nullptr && website != nullptr && log_in != nullptr &&
                                 cancel != nullptr;
    expect(dialog_controls, "the dialog has its message, token box, website button, Log in and Cancel");
    if (!dialog_controls) {
      delete dialog;
      editor.removeFromDesktop();
      return;
    }
    expect(message->getText().startsWith("No API token found.") && message->hasKeyboardFocus(false),
           "the dialog says no token was found, and opens on that: " + message->getText().toStdString());
    expect(website->getDescription().contains("/accounts/tokens/ in your browser"),
           "the website button opens the site's API tokens page: " + website->getDescription().toStdString());
    expect(token->getPasswordCharacter() != 0 && token->getAccessibilityHandler() != nullptr &&
               token->getAccessibilityHandler()->getTextInterface() != nullptr &&
               token->getAccessibilityHandler()->getTextInterface()->isDisplayingProtectedText(),
           "the token box hides what is typed from the eye and the screen reader");

    spoken.messages.clear();
    log_in->onClick();
    pumpEditorEvents();
    expect(said("Paste your API token first") && token->hasKeyboardFocus(false) && server.requests().empty(),
           "Log in without a token asks for one and sends nothing: " + heard());
    token->setText("hello", true);
    spoken.messages.clear();
    log_in->onClick();
    pumpEditorEvents();
    expect(said("That is not an API token") && server.requests().empty(), "a token that is not one is refused: " + heard());

    // The library refuses the token: said, dialog open, token not kept.
    token->setText(kTestToken, true);
    spoken.messages.clear();
    log_in->onClick();
    pumpUntil([&] { return said("Unable to log in"); }, 10000);
    expect(said("Unable to log in. The library refused the API token") && atlas::community::storedApiToken().isEmpty() &&
               editor.communityLoginWindowForTesting() != nullptr && !editor.isShareFormVisible(),
           "a refused token is said and not kept, and the dialog stays: " + heard());
    for (const auto& message_said : spoken.messages)
      expect(!message_said.contains(kTestToken) && !message_said.contains("bpa_"), "the token is never said");

    // Accepted: kept with its account, the dialog closes and the form opens.
    spoken.messages.clear();
    log_in->onClick();
    pumpUntil([&] { return editor.isShareFormVisible(); }, 10000);
    pumpUntil([&] { return said("Share patch on"); }, 2000);
    expect(editor.communityLoginWindowForTesting() == nullptr && editor.isShareFormVisible(),
           "the dialog closes and the share form opens");
    expect(said("Logged in as Jay Ladder (jayladdermusic). Share patch on 127.0.0.1"),
           "the login and the form are said together: " + heard());
    expect(atlas::community::storedApiToken() == kTestToken &&
               credentials->secretOf("com.example.daw") == kTestToken.toStdString() &&
               atlas::community::storedAccount().username == "jayladdermusic",
           "the token that worked is kept in the store, with its account");

    // The patch form: no token box or Log out of its own.
    auto* name = dynamic_cast<TextEditor*>(titled(editor, "Share name"));
    auto* style = dynamic_cast<ComboBox*>(titled(editor, "Share style"));
    auto* tags = dynamic_cast<TextEditor*>(titled(editor, "Share tags"));
    auto* share = dynamic_cast<Button*>(titled(editor, "Share"));
    auto* folder = dynamic_cast<ComboBox*>(titled(editor, "Folder to share"));
    auto* genre = dynamic_cast<ComboBox*>(titled(editor, "Share genre"));
    auto* progress = titled(editor, "Upload progress");
    const bool form = name != nullptr && style != nullptr && tags != nullptr && share != nullptr && folder != nullptr &&
                      genre != nullptr && progress != nullptr;
    expect(form, "the share form has its controls");
    if (!form) {
      editor.removeFromDesktop();
      return;
    }
    expect(titled(editor, "API token") == nullptr && titled(editor, "Log out") == nullptr,
           "the form has no token box and no Log out: the dialog and Settings have them");
    expect(name->isShowing() && style->isShowing() && !folder->isVisible() && !genre->isVisible() && !progress->isVisible(),
           "the patch form shows name and style, not the bank's folder and genre, and no progress bar yet");
    expect(name->hasKeyboardFocus(false), "the name box has the focus");
    const auto& order = editor.getKeyboardFocusOrder();
    expect(std::find(order.begin(), order.end(), share) != order.end() &&
               std::find(order.begin(), order.end(), progress) == order.end(),
           "Tab reaches the Share button, and not the hidden progress bar");
    expect(style->getNumItems() == 24 && style->getItemText(0) == "None" &&
               style->getItemText(1) == "Atmosphere" && style->getItemText(4) == "Brass" &&
               style->getItemText(14) == "Piano" && style->getItemText(23) == "Other",
           "the style menu offers None and the library's twenty-three, Brass and Piano among them");
    name->setText("Shared Patch", true);
    int lead = -1;
    for (int i = 0; i < style->getNumItems(); ++i) {
      if (style->getItemText(i) == "Lead")
        lead = i;
    }
    expect(lead > 0, "the style menu has Lead");
    style->setSelectedItemIndex(lead, sendNotificationSync);
    tags->setText("glow, test", true);

    // The library refuses a field: the progress bar has the focus while the
    // patch uploads; the answer is said, the form stays open and Share has
    // the focus again.
    spoken.messages.clear();
    expect(titled(editor, "Cancel sharing") != nullptr && titled(editor, "Cancel upload") == nullptr,
           "before Share, the form's Cancel is Cancel sharing");
    share->onClick();
    const bool uploading = editor.isShareBusy() && progress->isShowing() && progress->hasKeyboardFocus(false) &&
                           progress->getAccessibilityHandler() != nullptr &&
                           progress->getAccessibilityHandler()->getTitle().startsWith("Upload progress, ");
    expect(uploading, "Share moves the focus onto the progress bar, named with its percentage");
    // The same button, next in the Tab order after the bar, stops the upload
    // while it is on, and says so.
    auto* cancel_upload = dynamic_cast<Button*>(titled(editor, "Cancel upload"));
    expect(cancel_upload != nullptr && titled(editor, "Cancel sharing") == nullptr &&
               cancel_upload->getDescription() == "Stop the upload and close the share form",
           "while the upload is on, the form's Cancel is Cancel upload");
    pumpUntil([&] { return !editor.isShareBusy(); }, 15000);
    expect(said("Sharing Shared Patch on 127.0.0.1. Escape or Cancel upload stops it") &&
               said("Unable to share Shared Patch. The upload was not accepted. Name: Ensure this value has at most 120 characters."),
           "the upload is said with how to stop it, and the library's field errors are said: " + heard());
    expect(editor.isShareFormVisible() && !progress->isVisible() && share->hasKeyboardFocus(false),
           "the form stays open, the bar goes and Share has the focus");
    expect(titled(editor, "Cancel sharing") != nullptr && titled(editor, "Cancel upload") == nullptr,
           "and the form's Cancel is Cancel sharing again");

    // Shared: said, form closed, focus back.
    spoken.messages.clear();
    share->onClick();
    pumpUntil([&] { return !editor.isShareBusy() && !editor.isShareFormVisible(); }, 15000);
    // Announcements wait while the screen reader is moved back to the Preset menu.
    pumpUntil([&] { return said("Shared Shared Patch"); }, 2000);
    expect(said("Shared Shared Patch on 127.0.0.1") && !editor.isShareFormVisible(),
           "the share is said and the form closes: " + heard());
    expect(preset_menu->hasKeyboardFocus(false), "the focus is back on the Preset menu");
    auto requests = server.requests();
    const auto uploads = std::count_if(requests.begin(), requests.end(),
                                       [](const auto& request) { return request.path == "/api/v1/presets/"; });
    expect(uploads == 2, "two uploads reached the library, got " + std::to_string(uploads));
    const auto shared_patch = std::find_if(requests.rbegin(), requests.rend(),
                                           [](const auto& request) { return request.path == "/api/v1/presets/"; });
    if (shared_patch != requests.rend()) {
      const String body = shared_patch->bodyText();
      expect(shared_patch->headers["authorization"] == "Bearer " + kTestToken &&
                 body.contains("filename=\"Shared Patch.atp\"") && body.contains("name=\"style\"\r\n\r\nlead\r\n") &&
                 body.contains("name=\"tags\"\r\n\r\nglow, test\r\n") && body.contains("\"settings\""),
             "the patch went with its name, style, tags and the saved token");
    }

    // The bank form, logged in: no dialog; the folder, a genre, the saved token.
    spoken.messages.clear();
    editor.shareBankForTesting();
    pumpUntil([&] { return genre->getNumItems() == 3 && said("Share bank on"); }, 10000);
    expect(editor.communityLoginWindowForTesting() == nullptr && editor.isShareFormVisible() && folder->isShowing() &&
               genre->isShowing() && !style->isVisible(),
           "logged in, Share bank opens the form at once, with the folder and genre");
    expect(folder->hasKeyboardFocus(false) && said("Share bank on 127.0.0.1"), "the folder menu has the focus: " + heard());
    int my_bank = -1;
    for (int i = 0; i < folder->getNumItems(); ++i) {
      if (folder->getItemText(i) == "My Bank (User)")
        my_bank = i;
    }
    expect(my_bank >= 0 && folder->getItemText(folder->getNumItems() - 1) == "Choose another folder...",
           "the folder menu offers every library's banks, named with their library, and another folder");
    folder->setSelectedItemIndex(my_bank, sendNotificationSync);
    expect(name->getText() == "My Bank", "choosing a folder names the bank after it");
    spoken.messages.clear();
    share->onClick();
    pumpEditorEvents();
    expect(said("Choose the bank's genre") && genre->hasKeyboardFocus(false), "a bank needs a genre: " + heard());
    genre->setSelectedItemIndex(2, sendNotificationSync);   // Speed Garage

    // The library refuses the saved token: the dialog comes back over the
    // form, and a token it accepts shares the bank without another Share.
    spoken.messages.clear();
    share->onClick();
    pumpUntil([&] { return editor.communityLoginWindowForTesting() != nullptr; }, 15000);
    dialog = editor.communityLoginWindowForTesting();
    content = dialog != nullptr ? dialog->getContentComponent() : nullptr;
    message = content != nullptr ? dynamic_cast<Label*>(titled(*content, "Login message")) : nullptr;
    token = content != nullptr ? dynamic_cast<TextEditor*>(titled(*content, "API token")) : nullptr;
    log_in = content != nullptr ? dynamic_cast<Button*>(titled(*content, "Log in")) : nullptr;
    expect(message != nullptr && token != nullptr && log_in != nullptr && editor.isShareFormVisible() &&
               message->getText().startsWith("Unable to share My Bank. The library refused the API token"),
           "a refused token brings the login dialog back, with the form still open: " +
               (message != nullptr ? message->getText().toStdString() : std::string()));
    if (token != nullptr && log_in != nullptr) {
      token->setText(kTestToken, true);
      spoken.messages.clear();
      log_in->onClick();
      pumpUntil([&] { return !editor.isShareFormVisible(); }, 15000);
      pumpUntil([&] { return said("Shared My Bank"); }, 2000);
    }
    expect(said("Sharing bank My Bank on 127.0.0.1") && said("Shared My Bank on 127.0.0.1") && !editor.isShareFormVisible(),
           "the bank is shared after the login, without another Share, and it is said: " + heard());
    requests = server.requests();
    const auto bank_request = std::find_if(requests.rbegin(), requests.rend(),
                                           [](const auto& request) { return request.path == "/api/v1/banks/"; });
    expect(bank_request != requests.rend(), "the bank reached the library");
    if (bank_request != requests.rend()) {
      const auto& sent = *bank_request;
      expect(sent.headers["authorization"] == "Bearer " + kTestToken &&
                 sent.bodyContains("name=\"genre\"\r\n\r\nspeed-garage\r\n") &&
                 sent.bodyContains("filename=\"My Bank.atb\"") && sent.bodyContains("Content-Type: application/zip") &&
                 sent.bodyContains("PK") && sent.bodyContains("My Bank/Presets/Bank Patch.atp"),
             "as an archive of the folder, with its genre and the saved token");
    }

    // Escape closes the form and the focus goes back.
    editor.shareBankForTesting();
    pumpEditorEvents();
    name->grabKeyboardFocus();
    Component& name_box = *name;
    name_box.keyPressed(KeyPress(KeyPress::escapeKey));
    pumpEditorEvents();
    expect(!editor.isShareFormVisible() && preset_menu->hasKeyboardFocus(false),
           "Escape closes the form and the focus goes back to the Preset menu");
    editor.removeFromDesktop();
  }

  // The account behind a token, the uploader of a share, and the setting that
  // shows or hides the browser.
  void testAccountsAndTheBrowserSetting() {
    using namespace atlas::community;
    Account account;
    expect(parseAccount(200, R"({"username": "jayladdermusic", "display_name": "Jay Ladder", "is_staff": false,
                                 "presets": 3, "banks": 1})", account).isEmpty() &&
               account.username == "jayladdermusic" && account.display_name == "Jay Ladder" && account.presets == 3 &&
               account.banks == 1,
           "/api/v1/me/ is read");
    expect(accountName(account) == "Jay Ladder (jayladdermusic)", "an account is named by both its names");
    account.display_name = "jayladdermusic";
    expect(accountName(account) == "jayladdermusic", "once when they are the same");
    expect(parseAccount(401, R"({"error": "A valid API token is required."})", account).contains("refused") &&
               !account.known(),
           "401 is a refused token");
    expect(parseAccount(500, "<html>", account).contains("is having trouble (500)") &&
               parseAccount(200, R"({"presets": 1})", account).contains("names no account"),
           "other answers are errors");
    expect(meEndpoint("https://atlas.blindproducers.org/") == "https://atlas.blindproducers.org/api/v1/me/",
           "the account page's address");
    const auto shared = parseUploadAnswer(201, R"({"id": 1, "name": "X",
        "uploader": {"username": "jayladdermusic", "display_name": "Jay Ladder"}})");
    expect(shared.uploader.username == "jayladdermusic" && shared.uploader.display_name == "Jay Ladder",
           "a share's answer names the account that shared it");

    TemporaryUndoLibrary library;   // its own config file
    expect(library.isolated(), "the test's config is its own");
    if (!library.isolated())
      return;
    forgetBrowserShownForTesting();
    expect(browserShown(), "the browser is shown unless turned off");
    setBrowserShown(false);
    forgetBrowserShownForTesting();
    expect(!browserShown() && LoadSave::getConfigJson()["community_browser"] == false,
           "turned off, it stays off, kept in the config");
    setBrowserShown(true);
    expect(browserShown(), "and on again");

    useTestCredentialStore();
    storeApiToken(kTestToken);
    Account jay;
    jay.username = "jayladdermusic";
    jay.display_name = "Jay Ladder";
    storeAccount(jay);
    expect(storedAccount().username == "jayladdermusic" && storedAccount().display_name == "Jay Ladder",
           "the account is kept beside the token");
    storeApiToken(kTestToken);
    expect(storedAccount().known(), "the same token keeps it");
    storeApiToken("bpa_" + String::repeatedString("ab", 20));
    expect(!storedAccount().known(), "another token forgets it");
    storeApiToken({});
    expect(storedApiToken().isEmpty() && !storedAccount().known(), "logging out forgets both");
  }

  void testAccountIsAskedOfTheLibrary() {
    TestLibraryServer server;
    expect(server.isListening(), "the test library listens on 127.0.0.1");
    if (!server.isListening())
      return;
    server.answer("GET", "/api/v1/me/", 200,
                  R"({"username": "jayladdermusic", "display_name": "Jay Ladder", "presets": 3, "banks": 1})");
    atlas::community::Account account;
    String error = atlas::community::fetchAccount(server.url(), kTestToken, account);
    expect(error.isEmpty() && account.username == "jayladdermusic" && account.banks == 1,
           "the account is fetched: " + error.toStdString());
    const auto requests = server.requests();
    expect(requests.size() == 1 && requests[0].method == "GET" && requests[0].path == "/api/v1/me/" &&
               requests[0].headers["authorization"] == "Bearer " + kTestToken,
           "with the token as a Bearer header");
    server.answer("GET", "/api/v1/me/", 401, R"({"error": "A valid API token is required."})");
    error = atlas::community::fetchAccount(server.url(), kTestToken, account);
    expect(error.contains("refused") && !account.known(), "a refused token is said: " + error.toStdString());
    expect(atlas::community::fetchAccount("file:///saved/presets.json", kTestToken, account).contains("web address"),
           "a saved library has no accounts");
    expect(atlas::community::fetchAccount("http://atlas.blindproducers.org", kTestToken, account).contains("not using HTTPS") &&
               !account.known(),
           "an account is never asked for over plain http to another computer");
  }

  Component* titledIn(Component& parent, const String& title) { return titled(parent, title); }

  // The toolbar's Settings button opens Settings: Plugin info, Tuning,
  // MIDI learn mappings, Accessibility, Online community browser and Keyboard
  // help, each a group a screen reader enters, none of them flat in the
  // window. The browser can be hidden, which takes it out of the preset
  // browser; logging in and out asks the library whose token it is.
  void testSettingsWindow() {
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;
    const auto credentials = useTestCredentialStore();
    atlas::community::forgetBrowserShownForTesting();
    TestLibraryServer server;
    expect(server.isListening(), "the test library listens on 127.0.0.1");
    if (!server.isListening())
      return;
    server.answer("GET", "/api/v1/me/", 401, R"({"error": "A valid API token is required."})");
    server.answer("GET", "/api/v1/me/", 200,
                  R"({"username": "jayladdermusic", "display_name": "Jay Ladder", "presets": 3, "banks": 1})");
    atlas::community::setLibraryUrlForTesting(server.url());
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;

    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };
    auto heard = [&] { return spoken.messages.joinIntoString(" | ").toStdString(); };

    SynthPlugin synth;
    // The mappings the user's own config brought along go; the test's config is its own.
    synth.clearAllMidiLearn();
    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    // The editor is called Atlas, and nothing is read after its name.
    expect(editor.getTitle() == "Atlas" && editor.getDescription().isEmpty(),
           "the editor's name is Atlas alone, description: " + editor.getDescription().toStdString());

    // The Settings button, first in the Toolbar, opens Settings at once: no menu.
    Button* settings_button = nullptr;
    for (auto* component : editor.getKeyboardFocusOrder())
      if (auto* button = dynamic_cast<Button*>(component); button != nullptr && button->getTitle() == "Settings")
        settings_button = button;
    const auto& keyboard_order = editor.getKeyboardFocusOrder();
    expect(settings_button != nullptr && settings_button->getButtonText() == "Settings" &&
               settings_button->getDescription().isEmpty() && keyboard_order.size() > 1 &&
               keyboard_order[0]->getTitle() == "Toolbar" && keyboard_order[1] == settings_button,
           "the editor starts with the Toolbar, the Settings button first in it");
    if (settings_button != nullptr)
      settings_button->triggerClick();
    pumpUntil([&] { return editor.settingsWindowForTesting() != nullptr; }, 5000);
    Component::SafePointer<DialogWindow> window(editor.settingsWindowForTesting());
    pumpEditorEvents();
    auto* content = window != nullptr ? window->getContentComponent() : nullptr;
    expect(content != nullptr && window->getName() == "Atlas Settings", "the Settings button opens the Settings window");
    if (content == nullptr) {
      editor.removeFromDesktop();
      return;
    }

    // Each group holds its controls, and the window holds the groups: a
    // screen reader enters one group at a time instead of reading one flat list.
    auto childTitles = [](AccessibilityHandler* handler) {
      StringArray titles;
      if (handler != nullptr)
        for (auto* child : handler->getChildren())
          titles.add(child->getTitle());
      return titles;
    };
    const StringArray window_children = childTitles(window->getAccessibilityHandler());
    const std::pair<const char*, const char*> groups[] = {
      { "Plugin info", "Report an issue on GitHub" },
      { "Tuning", "Load tuning file..." },
      { "Default patch", "Browse default patch..." },
      { "MIDI learn mappings", nullptr },
      { "Accessibility", "Other announcements" },
      { "Online community browser", "Show the community library in the preset browser" },
      { "Keyboard help", "Keyboard shortcuts" } };
    for (const auto& [title, control] : groups) {
      auto* group = titledIn(*content, title);
      auto* handler = group != nullptr ? group->getAccessibilityHandler() : nullptr;
      expect(handler != nullptr && handler->getRole() == AccessibilityRole::group && group->isFocusContainer(),
             std::string("the window has the group ") + title);
      expect(window_children.contains(title), std::string("the window's elements include the group ") + title);
      if (control == nullptr)
        continue;
      const StringArray inside = childTitles(handler);
      expect(inside.contains(control) && !window_children.contains(control),
             std::string(control) + " is inside " + title + ", not loose in the window: " +
                 inside.joinIntoString(", ").toStdString());
#if JUCE_MAC
      // What VoiceOver itself is given when it enters the group.
      if (handler != nullptr)
        expect(nativeAccessibilityChildTitles(*handler).contains(control),
               std::string("VoiceOver finds ") + control + " inside " + title);
#endif
    }
    expect(!window_children.contains("Undo and redo") && titledIn(*content, "Undo and redo") == nullptr,
           "Undo and Redo are in the toolbar, not in Settings");
    expect(!window_children.contains("Atlas settings"),
           "the window's layout is no element of its own: " + window_children.joinIntoString(", ").toStdString());

    // Tuning: the standard one, with nothing to clear.
    auto* tuning_name = dynamic_cast<Label*>(titledIn(*content, "Current tuning"));
    auto* clear_tuning = dynamic_cast<Button*>(titledIn(*content, "Clear tuning"));
    expect(tuning_name != nullptr && tuning_name->getText() == "Tuning: standard twelve-tone equal temperament." &&
               clear_tuning != nullptr && !clear_tuning->isVisible(),
           "Tuning says the standard tuning, with no Clear tuning");

    // Default patch: none chosen, so Browse and no Reset.
    auto* default_status = dynamic_cast<Label*>(titledIn(*content, "Current default patch"));
    auto* browse_default = dynamic_cast<Button*>(titledIn(*content, "Browse default patch..."));
    auto* reset_default = dynamic_cast<Button*>(titledIn(*content, "Reset default patch"));
    expect(default_status != nullptr && default_status->getText() == "No default patch chosen." &&
               browse_default != nullptr && browse_default->isVisible() &&
               reset_default != nullptr && !reset_default->isVisible(),
           "Default patch says none is chosen, with Browse default patch and no Reset default patch");

    // MIDI learn mappings: none yet.
    auto* midi_status = dynamic_cast<Label*>(titledIn(*content, "MIDI learn mapping count"));
    expect(midi_status != nullptr && midi_status->getText().startsWith("No MIDI learn mappings."),
           "MIDI learn mappings says there are none");

    // Plugin info.
    auto* version = dynamic_cast<Label*>(titledIn(*content, "Version"));
    expect(version != nullptr && version->getText() == "Atlas " + SynthEditor::versionText() &&
               SynthEditor::versionText().startsWith(ProjectInfo::versionString),
           "the version is shown: " + (version != nullptr ? version->getText().toStdString() : std::string()));
    expect(version != nullptr && version->hasKeyboardFocus(false), "the version has the focus first");
    std::cout << "  version: " << SynthEditor::versionText() << '\n';
    const std::pair<const char*, const char*> links[] = {
      { "Report an issue on GitHub", "https://github.com/BlindProducers/atlas/issues/new" },
      { "Source code on GitHub", "https://github.com/BlindProducers/atlas in your browser" },
      { "Releases on GitHub", "https://github.com/BlindProducers/atlas/releases" } };
    for (const auto& [title, url] : links) {
      auto* link = dynamic_cast<Button*>(titledIn(*content, title));
      expect(link != nullptr && link->getDescription().contains(url), std::string("the link ") + title + " opens " + url);
    }

    // Accessibility: the speech categories, which change the settings.
    auto* accessibility = titledIn(*content, "Accessibility");
    int toggles = 0;
    if (accessibility != nullptr) {
      for (auto* child : accessibility->getChildren())
        if (dynamic_cast<ToggleButton*>(child) != nullptr)
          ++toggles;
    }
    expect(toggles == 10, "the Accessibility group has the ten speech settings, got " + std::to_string(toggles));
    auto* other = dynamic_cast<Button*>(titledIn(*content, "Other announcements"));
    const bool other_was = atlas::speech::settings().other;
    if (other != nullptr) {
      other->triggerClick();
      pumpEditorEvents();
    }
    expect(other != nullptr && atlas::speech::settings().other != other_was, "a toggle changes its setting");
    atlas::speech::settings().other = other_was;
    if (other != nullptr)
      other->setToggleState(other_was, dontSendNotification);

    // Use speech feedback switches every announcement category with it, and a
    // category switched on switches it back on. Frequency values in Hz, how
    // values are read, is on for a new user and stays as it is.
    {
      auto& speech = atlas::speech::settings();
      const auto speech_before = speech;
      auto* master = dynamic_cast<Button*>(titledIn(*content, "Use speech feedback"));
      auto* navigation = dynamic_cast<Button*>(titledIn(*content, "Navigation announcements"));
      auto* hz = dynamic_cast<Button*>(titledIn(*content, "Frequency values in Hz"));
      expect(master != nullptr && navigation != nullptr && other != nullptr && hz != nullptr && hz->getToggleState() &&
                 speech.frequency_values_in_hz,
             "Settings has the speech switches, with Frequency values in Hz on for a new user");
      if (master != nullptr && navigation != nullptr && other != nullptr && hz != nullptr) {
        master->triggerClick();
        pumpEditorEvents();
        expect(!master->getToggleState() && !navigation->getToggleState() && !other->getToggleState() &&
                   !speech.speech_feedback && !speech.navigation && !speech.modulation && !speech.lfo_editor &&
                   hz->getToggleState() && speech.frequency_values_in_hz,
               "Use speech feedback off turns every category off, and leaves Frequency values in Hz on");
        navigation->triggerClick();
        pumpEditorEvents();
        expect(master->getToggleState() && navigation->getToggleState() && !other->getToggleState() &&
                   speech.speech_feedback && speech.navigation && !speech.other,
               "Navigation announcements on switches speech feedback back on, and only that category");
        master->triggerClick();
        pumpEditorEvents();
        master->triggerClick();
        pumpEditorEvents();
        expect(master->getToggleState() && navigation->getToggleState() && other->getToggleState() &&
                   speech.other && speech.parameters,
               "Use speech feedback on turns every category on");
      }
      // Back as they were, the switches with them.
      speech = speech_before;
      using Settings = atlas::speech::Settings;
      const std::pair<const char*, bool Settings::*> switches[] = {
        { "Use speech feedback", &Settings::speech_feedback },
        { "Navigation announcements", &Settings::navigation },
        { "Preset announcements", &Settings::presets },
        { "LFO and Trance gate editor announcements", &Settings::lfo_editor },
        { "Wavetable editor announcements", &Settings::wavetable_editor },
        { "Sample and granular announcements", &Settings::sample_browser },
        { "Modulation announcements", &Settings::modulation },
        { "Parameter value announcements", &Settings::parameters },
        { "Other announcements", &Settings::other },
        { "Frequency values in Hz", &Settings::frequency_values_in_hz } };
      for (const auto& [title, field] : switches)
        if (auto* toggle = dynamic_cast<Button*>(titledIn(*content, title)))
          toggle->setToggleState(speech.*field, dontSendNotification);
    }

    // Online community browser.
    auto* show = dynamic_cast<Button*>(titledIn(*content, "Show the community library in the preset browser"));
    auto* account = dynamic_cast<Label*>(titledIn(*content, "Account"));
    auto* storage = dynamic_cast<Label*>(titledIn(*content, "API token storage"));
    auto* token = dynamic_cast<TextEditor*>(titledIn(*content, "API token"));
    auto* log_in = dynamic_cast<Button*>(titledIn(*content, "Log in"));
    auto* log_out = dynamic_cast<Button*>(titledIn(*content, "Log out"));
    auto* website = dynamic_cast<Button*>(titledIn(*content, "Open 127.0.0.1"));
    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    const bool controls = show != nullptr && account != nullptr && storage != nullptr && token != nullptr &&
                          log_in != nullptr && log_out != nullptr && website != nullptr && collection != nullptr;
    expect(controls, "the community group has its controls");
    if (!controls) {
      delete window.getComponent();
      editor.removeFromDesktop();
      return;
    }
    expect(show->getToggleState() && collection->getNumRows() == 4, "the browser is shown by default");
    expect(account->isShowing() && account->getText() == "Not logged in." && token->isShowing() && log_in->isShowing() &&
               !log_out->isVisible() && website->isShowing() && website->getDescription().contains(server.url()),
           "not logged in: the token box, Log in and the website: " + account->getText().toStdString());
    expect(storage->isShowing() && storage->getText().startsWith("Logging in keeps your API token in the test store, for ") &&
               log_in->getDescription().endsWith(" and keeps it in the test store"),
           "and where a token would be kept: " + storage->getText().toStdString());
    expect(token->getAccessibilityHandler() != nullptr && token->getAccessibilityHandler()->getTextInterface() != nullptr &&
               token->getAccessibilityHandler()->getTextInterface()->isDisplayingProtectedText(),
           "the token box hides what is typed");

    show->triggerClick();
    pumpEditorEvents();
    expect(!atlas::community::browserShown() && collection->getNumRows() == 2 && !account->isVisible() &&
               !token->isVisible() && !website->isVisible(),
           "hidden: the Collection list has two rows and the group only the toggle");
    show->triggerClick();
    pumpEditorEvents();
    expect(atlas::community::browserShown() && collection->getNumRows() == 4 && token->isShowing(),
           "shown again: the community rows and controls are back");

    // Log in: a refused token, then one that works.
    spoken.messages.clear();
    log_in->onClick();
    pumpEditorEvents();
    expect(said("Paste your API token first") && server.requests().empty(), "Log in without a token asks for one");
    token->setText(kTestToken, true);
    spoken.messages.clear();
    log_in->onClick();
    pumpUntil([&] { return said("Unable to log in"); }, 10000);
    expect(said("Unable to log in. The library refused the API token") && atlas::community::storedApiToken().isEmpty() &&
               account->getText().startsWith("Not logged in."),
           "a refused token is said and not kept: " + heard());
    spoken.messages.clear();
    log_in->onClick();
    pumpUntil([&] { return said("Logged in"); }, 10000);
    expect(said("Logged in as Jay Ladder (jayladdermusic)") && atlas::community::storedApiToken() == kTestToken &&
               atlas::community::storedAccount().username == "jayladdermusic",
           "a token that works is kept with its account: " + heard());
    expect(account->getText() == "Logged in as Jay Ladder (jayladdermusic). 3 presets and 1 bank shared." &&
               log_out->isShowing() && !token->isVisible() && !log_in->isVisible() && token->getText().isEmpty(),
           "the account is shown, with Log out in place of the token box: " + account->getText().toStdString());
    expect(credentials->secretOf("com.example.daw") == kTestToken.toStdString() &&
               storage->getText().startsWith("Your API token is kept in the test store, for ") &&
               storage->getText().endsWith(" only. Each DAW logs in on its own.") &&
               log_out->getDescription() == "Removes the API token from the test store",
           "the token is in the store, and Settings says where: " + storage->getText().toStdString());
    for (const auto& message : spoken.messages)
      expect(!message.contains(kTestToken) && !message.contains("bpa_"), "the token is never said");

    // While Settings is closed: a tuning and two MIDI learn mappings.
    delete window.getComponent();
    pumpEditorEvents();
    const File scale = library.folder.getChildFile("Test Fifths.scl");
    scale.replaceWithText("! Test Fifths.scl\nFifths only\n 1\n!\n 701.955\n");
    synth.loadTuningFile(scale);
    prepare(synth);
    auto learn = [&](const char* parameter, int controller) {
      synth.armMidiLearn(parameter);
      MidiBuffer midi;
      midi.addEvent(MidiMessage::controllerEvent(1, controller, 100), 0);
      renderMidi(synth, midi);
      synth.midiLearnMappings();
    };
    learn("osc_1_tune", 16);
    learn("osc_2_tune", 17);
    expect(synth.midiLearnMappings().size() == 2, "two MIDI learn mappings are made");

    // Opened again, it asks the library for the account behind the saved token.
    window = editor.showSettingsForTesting();
    pumpEditorEvents();
    content = window != nullptr ? window->getContentComponent() : nullptr;
    account = content != nullptr ? dynamic_cast<Label*>(titledIn(*content, "Account")) : nullptr;
    pumpUntil([&] { return account != nullptr && account->getText().contains("shared"); }, 10000);
    expect(account != nullptr && account->getText() == "Logged in as Jay Ladder (jayladdermusic). 3 presets and 1 bank shared.",
           "the saved token's account is checked when Settings opens");

    // The tuning, which Clear tuning takes back to the standard one.
    tuning_name = content != nullptr ? dynamic_cast<Label*>(titledIn(*content, "Current tuning")) : nullptr;
    clear_tuning = content != nullptr ? dynamic_cast<Button*>(titledIn(*content, "Clear tuning")) : nullptr;
    expect(tuning_name != nullptr && tuning_name->getText() == "Tuning: Test Fifths." && clear_tuning != nullptr &&
               clear_tuning->isVisible(),
           "Tuning names the loaded tuning, with Clear tuning: " +
               (tuning_name != nullptr ? tuning_name->getText().toStdString() : std::string()));
    if (clear_tuning != nullptr && tuning_name != nullptr) {
      spoken.messages.clear();
      clear_tuning->onClick();
      pumpEditorEvents();
      expect(synth.getTuning()->isDefault() && tuning_name->getText() == "Tuning: standard twelve-tone equal temperament." &&
                 !clear_tuning->isVisible() && said("Tuning cleared"),
             "Clear tuning goes back to the standard tuning and says so: " + heard());
    }

    // The mappings, a Remove button each, and Remove all.
    auto mappingButtons = [&] {
      std::vector<Button*> buttons;
      if (auto* group = content != nullptr ? titledIn(*content, "MIDI learn mappings") : nullptr)
        for (auto* child : group->getChildren())
          if (auto* button = dynamic_cast<Button*>(child);
              button != nullptr && button->isVisible() && button->getTitle().startsWith("Remove CC"))
            buttons.push_back(button);
      return buttons;
    };
    midi_status = content != nullptr ? dynamic_cast<Label*>(titledIn(*content, "MIDI learn mapping count")) : nullptr;
    auto* remove_all = content != nullptr ? dynamic_cast<Button*>(titledIn(*content, "Remove all MIDI learn mappings"))
                                          : nullptr;
    auto buttons = mappingButtons();
    expect(midi_status != nullptr && midi_status->getText() == "2 MIDI learn mappings." && buttons.size() == 2 &&
               buttons[0]->getTitle().startsWith("Remove CC 16 to ") && buttons[1]->getTitle().startsWith("Remove CC 17 to ") &&
               remove_all != nullptr && remove_all->isVisible(),
           "MIDI learn mappings lists both, each with its Remove button");
    if (buttons.size() == 2 && midi_status != nullptr && remove_all != nullptr) {
      const String second = buttons[1]->getTitle();
      spoken.messages.clear();
      buttons[0]->onClick();
      pumpEditorEvents();
      buttons = mappingButtons();
      expect(synth.midiLearnMappings().size() == 1 && midi_status->getText() == "1 MIDI learn mapping." &&
                 buttons.size() == 1 && buttons[0]->getTitle() == second && said("Removed CC 16 to "),
             "Remove takes one mapping away and says so: " + heard());
      spoken.messages.clear();
      remove_all->onClick();
      pumpEditorEvents();
      expect(synth.midiLearnMappings().empty() && midi_status->getText().startsWith("No MIDI learn mappings.") &&
                 mappingButtons().empty() && !remove_all->isVisible() && said("Removed all MIDI learn mappings"),
             "Remove all takes the rest and says so: " + heard());
    }

    // Log out.
    log_out = content != nullptr ? dynamic_cast<Button*>(titledIn(*content, "Log out")) : nullptr;
    spoken.messages.clear();
    if (log_out != nullptr)
      log_out->onClick();
    pumpEditorEvents();
    expect(said("Logged out") && atlas::community::storedApiToken().isEmpty() && !atlas::community::storedAccount().known() &&
               !credentials->holds("com.example.daw") && account != nullptr && account->getText() == "Not logged in.",
           "Log out forgets the token, in the store too, and its account: " + heard());

    // The keyboard help, whose text box hands Escape to the window: it closes.
    auto* help = content != nullptr ? dynamic_cast<TextEditor*>(titledIn(*content, "Keyboard shortcuts")) : nullptr;
    expect(help != nullptr && help->getText().contains("the toolbar's Undo and Redo do the same") &&
               help->getText().contains("Escape - Close Settings") && !help->getText().contains("Editor menu"),
           "the keyboard help is in Settings and names it");
    if (help != nullptr) {
      help->keyPressed(KeyPress(KeyPress::escapeKey));
      pumpUntil([&] { return window == nullptr; }, 5000);
    }
    expect(window == nullptr, "Escape in the keyboard help closes Settings");

    editor.removeFromDesktop();
  }

  void testSettingsCloseWithTheEditor() {
    SynthPlugin synth;
    Component::SafePointer<DialogWindow> window;
    {
      SynthEditor editor(synth);
      editor.updateFullGui();
      editor.addToDesktop(0);
      editor.setVisible(true);
      pumpEditorEvents();
      window = editor.showSettingsForTesting();
      pumpEditorEvents();
      expect(window != nullptr, "Settings opens");
      editor.removeFromDesktop();
    }
    pumpEditorEvents();
    expect(window == nullptr, "and closes when the editor does");
  }

  // The system's credential store itself, under a service of the tests' own:
  // a secret is saved, read, replaced and removed. Keychain dialogs are turned
  // off for this process, so where the keychain would ask (locked, as on a
  // build machine) the test is skipped, as it is where there is no store.
  int runCredentialStoreTests() {
    using atlas::credentials::Status;
    atlas::credentials::disallowInteractionForTesting();
#if JUCE_MAC
    // The tests are signed ad hoc, as a self-built host is: the plugin gives
    // such an app no keychain, which would ask for the user's password after
    // each new build.
    const auto guarded = atlas::credentials::systemStore("org.blindproducers.atlas.tests");
    expect(guarded->place().empty() && guarded->unavailableReason().find("no developer signature") != std::string::npos,
           "an app signed ad hoc gets no keychain: " + guarded->unavailableReason());
#endif
    const auto store = atlas::credentials::systemStore("org.blindproducers.atlas.tests", false);
    const std::string account = "round-trip-" + Uuid().toString().toStdString();
    const std::string first = "not-a-token-" + Uuid().toString().toStdString();
    const std::string second = first + "-replaced";
    std::string error;
    const Status written = store->write(account, "Atlas tests", first, error);
    if (written == Status::unavailable || written == Status::refused) {
      std::cout << "Atlas credential store test skipped: " << (error.empty() ? std::string("no store") : error) << '\n';
      return 0;
    }
    expect(written == Status::ok, "a secret is saved in " + store->place());
    std::string secret;
    expect(store->read(account, secret, error) == Status::ok && secret == first, "and read back: " + error);
    expect(store->write(account, "Atlas tests", second, error) == Status::ok &&
               store->read(account, secret, error) == Status::ok && secret == second,
           "saving again replaces it: " + error);
    expect(store->remove(account, error) == Status::ok, "it is removed: " + error);
    expect(store->read(account, secret, error) == Status::not_found && store->remove(account, error) == Status::not_found,
           "and is gone");
    store->remove(account, error);   // whatever failed above, nothing is left behind
    if (failures == 0)
      std::cout << "Atlas credential store tests passed (" << store->place() << ")\n";
    return failures == 0 ? 0 : 1;
  }

  // A download's progress is told to its caller as the bytes land, and a
  // caller that says stop ends it with nothing left behind. In the browser,
  // Download progress and Cancel download show after the Autoload toggle while
  // a download is on, the bar's name carries the percentage, Cancel download
  // stops the download and puts the focus back on the preset list, and the
  // same preset asked for again then lands and loads.
  void testDownloadProgressAndCancel() {
    TestLibraryServer server;
    expect(server.isListening(), "the test library listens on 127.0.0.1");
    if (!server.isListening())
      return;
    TemporaryUndoLibrary library;
    expect(library.isolated(), "the test's library is its own temporary folder");
    if (!library.isolated())
      return;

    // The library's file: a patch as saved, which arrives in many chunks.
    SynthPlugin synth;
    const File saved = library.folder.getChildFile("SavedLibrary");
    saved.createDirectory();
    const File source = saved.getChildFile("Big Lead.atp");
    expect(synth.saveCopyToFile(source), "a preset is saved to stand in for the library's file");
    const String body = source.loadFileAsString();
    const String sha256 = sha256OfFile(source);
    const int64 size = source.getSize();
    expect(size > 16 * 1024, "the preset is bigger than a few chunks: " + std::to_string(size) + " bytes");
    const String file_path = "/media/big.atp";
    const URL file_url(server.url() + file_path);

    // The library function hears the bytes as they land, of the size listed.
    server.answer("GET", file_path, 200, body, 4096, 5);
    const File target = library.folder.getChildFile("Downloads").getChildFile("Big Lead.atp");
    const File part = target.getSiblingFile("Big Lead.atp.part");
    std::vector<std::pair<int64, int64>> steps;
    String error = atlas::community::downloadFile(file_url, target, sha256, size, nullptr,
                                                  [&](int64 received, int64 total) {
                                                    steps.emplace_back(received, total);
                                                    return true;
                                                  });
    expect(error.isEmpty() && target.existsAsFile() && sha256OfFile(target) == sha256,
           "the download lands: " + error.toStdString());
    bool steps_in_order = steps.size() >= 3 && steps.front().first == 0 && steps.back().first == size;
    for (size_t i = 0; i < steps.size(); ++i) {
      steps_in_order = steps_in_order && steps[i].second == size &&
                       (i == 0 || steps[i].first >= steps[i - 1].first);
    }
    expect(steps_in_order, "the progress runs from nothing to the whole, of the listed size, in " +
                               std::to_string(steps.size()) + " steps");

    // A caller that says stop: the download ends with nothing left behind.
    target.deleteFile();
    server.answer("GET", file_path, 200, body, 4096, 5);
    int calls = 0;
    error = atlas::community::downloadFile(file_url, target, sha256, size, nullptr,
                                           [&](int64, int64) { return ++calls < 3; });
    expect(error == "Cancelled" && !target.existsAsFile() && !part.existsAsFile(),
           "stopped, the download says Cancelled and leaves no file and no part file: " + error.toStdString());

    // The browser, with the library listing the preset on the test server.
    json page;
    page["count"] = 1;
    page["next"] = nullptr;
    page["results"] = json::array();
    json entry = communityPresetJson(1, "Big Lead", source, sha256, "lead", "Lead", { "test" });
    entry["file"]["url"] = file_url.toString(false).toStdString();
    page["results"].push_back(entry);
    server.answer("GET", "/api/v1/presets/?kind=patch&page_size=200&page=1", 200, String(page.dump()));
    atlas::community::setLibraryUrlForTesting(server.url());
    struct RealLibraryAgain { ~RealLibraryAgain() { atlas::community::setLibraryUrlForTesting({}); } } real_library_again;
    SpokenMessages spoken;
    auto said = [&](const String& words) {
      for (const auto& message : spoken.messages)
        if (message.contains(words))
          return true;
      return false;
    };
    auto heard = [&] { return spoken.messages.joinIntoString(" | ").toStdString(); };

    SynthEditor editor(synth);
    editor.updateFullGui();
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.toFront(true);
    pumpEditorEvents();
    editor.refreshPresetLibrary();
    pumpUntil([&] { return !editor.isPresetLibraryBusy(); }, 60000);

    auto* collection = dynamic_cast<TableListBox*>(titled(editor, "Collection"));
    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    auto* progress = titled(editor, "Download progress");
    auto* cancel = dynamic_cast<Button*>(titled(editor, "Cancel download"));
    const bool browser = collection != nullptr && results != nullptr && autoload != nullptr && progress != nullptr &&
                         cancel != nullptr;
    expect(browser, "the editor has its preset browser, with Download progress and Cancel download");
    if (!browser) {
      editor.removeFromDesktop();
      return;
    }
    const bool autoload_was_on = autoload->getToggleState();
    autoload->setToggleState(false, dontSendNotification);
    auto finish = [&] {
      autoload->setToggleState(autoload_was_on, dontSendNotification);
      editor.removeFromDesktop();
    };
    // Where Tab goes, in order.
    auto tabOrder = [&] { return static_cast<Component&>(editor).createKeyboardFocusTraverser()->getAllComponents(&editor); };
    auto tabIndex = [&](const std::vector<Component*>& order, Component* component) {
      const auto found = std::find(order.begin(), order.end(), component);
      return found == order.end() ? -1 : static_cast<int>(found - order.begin());
    };
    auto order = tabOrder();
    expect(!progress->isVisible() && !cancel->isVisible() && tabIndex(order, progress) < 0 && tabIndex(order, cancel) < 0,
           "while nothing downloads, the bar and Cancel download are hidden and out of the Tab order");

    collection->selectRow(2);
    pumpEditorEvents();
    pumpUntil([&] { return !editor.isCommunityLibraryBusy(); }, 10000);
    expect(editor.presetRowCount() == 1, "the community list arrives with the preset: " +
                                             editor.communityLibraryError().toStdString());
    if (editor.presetRowCount() != 1) {
      finish();
      return;
    }
    const File expected = library.folder.getChildFile("Community").getChildFile("Banks").getChildFile("tester")
                              .getChildFile("Lead").getChildFile("Big Lead.atp");
    results->selectRow(0);
    pumpEditorEvents();
    editor.ensureComponentVisible(results);
    results->grabKeyboardFocus();
    pumpEditorEvents();

    // A slow download: the bar and Cancel download show at once, after the
    // Autoload toggle, and the bar is named with how much has arrived.
    server.answer("GET", file_path, 200, body, 2048, 40);
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the row is handled");
    expect(editor.isCommunityDownloadBusy() && progress->isVisible() && cancel->isVisible(),
           "the bar and Cancel download show as the download starts");
    expect(said("Downloading Big Lead from 127.0.0.1, "), "the download is said with its size: " + heard());
    order = tabOrder();
    expect(tabIndex(order, progress) == tabIndex(order, autoload) + 1 && tabIndex(order, cancel) == tabIndex(order, progress) + 1,
           "Tab reaches the bar after the Autoload toggle, and Cancel download after the bar");
    pumpUntil([&] { return editor.downloadProgressPercentForTesting() > 0; }, 10000);
    const int percent = editor.downloadProgressPercentForTesting();
    expect(percent > 0 && percent <= 100 && progress->getAccessibilityHandler() != nullptr &&
               progress->getAccessibilityHandler()->getTitle() == "Download progress, " + String(percent) + " percent",
           "the bar's name carries the percentage that has arrived: " + std::to_string(percent));
    expect(editor.isCommunityDownloadBusy(), "the download is still on");

    // Cancel download, from the button: the download is dropped and said to
    // be, the bar and button go, the focus is back on the preset list, and
    // nothing is left behind.
    cancel->grabKeyboardFocus();
    pumpEditorEvents();
    spoken.messages.clear();
    cancel->onClick();
    expect(!editor.isCommunityDownloadBusy() && !progress->isVisible() && !cancel->isVisible(),
           "Cancel download drops the download, and the bar and button go");
    expect(said("Download of Big Lead cancelled"), "the cancel is said: " + heard());
    expect(results->hasKeyboardFocus(false), "the focus is back on the preset list");
    const File expected_part = expected.getSiblingFile("Big Lead.atp.part");
    pumpUntil([&] { return !expected_part.existsAsFile(); }, 5000);
    pumpUntil([&] { return false; }, 300);   // the thread's last chunk
    expect(!expected.existsAsFile() && !expected_part.existsAsFile(),
           "the cancelled download leaves no file and no part file");
    expect(!said("Downloaded Big Lead") && !said("Unable to download"), "and nothing more is said of it: " + heard());

    // Asked for again, at full speed: it lands and loads, and the bar goes.
    server.answer("GET", file_path, 200, body);
    spoken.messages.clear();
    expect(results->keyPressed(KeyPress(KeyPress::returnKey)), "Return on the row is handled again");
    pumpUntil([&] { return !editor.isCommunityDownloadBusy() && synth.getActiveFile() == expected; }, 15000);
    expect(expected.existsAsFile() && sha256OfFile(expected) == sha256 && synth.getActiveFile() == expected,
           "the download lands and loads: " + synth.getActiveFile().getFullPathName().toStdString());
    expect(!progress->isVisible() && !cancel->isVisible() && said("Downloaded Big Lead"),
           "the bar and Cancel download go once it has landed, and the download is said: " + heard());
    finish();
  }

  int runCommunityLibraryTests() {
    testSha256MatchesTheStandardVectors();
    testPresetsPageParses();
    testDownloadsLandInTheCommunityFolder();
    testDownloadRecords();
    testSavedLibraryRoundTrip();
    testCommunityCollectionListsAndDownloads();
    testDownloadProgressAndCancel();
    testCommunityPreviewPlays();
    testBanksPageParses();
    testCommunityBanksListAndImport();
    testShareRequestsAndAnswers();
    testApiTokenIsKeptInTheCredentialStore();
    testUploadReachesTheLibrary();
    testShareFormsUploadAndSpeak();
    testAccountsAndTheBrowserSetting();
    testAccountIsAskedOfTheLibrary();
    testSettingsWindow();
    testSettingsCloseWithTheEditor();
    if (failures == 0)
      std::cout << "Atlas community library tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}

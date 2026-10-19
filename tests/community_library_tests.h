#pragma once

// The community library: the preset browser's Community collection lists the
// presets shared on atlas.blindproducers.org and Return downloads one, checks
// it against the library's checksum and loads it. A saved page and a saved
// preset file stand in for the library, so none of this needs a network.
// Included after editor_accessibility_tests.h, editor_speech_tests.h and
// undo_tests.h, whose helpers it uses.

#include "community_library.h"

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
    expect(community == data.getChildFile("Community").getChildFile("Presets"),
           "downloads live in Community/Presets under the data folder");
    const File glow = atlas::community::localFile(presets[0], data);
    expect(glow == community.getChildFile("jay").getChildFile("Lead").getChildFile("Glow Lead.atp"),
           "a preset lands under its uploader and style: " + glow.getFullPathName().toStdString());
    const File sneaky = atlas::community::localFile(presets[1], data);
    expect(sneaky.isAChildOf(community) && sneaky.getParentDirectory().getParentDirectory().getParentDirectory() == community,
           "dots and slashes in the names stay inside the folder: " + sneaky.getFullPathName().toStdString());
    expect(sneaky.getParentDirectory().getFileName() == "Other" &&
               sneaky.getParentDirectory().getParentDirectory().getFileName() == "root",
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
    auto* categories = dynamic_cast<TableListBox*>(titled(editor, "Category"));
    auto* tags = dynamic_cast<TableListBox*>(titled(editor, "Tags"));
    auto* results = dynamic_cast<TableListBox*>(titled(editor, "Preset results table"));
    auto* search = dynamic_cast<TextEditor*>(titled(editor, "Preset search"));
    auto* autoload = dynamic_cast<Button*>(titled(editor, "Autoload preset when scrolling"));
    const bool browser = made && collection != nullptr && libraries != nullptr && categories != nullptr &&
                         tags != nullptr && results != nullptr && search != nullptr && autoload != nullptr;
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

    // Into the Community collection: the list is fetched and shown.
    expect(collection->getNumRows() == 3 && collection->getTableListBoxModel()->getCellTooltip(2, 1) == "Community",
           "the Collection list ends with Community");
    collection->selectRow(2);
    pumpEditorEvents();
    pumpUntil([&] { return !editor.isCommunityLibraryBusy(); }, 10000);
    expect(editor.isCommunityLibraryLoaded(),
           "the community list arrives: " + editor.communityLibraryError().toStdString());
    expect(synth.getEditorMemory().preset_collection == "Community", "the collection is remembered for the instance");
    expect(said("Community presets loaded, 2 found"), "the list's arrival is said");
    expect(editor.presetRowCount() == 2, "both shared presets are listed, got " + std::to_string(editor.presetRowCount()));
    if (editor.presetRowCount() != 2) {
      finish();
      return;
    }
    const File expected = library.folder.getChildFile("Community").getChildFile("Presets").getChildFile("tester")
                              .getChildFile("Lead").getChildFile("Shared Lead.atp");
    // Sorted by name: Broken Bass, then Shared Lead.
    expect(editor.presetFileAtRow(1) == expected,
           "a row is where its download lands: " + editor.presetFileAtRow(1).getFullPathName().toStdString());
    expect(cell(1, kName) == "Shared Lead" && cell(1, kAuthor) == "Tester" && cell(1, kDownloaded) == "Not downloaded" &&
               cell(1, kTags) == "glow, test" && cell(1, kCategory) == "Lead" && cell(1, kBank) == "tester" &&
               cell(1, kLibrary) == "Community",
           "the row shows the name, author, download state, tags, style, uploader and library: " +
               (cell(1, kName) + " | " + cell(1, kAuthor) + " | " + cell(1, kDownloaded) + " | " + cell(1, kTags) + " | " +
                cell(1, kCategory) + " | " + cell(1, kBank) + " | " + cell(1, kLibrary)).toStdString());
    expect(results->getHeader().getColumnName(kDownloaded) == "Downloaded", "the Favourite column is named Downloaded");
    expect(facetRows(*libraries) == StringArray { "All libraries", "Community" } &&
               facetRows(*categories) == StringArray { "All categories", "Bass", "Lead" } &&
               facetRows(*tags) == StringArray { "All tags", "glow", "test" },
           "the Library, Category and Tags lists come from the library's styles and tags: " +
               (facetRows(*libraries).joinIntoString(", ") + " / " + facetRows(*categories).joinIntoString(", ") + " / " +
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
           "the preset is downloaded into Community/Presets/tester/Lead");
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
    const File refused = library.folder.getChildFile("Community").getChildFile("Presets").getChildFile("tester")
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

  int runCommunityLibraryTests() {
    testSha256MatchesTheStandardVectors();
    testPresetsPageParses();
    testDownloadsLandInTheCommunityFolder();
    testSavedLibraryRoundTrip();
    testCommunityCollectionListsAndDownloads();
    testCommunityPreviewPlays();
    if (failures == 0)
      std::cout << "Atlas community library tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "multisample_container.h"

#include <algorithm>
#include <filesystem>
#include <mutex>

namespace vital {
namespace multisample {
namespace container {

  namespace {
    // JUCE's zip writer has no ZIP64 support.
    constexpr int64_t kMaxContainerBytes = 0xFFFFFFFFll - (64ll << 20);
    constexpr int kMaxContainerEntries = 65000;
    constexpr const char* kCompleteMarker = ".complete";

    std::mutex& cacheRootMutex() {
      static std::mutex mutex;
      return mutex;
    }

    juce::File& cacheRootOverride() {
      static juce::File folder;
      return folder;
    }

    // Each instrument has one folder in the cache; never extract two at once.
    std::mutex& cacheWriteMutex() {
      static std::mutex mutex;
      return mutex;
    }

    std::string entryName(const std::string& hash, const std::string& relative) {
      return "multisample/" + hash + "/" + relative;
    }

    bool safeRelative(const std::string& relative) {
      if (relative.empty() || relative[0] == '/' || relative[0] == '\\' || relative.find(':') != std::string::npos)
        return false;
      juce::StringArray parts = juce::StringArray::fromTokens(juce::String(relative), "/\\", "");
      for (const juce::String& part : parts) {
        if (part == "..")
          return false;
      }
      return true;
    }

    bool isTextFile(const juce::String& name) {
      return name.endsWithIgnoreCase(".sfz") || name.endsWithIgnoreCase(".sfzh") ||
             name.endsWithIgnoreCase(".txt") || name.endsWithIgnoreCase(".json");
    }

    bool finishCacheFolder(const juce::File& partial, const std::string& hash) {
      const juce::File folder = cacheFolder(hash);
      if (folder.exists())
        folder.deleteRecursively();
      if (!partial.moveFileTo(folder))
        return false;
      return folder.getChildFile(kCompleteMarker).replaceWithText(juce::String(hash));
    }

    int64_t folderSize(const juce::File& folder) {
      int64_t total = 0;
      for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
        total += entry.getFileSize();
      return total;
    }
  } // namespace

  bool isContainer(const juce::File& file) {
    juce::FileInputStream stream(file);
    if (!stream.openedOk() || stream.getTotalLength() < 4)
      return false;
    char magic[4] = {};
    stream.read(magic, 4);
    return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
  }

  bool readPresetText(const juce::File& file, std::string& text, int64_t max_bytes) {
    std::unique_ptr<juce::InputStream> stream;
    std::unique_ptr<juce::ZipFile> zip;
    if (isContainer(file)) {
      zip = std::make_unique<juce::ZipFile>(file);
      const int index = zip->getIndexOfFileName(kPresetEntry);
      if (index < 0)
        return false;
      stream.reset(zip->createStreamForEntry(index));
    }
    else
      stream = std::make_unique<juce::FileInputStream>(file);
    if (stream == nullptr)
      return false;
    if (auto* file_stream = dynamic_cast<juce::FileInputStream*>(stream.get())) {
      if (!file_stream->openedOk())
        return false;
    }

    juce::MemoryOutputStream output;
    if (max_bytes > 0)
      output.writeFromInputStream(*stream, max_bytes);
    else
      output.writeFromInputStream(*stream, -1);
    text.assign(static_cast<const char*>(output.getData()), output.getDataSize());
    return true;
  }

  bool write(const juce::File& target, const std::string& preset_json, const std::vector<Instrument>& instruments,
             std::string& error) {
    juce::ZipFile::Builder builder;
    builder.addEntry(new juce::MemoryInputStream(preset_json.data(), preset_json.size(), true), 0,
                     kPresetEntry, juce::Time::getCurrentTime());

    int64_t total = static_cast<int64_t>(preset_json.size());
    int entries = 1;
    std::set<std::string> written;
    for (const Instrument& instrument : instruments) {
      if (!written.insert(instrument.hash).second)
        continue;
      for (const FileInfo& info : instrument.files) {
        if (!safeRelative(info.relative)) {
          error = "Cannot store " + info.relative + " in a preset.";
          return false;
        }
        if (!info.file.existsAsFile()) {
          error = "The instrument file " + info.file.getFullPathName().toStdString() + " is missing.";
          return false;
        }
        total += info.file.getSize();
        entries++;
        builder.addFile(info.file, info.text ? 6 : 0, juce::String(entryName(instrument.hash, info.relative)));
      }
    }
    if (total > kMaxContainerBytes || entries > kMaxContainerEntries) {
      error = "The instruments are too large to store in one preset (presets hold up to 4 GB and 65,000 files).";
      return false;
    }

    juce::File parent = target.getParentDirectory();
    if (!parent.exists() && !parent.createDirectory().wasOk()) {
      error = "Cannot create " + parent.getFullPathName().toStdString();
      return false;
    }
    juce::TemporaryFile temporary(target);
    {
      juce::FileOutputStream output(temporary.getFile());
      if (!output.openedOk()) {
        error = "Cannot write " + target.getFullPathName().toStdString();
        return false;
      }
      if (!builder.writeToStream(output, nullptr)) {
        error = "Writing the preset failed.";
        return false;
      }
      output.flush();
      if (output.getStatus().failed()) {
        error = output.getStatus().getErrorMessage().toStdString();
        return false;
      }
    }
    if (!temporary.overwriteTargetFileWithTemporary()) {
      error = "Cannot replace " + target.getFullPathName().toStdString();
      return false;
    }
    return true;
  }

  bool replacePresetText(const juce::File& file, const std::string& preset_json, std::string& error) {
    if (!isContainer(file)) {
      if (!file.replaceWithText(juce::String(preset_json))) {
        error = "Cannot write " + file.getFullPathName().toStdString();
        return false;
      }
      return true;
    }

    juce::ZipFile zip(file);
    juce::ZipFile::Builder builder;
    builder.addEntry(new juce::MemoryInputStream(preset_json.data(), preset_json.size(), true), 0,
                     kPresetEntry, juce::Time::getCurrentTime());
    for (int i = 0; i < zip.getNumEntries(); ++i) {
      const juce::ZipFile::ZipEntry* entry = zip.getEntry(i);
      if (entry == nullptr || entry->filename == kPresetEntry)
        continue;
      juce::InputStream* stream = zip.createStreamForEntry(i);
      if (stream == nullptr) {
        error = "The preset is damaged.";
        return false;
      }
      builder.addEntry(stream, isTextFile(entry->filename) ? 6 : 0, entry->filename, entry->fileTime);
    }

    juce::TemporaryFile temporary(file);
    {
      juce::FileOutputStream output(temporary.getFile());
      if (!output.openedOk() || !builder.writeToStream(output, nullptr)) {
        error = "Cannot write " + file.getFullPathName().toStdString();
        return false;
      }
      output.flush();
    }
    if (!temporary.overwriteTargetFileWithTemporary()) {
      error = "Cannot replace " + file.getFullPathName().toStdString();
      return false;
    }
    return true;
  }

  juce::File cacheRoot() {
    std::lock_guard<std::mutex> lock(cacheRootMutex());
    if (cacheRootOverride() != juce::File())
      return cacheRootOverride();
    const juce::File base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    return base.getChildFile("Application Support/Atlas/Multi-sample Cache");
#else
    return base.getChildFile("Atlas/Multi-sample Cache");
#endif
  }

  void setCacheRootForTesting(const juce::File& folder) {
    std::lock_guard<std::mutex> lock(cacheRootMutex());
    cacheRootOverride() = folder;
  }

  juce::File cacheFolder(const std::string& hash) {
    return cacheRoot().getChildFile(juce::String(hash));
  }

  juce::File workingFolder(const std::string& hash) {
    return cacheRoot().getChildFile("Working").getChildFile(juce::String(hash));
  }

  bool isCached(const std::string& hash) {
    return !hash.empty() && cacheFolder(hash).getChildFile(kCompleteMarker).existsAsFile();
  }

  void touch(const std::string& hash) {
    if (isCached(hash))
      cacheFolder(hash).getChildFile(kCompleteMarker).setLastModificationTime(juce::Time::getCurrentTime());
  }

  bool extractToCache(const juce::File& container, const std::string& hash, const std::vector<FileInfo>& files,
                      const std::function<bool(float)>& progress, std::string& error) {
    std::lock_guard<std::mutex> lock(cacheWriteMutex());
    if (isCached(hash)) {
      touch(hash);
      return true;
    }

    juce::ZipFile zip(container);
    const juce::File partial = cacheRoot().getChildFile(juce::String(hash) + ".partial");
    partial.deleteRecursively();
    if (!partial.createDirectory().wasOk()) {
      error = "Cannot create " + partial.getFullPathName().toStdString();
      return false;
    }

    int64_t total = 0;
    for (const FileInfo& info : files)
      total += info.size;
    int64_t done = 0;
    std::vector<char> buffer(1 << 20);
    for (const FileInfo& info : files) {
      if (!safeRelative(info.relative)) {
        error = "The preset names an unsafe path: " + info.relative;
        partial.deleteRecursively();
        return false;
      }
      const int index = zip.getIndexOfFileName(juce::String(entryName(hash, info.relative)));
      std::unique_ptr<juce::InputStream> input(index >= 0 ? zip.createStreamForEntry(index) : nullptr);
      if (input == nullptr) {
        error = "The preset is missing " + info.relative;
        partial.deleteRecursively();
        return false;
      }
      const juce::File target = partial.getChildFile(juce::String(info.relative));
      if (!target.isAChildOf(partial)) {
        error = "The preset names an unsafe path: " + info.relative;
        partial.deleteRecursively();
        return false;
      }
      target.getParentDirectory().createDirectory();
      juce::FileOutputStream output(target);
      if (!output.openedOk()) {
        error = "Cannot write " + target.getFullPathName().toStdString();
        partial.deleteRecursively();
        return false;
      }
      uint32_t crc = 0;
      int64_t size = 0;
      while (!input->isExhausted()) {
        const int read = input->read(buffer.data(), static_cast<int>(buffer.size()));
        if (read <= 0)
          break;
        crc = crc32(buffer.data(), static_cast<size_t>(read), crc);
        output.write(buffer.data(), static_cast<size_t>(read));
        size += read;
        done += read;
        if (progress && !progress(static_cast<float>(done) / std::max<int64_t>(1, total))) {
          output.flush();
          partial.deleteRecursively();
          return false;
        }
      }
      output.flush();
      if (output.getStatus().failed()) {
        error = "Cannot write " + target.getFullPathName().toStdString() + ": the disk may be full.";
        partial.deleteRecursively();
        return false;
      }
      if (size != info.size || crc != info.crc) {
        error = "The preset is damaged: " + info.relative + " does not match its checksum.";
        partial.deleteRecursively();
        return false;
      }
    }

    if (!finishCacheFolder(partial, hash)) {
      error = "Cannot finish the instrument cache in " + cacheRoot().getFullPathName().toStdString();
      partial.deleteRecursively();
      return false;
    }
    return true;
  }

  // Copies in pieces, so a cancel (closing the plugin) stops a copy of a
  // large SoundFont within moments rather than after the whole file.
  static bool copyFileCancellable(const juce::File& source, const juce::File& target,
                           const std::function<bool()>& cancelled) {
    juce::FileInputStream input(source);
    if (!input.openedOk())
      return false;
    target.deleteFile();
    juce::FileOutputStream output(target);
    if (!output.openedOk())
      return false;
    std::vector<char> buffer(4 << 20);
    for (;;) {
      if (cancelled && cancelled())
        return false;
      const int read = input.read(buffer.data(), static_cast<int>(buffer.size()));
      if (read < 0)
        return false;
      if (read == 0)
        break;
      if (!output.write(buffer.data(), static_cast<size_t>(read)))
        return false;
    }
    output.flush();
    return !output.getStatus().failed() && input.isExhausted();
  }

  bool populateCache(const std::string& hash, const std::vector<FileInfo>& files, int64_t max_copy_bytes,
                     const std::function<bool()>& cancelled) {
    std::lock_guard<std::mutex> lock(cacheWriteMutex());
    if (hash.empty())
      return false;
    if (isCached(hash)) {
      touch(hash);
      return true;
    }

    const juce::File partial = cacheRoot().getChildFile(juce::String(hash) + ".partial");
    partial.deleteRecursively();
    if (!partial.createDirectory().wasOk())
      return false;

    std::vector<const FileInfo*> to_copy;
    int64_t copy_bytes = 0;
    for (const FileInfo& info : files) {
      if (!safeRelative(info.relative) || !info.file.existsAsFile()) {
        partial.deleteRecursively();
        return false;
      }
      const juce::File target = partial.getChildFile(juce::String(info.relative));
      target.getParentDirectory().createDirectory();
      std::error_code code;
      // u8path: a plain std::string path is read in the ANSI code page on Windows,
      // which breaks (or throws on) any path with characters outside it.
      std::filesystem::create_hard_link(std::filesystem::u8path(info.file.getFullPathName().toStdString()),
                                        std::filesystem::u8path(target.getFullPathName().toStdString()), code);
      if (code) {
        to_copy.push_back(&info);
        copy_bytes += info.size;
      }
    }
    if (copy_bytes > max_copy_bytes) {
      partial.deleteRecursively();
      return false;
    }
    for (const FileInfo* info : to_copy) {
      if (!copyFileCancellable(info->file, partial.getChildFile(juce::String(info->relative)), cancelled)) {
        partial.deleteRecursively();
        return false;
      }
    }
    if (!finishCacheFolder(partial, hash)) {
      partial.deleteRecursively();
      return false;
    }
    return true;
  }

  void pruneCache(int64_t budget_bytes, const std::set<std::string>& in_use,
                  const std::function<bool()>& cancelled) {
    std::lock_guard<std::mutex> lock(cacheWriteMutex());
    const juce::File root = cacheRoot();
    if (!root.isDirectory())
      return;

    struct Entry {
      juce::File folder;
      juce::Time used;
      int64_t size;
    };
    std::vector<Entry> entries;
    int64_t total = 0;
    for (const auto& item : juce::RangedDirectoryIterator(root, false, "*", juce::File::findDirectories)) {
      if (cancelled && cancelled())
        return;
      const juce::File folder = item.getFile();
      const juce::String name = folder.getFileName();
      if (name.endsWith(".partial")) {
        folder.deleteRecursively();
        continue;
      }
      const juce::File marker = folder.getChildFile(kCompleteMarker);
      if (!marker.existsAsFile())
        continue;
      Entry entry { folder, marker.getLastModificationTime(), folderSize(folder) };
      total += entry.size;
      entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const Entry& entry : entries) {
      if (total <= budget_bytes || (cancelled && cancelled()))
        break;
      const std::string hash = entry.folder.getFileName().toStdString();
      if (in_use.count(hash))
        continue;
      entry.folder.deleteRecursively();
      workingFolder(hash).deleteRecursively();
      total -= entry.size;
    }
  }
} // namespace container
} // namespace multisample
} // namespace vital

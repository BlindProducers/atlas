/* Copyright 2013-2019 Matt Tytel
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "JuceHeader.h"
#include "midi_manager.h"
#include "nlohmann/json.hpp"

#include <map>
#include <set>
#include <string>

using json = nlohmann::json;

namespace vital {
  class StringLayout;
}

class SynthBase;

class LoadSave {
  public:
    class FileSorterAscending {
      public:
        FileSorterAscending() { }

        static int compareElements(File a, File b) {
          return a.getFullPathName().toLowerCase().compareNatural(b.getFullPathName().toLowerCase());
        }

      private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileSorterAscending)
    };

    enum PresetStyle {
      kBass,
      kLead,
      kKeys,
      kPad,
      kPercussion,
      kSequence,
      kExperimental,
      kSfx,
      kTemplate,
      kNumPresetStyles
    };

    static const int kMaxCommentLength = 500;
    static constexpr int kEngineBehaviorVersion = 2;
    static const std::string kUserDirectoryName;
    static const std::string kPresetFolderName;
    static const std::string kWavetableFolderName;
    static const std::string kSkinFolderName;
    static const std::string kSampleFolderName;
    static const std::string kLfoFolderName;
    static const std::string kFxFolderName;
    static const std::string kIrFolderName;
    static const std::string kAdditionalWavetableFoldersName;
    static const std::string kAdditionalSampleFoldersName;

    static void convertBufferToPcm(json& data, const std::string& field);
    static void convertPcmToFloatBuffer(json& data, const std::string& field);
    static json stateToJson(SynthBase* synth, const CriticalSection& critical_section);

    static void loadControls(SynthBase* synth, const json& data);
    static void loadModulations(SynthBase* synth, const json& modulations);
    // Loads each oscillator slot's sample from "osc_N_sample" entries.
    static void loadSlotSamples(SynthBase* synth, const json& settings);
    // Multi-sample instruments (osc_N_multisample); slots without one are cleared.
    static void loadSlotInstruments(SynthBase* synth, const json& settings);
    // Rewrites a preset saved with fixed Sample/Granular/Spectral/Noise sources
    // into oscillator slots 5-8. Safe to call on presets already migrated.
    static void migrateFixedSourcesToSlots(json& settings, json& modulations);
    static void migrateSlotTypeLevels(json& settings, json& modulations);
    static void loadConvolutionIrs(SynthBase* synth, const json& convolution_irs);
    static void loadWavetables(SynthBase* synth, const json& wavetables);
    static void loadFilterTables(SynthBase* synth, const json& filter_tables);
    static void loadLfos(SynthBase* synth, const json& lfos);
    static void loadSaveState(std::map<std::string, String>& save_info, json data);

    static void initSaveInfo(std::map<std::string, String>& save_info);
    static json updateFromOldVersion(json state);
    static bool jsonToState(SynthBase* synth, std::map<std::string, String>& save_info, json state);

    static String getAuthorFromFile(const File& file);
    static String getStyleFromFile(const File& file);
    static String getTagsFromFile(const File& file);
    static std::string getAuthor(json file);
    static std::string getLicense(json state);

    static File getConfigFile();
    static void writeCrashLog(String crash_log);
    static void writeErrorLog(String error_log);
    static json getConfigJson();
    static File getFavoritesFile();
    static File getDefaultSkin();
    static json getFavoritesJson();
    static void addFavorite(const File& new_favorite);
    static void removeFavorite(const File& old_favorite);
    static std::set<std::string> getFavorites();
    static bool hasDataDirectory();
    static File getAvailablePacksFile();
    static json getAvailablePacks();
    static File getInstalledPacksFile();
    static json getInstalledPacks();
    static void saveInstalledPacks(const json& packs);
    static void markPackInstalled(int id);
    static void markPackInstalled(const std::string& name);
    static void saveDataDirectory(const File& data_directory);
    static bool isInstalled();
    static bool wasUpgraded();
    static bool isExpired();
    static bool doesExpire();
    static int getDaysToExpire();
    static bool shouldCheckForUpdates();
    static bool shouldWorkOffline();
    static std::string getLoadedSkin();
    static bool shouldAnimateWidgets();
    static bool shouldScanDownloads();
    static bool displayHzFrequency();
    static bool authenticated();
    static int getOversamplingAmount();
    static float loadWindowSize();
    static String loadVersion();
    static String loadContentVersion();
    static void saveJsonToConfig(json config_state);
    static void saveJsonToFavorites(json favorites_json);
    static void saveAuthor(std::string author);
    static void savePreferredTTWTLanguage(std::string language);
    static void saveLayoutConfig(vital::StringLayout* layout);
    static void saveVersionConfig();
    static void saveContentVersion(std::string version);
    static void saveUpdateCheckConfig(bool check_for_updates);
    static void saveWorkOffline(bool work_offline);
    static void saveLoadedSkin(const std::string& name);
    static void saveAnimateWidgets(bool animate_widgets);
    static void saveScanDownloads(bool scan_downloads);
    static void saveDisplayHzFrequency(bool display_hz);
    static void saveAuthenticated(bool authenticated);
    static void saveWindowSize(float window_size);
    static void saveMidiMapConfig(MidiManager* midi_manager);
    // The map is read by the audio thread: copy it under the callback lock,
    // then write the file from the copy with the lock released.
    static void saveMidiMapConfig(const MidiManager::midi_map& midi_learn_map);
    static void loadConfig(MidiManager* midi_manager, vital::StringLayout* layout = nullptr);

    static std::wstring getComputerKeyboardLayout();
    static std::string getPreferredTTWTLanguage();
    static std::string getAuthor();
    static std::pair<wchar_t, wchar_t> getComputerKeyboardOctaveControls();
    static void saveAdditionalFolders(const std::string& name, std::vector<std::string> folders);
    static std::vector<std::string> getAdditionalFolders(const std::string& name);

    static File getDataDirectory();
    // Documents/Blind Producers Plugins/Atlas; the library used to live in
    // Documents/Alessio Plugins/Atlas and is moved from there once.
    static File getDefaultDataDirectory();
    static File getLegacyDataDirectory();
    static void migrateLegacyDataDirectory();
    static std::vector<File> getDirectories(const String& folder_name);
    static std::vector<File> getPresetDirectories();
    static std::vector<File> getWavetableDirectories();
    static std::vector<File> getSkinDirectories();
    static std::vector<File> getSampleDirectories();
    static std::vector<File> getLfoDirectories();
    static std::vector<File> getFxDirectories();
    static std::vector<File> getIrDirectories();
    static File getUserDirectory();
    static File getUserPresetDirectory();
    static File getUserWavetableDirectory();
    static File getUserSkinDirectory();
    static File getUserSampleDirectory();
    static File getUserLfoDirectory();
    static File getUserFxDirectory();
    static File getUserIrDirectory();
    static File getDownloadsDirectory();
    static int scanDownloadsForPresets();
    static void getAllFilesOfTypeInDirectories(Array<File>& files, const String& extensions,
                                               const std::vector<File>& directories);
    static void getAllPresets(Array<File>& presets);
    static void getAllWavetables(Array<File>& wavetables);
    static void getAllSkins(Array<File>& skins);
    static void getAllLfos(Array<File>& lfos);
    static void getAllSamples(Array<File>& samples);
    static void getAllFx(Array<File>& fx);
    static void getAllUserPresets(Array<File>& presets);
    static void getAllUserWavetables(Array<File>& wavetables);
    static void getAllUserLfos(Array<File>& lfos);
    static void getAllUserSamples(Array<File>& samples);
    static void getAllUserFx(Array<File>& fx);
    static void getAllIrs(Array<File>& irs);
    static void getAllUserIrs(Array<File>& irs);
    static int compareFeatureVersionStrings(String a, String b);
    static int compareVersionStrings(String a, String b);

    static File getShiftedFile(const String directory_name, const String& extensions,
                               const std::string& additional_folders_name, const File& current_file, int shift);

  private:
    LoadSave() { }
};

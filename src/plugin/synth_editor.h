/* Copyright 2013-2019 Matt Tytel
 *
 * pylon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pylon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pylon.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "JuceHeader.h"
#include "synth_plugin.h"
#include "parameter_slider.h"
#include "sequence_keyboard_component.h"
#include "trance_gate_editor.h"
#include "synth_gui_interface.h"

#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

class AccessibleParameterRow;
class CpuUsageMeter;
class LineGenerator;
class WaveSource;
class WavetableComponent;
class PresetFacetTableModel;
class PresetResultsTableModel;
class ValueBridge;

namespace vital {
  class ConvolutionReverbModule;
}

struct PresetBrowserItem {
  File file;
  int64 file_size = 0;
  int64 modified_time = 0;
  String library;
  String bank;
  String category;
  StringArray tags;
  String author;
  String comments;
  String style;
  String label;
  String searchable;
};

struct PresetFacetItem {
  int kind = 0;
  String section;
  String value;
  int count = 0;
  bool active = false;
};

class PresetTableListBox : public TableListBox {
  public:
    using TableListBox::TableListBox;

    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<void()> onFocusGained;

    void focusGained(FocusChangeType cause) override {
      TableListBox::focusGained(cause);
      if (onFocusGained)
        onFocusGained();
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed && onKeyPressed(key))
        return true;
      return TableListBox::keyPressed(key);
    }
};

class AccessibleComboBox : public ComboBox {
  public:
    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<void()> onFocusGained;

    void focusGained(FocusChangeType cause) override {
      ComboBox::focusGained(cause);
      if (onFocusGained)
        onFocusGained();
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed && onKeyPressed(key))
        return true;
      return ComboBox::keyPressed(key);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
};

class AccessibleTextButton : public TextButton {
  public:
    using TextButton::TextButton;

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
};

class ModulationRemapKeyboardComponent : public Component {
  public:
    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<String()> getStatusText;

    ModulationRemapKeyboardComponent() {
      setWantsKeyboardFocus(true);
      setTitle("Modulation remap keyboard editor");
      setDescription("Keyboard editor for the selected modulation route's input to output remap curve.");
      setHelpText("Comma and period select points. Shift Left and Shift Right move an interior point's input; add Alt for a larger step. "
                  "Up and Down change its output. A adds a point. Delete removes an interior point. "
                  "I and K change the segment curve. H flips horizontally. V flips vertically. "
                  "Backspace resets the remap to linear. Navigation announces point details; edits announce only the changed value or curve.");
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed && onKeyPressed(key)) {
        repaint();
        return true;
      }
      return Component::keyPressed(key);
    }

    void paint(Graphics& g) override {
      g.fillAll(Colour(0xff20242b));
      g.setColour(Colour(0xffd9dee2));
      g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
      g.setColour(Colours::white);
      g.drawText(getStatusText ? getStatusText() : String("Modulation remap keyboard editor"),
                 getLocalBounds().reduced(12), Justification::centredLeft, true);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class Handler final : public AccessibilityHandler {
        public:
          explicit Handler(ModulationRemapKeyboardComponent& component) :
              AccessibilityHandler(component, AccessibilityRole::group), component_(component) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }

          String getDescription() const override {
            return component_.getStatusText ? component_.getStatusText() : String();
          }

        private:
          ModulationRemapKeyboardComponent& component_;
      };
      return std::make_unique<Handler>(*this);
    }
};

class WavetableKeyboardComponent : public Component {
  public:
    std::function<bool(const KeyPress&, Component&)> onKeyPressed;
    std::function<String()> getStatusText;

    WavetableKeyboardComponent() {
      setWantsKeyboardFocus(true);
      setDescription("Keyboard wavetable editor with starting waveforms, frame selection, harmonic editing, and interpolation.");
      setHelpText("Keys 1 through 6 apply Sine, Saturated sine, Triangle, Square, Pulse, or Saw to the current frame selection. Left and right move between frames. Shift left and right extend frame selection. Home and End move to the first and last frame. S toggles frame selection. R clears selection. Command A or Shift A selects all frames. Comma and period move between harmonics. Page Up and Page Down move by ten harmonics. Up and Down change harmonic level by one percent; hold Shift for five percent. J and L change harmonic phase by one degree; hold Shift for fifteen degrees. Backspace clears the harmonic. F removes the fundamental. Zero removes DC. N normalizes. A adds a frame. E or Delete deletes frames. C and V copy and paste frames. I cycles interpolation modes.");
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed && onKeyPressed(key, *this)) {
        repaint();
        return true;
      }
      return Component::keyPressed(key);
    }

    void paint(Graphics& g) override {
      g.fillAll(Colour(0xff20242b));
      g.setColour(Colour(0xffd9dee2));
      g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
      g.setColour(Colours::white);
      g.drawText(getStatusText ? getStatusText() : String("Wavetable keyboard editor"),
                 getLocalBounds().reduced(12), Justification::centredLeft, true);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class Handler final : public AccessibilityHandler {
        public:
          explicit Handler(WavetableKeyboardComponent& component) :
              AccessibilityHandler(component, AccessibilityRole::group), component_(component) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }

          String getDescription() const override {
            return component_.getStatusText ? component_.getStatusText() : String();
          }

        private:
          WavetableKeyboardComponent& component_;
      };
      return std::make_unique<Handler>(*this);
    }
};

namespace vital {
namespace multisample {
  struct PresetName;
  struct LoadResult;
}
}

class SynthEditor : public AudioProcessorEditor, public SynthGuiInterface,
                    private Timer {
  public:
    // Loads a wavetable into a filter's table and updates its controls in place (no rebuild).
    void loadFilterTableFile(const String& filter_prefix, const File& file);
    SynthEditor(SynthPlugin&);
    ~SynthEditor() override;

    void paint(Graphics&) override;
    void resized() override;
    void updateFullGui() override;
    // The SF2 preset menu: flat for a few presets, otherwise grouped by bank and
    // into runs of 16 programs so every submenu fits on screen (a screen reader
    // reaches only the menu items that are visible). Item ids are list positions + 1.
    static PopupMenu createInstrumentPresetMenu(const std::vector<vital::multisample::PresetName>& presets,
                                                int selected_position);
    void notifyMidiLearned(const std::string& name, int midi_id) override;
    bool keyPressed(const KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    std::unique_ptr<ComponentTraverser> createFocusTraverser() override;
    std::unique_ptr<ComponentTraverser> createKeyboardFocusTraverser() override;

    const std::vector<Component*>& getAccessibleFocusOrder() const { return accessibility_focus_order_; }
    const std::vector<Component*>& getKeyboardFocusOrder() const { return focus_order_; }
    void ensureComponentVisible(Component* component);
    // The context menu of an effect slot header: copy, paste to another bus, replace, remove.
    // Each item carries its own action, so the menu can be exercised without showing it.
    PopupMenu createEffectSlotContextMenu(const String& sectionName, int slot, const String& focusedTitle);
    // The Editor menu's MIDI learn mappings: one item per mapping removes it.
    PopupMenu createMidiLearnMenu();
    // Runs one poll of the 20 Hz state timer now (tests interleave it with actions).
    void pollStateNow() { timerCallback(); }
    // What a row's Assign modulation does, with `amountText` typed into the amount prompt.
    // `target` is the control the menu was opened on; focus returns to it.
    String assignModulation(const String& sourceId, const String& destinationId, const String& amountText,
                            Component* target = nullptr) {
      connectModulationAndPromptForAmount(sourceId, destinationId, target != nullptr ? *target : *this);
      modulation_amount_editor_.setText(amountText, dontSendNotification);
      const float amount = modulationAmountFromPeakText(destinationId, amountText);
      applyInlineTextPrompt();
      return modulationAmountSummary(sourceId, destinationId, amount);
    }
    // The focusable control of the row showing `parameterId`, and the focused row's parameter.
    Component* controlForParameter(const String& parameterId) const;
    String focusedRowParameterId() const { return focusedParameterId(); }
    // What a row's Remove modulation does when chosen from the menu opened on `target`.
    void removeModulation(const String& sourceId, const String& destinationId, Component& target) {
      removeModulationFromParameter(sourceId, destinationId, target);
    }
    // Preset library: refresh it (as the Preset menu does) and ask whether a scan is running.
    void refreshPresetLibrary() { refreshPresetList(false); }
    bool isPresetLibraryBusy() const { return preset_list_loading_; }
    int presetLibrarySize() const { return all_presets_.size(); }
    File presetFileAtRow(int row) const {
      return isPositiveAndBelow(row, filtered_presets_.size()) ? filtered_presets_[row] : File();
    }
    // The grouped source menu a parameter row offers (tests walk it like the slot menus).
    PopupMenu createModulationSourceSubmenu(const String& destinationId, std::map<int, String>& choices,
                                            int firstItemId);

  private:
    struct CopiedLfoPoint {
      std::pair<float, float> point;
      float power = 0.0f;
    };

    void timerCallback() override;
    // Choice values that decide which rows an effect slot shows.
    std::map<String, int> effect_slot_modes_;
    std::map<String, float> floaty_sync_durations_;
    void buildSections();
    void buildGroups();
    void selectSectionByName(const String& section, bool announce);
    void showAllSections(bool announce);
    void clearSectionComponents();
    std::unique_ptr<AccessibleParameterRow> createAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                         const String& sectionName);
    int sampleLoopPointLength(const String& parameterId) const;
    void toggleSampleLoopFineTune();
    void wireSampleLoopFineTune(AccessibleParameterRow& row);
    bool shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const;
    bool refreshFilterRowsIfNeeded();
    bool refreshRandomAmplitudeRowsIfNeeded();
    bool isAnalogOscillator(int oscillator) const;
    int sourceSlotType(int oscillator) const;
    void setAutomationProxyCallbacks(AccessibleParameterRow& row);
    int oscillatorModeKey(int oscillator) const;
    bool refreshOscillatorModeRowsIfNeeded();
    bool refreshZoneCrossfadeRowIfNeeded();
    bool refreshEnvelopeAnalogRowsIfNeeded();
    String focusedParameterId() const;
    String focusedAccessibleTitle() const;
    // The SectionName of the focused control or its nearest group, so a rebuild
    // can find the same control when several sections share its title.
    String focusedSectionKey() const;
    Component* persistentFocusedComponent() const;
    // Focuses the control a rebuild replaced; false when there is none to focus.
    // refocusScreenReader moves the screen reader to it again once the rows
    // settle; callers that announce the change leave it off so the move does
    // not cut the announcement short.
    bool restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                  const String& accessibleTitle = {}, const String& preferredSection = {},
                                  bool refocusScreenReader = false);
    Component* findRebuildFocusTarget(const String& parameterId, Component* persistentComponent,
                                      const String& accessibleTitle, const String& preferredSection) const;
    void moveToSection(int direction);
    void showNavigationMenu();
    void armOrCancelMidiLearn(const String& parameterId);
    void showAccessibilitySettingsMenu();
    void showKeyboardHelp();
    void initializePatch();
    void savePatchAsDefault();
    File defaultPatchFile() const;
    void rebuildFocusOrder();
    void ensurePresetListLoaded();
    void startPresetListLoad(bool announce, bool forceRefresh = false);
    void applyPresetList(std::vector<PresetBrowserItem> items, bool announce, int generation,
                         bool stillScanning = false);
    const PresetBrowserItem* presetItemForFile(const File& file) const;
    void refreshPresetList(bool announce = true);
    void populatePresetFilters();
    void filterPresetList();
    void schedulePresetFilterUpdate(bool rebuildFilters);
    void activatePresetFacet(int kind, int row);
    int presetFacetRowCount(int kind) const;
    const PresetFacetItem* presetFacetForRow(int kind, int row) const;
    PresetTableListBox* presetFacetTableForKind(int kind);
    void presetSelectionChanged(int row, bool allowAutoload = true);
    File selectedPresetFile() const;
    String presetTableCellText(int row, int columnId) const;
    String presetFacetCellText(int kind, int row) const;
    bool isPresetFavorite(const File& file) const;
    bool isPresetEditable(const File& file) const;
    void toggleSelectedPresetFavorite();
    void renameSelectedPreset();
    void showPresetMetadataDialog();
    void deleteSelectedPreset();
    void revealSelectedPreset();
    void updatePresetManagementControls();
    void showPresetMenu();
    void showSelectedPresetMenu(Component& target);
    void toggleScanDownloads();
    void selectPresetFile(const File& file);
    void loadSelectedPreset();
    void choosePresetFile();
    void loadPresetFile(File file, bool preview = false, bool updateBrowserFilters = false);
    void chooseBankFile();
    void importBankFile(const File& file);
    void chooseFolderToExportBank();
    void exportFolderAsBank(const File& sourceFolder, const File& destinationFile);
    void loadTuningFile();
    void clearTuning();
    void exportPreset();
    void savePresetToUserDirectory();
    void savePresetAs();
    void savePatch();
    void showSavePresetDialog();
    void hideSavePresetDialog();
    void commitSavePresetDialog();
    void savePresetFile(const File& file);
    void showPresetControls();
    void updatePresetSummary();
    void refreshWavetableNameLabel(int oscillator);
    void refreshSampleNameLabel(int sampleTarget);
    String sourceStatusTitleForSection(const String& sectionName) const;
    void loadShiftedWavetable(int oscillator, int direction);
    void updateRoutingSummary();
    int filterRoutingMode(bool activeOnly) const;
    void refreshRoutingControls();
    void applyRoutingPreset(int preset);
    void chooseWavetableFile();
    void chooseWavetableFile(int audioLoadStyle);
    void loadWavetableFile(const File& file);
    void loadWavetableFile(const File& file, int audioLoadStyle);
    void synthesizePresetToWavetable(int oscillator);
    void saveWavetableFile(int oscillator);
    void exportWavetableWav(int oscillator);
    void showWavetableActionsMenu(int oscillator, Component& target);
    int wavetableFrameCount(int oscillator) const;
    String wavetableEditorSummary(int oscillator) const;
    String wavetableKeyboardStatusText(int oscillator) const;
    String wavetableFrameStatusText(int oscillator) const;
    String wavetableHarmonicStatusText(int oscillator, bool includeLevel, bool includePhase) const;
    String wavetableInterpolationName(int oscillator) const;
    bool handleWavetableShortcut(int oscillator, const KeyPress& key, Component& target);
    void moveWavetableFrameCursor(int oscillator, int direction, bool extendSelection);
    void moveWavetableHarmonicCursor(int oscillator, int direction);
    void toggleWavetableFrameSelection(int oscillator);
    void clearWavetableFrameSelection(int oscillator);
    void selectAllWavetableFrames(int oscillator);
    void adjustWavetableHarmonicLevel(int oscillator, float deltaPercent);
    void adjustWavetableHarmonicPhase(int oscillator, float deltaDegrees);
    void copyWavetableFrames(int oscillator);
    void pasteWavetableFrames(int oscillator);
    void addWavetableFrame(int oscillator);
    void deleteWavetableFrames(int oscillator);
    void setWavetableFramePosition(int oscillator, int frame, int position);
    void clearWavetableDc(int oscillator);
    void cycleWavetableInterpolation(int oscillator, int direction);
    void setWavetableInterpolation(int oscillator, int interpolation);
    int addAccessibleWavetableEditorControls(int oscillator, Component& parent, int y, int x,
                                             int width, int& focusOrder);
    // Filter Table: load, browse, cycle and reset the wavetable a filter reads.
    void addFilterTableControlsGroup(const String& filter_prefix, Component& parent, int& y, int x, int width,
                                     int& focus_order, const String& section_name);
    void chooseFilterTableFile(const String& filter_prefix);
    String filterTableName(const String& filter_prefix) const;
    void refreshFilterTableControls(const String& filter_prefix);
    void loadShiftedFilterTable(const String& filter_prefix, int direction);
    void showFilterTableBrowserMenu(const String& filter_prefix, Component& target);
    bool loadWavetableIntoCreator(WavetableCreator& creator, const File& file, int audioLoadStyle, String& error);
    int filterModelForPrefix(const String& filter_prefix) const;
    // `appendRows` adds parameter rows inside the group (after its buttons).
    using GroupRowAppender = std::function<void(Component& group, int& innerY, int innerWidth)>;
    void addWavetableControlsGroup(int oscillator, Component& parent, int& y, int x,
                                   int width, int& focusOrder, const String& sectionName,
                                   const GroupRowAppender& appendRows = {});
    void addWavetableModifier(int oscillator, int modifierType);
    void removeWavetableModifier(int oscillator, int componentIndex);
    void resetWavetableModifier(int oscillator, int componentIndex);
    void moveWavetableModifier(int oscillator, int componentIndex, int direction);
    // Layered editing (Vital's groups): the selected layer's source is component
    // 0, its modifiers follow. Keyframe and value edits act on the selected keyframe.
    WaveSource* currentWaveSource(int oscillator) const;
    WaveSource* editableWaveSource(int oscillator);
    WavetableComponent* wavetableComponent(int oscillator, int componentIndex) const;
    void selectWavetableLayer(int oscillator, int layer);
    void addWavetableLayer(int oscillator, int sourceType);
    void removeWavetableLayer(int oscillator);
    void moveWavetableLayer(int oscillator, int direction);
    void setWavetableComponentValue(int oscillator, int componentIndex, const std::string& id, double value);
    void moveWavetableKeyframe(int oscillator, int componentIndex, int position);
    void addWavetableKeyframe(int oscillator, int componentIndex, const String& focusPrefix);
    void removeWavetableKeyframe(int oscillator, int componentIndex, const String& focusPrefix);
    void spreadWavetableKeyframes(int oscillator, int componentIndex, const String& focusPrefix);
    void setWavetableLinePointValue(int oscillator, int componentIndex, const std::string& field, double value);
    void addWavetableLinePoint(int oscillator, int componentIndex, const String& focusPrefix);
    void removeWavetableLinePoint(int oscillator, int componentIndex, const String& focusPrefix);
    void chooseWavetableLayerAudio(int oscillator);
    void addWavetableAudioLayer(int oscillator, const File& file);
    void replaceWavetableLayerAudio(int oscillator, const File& file);
    void flattenWavetableToFrames(int oscillator);
    void rebuildWavetableEditor(int oscillator, const String& focusTitle);
    std::vector<int> wavetableTargetFrameIndices(int oscillator, int frameCount) const;
    void pruneWavetableFrameSelection(int oscillator, int frameCount);
    void setWavetableEditorVisible(int oscillator, bool visible);
    void applyWavetableHarmonicEdit(int oscillator, int frame, bool allFrames,
                                    int harmonic, float levelPercent, float phaseDegrees);
    void applyWavetableStartingShape(int oscillator, int frame, bool allFrames, int shape);
    void clearWavetableHarmonic(int oscillator, int frame, bool allFrames, int harmonic);
    void normalizeWavetableFrame(int oscillator, int frame, bool allFrames);
    void removeWavetableFundamental(int oscillator, int frame, bool allFrames);
    void showWavetableBrowserMenu(int oscillator, Component& target);
    void chooseSampleFile(int sample_target = 0);
    void addSampleControlsGroup(int sample_target, Component& parent, int& y, int x, int width,
                                int& focus_order, const String& section_name,
                                const GroupRowAppender& appendRows = {});
    // Multi-sample oscillators: the Instrument group (load, status, SF2 preset,
    // SFZ editing) and the status the editor polls while instruments load.
    void addMultiSampleControlsGroup(int oscillator, Component& parent, int& y, int x, int width,
                                     int& focus_order, const String& section_name,
                                     const GroupRowAppender& appendRows = {});
    void chooseInstrumentFile(int oscillator);
    // Moves an SF2 slot to the previous (-1) or next (1) preset, wrapping.
    void stepInstrumentPreset(int oscillator, int direction);
    void openInstrumentInTextEditor(int oscillator);
    void reloadInstrument(int oscillator);
    // Empties a Multi-sample slot (and cancels a load in progress).
    void clearInstrument(int oscillator);
    void refreshMultiSampleStatus();
    // Keeps the SF2 preset menu on the SoundFont the slot plays: a new
    // selection moves the menu, another SoundFont rebuilds the rows.
    void syncInstrumentPresetMenu(int oscillator);
    String instrumentStatusTitle(int oscillator) const;
    String instrumentDetails(int oscillator) const;
    // 0 nothing loaded, 1 SFZ or DecentSampler, 2 SF2 with one preset, 3 SF2
    // with several, 4 DecentSampler with controls.
    int multiSampleUiState(int oscillator) const;
    // A slider for each knob of the instrument and a menu for each menu and button.
    void addInstrumentControlRows(int oscillator, const vital::multisample::LoadResult& result,
                                  const std::function<Component*(std::unique_ptr<Component>, int)>& place,
                                  const String& section_name);
    // Keeps those rows on the values the slot plays; another instrument rebuilds them.
    void syncInstrumentControls(int oscillator);
    // The Braids synthesis mode a slot has selected, 0 to 47.
    int braidsModel(int oscillator) const;
    // A section row with every callback wired (modulation, MIDI learn, value entry, automation).
    std::unique_ptr<AccessibleParameterRow> createConfiguredSectionRow(AudioProcessorParameter& parameter,
                                                                       const String& sectionName);
    // An oscillator section's content: material groups, output rows, then the
    // Pitch and unison (with Scale lock), Warp and per-type groups the slot's type uses.
    void addOscillatorSectionContent(int oscillator, const String& sectionName, Component& parent,
                                     int& y, int x, int width, int& focusOrder);
    void loadSampleFile(const File& file, int sample_target = 0);
    void showSampleBrowserMenu(Component& target, int sample_target = 0);
    void loadShiftedSample(int direction, int sample_target = 0);
    void chooseLfoPresetFile(int lfoIndex);
    void loadLfoPresetFile(int lfoIndex, const File& file);
    void saveLfoPresetFile(int lfoIndex);
    vital::ConvolutionReverbModule* convolutionModuleForPrefix(const String& modulePrefix);
    void chooseConvolutionIrFile(const String& modulePrefix);
    void loadConvolutionIrFile(const String& modulePrefix, const File& file);
    void showConvolutionIrBrowserMenu(const String& modulePrefix, Component& target);
    void loadShiftedConvolutionIr(const String& modulePrefix, int direction);
    void saveConvolutionIrFile(const String& modulePrefix);
    void refreshIrBrowserCache();
    void refreshConvolutionIrLoads();
    void chooseEffectPresetFile(const String& sectionName);
    void loadEffectPresetFile(const String& sectionName, const File& file);
    void saveEffectPresetFile(const String& sectionName);
    void chooseFxBusPresetFile(const String& sectionName, bool save);
    void addFxBusControlsGroup(const String& sectionName, Component& parent, int& y, int x,
                               int width, int& focusOrder);
    int effectSlotType(const String& sectionName, int slot) const;
    int firstEmptyEffectSlot(const String& sectionName) const;
    std::vector<int> activeEffectSlots(const String& sectionName) const;
    String effectSlotPrefix(const String& sectionName, int slot) const;
    String effectSlotTitle(const String& sectionName, int slot) const;
    void swapEffectSlots(const String& sectionName, int firstSlot, int secondSlot);
    // Moves the impulse response, Filter FX table and MIDI learn mappings
    // keyed by the slot prefixes along with a swapped pair of slots.
    void swapEffectSlotModuleState(const String& firstPrefix, const String& secondPrefix);
    void clearEffectSlot(const String& sectionName, int slot);
    void removeEffectSlot(const String& sectionName, int slot, const String& focusedTitle);
    void replaceEffectSlot(const String& sectionName, int slot, int effect);
    void showEffectSlotContextMenu(const String& sectionName, int slot, Component& target);
    void copyEffectSlotToClipboard(const String& sectionName, int slot, bool includeModulations);
    void pasteEffectSlotClipboard(const String& targetSection, const String& focusSection,
                                  const String& focusedTitle);
    std::vector<String> effectChainSectionNames() const;
    bool isFxBusEnabled(const String& sectionName) const;
    void refreshFxBusToggles();
    // Text for each effects chain's "Effects chain order" label.
    String effectChainSummaryText(const String& sectionName) const;
    void refreshEffectChainSummaries();
    void addEffectToChain(const String& sectionName, int effect, Component& target);
    void showAddEffectMenu(const String& sectionName, Component& target);
    bool isEffectSlotParameter(const String& sectionName, const String& parameterId) const;
    void addEffectSlotGroups(const String& sectionName, Component& parent, int& y, int x, int width,
                             int& focusOrder);
    void readEffectChainOrder(const String& sectionName, int* order) const;
    void writeEffectChainOrder(const String& sectionName, const int* order);
    void scheduleEffectChainRebuild(const String& preferredSection, const String& focusedTitle = {},
                                    const String& focusedParameter = {});
    void rebuildSectionsAfterEffectOrderChange(const String& preferredSection, const String& focusedTitle = {},
                                               const String& focusedParameter = {});
    void connectModulationAndPromptForAmount(const String& sourceId, const String& destinationId, Component& target);
    void promptForInitialModulationAmount(const String& sourceId, const String& destinationId, bool created, Component& target);
    void promptForParameterValue(const String& parameterId, Component& target);
    void promptForMacroName(int macroIndex, Component& target);
    void promptForCustomValue(const String& title, const String& value, Component& target,
                              std::function<void(const String&)> apply);
    void applyInlineTextPrompt();
    void applyInitialModulationAmount();
    void applyParameterValue();
    void cancelInlineTextPrompt();
    void cancelInitialModulationAmount();
    // Where the inline text prompt sends focus back when it closes.
    void rememberPromptFocus(Component& target);
    // Focuses that control again, after rebuilding the sections if `rebuild`.
    // Call before hiding the prompt: hiding the focused text field first hands
    // focus to the top of the window. Falls back to fallbackParameterId's row.
    void returnFocusFromPrompt(bool rebuild, const String& fallbackParameterId);
    void hideInlineTextPrompt();
    ValueBridge* parameterBridge(const String& id) const;
    void setParameterEngineValue(const String& id, float engineValue);
    void randomizeAmplitudeSeed(const String& id);
    void setInternalParameterEngineValue(const String& id, float engineValue);
    void refreshVisibleModulationLabels();
    LineGenerator* activeModulationRemapGenerator() const;
    int selectedModulationRemapPointIndex() const;
    bool selectModulationRemapSlot(int slot);
    String modulationRemapPointDescriptionFor(LineGenerator* generator, int pointIndex) const;
    String modulationRemapPointDescription(int pointIndex) const;
    String modulationRemapStatusTextForSlot(int slot) const;
    void finishModulationRemapEdit(const String& announcement = {});
    void selectModulationRemapPoint(int direction);
    void addModulationRemapPoint();
    void deleteModulationRemapPoint();
    void moveModulationRemapPointInput(float delta);
    void moveModulationRemapPointOutput(float delta);
    void setModulationRemapCurve(int curve);
    void flipModulationRemap(bool horizontal);
    void resetModulationRemap();
    bool handleModulationRemapShortcut(const KeyPress& key);
    bool isModulationDestinationId(const String& id) const;
    std::vector<std::pair<String, String>> modulationSourcesForDestination(const String& destinationId) const;
    void removeModulationFromParameter(const String& sourceId, const String& destinationId, Component& target);
    // Disconnects a modulation slot (1-based) and resets its amount, options and remap.
    void clearModulationSlot(int slot);
    void addTranceGateEditor(const String& prefix, const String& section, Component& parent,
                             int& y, int x, int width, int& focusOrder);
    // Re-reads the active LFO's mode, cycle length and points before an edit.
    void syncLfoEditorState();
    void rebuildLfoEditorItems();
    void selectLfoEditorItemAtPhase(float phase, bool notify);
    void setLfoEditorItem(int index, bool notify);
    void lfoEditorItemChosen();
    bool handleLfoMsegShortcut(const KeyPress& key);
    String lfoMsegStatusText() const;
    void applyLfoMode(int mode);
    void applyLfoCycleLength();
    int lfoCycleIndexFromParameters(int lfoIndex) const;
    bool syncLfoCycleLengthFromParameters();
    void moveLfoCursor(float delta);
    void moveToLfoPoint(int direction);
    void moveToLfoPerformerBlock(int direction);
    void stepLfoCurve(int direction);
    void stepLfoShapePreset(int direction);
    void stepLfoCycleLength(int direction);
    void stepLfoGrid(int direction);
    void switchLfoFromShortcut(int lfoIndex);
    void clearLfoShape();
    void applyLfoShapePreset();
    void addLfoPoint();
    void deleteLfoPoint();
    void clearLfoPerformerBlock();
    void moveLfoPointTime(float delta);
    void moveLfoPointValue(float delta);
    void toggleLfoPointSelection();
    void toggleLfoMultiSelectionMode();
    void clearLfoPointSelection();
    void copyLfoPoints();
    void pasteLfoPoints();
    void copyLfoShape();
    void pasteLfoShape();
    void duplicateLfoShapeToNextSlot();
    void setLfoPointCurve(int curveIndex);
    LineGenerator* lfoGeneratorForIndex(int lfoIndex) const;
    LineGenerator* activeLfoGenerator() const;
    int selectedLfoPointIndex() const;
    int lfoPointIndexAtPhase(float phase) const;
    int currentLfoPointIndex() const;
    std::vector<int> selectedLfoPointIndices() const;
    bool isLfoPointSelected(float phase) const;
    void pruneSelectedLfoPointPhases();
    float lfoCycleQuarterNotes() const;
    float lfoGridQuarterNotes() const;
    float lfoGridAmount() const;
    float lfoEditorBlockStart(float phase) const;
    float lfoEditorBlockEnd(float start) const;
    float lfoEditorBlockSamplePhase(float start) const;
    std::vector<int> lfoEditorBlockPointIndices(LineGenerator* generator, float start) const;
    bool lfoPerformerBlockHasShape(LineGenerator* generator, float start) const;
    void writeLfoPoints(LineGenerator* generator, std::vector<CopiedLfoPoint> points, bool smooth);
    void replaceLfoEditorBlock(const std::vector<CopiedLfoPoint>& replacement, bool removeEndPoint);
    float lfoPhaseToQuarterNotes(float phase) const;
    float lfoQuarterNotesToPhase(float quarterNotes) const;
    float snapLfoPhaseToGrid(float phase) const;
    String lfoPointDescriptionFor(int lfoIndex, int pointIndex) const;
    String lfoPointDescription(int pointIndex) const;
    String lfoTimeDescriptionFor(int lfoIndex, float phase) const;
    String lfoTimeDescription(float phase) const;
    String lfoMsegStatusTextFor(int lfoIndex) const;
    String modulationSlotHeaderTitle(int slot) const;
    String modulationSlotTitle(int slot) const;
    String modulationControlTitle(const String& parameterId) const;
    bool focusShortcutTarget(const KeyPress& key);
    void toggleQwertyKeyboard();
    bool updateQwertyNotes();
    void allQwertyNotesOff();
    bool isQwertyKeyboardActive() const;
    void attachQwertyKeyInterceptor();
    void detachQwertyKeyInterceptor();
    bool focusGroupShortcut(const String& group, const String& fallbackSection = {});
    bool focusSectionShortcut(const String& section);
    bool handleMacroShortcut(const String& parameterId, const KeyPress& key, Component& target);
    bool handleEffectShortcut(const String& sectionName, const KeyPress& key, Component& target);
    bool moveEffectInSection(const String& sectionName, const String& effectId, int direction,
                             const String& focusedTitle);
    float modulationAmountFromPeakText(const String& destinationId, const String& text) const;
    String modulationAmountSummary(const String& sourceId, const String& destinationId, float amount) const;
    bool isMacroBipolar(int macroIndex) const;
    void setMacroBipolar(int macroIndex, bool bipolar);
    void refreshWavetableBrowserCache();
    void refreshSampleBrowserCache();
    void primeBrowserFileCaches();
    int oscillatorIndexForSectionName(const String& sectionName) const;
    void showOscillatorContextMenu(int targetOscillator, Component& target);
    void copyOscillatorToClipboard(int sourceOscillator, bool includeModulations);
    void pasteOscillatorClipboard(int targetOscillator, Component& target);

    std::map<String, TranceGateEditor::State> trance_gate_editor_states_;
    SynthPlugin& synth_;
    void addCpuMeter(Component& parent, int x, int& y, int width, int& focusOrder);
    std::unique_ptr<CpuUsageMeter> cpu_meter_;
    Label title_;
    TextButton menu_button_ { "Menu" };
    // The group that number keys 1 to 9 choose an element from.
    String current_group_;
    Label preset_summary_;
    Label preset_details_;
    TextButton preset_menu_ { "Preset menu" };
    TextEditor preset_search_;
    PresetTableListBox preset_collection_table_ { "Collection" };
    PresetTableListBox preset_library_table_ { "Library" };
    PresetTableListBox preset_bank_table_ { "Bank" };
    PresetTableListBox preset_category_table_ { "Category" };
    PresetTableListBox preset_tag_table_ { "Tags" };
    PresetTableListBox preset_table_ { "Preset table" };
    ToggleButton preset_preview_ { "Autoload preset when scrolling" };
    Label save_patch_prompt_;
    TextEditor save_patch_name_;
    TextEditor save_patch_author_;
    TextEditor save_patch_bank_;
    TextEditor save_patch_category_;
    TextEditor save_patch_tags_;
    TextEditor save_patch_comments_;
    TextButton save_patch_ok_ { "Save" };
    TextButton save_patch_cancel_ { "Cancel" };
    Label routing_summary_;
    AccessibleComboBox routing_mode_;
    TextButton routing_default_ { "Default routing" };
    TextButton routing_serial_forward_ { "Serial filters 1 into 2" };
    TextButton routing_serial_backward_ { "Serial filters 2 into 1" };
    Label modulation_amount_prompt_;
    TextEditor modulation_amount_editor_;
    TextButton modulation_amount_ok_ { "OK" };
    TextButton modulation_amount_cancel_ { "Cancel" };
    Viewport viewport_;
    Component rows_container_;
    std::vector<std::unique_ptr<Component>> section_headers_;
    std::vector<std::unique_ptr<AccessibleParameterRow>> rows_;
    std::vector<std::unique_ptr<ValueBridge>> internal_fx_bridges_;
    bool fine_tune_sample_loop_ = false;
    std::map<String, std::vector<AudioProcessorParameter*>> sections_;
    std::map<String, StringArray> group_sections_;
    std::map<String, ValueBridge*> parameters_by_id_;
    StringArray group_names_;
    StringArray section_names_;
    Array<File> all_presets_;
    Array<File> filtered_presets_;
    std::vector<PresetBrowserItem> preset_index_;
    std::unordered_map<std::string, size_t> preset_index_by_path_;
    std::set<std::string> preset_favorites_;
    std::vector<PresetFacetItem> preset_facets_;
    StringArray preset_libraries_;
    StringArray preset_banks_;
    StringArray preset_categories_;
    StringArray preset_tags_;
    std::unique_ptr<PresetFacetTableModel> preset_collection_model_;
    std::unique_ptr<PresetFacetTableModel> preset_library_model_;
    std::unique_ptr<PresetFacetTableModel> preset_bank_model_;
    std::unique_ptr<PresetFacetTableModel> preset_category_model_;
    std::unique_ptr<PresetFacetTableModel> preset_tag_model_;
    std::unique_ptr<PresetResultsTableModel> preset_table_model_;
    StringArray modulation_source_ids_;
    struct CopiedOscillatorParameter {
      String suffix;
      float engine_value = 0.0f;
    };
    struct CopiedModulation {
      String source_id;
      String destination_suffix;
      float amount = 0.0f;
      float power = 0.0f;
      float bipolar = 0.0f;
      float stereo = 0.0f;
      float bypass = 0.0f;
      float random_mode = 0.0f;
    };
    struct CopiedOscillatorState {
      int source_oscillator = -1;
      bool include_modulations = false;
      json wavetable;
      json sample;
      // A Multi-sample slot's instrument (shared, not reloaded, when pasted).
      json instrument;
      std::vector<CopiedOscillatorParameter> parameters;
      std::vector<CopiedModulation> modulations;
    };
    CopiedOscillatorState copied_oscillator_;
    struct CopiedEffectSlot {
      int source_bus = -1;
      int type = 0;
      bool enabled = true;
      bool include_modulations = false;
      // Suffixes after the slot prefix, e.g. "delay_feedback", with engine values.
      std::vector<std::pair<String, float>> parameters;
      json impulse_response;
      json filter_table;
      std::vector<CopiedModulation> modulations;
    };
    CopiedEffectSlot copied_effect_slot_;
    std::vector<std::pair<String, Component::SafePointer<Button>>> fx_bus_toggles_;
    std::vector<CopiedLfoPoint> copied_lfo_points_;
    std::vector<CopiedLfoPoint> copied_lfo_shape_;
    bool copied_lfo_shape_smooth_ = false;
    int copied_lfo_shape_mode_ = 0;
    std::vector<float> selected_lfo_point_phases_;
    // The keyboard editor's items (points, performer shapes or steps) and the chosen one.
    std::vector<float> lfo_editor_item_phases_;
    int lfo_editor_item_index_ = -1;
    // The shape that Shift A applies and G and H step through, as an index into the preset names.
    int lfo_shape_preset_index_ = 0;
    struct WavetableKeyboardState {
      int frame = 0;
      int harmonic = 1;
      int starting_waveform = 0;
      std::set<int> selected_frames;
      int layer = 0;
      int new_layer_type = 0;
      int add_modifier_type = 0;
      int line_point = 0;
      std::map<int, int> keyframes;   // selected keyframe per component of the selected layer
    };
    struct CopiedWavetableFrame {
      std::vector<float> samples;
      float frequency_ratio = vital::WaveFrame::kDefaultFrequencyRatio;
      float sample_rate = vital::WaveFrame::kDefaultSampleRate;
    };
    WavetableKeyboardState wavetable_keyboard_state_[vital::kNumOscillators];
    std::vector<CopiedWavetableFrame> copied_wavetable_frames_;
    std::vector<File> cached_wavetable_browser_files_;
    std::vector<File> cached_sample_browser_files_;
    std::vector<File> cached_ir_browser_files_;
    struct IrBrowserScan {
      std::atomic<bool> cancelled { false }, ready { false };
      std::vector<File> files;
    };
    std::shared_ptr<IrBrowserScan> ir_browser_scan_;
    std::thread ir_browser_scan_thread_;
    struct PresetScan {
      std::thread thread;
      std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<PresetScan> preset_scans_;
    std::shared_ptr<std::atomic<bool>> preset_scan_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    struct IrNavigation {
      File start;
      uint64_t request = 0;
      int64_t steps = 0;
      int first_step = 0;
    };
    std::map<String, IrNavigation> pending_ir_navigation_;
    Component::SafePointer<Component> pending_ir_browser_target_;
    String pending_ir_browser_prefix_;
    std::map<String, uint64_t> pending_ir_loads_;
    struct IrControls {
      Component::SafePointer<Component> group, browse;
      uint64_t revision = UINT64_MAX;
    };
    std::map<String, IrControls> ir_controls_;
    struct FilterTableControls {
      Component::SafePointer<Component> group, current;
    };
    std::map<String, FilterTableControls> filter_table_controls_;
    std::vector<Component*> focus_order_;
    std::vector<Component*> accessibility_focus_order_;
    std::vector<Component*> row_focus_order_;
    std::vector<Component*> top_level_accessibility_order_;
    std::unique_ptr<FileChooser> preset_chooser_;
    std::unique_ptr<FileChooser> wavetable_chooser_;
    std::unique_ptr<FileChooser> sample_chooser_;
    std::unique_ptr<FileChooser> instrument_chooser_;
    std::unique_ptr<ChildProcess> text_editor_process_;
    File last_instrument_folder_;
    struct InstrumentAnnouncementState {
      uint32_t revision = 0;
      int state = 0;
      int announced_percent = 0;
      uint32 announced_at = 0;
    };
    std::array<InstrumentAnnouncementState, vital::kNumOscillators> instrument_announcements_ {};
    std::unique_ptr<FileChooser> lfo_chooser_;
    std::unique_ptr<FileChooser> fx_chooser_;
    std::unique_ptr<FileChooser> fx_bus_chooser_;
    std::unique_ptr<FileChooser> ir_chooser_;
    std::unique_ptr<FileChooser> bank_chooser_;
    std::unique_ptr<FileChooser> tuning_chooser_;
    std::unique_ptr<FileChooser> export_chooser_;
    String pending_modulation_source_;
    String pending_modulation_destination_;
    String pending_parameter_value_id_;
    // The control the inline text prompt returns to, and how to find it again
    // if a rebuild replaces it while the prompt is open.
    Component::SafePointer<Component> pending_parameter_value_focus_;
    String pending_prompt_focus_parameter_;
    String pending_prompt_focus_title_;
    String pending_prompt_focus_section_;
    String pending_custom_value_title_;
    std::function<void(const String&)> pending_custom_value_apply_;
    bool pending_modulation_created_ = false;
    int active_oscillator_ = -1;
    int active_lfo_index_ = 0;
    std::map<String, std::pair<int, int>> last_filter_states_;
    std::map<String, bool> last_random_amplitude_modes_;
    std::map<String, int> last_oscillator_modes_;
    // An SF2 preset menu lists another SoundFont than its slot plays.
    bool instrument_preset_menu_stale_ = false;
    int last_glide_zones_ = -1;
    int last_envelope_analog_ = -1;
    int lfo_cycle_index_ = 3;
    int lfo_grid_index_ = 14;
    float lfo_cursor_phase_ = 0.0f;
    bool preset_list_loaded_ = false;
    bool preset_list_loading_ = false;
    String preset_index_progress_;       // spoken in the summary while a scan indexes files
    double last_preset_autoload_ms_ = -1.0e9;
    bool preset_autoload_scheduled_ = false;
    bool preset_filter_update_pending_ = false;
    bool preset_filter_rebuild_pending_ = false;
    bool preset_favorites_only_ = false;
    int preset_sort_column_ = 1;
    bool preset_sort_ascending_ = true;
    bool preset_metadata_edit_mode_ = false;
    File preset_metadata_file_;
    int preset_list_generation_ = 0;
    bool save_patch_dialog_visible_ = false;
    bool updating_preset_list_ = false;
    bool updating_routing_controls_ = false;
    bool wavetable_browser_cache_valid_ = false;
    bool wavetable_editor_visible_[vital::kNumOscillators] = {};
    bool preset_to_wavetable_rendering_ = false;
    bool sample_browser_cache_valid_ = false;
    bool ir_browser_cache_valid_ = false;
    int active_modulation_remap_connection_ = -1;
    int modulation_remap_point_index_ = 0;
    bool lfo_multi_selection_mode_ = false;
    bool modulation_amount_prompt_visible_ = false;
    bool parameter_value_prompt_visible_ = false;
    bool qwerty_keyboard_on_ = false;
    int qwerty_octave_ = 5;
    int qwerty_velocity_ = 127;
    std::set<int> qwerty_notes_down_;
    std::set<int> qwerty_control_keys_down_;
    // Focused controls see a key before this editor does. While the virtual keyboard is on,
    // this listener sits on the focused control and hands note, octave and velocity keys to
    // SynthEditor::keyPressed first, so they play notes instead of also running the
    // control's own shortcut (for example I and K in the LFO editor).
    class QwertyKeyInterceptor : public KeyListener {
      public:
        explicit QwertyKeyInterceptor(SynthEditor& editor) : editor_(editor) { }
        bool keyPressed(const KeyPress& key, Component* origin) override;

      private:
        SynthEditor& editor_;
    };
    QwertyKeyInterceptor qwerty_key_interceptor_ { *this };
    Component::SafePointer<Component> qwerty_key_interceptor_target_;
    bool effect_chain_rebuild_queued_ = false;
    // Bumped by each rebuild so only the last one re-focuses the screen reader.
    int screen_reader_refocus_generation_ = 0;
    // Set when a queued rebuild should re-focus the screen reader once it runs.
    bool screen_reader_refocus_requested_ = false;
    String queued_effect_chain_rebuild_section_;
    String queued_effect_chain_rebuild_title_;
    String queued_effect_chain_rebuild_parameter_;
    // A request that arrived while a rebuild was already queued (or running) and
    // may still need a rebuild of its own once the current one has finished.
    bool queued_effect_chain_rebuild_pending_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthEditor)

    friend class PresetFacetTableModel;
    friend class PresetResultsTableModel;
};

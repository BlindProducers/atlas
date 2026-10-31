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
#include "community_library.h"
#include "library_layout.h"

#include <array>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

class AccessibleParameterRow;
class AccessibleSectionHeader;
class CpuUsageMeter;
class TransferProgressBar;
class LineGenerator;
class WaveSource;
class WavetableComponent;
class PresetFacetTableModel;
class PresetResultsTableModel;
class ValueBridge;

namespace vital {
  class ConvolutionReverbModule;
}

// Key names for help texts and menus. Command is a key of its own only on
// macOS; elsewhere JUCE reports Ctrl as the command modifier, and there is no
// VoiceOver to open a context menu.
namespace atlas::keys {
  inline String commandName() {
#if JUCE_MAC
    return "Command";
#else
    return "Ctrl";
#endif
  }

  inline String contextMenuKeys() {
#if JUCE_MAC
    return "Shift M, the right bracket key, or VoiceOver Shift M";
#else
    return "Shift M or Shift F10";
#endif
  }
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
  // A preset shared on the community library (atlas.blindproducers.org)
  // rather than one on disk: where to fetch it, what the download must hash
  // to, and what the site says about it. `file` is where the download lands,
  // which may not exist yet; `downloaded` says whether it does.
  String remote_url;
  String remote_sha256;
  String remote_uploader;
  int remote_downloads = 0;
  bool downloaded = false;
  bool isRemote() const { return remote_url.isNotEmpty(); }
  // Its audio preview on the library, when the uploader added one: where to
  // fetch it, where the fetched copy is kept, its size and its length.
  String remote_preview_url;
  File preview_file;
  int64 preview_size = 0;
  double preview_seconds = 0.0;
  bool hasPreview() const { return remote_preview_url.isNotEmpty(); }
  // A bank shared on the library rather than a preset: `file` is its .atb
  // archive, `downloaded` says it was fetched and unpacked, and the row says
  // what it holds ("62 presets") and how much room that takes.
  bool remote_bank = false;
  String bank_contents;
  int64 unpacked_bytes = 0;
};

struct PresetFacetItem {
  int kind = 0;
  String section;
  String value;
  int count = 0;
  bool active = false;
};

// The Collection facet: every preset on disk, the favourites among them, or
// the community library's presets or banks, listed from its JSON API.
enum class PresetCollection { kAll, kFavourites, kCommunity, kCommunityBanks };

// A list keeps the keyboard focus while the arrow, Page and Home/End keys move
// its selection. JUCE reports the new selection to Windows screen readers on
// the list rather than on the row, so NVDA says nothing; onSelectionKey runs
// after those keys, even at either end of the list, so the editor can say the row.
template <typename ListType>
class SelectionKeyList : public ListType {
  public:
    using ListType::ListType;

    std::function<void()> onSelectionKey;

    bool keyPressed(const KeyPress& key) override {
      if (!ListType::keyPressed(key))
        return false;
      if (onSelectionKey && isSelectionKey(key))
        onSelectionKey();
      return true;
    }

  private:
    static bool isSelectionKey(const KeyPress& key) {
      for (const int code : { KeyPress::upKey, KeyPress::downKey, KeyPress::pageUpKey,
                              KeyPress::pageDownKey, KeyPress::homeKey, KeyPress::endKey }) {
        if (key.isKeyCode(code))
          return true;
      }
      return false;
    }
};

class PresetTableListBox : public SelectionKeyList<TableListBox> {
  public:
    using SelectionKeyList::SelectionKeyList;

    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<void()> onFocusGained;

    void focusGained(FocusChangeType cause) override {
      SelectionKeyList::focusGained(cause);
      if (onFocusGained)
        onFocusGained();
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed && onKeyPressed(key))
        return true;
      return SelectionKeyList::keyPressed(key);
    }
};

// A toggle that says its new state after the user flips it. JUCE reports a
// toggle's change to Windows screen readers only as a new value, which NVDA
// never speaks for a check box; VoiceOver reads the change itself, so this
// speaks on Windows only (see SpokenToggleButton::clicked).
class SpokenToggleButton : public ToggleButton {
  public:
    using ToggleButton::ToggleButton;

  protected:
    void clicked() override;
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
      if (isArrowKeyFromAnOpenMenu(*this, key))
        return true;
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

// A text box VoiceOver reaches while it is scrolled out of view. JUCE leaves
// a control out of its group's accessibility children when it is not in view
// unless its handler reports it accessible offscreen, as the rows, buttons and
// menus here do, and VoiceOver's own navigation never scrolls the editor: a
// plain TextEditor only turned up once Tab had scrolled it into view. Its
// help is the help text, where JUCE's handler reads the tooltip.
class AccessibleTextEditor : public TextEditor {
  public:
    using TextEditor::TextEditor;

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override;
};

// The modulation amount prompt's slider, after its text box. It moves through the values the modulation can take its destination to, and
// steps and reads them as the destination's own slider does; the route plays
// each amount at once. Return keeps the amount, as OK does; Escape is Cancel.
class ModulationAmountSlider : public ParameterSlider {
  public:
    std::function<void()> onReturnKey;
    std::function<void()> onEscapeKey;

    bool keyPressed(const KeyPress& key) override {
      if (key.getKeyCode() == KeyPress::returnKey && onReturnKey) {
        onReturnKey();
        return true;
      }
      if (key.getKeyCode() == KeyPress::escapeKey && onEscapeKey) {
        onEscapeKey();
        return true;
      }
      return ParameterSlider::keyPressed(key);
    }

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
      setHelpText("Keys 1 through 6 apply Sine, Saturated sine, Triangle, Square, Pulse, or Saw to the current frame selection. Left and right move between frames. Shift left and right extend frame selection. Home and End move to the first and last frame. S toggles frame selection. R clears selection. " + atlas::keys::commandName() + " A or Shift A selects all frames. Comma and period move between harmonics. Page Up and Page Down move by ten harmonics. Up and Down change harmonic level by one percent; hold Shift for five percent. J and L change harmonic phase by one degree; hold Shift for fifteen degrees. Backspace clears the harmonic. F removes the fundamental. Zero removes DC. N normalizes. A adds a frame. E or Delete deletes frames. C and V copy and paste frames. I cycles interpolation modes.");
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
    // The Browse menus' lists of LFO presets, FX presets, FX bus presets and
    // multisample instruments: every library's folder of the kind, scanned on
    // first use, again after Rescan and after a save of that kind.
    enum class PresetFileList { kLfoPresets, kFxPresets, kFxBusPresets, kInstruments };
    const std::vector<File>& presetFiles(PresetFileList list);
    // The FX presets saved for `effectId`, in the Browse menu's order.
    std::vector<File> effectPresetFiles(const String& effectId);
    // Loads an FX preset into the effect in a chain's slot; false when it is
    // for another effect or sets nothing.
    bool loadEffectPresetIntoSlot(const String& sectionName, int slot, const File& file);
    // Previous and Next: the preset before or after the one last loaded,
    // through every library, wrapping at either end.
    void loadShiftedEffectPreset(const String& sectionName, int slot, int direction);
    void loadShiftedLfoPreset(int lfoIndex, int direction);
    // Says what the library migration did (SynthPlugin::createEditor runs it
    // before the editor is made), once the editor is up.
    void announceLibraryMigration(const atlas::library::MigrationReport& migration);
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
    void focusGained(FocusChangeType cause) override;
    void focusOfChildComponentChanged(FocusChangeType cause) override;

    const std::vector<Component*>& getAccessibleFocusOrder() const { return accessibility_focus_order_; }
    const std::vector<Component*>& getKeyboardFocusOrder() const { return focus_order_; }
    void ensureComponentVisible(Component* component);
    // The context menu of an effect slot header: copy, paste to another bus, replace, remove.
    // Each item carries its own action, so the menu can be exercised without showing it.
    PopupMenu createEffectSlotContextMenu(const String& sectionName, int slot, const String& focusedTitle);
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
    // Tests: Assign modulation's amount prompt left open, with its text box
    // and its slider.
    void openModulationAmountPromptForTesting(const String& sourceId, const String& destinationId,
                                              Component* target = nullptr) {
      connectModulationAndPromptForAmount(sourceId, destinationId, target != nullptr ? *target : *this);
    }
    bool isModulationAmountPromptOpen() const { return modulation_amount_prompt_visible_; }
    TextEditor& modulationAmountEditorForTesting() { return modulation_amount_editor_; }
    ParameterSlider& modulationAmountSliderForTesting() { return modulation_amount_slider_; }
    ToggleButton& modulationBipolarToggleForTesting() { return modulation_bipolar_toggle_; }
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
    int presetRowCount() const { return filtered_presets_.size(); }
    // Community library: whether its list is being fetched or a preset is being
    // downloaded, whether the list arrived, and why it did not.
    bool isCommunityLibraryBusy() const { return community_list_loading_; }
    bool isCommunityLibraryLoaded() const { return community_list_loaded_; }
    bool isCommunityDownloadBusy() const { return !community_downloads_.empty(); }
    // The download bar's percentage, -1 before anything has landed or while
    // no download is on.
    int downloadProgressPercentForTesting() const;
    String communityLibraryError() const { return community_list_error_; }
    // Settings, for the tests: the window it opened, or null.
    DialogWindow* showSettingsForTesting() {
      showSettings();
      return settings_window_.getComponent();
    }
    // The Settings window, if open (the Settings button opens it).
    DialogWindow* settingsWindowForTesting() const { return settings_window_.getComponent(); }
    bool isPresetBrowserShown() const { return preset_browser_shown_; }
    // "2.0.0 beta 2": the version and, for a pre-release, the beta the
    // release workflow builds (ATLAS_PRERELEASE in .github/workflows/release.yml).
    static String versionText();
    // The Preset menu's Share patch and Share bank, for the tests: the form,
    // or first the login dialog while this DAW is not logged in.
    void sharePatchForTesting() { shareOnCommunity(false); }
    void shareBankForTesting() { shareOnCommunity(true); }
    bool isShareFormVisible() const { return share_form_visible_; }
    bool isShareBusy() const { return share_busy_; }
    // The login dialog (showCommunityLogin) while it is open, or null.
    DialogWindow* communityLoginWindowForTesting() const { return login_window_.getComponent(); }
    bool isCommunityBankLibraryBusy() const { return community_banks_loading_; }
    bool isCommunityBankLibraryLoaded() const { return community_banks_loaded_; }
    String communityBankLibraryError() const { return community_banks_error_; }
    // Community previews: the cached file of the preview playing or being
    // fetched (none when idle), and whether one is being fetched or plays.
    File communityPreviewFile() const { return preview_file_; }
    bool isCommunityPreviewBusy() const { return preview_fetching_; }
    bool isCommunityPreviewPlaying() const { return preview_transport_.isPlaying(); }
    // Tests: what a message box, file chooser or dialog window does once it
    // closes, having opened while `returnTo` had the focus.
    void closeOsWindowForTesting(Component& returnTo) {
      FocusReturn focus;
      focus.component = &returnTo;
      focus.title = returnTo.getTitle();
      closeOsWindow(openOsWindow(std::move(focus)), {});
    }
    // Tests: a rebuild's focus restore for a control titled `title` in `section`.
    bool restoreFocusForTesting(const String& title, const String& section) {
      return restoreFocusAfterRebuild({}, nullptr, title, section, false);
    }
    // Tests: what moving a preset to Trash does to its file (a test
    // deletes the presets it made instead). Empty for the real move.
    static std::function<bool(const File&)>& trashFileForTesting();
    // Tests: the Preset menu's Save patch as default and Initialize patch.
    void savePatchAsDefaultForTesting() { savePatchAsDefault(); }
    bool chooseDefaultPatchForTesting(const File& preset) { return chooseDefaultPatch(preset, nullptr); }
    bool resetDefaultPatchForTesting() { return resetDefaultPatch(nullptr); }
    String defaultPatchNameForTesting() const { return defaultPatchName(); }
    void initializePatchForTesting() { initializePatch(); }
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
    std::unique_ptr<AccessibleParameterRow> buildAccessibleParameterRow(AudioProcessorParameter& parameter,
                                                                        const String& sectionName);
    int sampleLoopPointLength(const String& parameterId) const;
    void toggleSampleLoopFineTune();
    void wireSampleLoopFineTune(AccessibleParameterRow& row);
    bool shouldShowParameterInSection(const String& sectionName, AudioProcessorParameter* parameter) const;
    bool refreshFilterRowsIfNeeded();
    bool refreshSeedRowsIfNeeded();
    // An LFO's Simple and Custom views show different rows, and the Simple
    // view shows the one rate row its Sync uses.
    static int lfoIndexForParameterId(const String& parameterId);
    bool isLfoSimpleView(int lfo) const;
    int lfoSyncOption(int lfo) const;
    int lfoRowLayout(int lfo) const;
    void setLfoView(int lfo, int view);
    bool refreshLfoRowsIfNeeded();
    bool isAnalogOscillator(int oscillator) const;
    int sourceSlotType(int oscillator) const;
    void setAutomationProxyCallbacks(AccessibleParameterRow& row);
    int oscillatorModeKey(int oscillator) const;
    bool refreshOscillatorModeRowsIfNeeded();
    bool refreshZoneCrossfadeRowIfNeeded();
    bool refreshEnvelopeAnalogRowsIfNeeded();
    int currentGlideZones() const;
    String focusedParameterId() const;
    String focusedAccessibleTitle() const;
    // The SectionName of the focused control or its nearest group, so a rebuild
    // can find the same control when several sections share its title.
    String focusedSectionKey() const;
    Component* persistentFocusedComponent() const;
    // Focuses the control a rebuild replaced; false when there is none to focus.
    // refocusScreenReader moves the screen reader to it again once the rows
    // settle. Announcements made meanwhile wait for that move, which would
    // otherwise cut them short.
    bool restoreFocusAfterRebuild(const String& parameterId, Component* persistentComponent,
                                  const String& accessibleTitle = {}, const String& preferredSection = {},
                                  bool refocusScreenReader = false);
    Component* findRebuildFocusTarget(const String& parameterId, Component* persistentComponent,
                                      const String& accessibleTitle, const String& preferredSection) const;
    // The heading of `section`, or of the nearest section whose key it starts
    // with ("Oscillator 1" for "Oscillator 1 pitch and unison"); null if none.
    Component* sectionHeadingFor(const String& section) const;
    // On Windows, once a rebuild has settled with no control focused while the
    // plugin's window still has the keyboard (JUCE would give every key to the
    // host), focuses `section`'s heading, or else the editor.
    void refocusIfFocusOrphaned(const String& section);
    // A control to give focus back to. Its parameter, title and section find
    // its replacement when a rebuild has deleted it.
    struct FocusReturn {
      String parameter_id, title, section;
      Component::SafePointer<Component> component;
      bool isEmpty() const { return parameter_id.isEmpty() && title.isEmpty() && component == nullptr; }
    };
    // The focused control or, when the window has lost focus to a file
    // chooser, a menu or the host, the control it will give focus back to.
    FocusReturn focusToReturnTo() const;
    // Opens a file chooser and, once it closes, puts keyboard and screen reader
    // focus back on the control that opened it, or on its replacement.
    void launchFileChooser(FileChooser& chooser, int flags, std::function<void(const FileChooser&)> callback);
    // Shows a native message box and, once it closes and `callback` has run,
    // puts focus back on `returnTo` (else the control focused when it opened)
    // or its replacement. On Windows JUCE runs the box on a thread of its own,
    // and when it closes Windows gives the focus to the host's window, not
    // back to the plugin's, so NVDA ends up in the host. The callback gets the
    // plain index of the button pressed (0 for the first).
    void showMessageBox(const MessageBoxOptions& options, std::function<void(int)> callback = {},
                        Component* returnTo = nullptr);
    void showWarningBox(const String& title, const String& message, Component* returnTo = nullptr);
    // Opens a dialog window (keyboard help, accessibility settings) and puts
    // focus back once it has closed, as for a file chooser.
    DialogWindow* launchDialogWindow(DialogWindow::LaunchOptions& options);
    // A file chooser, message box or dialog window that has not closed yet,
    // and the control to give focus back to when it does.
    struct OpenOsWindow;
    std::shared_ptr<OpenOsWindow> openOsWindow(FocusReturn focus);
    // Runs a window's callback once it has closed; when no other window is
    // still open, gives focus back. A window the callback opens (an error box
    // after a file chooser) gives focus back to the same control instead.
    // Static: the editor may be gone by the time the window closes.
    static void closeOsWindow(const std::shared_ptr<OpenOsWindow>& window, const std::function<void()>& callback);
    void restoreFocusAfterOsWindow(const FocusReturn& focus);
    // The control a message box should give focus back to: the focused one,
    // or the one an inline prompt that is closing returns to.
    FocusReturn focusForMessageBox() const;
    // Asks a yes or no question in the editor, in the inline prompt with its
    // text box hidden: the first button (`confirmLabel`) runs `confirm`,
    // Cancel (`cancelLabel`) and Escape do nothing and say nothing, and both go
    // back to `target`.
    // Used on Windows in place of a native box, which would take focus out of the plugin.
    void promptForConfirmation(const String& question, const String& confirmLabel, Component& target,
                               std::function<void()> confirm, const String& cancelLabel = "Cancel");
    // Gives the inline prompt back its text box and OK button after a question.
    void endConfirmationPrompt();
    bool isConfirmationButtonFocused() const {
      return confirmation_prompt_visible_ &&
             (modulation_amount_ok_.hasKeyboardFocus(false) || modulation_amount_cancel_.hasKeyboardFocus(false));
    }
    // On Windows a rebuild that a choice deciding the rows starts (an
    // oscillator's Type, a filter's Model, an effect's mode, or Envelope 1's
    // Analog switch) keeps the focused row of such a choice and the groups
    // around it, and the new rows take them
    // back in place of fresh copies (makeSectionHeader, createAccessibleParameterRow).
    // NVDA then stays on the same control in the same groups and reads only its
    // new value, instead of every group around it and the control again, which
    // the next quick step cut short. Call before the old rows are cleared, and
    // releaseKeptFocus once focus is back.
    void keepFocusedSwitchingRow();
    void releaseKeptFocus();
    std::unique_ptr<AccessibleSectionHeader> makeSectionHeader(const String& title, int groupRank, bool topLevel,
                                                               const String& sectionKey = {});
    void moveToSection(int direction);
    // Comma and period on an effects chain (Windows): from the chain into
    // Slot 1, Slot 2 and on through the slots that hold an effect, then to the
    // next element, and back the same way. False when the step leaves the
    // chain, which is then the element moveToSection steps on from.
    bool moveThroughEffectSlots(int direction);
    // Focuses the header of the effect at `position` (0 for Slot 1) in the
    // chain `sectionName`; false when there is none.
    bool focusEffectSlotHeader(const String& sectionName, int position);
    // Where comma and period stop in the chain `sectionName`, in order: each
    // occupied slot's header and, on Windows, the grouping headings inside it
    // (kSectionKeysStepThroughEffectGroups).
    std::vector<Component*> effectChainStops(const String& sectionName) const;
    void focusEffectChainStop(Component& stop);
    void armOrCancelMidiLearn(const String& parameterId);
    // The Settings window, which the toolbar's Settings button opens: Plugin
    // info, Tuning, MIDI learn mappings, Accessibility, Online community
    // browser and Keyboard help, each a group a screen reader enters.
    void showSettings();
    // The toolbar's Undo and Redo name the change they would undo or redo.
    // With `evenIfPending` false (the timer) they wait while a change is still
    // becoming a step, since describing the history would end it early.
    void updateToolbarUndoButtons(bool evenIfPending);
    // Shows or hides the preset browser in the Presets group, for this instance.
    void setPresetBrowserShown(bool shown);
    // Shows or hides the community library in the preset browser and the
    // Preset menu, for every editor from now on (community_library.h).
    void setCommunityBrowserShown(bool shown);
    // Every keyboard shortcut, with this platform's key names, as the Keyboard
    // help group of Settings shows it.
    static String keyboardHelpText();
    void initializePatch();
    void savePatchAsDefault();
    // Settings' Default patch group. Browse copies the chosen patch to the
    // default patch, and Reset default patch moves it to Trash; undo takes
    // either back. False when the file could not be read, written or moved.
    bool chooseDefaultPatch(const File& preset, Component* returnTo);
    bool resetDefaultPatch(Component* returnTo);
    // The default patch's name, "Untitled" when it has none; empty with no default patch.
    String defaultPatchName() const;
    File defaultPatchFile() const;
    // Command Z and Shift Command Z (or Command Y): undoes or redoes the last
    // change, shows the patch as it is then and says what was undone.
    void undoOrRedo(bool redo);
    String undoAnnouncement(const UndoHistory::Outcome& outcome, bool redo) const;
    void rebuildFocusOrder();
    void ensurePresetListLoaded();
    void startPresetListLoad(bool announce, bool forceRefresh = false);
    void applyPresetList(std::vector<PresetBrowserItem> items, bool announce, int generation,
                         bool stillScanning = false);
    const PresetBrowserItem* presetItemForFile(const File& file) const;
    void refreshPresetList(bool announce = true);
    // The Collection facet's rows. Its Community presets row lists the presets
    // shared on the community library, fetched from its JSON API on a
    // background thread and shown like the local ones; Return on one
    // downloads it into the Community library folder, checks it against the
    // library's checksum and loads it. Its Community banks row lists the
    // shared banks the same way; Return on one downloads the archive into
    // Community/Banks, checks it and unpacks it into the Community library.
    PresetCollection presetCollection() const;
    void setPresetCollection(PresetCollection collection);
    String presetCollectionName() const;
    bool inCommunityCollection() const { return presetCollection() == PresetCollection::kCommunity; }
    bool inCommunityBankCollection() const { return presetCollection() == PresetCollection::kCommunityBanks; }
    bool inAnyCommunityCollection() const { return inCommunityCollection() || inCommunityBankCollection(); }
    // The browser's lists and columns as the collection names them: on the
    // community library's side the Bank list and column are its Author (the
    // uploader), Category is Style for presets and Genre for banks, and the
    // Library list and column, which would name only Community, are left out.
    void namePresetBrowserForCollection();
    void namePresetFacetTable(PresetTableListBox& table, const String& title);
    // The Sort presets list beside the search: its choices for the collection
    // (Most downloaded only on the community's side), the name of a sort, and
    // choosing one, which a column header pressed with the mouse also does.
    void updatePresetSortChoices();
    String presetSortName(int column, bool ascending) const;
    void setPresetSort(int column, bool ascending);
    // The facet lists side by side in `preset_facet_area_`, without the hidden ones.
    void layoutPresetFacets();
    // The items the facets and the table are built from: a community list in
    // its collection, the local index otherwise.
    const std::vector<PresetBrowserItem>& activePresetIndex() const;
    bool activePresetListLoaded() const {
      return inCommunityBankCollection() ? community_banks_loaded_
           : inCommunityCollection() ? community_list_loaded_ : preset_list_loaded_;
    }
    void ensureCommunityListLoaded();
    void startCommunityListLoad(bool announce, bool forceRefresh = false);
    void applyCommunityList(std::vector<PresetBrowserItem> items, const String& error, bool announce,
                            bool fetched, int generation);
    void rebuildCommunityLookup();
    void downloadCommunityPreset(const PresetBrowserItem& item, bool loadAfter);
    void finishCommunityDownload(const File& file, const String& name, const String& error, bool loadAfter,
                                 int generation);
    void ensureCommunityBankListLoaded();
    void startCommunityBankListLoad(bool announce, bool forceRefresh = false);
    void applyCommunityBankList(std::vector<PresetBrowserItem> items, const String& error, bool announce,
                                bool fetched, int generation);
    void rebuildCommunityBankLookup();
    // Fetches a bank's archive (unless the copy here is the one the library
    // lists) and unpacks it into the Atlas folder, both in the background.
    void downloadCommunityBank(const PresetBrowserItem& item);
    void finishCommunityBankImport(const File& file, const String& name, const String& error, int generation);
    struct CommunityDownload {
      String name;
      int generation = 0;
      int64 received = 0;
      int64 total = -1;   // the library's size, or -1 while unknown
      std::shared_ptr<std::atomic<bool>> cancelled;
    };
    // A download's life in `community_downloads_`: begun with its name and the
    // size the library lists (the bar and Cancel download show), its progress
    // as its thread reports it, and ended when it lands or fails. `end` says
    // whether the download was still current, since Cancel download drops it
    // first and its thread's news then counts for nothing.
    CommunityDownload& beginCommunityDownload(const std::string& key, const String& name, int64 total);
    void setCommunityDownloadProgress(const std::string& key, int generation, int64 received, int64 total);
    // What a download's thread reports with: the progress above, posted as the
    // percentage changes, and false once Cancel download has set its flag.
    atlas::community::DownloadProgress communityDownloadProgress(const std::string& key,
                                                                 const CommunityDownload& download);
    bool endCommunityDownload(const std::string& key, int generation);
    // Cancel download: every download under way stops, and is said to.
    void cancelCommunityDownloads();
    // The bar and Cancel download follow the downloads: shown while any is on,
    // with the percentage of all of them together, said now and then while
    // one of the two has the focus.
    void updateDownloadProgress();
    // Adds a file that just appeared (a download) to the local index and the
    // process-wide cache ahead of the next scan.
    void addPresetToLocalIndex(const File& file);
    // A community row's audio preview, played through the plugin's output
    // beside the synth (SynthPlugin::setAuditionSource). P on the row, or its
    // context menu, fetches the preview into Community/Previews the first
    // time, decodes it with a JUCE AudioFormatReader read ahead on
    // preview_thread_ and plays it from preview_transport_; the same key,
    // another row's preview or closing the editor stops it, and the end of
    // the recording is said.
    void toggleCommunityPreview();
    void startCommunityPreview(const PresetBrowserItem& item);
    void playCommunityPreview(const File& file, const String& name, int generation);
    void stopCommunityPreview(bool announce);
    // Lets go of the preview: stops it, takes it out of the plugin's output
    // and drops a fetch under way.
    void detachCommunityPreview();
    // Whether `item`'s preview is the one playing or being fetched.
    bool isCommunityPreviewOf(const PresetBrowserItem& item) const;
    // From the timer: lets go of a preview that has reached its end and says so.
    void finishCommunityPreviewIfEnded();
    // Runs `task` on a thread that is joined when the editor closes, as the
    // preset scans are; `task` stops when the flag it is given is set.
    void runPresetBackgroundTask(std::function<void(const std::shared_ptr<std::atomic<bool>>&)> task);
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
    // Moves `file` to Trash and selects the
    // preset after it in the list, or the one before when it was the last.
    void movePresetToTrash(const File& file);
    // The browser drops `file`, or shows `to` in place of `from`, at once,
    // ahead of the rescan that a trash, rename or metadata edit starts, so
    // the list never offers a preset file that is gone. When `to` no longer
    // passes the filters (moved to another bank), the preset after `from` is
    // selected, as after a trash.
    void forgetPreset(const File& file);
    void replacePresetInBrowser(const File& from, const File& to);
    void rebuildPresetLookup();
    // The preset after `file` in the shown list, or the one before when it is
    // the last; empty when it is the only one.
    String presetPathAfter(const File& file) const;
    // What populatePresetFilters gave up, given the filters before it ran: a
    // library, bank, category or tag with no presets left goes back to All,
    // and this says so ("No presets left in X. Showing all banks"); empty if none.
    String presetFilterFallbackNotice(const String& library, const String& bank, const String& category,
                                      const String& tag) const;
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
    // Export bank: a menu of every bank in every library, then Other folder,
    // then where to write the archive.
    void chooseBankToExport();
    void chooseFolderToExport();
    void chooseBankExportDestination(const File& sourceFolder);
    void exportFolderAsBank(const File& sourceFolder, const File& destinationFile);
    // Loads a tuning chosen in Settings and says so.
    void loadTuningFile(const File& file);
    void clearTuning();
    void exportPreset();
    void savePresetToUserDirectory();
    void savePresetAs();
    void savePatch();
    void showSavePresetDialog();
    void hideSavePresetDialog();
    void commitSavePresetDialog();
    // The Save form's Bank list: the User banks, then New bank, which shows
    // the name box; `select` is chosen, or typed as the new bank's name when
    // it is no bank yet.
    void populateSaveBanks(const String& select);
    bool newSaveBankChosen() const;
    // The bank the form names: the chosen one, or the new name made legal;
    // empty when New bank is chosen and no name is typed.
    String chosenSaveBank() const;
    // Share patch / Share bank on the community library (the Preset menu):
    // the Share form, or first the login dialog while this DAW is not logged
    // in, which opens the form once a token is accepted.
    void shareOnCommunity(bool bank);
    // The login dialog (CommunityLoginContent): `message` says why it is
    // here, with a box for the API token, a button that opens the site's API
    // tokens page, Log in and Cancel. Log in checks the token with the
    // library and keeps it (checkCommunityToken); `on_logged_in` then runs,
    // once the dialog has closed and the focus is back.
    void showCommunityLogin(const String& message, std::function<void()> on_logged_in);
    // Asks the library whose `token` is, in the background, and keeps the
    // token with its account (community_library.h, storeApiToken) when the
    // library accepts it; `done` then gets the account, or what went wrong,
    // on the message thread. Settings' Log in and the login dialog share it.
    void checkCommunityToken(const String& token,
                             std::function<void(const atlas::community::Account&, const String& error)> done);
    // The Share form: an inline form like Save patch's. For a patch, its name,
    // style, tags and description; for a bank, the library folder to share,
    // its name, genre, tags and description. Share writes the patch or
    // exports the folder to a temporary file and posts it on a background
    // thread with the saved API token, with the upload's progress in a bar the
    // focus moves to (ShareProgressBar); the library's answer is said, and the
    // form stays open on its field errors. A token the store will not give,
    // or one the library refuses, brings up the login dialog, and Share goes
    // on once a new one is accepted. Cancel or Escape while it uploads stops
    // the upload. `preface` opens what the form says ("Logged in as ...").
    void showShareForm(bool bank, const String& preface = {});
    void hideShareForm();
    void commitShareForm();
    void populateShareFolders(const File& select);
    void chooseShareFolder();
    void loadShareGenres();
    // The upload is under way: the bar shows, focused, and says what is being shared.
    void startShareUpload(const String& message);
    // What the upload's thread reports: -1 before the library has taken any of
    // it; said now and then while the bar has the focus.
    void setShareProgress(int percent, int generation);
    void stopShareUpload();
    // The form's Cancel is Cancel upload while the upload is on, since it
    // stops the upload then, and Cancel sharing otherwise.
    void nameShareCancel(bool uploading);
    void finishShare(const atlas::community::UploadResult& result, const String& name, bool bank, int generation);
    bool shareFormHasFocus() const;
    void savePresetFile(const File& file);
    void showPresetControls();
    void updatePresetSummary();
    void refreshWavetableNameLabel(int oscillator);
    void refreshSampleNameLabel(int sampleTarget);
    void refreshOscillatorHeaderTitles();
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
    void loadInstrumentFile(int oscillator, const File& file);
    void showInstrumentBrowserMenu(int oscillator, Component& target);
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
    // Speech oscillators: the Speech controls group, with the text box for
    // the words to sing above the language and variant menus.
    void addSpeechControlsGroup(int oscillator, Component& parent, int& y, int x, int width,
                                int& focus_order, const String& section_name,
                                const GroupRowAppender& appendRows = {});
    // Hands the text box's words to the slot when they changed.
    void commitSpeechText(int oscillator, const String& text);
    // Announces a finished render the text box asked for and keeps the text
    // boxes on the slots' text.
    void refreshSpeechStatus();
    String speechStatusDescription(int oscillator);
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
    void showLfoPresetBrowserMenu(int lfoIndex, Component& target);
    // A Browse menu of `files`: library, then packs and folders, then file,
    // and Rescan, which scans `list` again and opens the menu again.
    void showPresetFileMenu(PresetFileList list, const std::vector<File>& files, const String& emptyText,
                            Component& target, std::function<void(const File&)> choose,
                            std::function<void()> reopen);
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
    // The FX presets group of an effect slot in a chain: Browse, Previous,
    // Next, Import and Save, acting on that slot's effect.
    void showEffectPresetBrowserMenu(const String& sectionName, int slot, Component& target);
    void chooseEffectPresetFileForSlot(const String& sectionName, int slot);
    void saveEffectPresetFromSlot(const String& sectionName, int slot);
    void chooseFxBusPresetFile(const String& sectionName, bool save);
    void loadFxBusPresetFile(const String& sectionName, const File& file,
                             const String& focusTitle = "Load bus FX preset");
    // A group a screen reader enters, holding a column of buttons, placed in
    // `parent` at `y`, which moves past it. Each button's action gets the
    // button, for a menu to open from.
    struct GroupButton {
      String title;
      String description;
      std::function<void(Component&)> action;
    };
    AccessibleSectionHeader* addButtonGroup(const String& title, const String& key, AccessibleSectionHeader& parent,
                                            int x, int& y, int width, int& focusOrder, int rank,
                                            const std::vector<GroupButton>& buttons);
    void showFxBusPresetBrowserMenu(const String& sectionName, Component& target);
    void addFxBusControlsGroup(const String& sectionName, Component& parent, int& y, int x,
                               int width, int& focusOrder);
    int effectSlotType(const String& sectionName, int slot) const;
    int effectSlotTypeInChain(int chain, int slot) const;
    int firstEmptyEffectSlot(const String& sectionName) const;
    std::vector<int> activeEffectSlots(const String& sectionName) const;
    String effectSlotPrefix(const String& sectionName, int slot) const;
    String effectSlotTitle(const String& sectionName, int slot) const;
    void swapEffectSlots(const String& sectionName, int firstSlot, int secondSlot);
    // Moves the impulse response, Filter FX table and MIDI learn mappings
    // keyed by the slot prefixes along with a swapped pair of slots.
    void swapEffectSlotModuleState(const String& firstPrefix, const String& secondPrefix);
    void clearEffectSlot(const String& sectionName, int slot);
    // Removes a slot. The focus then goes to the slot before it (or after it,
    // for the first) on Windows, and to focusedTitle on the Mac or once the
    // chain is empty.
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
    // Focuses that control again, after rebuilding the sections if `rebuild`,
    // and moves the screen reader back onto it once the prompt is hidden.
    // Call before hiding the prompt: hiding the focused text field first hands
    // focus to the top of the window. Falls back to fallbackParameterId's row.
    void returnFocusFromPrompt(bool rebuild, const String& fallbackParameterId);
    // Hides the prompt once the screen reader has followed focus off it.
    void hideInlineTextPrompt();
    void hideInlineTextPromptNow();
    ValueBridge* parameterBridge(const String& id) const;
    void setParameterEngineValue(const String& id, float engineValue);
    void randomizeSeed(const String& id);
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
    // The part of the state an LFO, wavetable or filter table edit changes,
    // for SynthPlugin::markStateChanged (UndoHistory::noteStructureEdit).
    std::string lfoUndoBlock(const LineGenerator* generator) const;
    std::string wavetableUndoBlock(int oscillator) const;
    std::string wavetableUndoBlock(const WavetableCreator* creator) const;
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
    // The amount that takes the destination from where it is now to
    // `targetValue`, a value of its own (0 to 1, as its host parameter).
    float modulationAmountForTarget(const ValueBridge& destination, double targetValue) const;
    // Shows the amount prompt's slider for `destination`, at `peakValue`, when
    // the setting is on, and hides it otherwise.
    void setUpModulationAmountSlider(ValueBridge* destination, double peakValue);
    // The slider moved: the route plays its new amount at once, so it can be
    // heard while the prompt is open. OK keeps it; anything else puts back
    // the amount the route had when the prompt opened (revertModulationAudition).
    void auditionModulationAmount();
    void revertModulationAudition();
    void setUpModulationBipolarToggle();
    void toggleModulationBipolar();
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
    // This instance's section and preset browser state (SynthPlugin::EditorMemory):
    // reopening the window returns to them, other Atlas instances keep their own.
    String& last_section_name_;
    String& last_preset_path_;
    String& last_preset_search_;
    String& last_preset_library_;
    String& last_preset_bank_;
    String& last_preset_category_;
    String& last_preset_tag_;
    String& last_preset_collection_;
    // Whether the Presets group shows the preset browser (search, filters,
    // results, details and autoload) or only the summary and Preset menu.
    bool& preset_browser_shown_;
    void addCpuMeter(Component& parent, int x, int& y, int width, int& focusOrder);
    std::unique_ptr<CpuUsageMeter> cpu_meter_;
    Label title_;
    TextButton settings_button_ { "Settings" };
    // Named after what they would do (updateToolbarUndoButtons).
    AccessibleTextButton undo_button_ { "Undo" };
    AccessibleTextButton redo_button_ { "Redo" };
    SpokenToggleButton preset_browser_toggle_ { "Show preset browser" };
    // The Toolbar group, before every other group: Settings, Undo, Redo and
    // Show preset browser. Built once and kept outside the scrolling rows, so
    // its controls keep the focus through every rebuild.
    std::unique_ptr<AccessibleSectionHeader> toolbar_;
    // The group that number keys 1 to 9 choose an element from.
    String current_group_;
    Label preset_summary_;
    Label preset_details_;
    TextButton preset_menu_ { "Preset menu" };
    TextEditor preset_search_;
    AccessibleComboBox preset_sort_;
    PresetTableListBox preset_collection_table_ { "Collection" };
    PresetTableListBox preset_library_table_ { "Library" };
    PresetTableListBox preset_bank_table_ { "Bank" };
    PresetTableListBox preset_category_table_ { "Category" };
    PresetTableListBox preset_tag_table_ { "Tags" };
    Rectangle<int> preset_facet_area_;
    PresetTableListBox preset_table_ { "Preset table" };
    ToggleButton preset_preview_ { "Autoload preset when scrolling" };
    // A community download's progress and its Cancel, after the Autoload
    // toggle while a download is on (updateDownloadProgress).
    std::unique_ptr<TransferProgressBar> download_progress_;
    AccessibleTextButton download_cancel_ { "Cancel download" };
    Label save_patch_prompt_;
    TextEditor save_patch_name_;
    TextEditor save_patch_author_;
    // The User banks to save into, and a name box for a new one.
    AccessibleComboBox save_patch_bank_;
    TextEditor save_patch_new_bank_;
    StringArray save_patch_banks_;
    TextEditor save_patch_category_;
    TextEditor save_patch_tags_;
    TextEditor save_patch_comments_;
    TextButton save_patch_ok_ { "Save" };
    TextButton save_patch_cancel_ { "Cancel" };
    Label share_prompt_;
    AccessibleComboBox share_folder_;
    AccessibleTextEditor share_name_;
    AccessibleComboBox share_style_;
    AccessibleComboBox share_genre_;
    AccessibleTextEditor share_tags_;
    AccessibleTextEditor share_description_;
    AccessibleTextButton share_ok_ { "Share" };
    AccessibleTextButton share_cancel_ { "Cancel" };
    std::unique_ptr<TransferProgressBar> share_progress_;
    Label routing_summary_;
    AccessibleComboBox routing_mode_;
    TextButton routing_default_ { "Default routing" };
    TextButton routing_serial_forward_ { "Serial filters 1 into 2" };
    TextButton routing_serial_backward_ { "Serial filters 2 into 1" };
    Label modulation_amount_prompt_;
    TextEditor modulation_amount_editor_;
    ModulationAmountSlider modulation_amount_slider_;
    // What the slider last wrote into the text box: while the box still says
    // it, OK sets the slider's own value, which the text may round.
    String modulation_slider_text_;
    // The route's amount when the prompt opened, and whether the slider has
    // changed it since (auditionModulationAmount).
    float modulation_amount_at_open_ = 0.0f;
    bool modulation_amount_auditioned_ = false;
    // Bipolar, after the slider: switches the route's polarity at once, and
    // Cancel puts back the one it had when the prompt opened. A macro's routes
    // follow the macro's own Bipolar, so its prompt has none.
    SpokenToggleButton modulation_bipolar_toggle_ { "Bipolar" };
    bool modulation_bipolar_at_open_ = false;
    bool modulation_bipolar_changed_ = false;
    TextButton modulation_amount_ok_ { "OK" };
    TextButton modulation_amount_cancel_ { "Cancel" };
    Viewport viewport_;
    Component rows_container_;
    std::vector<std::unique_ptr<Component>> section_headers_;
    std::vector<std::unique_ptr<AccessibleParameterRow>> rows_;
    // What keepFocusedSwitchingRow holds while a rebuild runs: the row, its
    // parameter (compared, never read, as a rebuild may replace its bridge),
    // its control, and its groups, outermost first, of which the rebuild has
    // taken back the first kept_switching_groups_taken_.
    std::unique_ptr<AccessibleParameterRow> kept_switching_row_;
    String kept_switching_parameter_id_;
    const AudioProcessorParameter* kept_switching_parameter_ = nullptr;
    Component* kept_switching_control_ = nullptr;
    std::vector<std::unique_ptr<AccessibleSectionHeader>> kept_switching_groups_;
    size_t kept_switching_groups_taken_ = 0;
    // An effect's mode changed: the rebuild it queued keeps the focused row.
    bool keep_switching_row_on_rebuild_ = false;
    std::vector<std::unique_ptr<ValueBridge>> internal_fx_bridges_;
    bool fine_tune_sample_loop_ = false;
    std::map<String, std::vector<AudioProcessorParameter*>> sections_;
    std::map<String, StringArray> group_sections_;
    std::map<String, ValueBridge*> parameters_by_id_;
    // Each effect slot's type control by chain (0 is the main chain) and slot,
    // looked up once: the timer reads every slot's type several times a tick.
    // Cleared with parameters_by_id_, whose bridges it may point to.
    struct EffectSlotTypeControl {
      ValueBridge* bridge = nullptr;
      vital::Value* control = nullptr;
    };
    mutable std::array<std::array<EffectSlotTypeControl, vital::constants::kMaxEffectSlots>, vital::kNumBuses + 1>
        effect_slot_type_controls_ {};
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
    struct SpeechAnnouncementState {
      uint32_t revision = 0;
      // The text box committed new words: say how the render went.
      bool announce = false;
    };
    std::array<SpeechAnnouncementState, vital::kNumOscillators> speech_announcements_ {};
    std::unique_ptr<FileChooser> lfo_chooser_;
    std::unique_ptr<FileChooser> fx_chooser_;
    std::unique_ptr<FileChooser> fx_bus_chooser_;
    std::unique_ptr<FileChooser> ir_chooser_;
    std::unique_ptr<FileChooser> bank_chooser_;
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
    std::map<String, bool> last_seed_modes_;
    std::map<int, int> last_lfo_layouts_;
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
    // The community library's presets, as PresetBrowserItems whose files may
    // not exist yet, and what is known about fetching them.
    std::vector<PresetBrowserItem> community_index_;
    std::unordered_map<std::string, size_t> community_index_by_path_;
    bool community_list_loaded_ = false;
    bool community_list_loading_ = false;
    String community_list_error_;
    int community_list_generation_ = 0;
    // The downloads under way, by the local path each lands at: what the
    // progress bar sums up, and what Cancel download stops. A download's
    // thread reports under its generation, so one cancelled and started again
    // is not confused with its earlier self.
    std::map<std::string, CommunityDownload> community_downloads_;
    int community_download_generation_ = 0;
    uint32 download_progress_said_ms_ = 0;
    int download_progress_said_ = -1;
    // The same for the library's banks.
    std::vector<PresetBrowserItem> community_bank_index_;
    std::unordered_map<std::string, size_t> community_bank_index_by_path_;
    bool community_banks_loaded_ = false;
    bool community_banks_loading_ = false;
    String community_banks_error_;
    int community_banks_generation_ = 0;
    // The community preview playing or being fetched (toggleCommunityPreview).
    AudioFormatManager preview_formats_;
    bool preview_formats_registered_ = false;
    TimeSliceThread preview_thread_ { "Atlas preview" };
    std::unique_ptr<AudioFormatReaderSource> preview_reader_;
    AudioTransportSource preview_transport_;
    File preview_file_;
    String preview_name_;
    bool preview_fetching_ = false;
    int preview_generation_ = 0;
    // The results' sort: a table column, or kPresetDownloadsSort, which no
    // column shows. Remembered for the instance like the filters.
    int& preset_sort_column_;
    bool& preset_sort_ascending_;
    // The Sort presets list's choices, its item ID less one indexing them.
    std::vector<std::pair<int, bool>> preset_sort_choices_;
    bool preset_metadata_edit_mode_ = false;
    File preset_metadata_file_;
    int preset_list_generation_ = 0;
    // A preset saved while the list was being rescanned, to select once the
    // rescan lands (selectPresetFile); cleared when the user picks another.
    String select_after_refresh_;
    bool save_patch_dialog_visible_ = false;
    // The Share form (showShareForm): whether it shows and for a bank, the
    // folders its menu offers, the genres the library lists, whether a share
    // is under way (with the flag that stops its upload, and when and at what
    // percentage its progress was last said), and where focus goes back to
    // when it closes.
    bool share_form_visible_ = false;
    bool share_bank_ = false;
    bool share_busy_ = false;
    int share_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> share_cancelled_;
    uint32 share_progress_said_ms_ = 0;
    int share_progress_said_ = -1;
    Array<File> share_folders_;
    std::vector<atlas::community::Genre> share_genres_;
    bool share_genres_loading_ = false;
    std::unique_ptr<FileChooser> share_folder_chooser_;
    FocusReturn share_return_focus_;
    // The Settings window while it is open; closed with the editor.
    Component::SafePointer<DialogWindow> settings_window_;
    // The login dialog (showCommunityLogin) while it is open.
    Component::SafePointer<DialogWindow> login_window_;
    // Where focus was when the save or metadata form opened; it goes back
    // there, or to its replacement after a rebuild, when the form closes.
    FocusReturn save_patch_return_focus_;
    bool updating_preset_list_ = false;
    bool updating_routing_controls_ = false;
    bool wavetable_browser_cache_valid_ = false;
    bool wavetable_editor_visible_[vital::kNumOscillators] = {};
    bool preset_to_wavetable_rendering_ = false;
    bool sample_browser_cache_valid_ = false;
    bool ir_browser_cache_valid_ = false;
    struct PresetFileCache {
      bool valid = false;
      std::vector<File> roots;
      std::vector<File> files;
      std::vector<String> effects;   // FX presets: each file's effect id, empty when it names none
    };
    std::array<PresetFileCache, 4> preset_file_caches_;
    // The FX preset last loaded into each effect slot, by its slot prefix.
    std::map<String, File> last_effect_preset_files_;
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
    // Bumped by each restoreFocusAfterRebuild so only the last one's delayed grab runs.
    int rebuild_focus_grab_generation_ = 0;
    // Set when a queued rebuild should re-focus the screen reader once it runs.
    bool screen_reader_refocus_requested_ = false;
    // The control the window had when it lost focus, until focus comes back.
    FocusReturn window_focus_return_;
    // File choosers, message boxes and dialog windows (openOsWindow) that have
    // not closed yet.
    int open_os_windows_ = 0;
    // While a closed window's callback runs, the control that window gives
    // focus back to (closeOsWindow).
    const FocusReturn* os_window_callback_focus_ = nullptr;
    // The inline prompt is asking a yes or no question (promptForConfirmation).
    bool confirmation_prompt_visible_ = false;
    // The inline prompt's action is running (applyParameterValue): a box it
    // opens returns to the control the prompt returns to (focusForMessageBox).
    bool inline_prompt_applying_ = false;
    // Bumped by each hideInlineTextPrompt so only the last one hides the prompt.
    int inline_prompt_hide_generation_ = 0;
    String queued_effect_chain_rebuild_section_;
    String queued_effect_chain_rebuild_title_;
    String queued_effect_chain_rebuild_parameter_;
    // A request that arrived while a rebuild was already queued (or running) and
    // may still need a rebuild of its own once the current one has finished.
    bool queued_effect_chain_rebuild_pending_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthEditor)

    friend class PresetFacetTableModel;
    friend class AtlasSettingsContent;
    friend class CommunityLoginContent;
    friend class PresetResultsTableModel;
};

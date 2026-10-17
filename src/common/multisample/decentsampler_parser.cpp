// SPDX-License-Identifier: GPL-3.0-or-later
#include "decentsampler_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace vital {
namespace multisample {

  // Named so that unity builds keep these helpers apart from the SFZ parser's.
  namespace decent_sampler {
  namespace {
    // DecentSampler documents no default release. A short one keeps samples
    // from clicking off at note-off without smearing drums.
    constexpr float kDefaultRelease = 0.1f;

    // Attribute names lowercased, values trimmed.
    using Attributes = std::map<std::string, std::string>;

    std::string lower(std::string text) {
      for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    }

    std::string upper(std::string text) {
      for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return text;
    }

    Attributes attributesOf(const juce::XmlElement& element) {
      Attributes attributes;
      for (int i = 0; i < element.getNumAttributes(); ++i) {
        attributes[lower(element.getAttributeName(i).toStdString())] =
            element.getAttributeValue(i).trim().toStdString();
      }
      return attributes;
    }

    const std::string* find(const Attributes& attributes, const std::string& name) {
      auto found = attributes.find(name);
      return found == attributes.end() ? nullptr : &found->second;
    }

    double toNumber(const std::string& text) {
      return juce::String(text).trim().getDoubleValue();
    }

    int toInt(const std::string& text) {
      return static_cast<int>(std::lround(toNumber(text)));
    }

    bool toBool(const std::string& text) {
      const std::string value = lower(text);
      return value == "true" || value == "1" || value == "yes" || value == "on";
    }

    std::string numberText(double value) {
      return juce::String(value, 9).toStdString();
    }

    // "3dB" is decibels, anything else a linear gain.
    float linearGain(const std::string& text) {
      const juce::String value = juce::String(text).trim();
      if (value.endsWithIgnoreCase("db"))
        return std::pow(10.0f, static_cast<float>(value.dropLastCharacters(2).trim().getDoubleValue()) / 20.0f);
      return std::max(0.0f, static_cast<float>(value.getDoubleValue()));
    }

    // Tags compare without case.
    std::vector<std::string> tagList(const std::string& text) {
      std::vector<std::string> tags;
      for (const juce::String& part : juce::StringArray::fromTokens(juce::String(text), ",", "")) {
        const std::string tag = lower(part.trim().toStdString());
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end())
          tags.push_back(tag);
      }
      return tags;
    }

    // "loCC64" with prefix "locc" gives 64.
    bool matchNumbered(const std::string& name, const char* prefix, int& number) {
      const size_t length = std::char_traits<char>::length(prefix);
      if (name.size() <= length || name.compare(0, length, prefix) != 0)
        return false;
      for (size_t i = length; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
          return false;
      }
      number = std::atoi(name.c_str() + length);
      return true;
    }

    // Attributes read from <groups>, <group> and <sample>; anything else is reported.
    bool isKnownAttribute(const std::string& name) {
      static const std::set<std::string> kKnown = {
        "path", "rootnote", "lonote", "hinote", "lovel", "hivel", "start", "end", "tuning", "globaltuning",
        "grouptuning", "volume", "globalvolume", "groupvolume", "pan", "globalpan", "grouppan", "pitchkeytrack",
        "tuningignoreskeytrack", "trigger", "releasetriggerdecay", "tags", "silencedbytags", "silencingmode",
        "silencingdecay", "seqmode", "seqlength", "seqposition", "loopstart", "loopend", "loopenabled",
        "loopduringrelease", "loopmode", "loopcrossfade", "loopcrossfademode", "ampenvenabled", "attack", "decay", "sustain", "release", "ampveltrack", "delay",
        "delayunit", "enabled", "name",
        // Informational, or about how DecentSampler reads files rather than how they sound.
        "length", "playbackmode",
      };
      if (kKnown.count(name))
        return true;
      int number = 0;
      return matchNumbered(name, "locc", number) || matchNumbered(name, "hicc", number) ||
             matchNumbered(name, "onlocc", number) || matchNumbered(name, "onhicc", number);
    }

    struct Group {
      Attributes attributes;
      // Values set by the controls' bindings at load; they win over the file's.
      Attributes overrides;
      std::vector<Attributes> samples;
      std::vector<std::string> tags;
      int oscillators = 0;
    };

    class Parser {
      public:
        Parser(const juce::File& main_file, const SfzParser::Options& options) :
            main_file_(main_file), options_(options) {
          if (options_.search_roots.empty())
            roots_.push_back(main_file.getParentDirectory());
          else
            roots_ = options_.search_roots;
          result_.instrument = std::make_shared<Instrument>();
          result_.instrument->format = "dspreset";
          result_.instrument->name = main_file.getFileNameWithoutExtension().toStdString();
          result_.instrument->source_path = main_file.getFullPathName().toStdString();
          for (float& value : result_.instrument->cc_defaults)
            value = 0.0f;
        }

        SfzParser::Result run(const std::string* text) {
          std::string contents;
          if (text != nullptr)
            contents = *text;
          else {
            if (!main_file_.existsAsFile()) {
              result_.error = "Cannot open " + main_file_.getFullPathName().toStdString();
              return std::move(result_);
            }
            contents = main_file_.loadFileAsString().toStdString();
          }
          result_.text_files.push_back(main_file_);

          juce::XmlDocument document(juce::String::fromUTF8(contents.c_str()));
          std::unique_ptr<juce::XmlElement> root = document.getDocumentElement();
          if (root == nullptr) {
            result_.error = "The DecentSampler preset is not valid XML: " + document.getLastParseError().toStdString();
            return std::move(result_);
          }
          if (!root->hasTagName("DecentSampler")) {
            result_.error = "Not a DecentSampler preset: the file starts with <" + root->getTagName().toStdString() + ">.";
            return std::move(result_);
          }
          const juce::XmlElement* groups = root->getChildByName("groups");
          if (groups == nullptr) {
            result_.error = "The DecentSampler preset has no <groups> element.";
            return std::move(result_);
          }

          readGroups(*groups);
          readTags(root->getChildByName("tags"));
          applyControls(root->getChildByName("ui"));
          for (const auto* child : root->getChildIterator()) {
            const std::string name = lower(child->getTagName().toStdString());
            if (name != "groups" && name != "tags" && name != "ui")
              ignored_elements_.insert(name);
          }
          buildRegions();

          Instrument& instrument = *result_.instrument;
          for (const auto& unsupported : unsupported_) {
            instrument.warnings.push_back("Ignored attribute " + unsupported.first + " (" +
                                          std::to_string(unsupported.second) + " uses)");
          }
          for (const auto& binding : ignored_bindings_) {
            instrument.warnings.push_back("Ignored control binding " + binding.first + " (" +
                                          std::to_string(binding.second) + " uses)");
          }
          for (const auto& element : ignored_elements_)
            instrument.warnings.push_back("Ignored <" + element + "> element");
          for (const auto& note : notes_)
            instrument.warnings.push_back(note);
          if (result_.error.empty() && instrument.regions.empty())
            result_.error = "The DecentSampler preset has no samples.";
          return std::move(result_);
        }

      private:
        void readGroups(const juce::XmlElement& groups) {
          groups_ = attributesOf(groups);
          tally(groups_);
          for (const auto* child : groups.getChildIterator()) {
            if (!child->hasTagName("group")) {
              ignored_elements_.insert(lower(child->getTagName().toStdString()));
              continue;
            }
            Group group;
            group.attributes = attributesOf(*child);
            tally(group.attributes);
            if (const std::string* tags = find(group.attributes, "tags"))
              group.tags = tagList(*tags);
            for (const auto* item : child->getChildIterator()) {
              if (item->hasTagName("sample")) {
                group.samples.push_back(attributesOf(*item));
                tally(group.samples.back());
              }
              else if (item->hasTagName("oscillator"))
                group.oscillators++;
              else
                ignored_elements_.insert(lower(item->getTagName().toStdString()));
            }
            groups_list_.push_back(std::move(group));
          }
        }

        void readTags(const juce::XmlElement* tags) {
          if (tags == nullptr)
            return;
          for (const auto* tag : tags->getChildWithTagNameIterator("tag")) {
            const Attributes attributes = attributesOf(*tag);
            const std::string* name = find(attributes, "name");
            if (name == nullptr)
              continue;
            const std::string key = lower(*name);
            if (const std::string* volume = find(attributes, "volume"))
              tag_volumes_[key] = linearGain(*volume);
            if (const std::string* enabled = find(attributes, "enabled"))
              tag_enabled_[key] = toBool(*enabled);
            if (const std::string* polyphony = find(attributes, "polyphony")) {
              if (toInt(*polyphony) > 0)
                ignored_bindings_["tag polyphony"]++;
            }
          }
        }

        // DecentSampler fires each control's bindings when it loads a preset,
        // and again whenever the control moves, so the value a control has
        // (the file's, or one the user chose) replaces what the groups set.
        // Controls that change the sound here are listed on the instrument.
        void applyControls(const juce::XmlElement* ui) {
          if (ui == nullptr)
            return;
          int index = 0;
          const size_t first_listed = result_.instrument->controls.size();
          for (const auto* tab : ui->getChildWithTagNameIterator("tab")) {
            std::vector<const juce::XmlElement*> labels;
            for (const auto* label : tab->getChildWithTagNameIterator("label"))
              labels.push_back(label);
            const juce::XmlElement* previous = nullptr;
            for (const auto* element : tab->getChildIterator()) {
              const juce::XmlElement* before = previous;
              previous = element;
              const std::string kind = lower(element->getTagName().toStdString());
              const bool knob = kind == "labeled-knob" || kind == "control" || kind == "slider";
              if (!knob && kind != "menu" && kind != "button")
                continue;

              const Attributes attributes = attributesOf(*element);
              Control control;
              control.index = index++;
              control.label = controlLabel(*element, attributes, before, labels);
              auto chosen = options_.control_values.find(control.index);
              const bool user_set = chosen != options_.control_values.end();
              bool audible = false;

              if (knob) {
                control.kind = Control::Kind::kKnob;
                control.minimum = find(attributes, "minvalue") ? toNumber(*find(attributes, "minvalue")) : 0.0;
                control.maximum = find(attributes, "maxvalue") ? toNumber(*find(attributes, "maxvalue")) : 1.0;
                if (control.maximum < control.minimum)
                  std::swap(control.minimum, control.maximum);
                control.integer = find(attributes, "type") && lower(*find(attributes, "type")) == "integer";
                control.default_value = knobValue(control, find(attributes, "value")
                                                               ? toNumber(*find(attributes, "value")) : control.minimum);
                control.value = user_set ? knobValue(control, chosen->second) : control.default_value;
                for (const auto* binding : element->getChildWithTagNameIterator("binding")) {
                  const Attributes binding_attributes = attributesOf(*binding);
                  audible = audible || applies(binding_attributes);
                  fire(binding_attributes, control.value, control.minimum, control.maximum, user_set);
                }
              }
              else {
                // Menu values count options from 1 (0 selects none); button values count states from 0.
                const bool menu = kind == "menu";
                control.kind = menu ? Control::Kind::kMenu : Control::Kind::kButton;
                const char* item_tag = menu ? "option" : "state";
                std::vector<const juce::XmlElement*> items;
                for (const auto* item : element->getChildWithTagNameIterator(item_tag)) {
                  items.push_back(item);
                  const Attributes item_attributes = attributesOf(*item);
                  const std::string* name = find(item_attributes, "name");
                  control.choices.push_back(name && !name->empty() ? *name
                                                                   : (menu ? "Option " : "State ") +
                                                                     std::to_string(control.choices.size() + 1));
                  for (const auto* binding : item->getChildWithTagNameIterator("binding"))
                    audible = audible || applies(attributesOf(*binding));
                }
                const int first = menu ? 1 : 0;
                const int last = first + std::max(0, static_cast<int>(items.size()) - 1);
                auto clampChoice = [&](double value) {
                  return static_cast<double>(std::max(first, std::min(last, static_cast<int>(std::lround(value)))));
                };
                const double file_value = find(attributes, "value") ? toNumber(*find(attributes, "value")) : first;
                // A menu at 0 selects nothing, so its default plays no option.
                const bool none = menu && std::lround(file_value) == 0 && !user_set;
                control.default_value = clampChoice(file_value);
                control.value = user_set ? clampChoice(chosen->second) : control.default_value;
                control.minimum = first;
                control.maximum = last;
                control.integer = true;
                const int selected = static_cast<int>(control.value) - first;
                if (!none && selected >= 0 && selected < static_cast<int>(items.size())) {
                  for (const auto* binding : items[static_cast<size_t>(selected)]->getChildWithTagNameIterator("binding"))
                    fire(attributesOf(*binding), control.value, control.value, control.value, user_set);
                }
              }

              const bool visible = !find(attributes, "visible") || toBool(*find(attributes, "visible"));
              if (audible && visible && (knob || control.choices.size() > 1))
                result_.instrument->controls.push_back(std::move(control));
            }
          }

          // Unnamed controls are numbered by kind among those listed: "Menu 1".
          std::map<Control::Kind, int> counts;
          for (size_t i = first_listed; i < result_.instrument->controls.size(); ++i) {
            Control& control = result_.instrument->controls[i];
            const int number = ++counts[control.kind];
            if (control.label.empty()) {
              const char* kind = control.kind == Control::Kind::kKnob ? "Knob "
                                 : control.kind == Control::Kind::kMenu ? "Menu " : "Button ";
              control.label = kind + std::to_string(number);
            }
          }
        }

        static double knobValue(const Control& control, double value) {
          value = std::max(control.minimum, std::min(control.maximum, value));
          return control.integer ? std::round(value) : value;
        }

        // The name a control shows: its label, else its host parameter name,
        // else the <label> the panel shows on it. Panels place a label's text
        // (centred in its box) at the control's top edge, inside its width,
        // in whatever order the file lists them; without coordinates, a
        // label just before the control names it.
        static std::string controlLabel(const juce::XmlElement& element, const Attributes& attributes,
                                        const juce::XmlElement* before,
                                        const std::vector<const juce::XmlElement*>& labels) {
          for (const char* name : { "label", "parametername", "text" }) {
            const std::string* value = find(attributes, name);
            if (value != nullptr && !value->empty())
              return *value;
          }
          auto text = [](const juce::XmlElement& label) {
            return label.getStringAttribute("text").trim().toStdString();
          };
          auto placed = [](const juce::XmlElement& item) {
            return item.hasAttribute("x") && item.hasAttribute("y") && item.hasAttribute("width") &&
                   item.hasAttribute("height");
          };
          if (placed(element)) {
            const double x = element.getDoubleAttribute("x");
            const double y = element.getDoubleAttribute("y");
            const double width = element.getDoubleAttribute("width");
            const double height = std::max(1.0, element.getDoubleAttribute("height"));
            const juce::XmlElement* best = nullptr;
            double best_distance = std::numeric_limits<double>::max();
            for (const juce::XmlElement* label : labels) {
              if (!placed(*label) || text(*label).empty())
                continue;
              const double centre_x = label->getDoubleAttribute("x") + 0.5 * label->getDoubleAttribute("width");
              const double centre_y = label->getDoubleAttribute("y") + 0.5 * label->getDoubleAttribute("height");
              // Above the top edge by up to its height (at least 40), or a little into it.
              const double above = y - centre_y;
              if (centre_x < x || centre_x > x + width || above > std::max(40.0, height) || above < -0.25 * height)
                continue;
              const double distance = std::abs(above) + std::abs(centre_x - (x + 0.5 * width));
              if (distance < best_distance) {
                best_distance = distance;
                best = label;
              }
            }
            return best != nullptr ? text(*best) : std::string();
          }
          if (before != nullptr && before->hasTagName("label"))
            return text(*before);
          return {};
        }

        // Whether a binding changes something Atlas plays.
        static bool applies(const Attributes& binding) {
          if (find(binding, "enabled") && !toBool(*find(binding, "enabled")))
            return false;
          const std::string level = find(binding, "level") ? lower(*find(binding, "level")) : "";
          const std::string parameter = find(binding, "parameter") ? upper(*find(binding, "parameter")) : "";
          if (level == "tag")
            return find(binding, "identifier") != nullptr && !tagAttribute(parameter).empty();
          if (level != "instrument" && level != "group")
            return false;
          return !bindingAttribute(parameter, level == "instrument").empty();
        }

        static std::string translate(const Attributes& binding, double input, double minimum, double maximum) {
          const std::string mode = find(binding, "translation") ? lower(*find(binding, "translation")) : "linear";
          if (mode == "fixed_value") {
            const std::string* value = find(binding, "translationvalue");
            return value ? *value : numberText(input);
          }
          const double factor = find(binding, "factor") ? toNumber(*find(binding, "factor")) : 1.0;
          double value = input * factor;
          if (mode == "table") {
            std::vector<std::pair<double, double>> table;
            const std::string* text = find(binding, "translationtable");
            for (const juce::String& pair : juce::StringArray::fromTokens(juce::String(text ? *text : "0,0;1,1"), ";", "")) {
              const int comma = pair.indexOfChar(',');
              if (comma > 0)
                table.push_back({ pair.substring(0, comma).trim().getDoubleValue(),
                                  pair.substring(comma + 1).trim().getDoubleValue() });
            }
            std::sort(table.begin(), table.end());
            if (table.size() < 2)
              return numberText(value);
            if (value <= table.front().first)
              return numberText(table.front().second);
            if (value >= table.back().first)
              return numberText(table.back().second);
            for (size_t i = 1; i < table.size(); ++i) {
              if (value <= table[i].first) {
                const double span = table[i].first - table[i - 1].first;
                const double t = span > 0.0 ? (value - table[i - 1].first) / span : 0.0;
                return numberText(table[i - 1].second + t * (table[i].second - table[i - 1].second));
              }
            }
            return numberText(table.back().second);
          }
          const std::string* low = find(binding, "translationoutputmin");
          const std::string* high = find(binding, "translationoutputmax");
          if (low == nullptr || high == nullptr)
            return numberText(value);
          const double span = (maximum - minimum) * factor;
          double t = span != 0.0 ? (value - minimum * factor) / span : 0.0;
          t = std::max(0.0, std::min(1.0, t));
          if (find(binding, "translationreversed") && toBool(*find(binding, "translationreversed")))
            t = 1.0 - t;
          return numberText(toNumber(*low) + t * (toNumber(*high) - toNumber(*low)));
        }

        // `moved`: the user set the control, which fires bindings that skip the load.
        void fire(const Attributes& binding, double input, double minimum, double maximum, bool moved) {
          if (find(binding, "enabled") && !toBool(*find(binding, "enabled")))
            return;
          if (!moved && find(binding, "triggeronload") && !toBool(*find(binding, "triggeronload")))
            return;
          const std::string level = find(binding, "level") ? lower(*find(binding, "level")) : "";
          const std::string parameter = find(binding, "parameter") ? upper(*find(binding, "parameter")) : "";
          // One control setting another control is not a sound parameter.
          if (level == "ui")
            return;
          const std::string value = translate(binding, input, minimum, maximum);

          if (level == "tag") {
            const std::string* tag = find(binding, "identifier");
            const std::string target = tagAttribute(parameter);
            if (target == "volume" && tag != nullptr)
              tag_volumes_[lower(*tag)] = linearGain(value);
            else if (target == "enabled" && tag != nullptr)
              tag_enabled_[lower(*tag)] = toBool(value);
            else
              ignored_bindings_[parameter.empty() ? "tag" : parameter]++;
            return;
          }

          const bool instrument = level == "instrument";
          if (!instrument && level != "group") {
            ignored_bindings_[parameter.empty() ? level : parameter]++;
            return;
          }
          const std::string attribute = bindingAttribute(parameter, instrument);
          if (attribute.empty()) {
            ignored_bindings_[parameter]++;
            return;
          }
          if (instrument) {
            instrument_overrides_[attribute] = value;
            return;
          }

          std::vector<std::string> tags;
          if (const std::string* group_tags = find(binding, "grouptags"))
            tags = tagList(*group_tags);
          else if (const std::string* any_tags = find(binding, "tags"))
            tags = tagList(*any_tags);
          if (!tags.empty()) {
            for (Group& group : groups_list_) {
              for (const std::string& tag : tags) {
                if (std::find(group.tags.begin(), group.tags.end(), tag) != group.tags.end()) {
                  group.overrides[attribute] = value;
                  break;
                }
              }
            }
            return;
          }
          const std::string* position = find(binding, "groupindex");
          if (position == nullptr)
            position = find(binding, "position");
          const int index = position ? toInt(*position) : 0;
          if (index >= 0 && index < static_cast<int>(groups_list_.size()))
            groups_list_[static_cast<size_t>(index)].overrides[attribute] = value;
        }

        // What a tag-level binding sets. Presets also write the group names
        // (Emerald Resonances' layer knobs use AMP_VOLUME).
        static std::string tagAttribute(const std::string& parameter) {
          if (parameter == "TAG_VOLUME" || parameter == "AMP_VOLUME")
            return "volume";
          if (parameter == "TAG_ENABLED" || parameter == "ENABLED")
            return "enabled";
          return {};
        }

        // The attribute a binding parameter sets, or "" for one with no effect here.
        static std::string bindingAttribute(const std::string& parameter, bool instrument) {
          static const std::map<std::string, std::string> kShared = {
            { "AMP_VEL_TRACK", "ampveltrack" }, { "ENV_ATTACK", "attack" }, { "ENV_DECAY", "decay" },
            { "ENV_SUSTAIN", "sustain" }, { "ENV_RELEASE", "release" }, { "PITCH_KEY_TRACK", "pitchkeytrack" },
            { "SAMPLE_START", "start" }, { "SAMPLE_END", "end" }, { "LOOP_START", "loopstart" },
            { "LOOP_END", "loopend" }, { "LO_NOTE", "lonote" }, { "HI_NOTE", "hinote" },
            { "ROOT_NOTE", "rootnote" }, { "SILENCING_MODE", "silencingmode" },
            { "SILENCING_DECAY", "silencingdecay" },
          };
          static const std::map<std::string, std::string> kInstrument = {
            { "AMP_VOLUME", "ampvolume" }, { "GLOBAL_TUNING", "globaltuning" }, { "PAN", "globalpan" },
          };
          static const std::map<std::string, std::string> kGroup = {
            { "AMP_VOLUME", "ampvolume" }, { "GROUP_TUNING", "grouptuning" }, { "PAN", "grouppan" },
            { "ENABLED", "enabled" }, { "AMP_ENV_ENABLED", "ampenvenabled" }, { "LO_VEL", "lovel" },
            { "HI_VEL", "hivel" },
          };
          const auto& own = instrument ? kInstrument : kGroup;
          auto found = own.find(parameter);
          if (found != own.end())
            return found->second;
          found = kShared.find(parameter);
          return found != kShared.end() ? found->second : std::string();
        }

        void tally(const Attributes& attributes) {
          for (const auto& attribute : attributes) {
            if (!isKnownAttribute(attribute.first))
              unsupported_[attribute.first]++;
          }
        }

        // Most attributes inherit from <groups> to <group> to <sample>, and a
        // binding's load value replaces the file's at its own level. Bindings
        // that map samples (notes, velocities, sample and loop points) set
        // every sample below them instead, since samples always carry those.
        const std::string* inherited(const Group& group, const Attributes& sample, const std::string& name) const {
          static const std::set<std::string> kMapping = {
            "start", "end", "loopstart", "loopend", "lonote", "hinote", "rootnote", "lovel", "hivel",
          };
          if (kMapping.count(name)) {
            if (const std::string* value = find(group.overrides, name))
              return value;
            if (const std::string* value = find(instrument_overrides_, name))
              return value;
          }
          if (const std::string* value = find(sample, name))
            return value;
          if (const std::string* value = find(group.overrides, name))
            return value;
          if (const std::string* value = find(group.attributes, name))
            return value;
          if (const std::string* value = find(instrument_overrides_, name))
            return value;
          return find(groups_, name);
        }

        double inheritedNumber(const Group& group, const Attributes& sample, const std::string& name,
                               double fallback) const {
          const std::string* value = inherited(group, sample, name);
          return value ? toNumber(*value) : fallback;
        }

        // A value that belongs to one level only: the binding's, then the file's.
        static const std::string* own(const Attributes& overrides, const Attributes& attributes,
                                      std::initializer_list<const char*> names) {
          for (const char* name : names) {
            if (const std::string* value = find(overrides, name))
              return value;
          }
          for (const char* name : names) {
            if (const std::string* value = find(attributes, name))
              return value;
          }
          return nullptr;
        }

        int64_t tagId(const std::string& tag) {
          auto found = tag_ids_.find(tag);
          if (found != tag_ids_.end())
            return found->second;
          const int64_t id = static_cast<int64_t>(tag_ids_.size()) + 1;
          tag_ids_[tag] = id;
          return id;
        }

        void buildRegions() {
          // Tags that silence something become choke groups.
          std::set<std::string> silencing;
          for (const Group& group : groups_list_) {
            for (const Attributes& sample : group.samples) {
              if (const std::string* list = inherited(group, sample, "silencedbytags")) {
                for (const std::string& tag : tagList(*list))
                  silencing.insert(tag);
              }
            }
          }

          // A volume knob multiplies the level the file sets, as in
          // DecentSampler: a release group at volume="20dB" under a knob at
          // 0.6 plays at 6 times, not 0.6.
          auto knobGain = [](const Attributes& overrides) {
            const std::string* value = find(overrides, "ampvolume");
            return value ? linearGain(*value) : 1.0f;
          };
          const std::string* global_volume = own({}, groups_, { "globalvolume", "volume" });
          const float instrument_gain = (global_volume ? linearGain(*global_volume) : 1.0f) * knobGain(instrument_overrides_);
          const std::string* global_pan = own(instrument_overrides_, groups_, { "globalpan" });
          const std::string* global_tuning = own(instrument_overrides_, groups_, { "globaltuning" });

          bool several_choke_tags = false;
          bool several_silencers = false;
          int oscillators = 0;
          int unsupported_delays = 0;
          int continuous = 0;
          int inferred_round_robins = 0;
          int missing_paths = 0;
          int other_loop_modes = 0;
          int crossfades = 0;
          for (const Group& group : groups_list_) {
            oscillators += group.oscillators;
            const std::string* enabled = own(group.overrides, group.attributes, { "enabled" });
            if (enabled != nullptr && !toBool(*enabled)) {
              // A control may enable the group later: its samples are still stored.
              for (const Attributes& sample : group.samples)
                noteSample(sample);
              continue;
            }
            const std::string* group_volume = own({}, group.attributes, { "groupvolume", "volume" });
            const float group_gain = (group_volume ? linearGain(*group_volume) : 1.0f) * knobGain(group.overrides);
            const std::string* group_pan = own(group.overrides, group.attributes, { "grouppan" });
            const std::string* group_tuning = own(group.overrides, group.attributes, { "grouptuning" });

            // A round robin without seqLength runs over the positions of the
            // group's samples that share some of its keys and velocities.
            auto overlaps = [&](const Attributes& a, const Attributes& b) {
              return inheritedNumber(group, a, "lonote", 0) <= inheritedNumber(group, b, "hinote", 127) &&
                     inheritedNumber(group, b, "lonote", 0) <= inheritedNumber(group, a, "hinote", 127) &&
                     inheritedNumber(group, a, "lovel", 0) <= inheritedNumber(group, b, "hivel", 127) &&
                     inheritedNumber(group, b, "lovel", 0) <= inheritedNumber(group, a, "hivel", 127);
            };
            auto positionsAround = [&](const Attributes& sample) {
              int positions = 1;
              for (const Attributes& other : group.samples) {
                if (overlaps(sample, other))
                  positions = std::max(positions, static_cast<int>(inheritedNumber(group, other, "seqposition", 1)));
              }
              return positions;
            };

            for (const Attributes& sample : group.samples) {
              const std::string* path = find(sample, "path");
              if (path == nullptr || path->empty()) {
                missing_paths++;
                continue;
              }

              std::vector<std::string> tags = group.tags;
              if (const std::string* own_tags = find(sample, "tags")) {
                for (const std::string& tag : tagList(*own_tags)) {
                  if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                    tags.push_back(tag);
                }
              }
              bool disabled = false;
              float tag_gain = 1.0f;
              for (const std::string& tag : tags) {
                auto enabled_tag = tag_enabled_.find(tag);
                disabled = disabled || (enabled_tag != tag_enabled_.end() && !enabled_tag->second);
                auto volume = tag_volumes_.find(tag);
                if (volume != tag_volumes_.end())
                  tag_gain *= volume->second;
              }
              if (disabled) {
                noteSample(sample);
                continue;
              }

              Region region;
              const int root = parseKey(inherited(group, sample, "rootnote") ? *inherited(group, sample, "rootnote")
                                                                              : std::string(), 60);
              region.pitch_keycenter = static_cast<float>(root);
              region.lokey = static_cast<int>(inheritedNumber(group, sample, "lonote", 0));
              region.hikey = static_cast<int>(inheritedNumber(group, sample, "hinote", 127));
              region.lovel = static_cast<int>(inheritedNumber(group, sample, "lovel", 0));
              region.hivel = static_cast<int>(inheritedNumber(group, sample, "hivel", 127));

              // Tuning is added to the note before key tracking unless told otherwise.
              const float keytrack = static_cast<float>(inheritedNumber(group, sample, "pitchkeytrack", 1.0));
              region.pitch_keytrack = keytrack * 100.0f;
              const std::string* ignores_keytrack = inherited(group, sample, "tuningignoreskeytrack");
              const double semitones = (global_tuning ? toNumber(*global_tuning) : 0.0) +
                                       (group_tuning ? toNumber(*group_tuning) : 0.0) +
                                       inheritedNumber(group, sample, "tuning", 0.0);
              region.tune_cents = static_cast<float>(semitones * 100.0) *
                                  (ignores_keytrack && toBool(*ignores_keytrack) ? 1.0f : keytrack);

              // The three volumes multiply; pan is one inherited position plus two offsets.
              const std::string* sample_volume = find(sample, "volume");
              region.amplitude = instrument_gain * group_gain * (sample_volume ? linearGain(*sample_volume) : 1.0f) * tag_gain;
              const double pan = inheritedNumber(group, sample, "pan", 0.0) +
                                 (global_pan ? toNumber(*global_pan) : 0.0) + (group_pan ? toNumber(*group_pan) : 0.0);
              region.pan = static_cast<float>(std::max(-100.0, std::min(100.0, pan)) / 100.0);

              // Gain follows velocity linearly: velocity 64 of 127 plays at about half.
              region.amp_veltrack = static_cast<float>(inheritedNumber(group, sample, "ampveltrack", 1.0));
              region.velocity_points = { { 0, 0.0f }, { 127, 1.0f } };

              if (const std::string* start = inherited(group, sample, "start"))
                region.offset = std::max<int64_t>(0, static_cast<int64_t>(toNumber(*start)));
              if (const std::string* end = inherited(group, sample, "end"))
                region.end = std::max(region.offset, static_cast<int64_t>(toNumber(*end))) + 1;

              if (const std::string* loop_start = inherited(group, sample, "loopstart")) {
                region.loop_start = std::max<int64_t>(0, static_cast<int64_t>(toNumber(*loop_start)));
                region.loop_start_set = true;
              }
              if (const std::string* loop_end = inherited(group, sample, "loopend")) {
                region.loop_end = static_cast<int64_t>(toNumber(*loop_end)) + 1;
                region.loop_end_set = true;
              }
              if (const std::string* loop = inherited(group, sample, "loopenabled")) {
                region.loop_mode_set = true;
                if (toBool(*loop)) {
                  const std::string* during_release = inherited(group, sample, "loopduringrelease");
                  region.loop_mode = during_release && !toBool(*during_release) ? LoopMode::kSustain
                                                                                : LoopMode::kContinuous;
                }
                else
                  region.loop_mode = LoopMode::kNoLoop;
              }
              if (const std::string* loop_mode = inherited(group, sample, "loopmode")) {
                const std::string value = lower(*loop_mode);
                region.loop_alternate = value == "pingpong" || value == "ping_pong" || value == "alternate";
                if (!region.loop_alternate && value != "forward")
                  other_loop_modes++;
              }
              // DecentSampler counts loopCrossfade in samples; under one is no crossfade.
              if (const std::string* crossfade = inherited(group, sample, "loopcrossfade")) {
                if (toNumber(*crossfade) >= 1.0)
                  crossfades++;
              }
              const std::string* envelope = inherited(group, sample, "ampenvenabled");
              if (envelope && !toBool(*envelope)) {
                region.loop_mode = LoopMode::kOneShot;
                region.loop_mode_set = true;
              }

              region.ampeg.attack = static_cast<float>(std::max(0.0, inheritedNumber(group, sample, "attack", 0.0)));
              region.ampeg.decay = static_cast<float>(std::max(0.0, inheritedNumber(group, sample, "decay", 0.0)));
              region.ampeg.sustain = static_cast<float>(
                  std::max(0.0, std::min(1.0, inheritedNumber(group, sample, "sustain", 1.0))));
              region.ampeg.release = static_cast<float>(
                  std::max(0.001, inheritedNumber(group, sample, "release", kDefaultRelease)));

              const std::string trigger = inherited(group, sample, "trigger") ? lower(*inherited(group, sample, "trigger")) : "";
              if (trigger == "release")
                region.trigger = Trigger::kRelease;
              else if (trigger == "first")
                region.trigger = Trigger::kFirst;
              else if (trigger == "legato")
                region.trigger = Trigger::kLegato;
              else if (trigger == "continuous")
                continuous++;

              // "3dB" falls 3 dB per second held; a linear value is the share lost per second.
              if (const std::string* decay = inherited(group, sample, "releasetriggerdecay")) {
                const juce::String text = juce::String(*decay).trim();
                if (text.endsWithIgnoreCase("db"))
                  region.rt_decay = std::fabs(static_cast<float>(text.dropLastCharacters(2).trim().getDoubleValue()));
                else {
                  const double kept = 1.0 - std::max(0.0, std::min(0.999999, text.getDoubleValue()));
                  region.rt_decay = static_cast<float>(-20.0 * std::log10(kept));
                }
              }

              applyCcAttributes(region, group.attributes);
              applyCcAttributes(region, sample);

              // seqMode defaults to "always", which plays every matching zone.
              // Takes numbered by seqPosition without a seqMode are a
              // mistake in the preset (DecentSampler's developer calls it a
              // bug), and stacking them is louder and smears the attack, so
              // they take turns. An explicit seqMode="always" still stacks.
              const std::string* mode_text = inherited(group, sample, "seqmode");
              std::string mode = mode_text ? lower(*mode_text) : "always";
              if (mode_text == nullptr && positionsAround(sample) > 1) {
                mode = "round_robin";
                inferred_round_robins++;
              }
              const int position = std::max(1, static_cast<int>(inheritedNumber(group, sample, "seqposition", 1)));
              int length = static_cast<int>(inheritedNumber(group, sample, "seqlength", 0));
              if (length <= 0 && mode != "always")
                length = positionsAround(sample);
              if (length > 1 && position <= length) {
                if (mode == "round_robin") {
                  region.seq_length = length;
                  region.seq_position = position;
                }
                else if (mode == "random" || mode == "true_random") {
                  region.lorand = static_cast<float>(position - 1) / length;
                  region.hirand = static_cast<float>(position) / length;
                }
              }

              // A region chokes through one tag and is choked by one.
              int choke_tags = 0;
              for (const std::string& tag : tags) {
                if (!silencing.count(tag))
                  continue;
                if (choke_tags++ == 0)
                  region.group = tagId(tag);
              }
              several_choke_tags = several_choke_tags || choke_tags > 1;
              if (const std::string* list = inherited(group, sample, "silencedbytags")) {
                const std::vector<std::string> silencers = tagList(*list);
                if (!silencers.empty())
                  region.off_by = tagId(silencers.front());
                several_silencers = several_silencers || silencers.size() > 1;
              }
              const std::string* decay = inherited(group, sample, "silencingdecay");
              if (decay && toNumber(*decay) > 0.0) {
                region.off_mode = OffMode::kTime;
                region.off_time = static_cast<float>(toNumber(*decay));
              }
              else if (const std::string* silencing_mode = inherited(group, sample, "silencingmode"))
                region.off_mode = lower(*silencing_mode) == "normal" ? OffMode::kNormal : OffMode::kFast;

              if (const std::string* delay = inherited(group, sample, "delay")) {
                const std::string unit = inherited(group, sample, "delayunit") ? lower(*inherited(group, sample, "delayunit"))
                                                                               : "seconds";
                if (unit == "seconds")
                  region.delay = static_cast<float>(std::max(0.0, toNumber(*delay)));
                else if (toNumber(*delay) > 0.0)
                  unsupported_delays++;
              }

              addSample(region, juce::String(*path).replaceCharacter('\\', '/').toStdString());
            }
          }

          if (missing_paths > 0)
            notes_.push_back("Skipped " + std::to_string(missing_paths) + " <sample> elements without a path");
          if (oscillators > 0)
            notes_.push_back("Ignored " + std::to_string(oscillators) + " <oscillator> elements");
          if (inferred_round_robins > 0)
            notes_.push_back(std::to_string(inferred_round_robins) +
                             " samples with seqPosition but no seqMode play as round robins");
          if (other_loop_modes > 0)
            notes_.push_back("loopMode other than forward or pingpong plays forward (" +
                             std::to_string(other_loop_modes) + " uses)");
          if (crossfades > 0)
            notes_.push_back("Loop crossfades were ignored (" + std::to_string(crossfades) + " uses)");
          if (continuous > 0)
            notes_.push_back("trigger=continuous plays like attack (" + std::to_string(continuous) + " uses)");
          if (unsupported_delays > 0)
            notes_.push_back("Delays in beats or samples were ignored (" + std::to_string(unsupported_delays) + " uses)");
          if (several_choke_tags)
            notes_.push_back("Samples with several silencing tags choke through the first one only");
          if (several_silencers)
            notes_.push_back("silencedByTags lists with several tags use the first one only");
        }

        static void applyCcAttributes(Region& region, const Attributes& attributes) {
          for (const auto& attribute : attributes) {
            int cc = 0;
            const bool low = matchNumbered(attribute.first, "locc", cc);
            if (low || matchNumbered(attribute.first, "hicc", cc)) {
              const float value = static_cast<float>(toNumber(attribute.second));
              if (cc < 0 || cc >= kNumCcs || value < 0.0f)
                continue;
              auto gate = std::find_if(region.cc_gates.begin(), region.cc_gates.end(),
                                       [cc](const CcRange& range) { return range.cc == cc; });
              if (gate == region.cc_gates.end()) {
                CcRange range;
                range.cc = cc;
                region.cc_gates.push_back(range);
                gate = region.cc_gates.end() - 1;
              }
              (low ? gate->low : gate->high) = value;
              continue;
            }
            const bool on_low = matchNumbered(attribute.first, "onlocc", cc);
            if (on_low || matchNumbered(attribute.first, "onhicc", cc)) {
              const float value = static_cast<float>(toNumber(attribute.second));
              if (cc < 0 || cc >= kNumCcs || value < 0.0f)
                continue;
              region.trigger = Trigger::kCc;
              region.on_cc = cc;
              (on_low ? region.on_low : region.on_high) = value;
            }
          }
        }

        void addSample(Region& region, const std::string& written) {
          bool absolute = false;
          const juce::File file = resolveInstrumentPath(written, roots_, options_.path_overrides, &absolute);
          region.sample_path = file.getFullPathName().toStdString();
          bool known = false;
          for (auto& reference : result_.samples) {
            if (reference.file == file) {
              reference.played = true;
              known = true;
            }
          }
          if (!known)
            result_.samples.push_back({ written, file, absolute, true });
          region.buildVelocityCurve();
          result_.instrument->regions.push_back(std::move(region));
        }

        // Reports the file of a sample no region plays now because its group
        // or tag is disabled. Presets store and hash the whole library, so a
        // control that enables the group later finds the file, and the hash
        // does not change with the controls.
        void noteSample(const Attributes& sample) {
          const std::string* path = find(sample, "path");
          if (path == nullptr || path->empty())
            return;
          const std::string written = juce::String(*path).replaceCharacter('\\', '/').toStdString();
          bool absolute = false;
          const juce::File file = resolveInstrumentPath(written, roots_, options_.path_overrides, &absolute);
          for (const auto& reference : result_.samples) {
            if (reference.file == file)
              return;
          }
          result_.samples.push_back({ written, file, absolute, false });
        }

        juce::File main_file_;
        SfzParser::Options options_;
        std::vector<juce::File> roots_;
        SfzParser::Result result_;

        Attributes groups_;
        Attributes instrument_overrides_;
        std::vector<Group> groups_list_;
        std::map<std::string, float> tag_volumes_;
        std::map<std::string, bool> tag_enabled_;
        std::map<std::string, int64_t> tag_ids_;

        std::map<std::string, int> unsupported_;
        std::map<std::string, int> ignored_bindings_;
        std::set<std::string> ignored_elements_;
        std::vector<std::string> notes_;
    };
  } // namespace
  } // namespace decent_sampler

  SfzParser::Result DecentSamplerParser::parse(const juce::File& file, const SfzParser::Options& options) {
    decent_sampler::Parser parser(file, options);
    return parser.run(nullptr);
  }

  SfzParser::Result DecentSamplerParser::parseText(const std::string& text, const juce::File& main_file,
                                                   const SfzParser::Options& options) {
    decent_sampler::Parser parser(main_file, options);
    return parser.run(&text);
  }
} // namespace multisample
} // namespace vital

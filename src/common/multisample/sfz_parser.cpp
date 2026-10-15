// SPDX-License-Identifier: GPL-3.0-or-later
#include "sfz_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace vital {
namespace multisample {

  namespace {
    bool isAbsolutePath(const std::string& path) {
      if (path.empty())
        return false;
      if (path[0] == '/')
        return true;
      return path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && path[2] == '/';
    }

    // Retries a missing relative path matching each component without case.
    juce::File findIgnoringCase(const juce::File& root, const std::string& relative) {
      juce::File current = root;
      juce::StringArray parts = juce::StringArray::fromTokens(juce::String(relative), "/", "");
      for (const juce::String& part : parts) {
        if (part.isEmpty() || part == ".")
          continue;
        if (part == "..") {
          current = current.getParentDirectory();
          continue;
        }
        juce::File exact = current.getChildFile(part);
        if (exact.exists()) {
          current = exact;
          continue;
        }
        bool found = false;
        for (const auto& entry : juce::RangedDirectoryIterator(current, false, "*",
                                                               juce::File::findFilesAndDirectories)) {
          if (entry.getFile().getFileName().equalsIgnoreCase(part)) {
            current = entry.getFile();
            found = true;
            break;
          }
        }
        if (!found)
          return {};
      }
      return current;
    }

  } // namespace

  bool isAbsoluteInstrumentPath(const std::string& path) {
    return isAbsolutePath(path);
  }

  juce::File resolveInstrumentPath(const std::string& written, const std::vector<juce::File>& roots,
                                   const std::map<std::string, juce::File>& overrides, bool* absolute) {
    auto override_entry = overrides.find(written);
    if (override_entry != overrides.end())
      return override_entry->second;
    if (isAbsolutePath(written)) {
      if (absolute)
        *absolute = true;
      return juce::File(juce::String(written));
    }
    if (absolute)
      *absolute = false;
    for (const juce::File& root : roots) {
      juce::File candidate = root.getChildFile(juce::String(written));
      if (candidate.exists())
        return candidate;
    }
    for (const juce::File& root : roots) {
      juce::File candidate = findIgnoringCase(root, written);
      if (candidate != juce::File())
        return candidate;
    }
    return roots.front().getChildFile(juce::String(written));
  }

  namespace {
    constexpr int kMaxIncludeDepth = 32;

    struct Opcode {
      std::string name;
      std::string value;
    };
    using OpcodeList = std::vector<Opcode>;

    bool isSpace(char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }

    bool isNameChar(char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
    }

    std::string trim(const std::string& text) {
      size_t start = 0;
      size_t end = text.size();
      while (start < end && isSpace(text[start]))
        start++;
      while (end > start && isSpace(text[end - 1]))
        end--;
      return text.substr(start, end - start);
    }

    std::string lower(std::string text) {
      for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    }

    std::string normalizeSlashes(std::string path) {
      std::replace(path.begin(), path.end(), '\\', '/');
      return path;
    }

    double toNumber(const std::string& text) {
      return juce::String(text).trim().getDoubleValue();
    }

    int toInt(const std::string& text) {
      return static_cast<int>(std::lround(toNumber(text)));
    }

    // "amplitude_oncc70" with prefix "amplitude_oncc" gives 70.
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

    class Parser {
      public:
        Parser(const juce::File& main_file, const SfzParser::Options& options) :
            main_file_(main_file), options_(options) {
          if (options_.search_roots.empty())
            roots_.push_back(main_file.getParentDirectory());
          else
            roots_ = options_.search_roots;
          result_.instrument = std::make_shared<Instrument>();
          result_.instrument->format = "sfz";
          result_.instrument->name = main_file.getFileNameWithoutExtension().toStdString();
          result_.instrument->source_path = main_file.getFullPathName().toStdString();
          for (float& value : result_.instrument->cc_defaults)
            value = 0.0f;
        }

        SfzParser::Result run(const std::string* text) {
          if (text != nullptr) {
            result_.text_files.push_back(main_file_);
            parseText(*text, 0);
          }
          else
            parseFile(main_file_, 0);
          finishRegion();

          Instrument& instrument = *result_.instrument;
          for (const auto& unsupported : unsupported_) {
            instrument.warnings.push_back("Ignored opcode " + unsupported.first + " (" +
                                          std::to_string(unsupported.second) + " uses)");
          }
          for (const auto& header : ignored_headers_)
            instrument.warnings.push_back("Ignored <" + header + "> blocks");
          if (result_.error.empty() && instrument.regions.empty())
            result_.error = "The SFZ file has no regions.";
          return std::move(result_);
        }

      private:
        enum class Level { kNone, kControl, kGlobal, kMaster, kGroup, kRegion, kIgnored };

        juce::File resolve(const std::string& written, bool* absolute = nullptr) const {
          return resolveInstrumentPath(written, roots_, options_.path_overrides, absolute);
        }

        void parseFile(const juce::File& file, int depth) {
          const juce::String path = file.getFullPathName();
          if (depth > kMaxIncludeDepth) {
            result_.instrument->warnings.push_back("Includes nested too deeply at " + path.toStdString());
            return;
          }
          if (include_stack_.count(path)) {
            result_.instrument->warnings.push_back("Circular include of " + path.toStdString());
            return;
          }
          if (!file.existsAsFile()) {
            if (depth == 0)
              result_.error = "Cannot open " + path.toStdString();
            else
              result_.instrument->missing_files.push_back(path.toStdString());
            return;
          }

          bool seen = false;
          for (const juce::File& existing : result_.text_files)
            seen = seen || existing == file;
          if (!seen)
            result_.text_files.push_back(file);

          include_stack_.insert(path);
          parseText(file.loadFileAsString().toStdString(), depth);
          include_stack_.erase(path);
        }

        std::string expand(const std::string& text) const {
          if (text.find('$') == std::string::npos || defines_.empty())
            return text;
          std::string result;
          size_t i = 0;
          while (i < text.size()) {
            if (text[i] == '$') {
              // Longest matching definition wins ($kick before $k).
              size_t best = 0;
              const std::string* replacement = nullptr;
              for (const auto& define : defines_) {
                const std::string& name = define.first;
                if (name.size() > best && text.compare(i, name.size(), name) == 0) {
                  best = name.size();
                  replacement = &define.second;
                }
              }
              if (replacement != nullptr) {
                result += *replacement;
                i += best;
                continue;
              }
            }
            result += text[i++];
          }
          return result;
        }

        void parseText(const std::string& text, int depth) {
          const size_t size = text.size();
          size_t i = 0;
          auto skipLine = [&] {
            while (i < size && text[i] != '\n')
              i++;
          };

          while (i < size) {
            const char c = text[i];
            if (isSpace(c)) {
              i++;
              continue;
            }
            if (c == '/' && i + 1 < size && text[i + 1] == '/') {
              skipLine();
              continue;
            }
            if (c == '/' && i + 1 < size && text[i + 1] == '*') {
              const size_t end = text.find("*/", i + 2);
              i = end == std::string::npos ? size : end + 2;
              continue;
            }
            if (c == '#') {
              size_t word_end = i + 1;
              while (word_end < size && std::isalpha(static_cast<unsigned char>(text[word_end])))
                word_end++;
              const std::string directive = text.substr(i + 1, word_end - i - 1);
              i = word_end;
              if (directive == "define") {
                while (i < size && (text[i] == ' ' || text[i] == '\t'))
                  i++;
                size_t name_start = i;
                while (i < size && !isSpace(text[i]))
                  i++;
                const std::string name = text.substr(name_start, i - name_start);
                size_t value_start = i;
                skipLine();
                std::string value = text.substr(value_start, i - value_start);
                const size_t comment = value.find("//");
                if (comment != std::string::npos)
                  value = value.substr(0, comment);
                if (!name.empty() && name[0] == '$')
                  defines_[name] = expand(trim(value));
              }
              else if (directive == "include") {
                while (i < size && (text[i] == ' ' || text[i] == '\t'))
                  i++;
                if (i < size && text[i] == '"') {
                  const size_t close = text.find('"', i + 1);
                  if (close == std::string::npos) {
                    skipLine();
                    continue;
                  }
                  const std::string written = normalizeSlashes(expand(text.substr(i + 1, close - i - 1)));
                  i = close + 1;
                  parseFile(resolve(written), depth + 1);
                }
                else
                  skipLine();
              }
              else
                skipLine();
              continue;
            }
            if (c == '<') {
              const size_t close = text.find('>', i + 1);
              if (close == std::string::npos)
                break;
              header(lower(trim(text.substr(i + 1, close - i - 1))));
              i = close + 1;
              continue;
            }

            // name=value. A value runs to the end of the line, a header, a
            // comment, or the next "name=" (sample names may contain spaces).
            size_t name_start = i;
            while (i < size && text[i] != '=' && !isSpace(text[i]) && text[i] != '<')
              i++;
            if (i >= size || text[i] != '=') {
              continue;
            }
            const std::string name = expand(text.substr(name_start, i - name_start));
            i++;
            size_t value_start = i;
            size_t limit = value_start;
            while (limit < size && text[limit] != '\n' && text[limit] != '\r' && text[limit] != '<' &&
                   !(text[limit] == '/' && limit + 1 < size && text[limit + 1] == '/')) {
              limit++;
            }
            size_t value_end = limit;
            for (size_t j = value_start; j < limit; ++j) {
              if (!isSpace(text[j]))
                continue;
              size_t k = j;
              while (k < limit && isSpace(text[k]))
                k++;
              size_t name_end = k;
              while (name_end < limit && isNameChar(text[name_end]))
                name_end++;
              if (name_end > k && name_end < limit && text[name_end] == '=') {
                value_end = j;
                break;
              }
            }
            const std::string value = expand(trim(text.substr(value_start, value_end - value_start)));
            i = value_end;
            opcode(lower(name), value);
          }
        }

        void header(const std::string& name) {
          finishRegion();
          if (name == "control") {
            level_ = Level::kControl;
          }
          else if (name == "global") {
            level_ = Level::kGlobal;
            global_.clear();
            master_.clear();
            group_.clear();
          }
          else if (name == "master") {
            level_ = Level::kMaster;
            master_.clear();
            group_.clear();
          }
          else if (name == "group") {
            level_ = Level::kGroup;
            group_.clear();
          }
          else if (name == "region") {
            level_ = Level::kRegion;
            region_.clear();
            in_region_ = true;
          }
          else {
            level_ = Level::kIgnored;
            ignored_headers_.insert(name);
          }
        }

        void opcode(const std::string& name, const std::string& value) {
          switch (level_) {
            case Level::kControl:
              controlOpcode(name, value);
              break;
            case Level::kGlobal:
              global_.push_back({ name, value });
              break;
            case Level::kMaster:
              master_.push_back({ name, value });
              break;
            case Level::kGroup:
              group_.push_back({ name, value });
              break;
            case Level::kRegion:
              region_.push_back({ name, value });
              break;
            case Level::kNone:
              // Opcodes before any header behave like <global> ones.
              global_.push_back({ name, value });
              break;
            case Level::kIgnored:
              break;
          }
        }

        void controlOpcode(const std::string& name, const std::string& value) {
          Instrument& instrument = *result_.instrument;
          int number = 0;
          if (name == "default_path")
            default_path_ = normalizeSlashes(value);
          else if (name == "note_offset")
            note_offset_ = toInt(value);
          else if (name == "octave_offset")
            octave_offset_ = toInt(value);
          else if (matchNumbered(name, "set_cc", number)) {
            if (number >= 0 && number < kNumCcs)
              instrument.cc_defaults[number] = static_cast<float>(std::max(0.0, std::min(127.0, toNumber(value))));
          }
          else if (matchNumbered(name, "set_hdcc", number)) {
            if (number >= 0 && number < kNumCcs)
              instrument.cc_defaults[number] = static_cast<float>(std::max(0.0, std::min(1.0, toNumber(value))) * 127.0);
          }
          else if (matchNumbered(name, "label_cc", number))
            instrument.cc_labels[number] = value;
          else if (name == "sw_default")
            instrument.sw_default = key(value, -1);
          else if (name.rfind("label_key", 0) == 0 || name == "hint_ram_based" || name.rfind("hint_", 0) == 0)
            return;
          else
            unsupported(name);
        }

        int key(const std::string& value, int fallback) const {
          const int parsed = parseKey(value, std::numeric_limits<int>::min());
          if (parsed == std::numeric_limits<int>::min())
            return fallback;
          if (parsed < 0)
            return parsed;
          return parsed + note_offset_ + 12 * octave_offset_;
        }

        void unsupported(const std::string& name) {
          // Count by family: "eq1_gain_oncc77" and "eq2_gain_oncc80" are one entry.
          std::string family;
          for (char c : name) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
              family += c;
            else if (family.empty() || family.back() != 'N')
              family += 'N';
          }
          unsupported_[family]++;
        }

        void finishRegion() {
          if (!in_region_)
            return;
          in_region_ = false;

          Region region;
          bool disabled = false;
          for (const OpcodeList* list : { &global_, &master_, &group_, &region_ }) {
            for (const Opcode& op : *list)
              apply(region, op.name, op.value, disabled);
          }
          if (disabled || region.sample_path.empty())
            return;
          if (region.sample_path[0] == '*') {
            if (lower(region.sample_path) != "*silence") {
              unsupported("sample=" + region.sample_path);
              return;
            }
            region.silent = true;
            region.buildVelocityCurve();
            result_.instrument->regions.push_back(std::move(region));
            return;
          }

          const std::string written = default_path_ + region.sample_path;
          bool absolute = false;
          juce::File file = resolve(written, &absolute);
          region.sample_path = file.getFullPathName().toStdString();
          bool known = false;
          for (const auto& reference : result_.samples)
            known = known || reference.file == file;
          if (!known)
            result_.samples.push_back({ written, file, absolute });

          region.buildVelocityCurve();
          result_.instrument->regions.push_back(std::move(region));
        }

        void apply(Region& region, const std::string& name, const std::string& value, bool& disabled) {
          int number = 0;
          Instrument& instrument = *result_.instrument;

          if (name == "sample")
            region.sample_path = normalizeSlashes(value);
          else if (name == "lokey")
            region.lokey = key(value, region.lokey);
          else if (name == "hikey")
            region.hikey = key(value, region.hikey);
          else if (name == "key") {
            const int k = key(value, region.lokey);
            region.lokey = k;
            region.hikey = k;
            if (k >= 0)
              region.pitch_keycenter = static_cast<float>(k);
          }
          else if (name == "pitch_keycenter") {
            if (lower(value) == "sample")
              region.keycenter_from_sample = true;
            else {
              region.keycenter_from_sample = false;
              region.pitch_keycenter = static_cast<float>(key(value, static_cast<int>(region.pitch_keycenter)));
            }
          }
          else if (name == "lovel")
            region.lovel = toInt(value);
          else if (name == "hivel")
            region.hivel = toInt(value);
          else if (name == "lorand")
            region.lorand = static_cast<float>(toNumber(value));
          else if (name == "hirand")
            region.hirand = static_cast<float>(toNumber(value));
          else if (name == "seq_length")
            region.seq_length = std::max(1, toInt(value));
          else if (name == "seq_position")
            region.seq_position = std::max(1, toInt(value));
          else if (matchNumbered(name, "locc", number))
            setGate(region, number, static_cast<float>(toNumber(value)), true);
          else if (matchNumbered(name, "hicc", number))
            setGate(region, number, static_cast<float>(toNumber(value)), false);
          else if (matchNumbered(name, "lohdcc", number))
            setGate(region, number, static_cast<float>(toNumber(value) * 127.0), true);
          else if (matchNumbered(name, "hihdcc", number))
            setGate(region, number, static_cast<float>(toNumber(value) * 127.0), false);
          else if (matchNumbered(name, "on_locc", number) || matchNumbered(name, "start_locc", number)) {
            region.trigger = Trigger::kCc;
            region.on_cc = number;
            region.on_low = static_cast<float>(toNumber(value));
          }
          else if (matchNumbered(name, "on_hicc", number) || matchNumbered(name, "start_hicc", number)) {
            region.trigger = Trigger::kCc;
            region.on_cc = number;
            region.on_high = static_cast<float>(toNumber(value));
          }
          else if (name == "trigger") {
            const std::string mode = lower(value);
            if (mode == "release" || mode == "release_key")
              region.trigger = Trigger::kRelease;
            else if (mode == "first")
              region.trigger = Trigger::kFirst;
            else if (mode == "legato")
              region.trigger = Trigger::kLegato;
            else
              region.trigger = Trigger::kAttack;
          }
          else if (name == "sw_last")
            region.sw_last = key(value, -1);
          else if (name == "sw_lokey")
            region.sw_lokey = key(value, -1);
          else if (name == "sw_hikey")
            region.sw_hikey = key(value, -1);
          else if (name == "sw_default")
            instrument.sw_default = key(value, -1);
          else if (name == "group" || name == "polyphony_group")
            region.group = static_cast<int64_t>(toNumber(value));
          else if (name == "off_by" || name == "offby")
            region.off_by = static_cast<int64_t>(toNumber(value));
          else if (name == "off_mode") {
            const std::string mode = lower(value);
            region.off_mode = mode == "normal" ? OffMode::kNormal : mode == "time" ? OffMode::kTime : OffMode::kFast;
          }
          else if (name == "off_time") {
            region.off_mode = OffMode::kTime;
            region.off_time = static_cast<float>(std::max(0.0, toNumber(value)));
          }
          else if (name == "pitch_keytrack")
            region.pitch_keytrack = static_cast<float>(toNumber(value));
          else if (name == "tune" || name == "pitch")
            region.tune_cents = static_cast<float>(toNumber(value));
          else if (name == "transpose")
            region.transpose = toInt(value);
          else if (name == "volume" || name == "gain")
            region.volume_db = static_cast<float>(toNumber(value));
          else if (name == "amplitude")
            region.amplitude = static_cast<float>(toNumber(value) / 100.0);
          else if (name == "pan")
            region.pan = static_cast<float>(std::max(-100.0, std::min(100.0, toNumber(value))) / 100.0);
          else if (name == "width")
            region.width = static_cast<float>(std::max(-100.0, std::min(100.0, toNumber(value))) / 100.0);
          else if (name == "amp_veltrack")
            region.amp_veltrack = static_cast<float>(toNumber(value) / 100.0);
          else if (matchNumbered(name, "amp_velcurve_", number)) {
            auto existing = std::find_if(region.velocity_points.begin(), region.velocity_points.end(),
                                         [number](const std::pair<int, float>& p) { return p.first == number; });
            const float gain = static_cast<float>(toNumber(value));
            if (existing != region.velocity_points.end())
              existing->second = gain;
            else
              region.velocity_points.push_back({ number, gain });
          }
          else if (name == "offset")
            region.offset = std::max<int64_t>(0, static_cast<int64_t>(toNumber(value)));
          else if (name == "offset_random")
            region.offset_random = static_cast<float>(std::max(0.0, toNumber(value)));
          else if (name == "end") {
            // end is the last frame played; 0 or -1 silences the region.
            const int64_t end = static_cast<int64_t>(toNumber(value));
            disabled = end <= 0;
            region.end = end + 1;
          }
          else if (name == "delay")
            region.delay = static_cast<float>(std::max(0.0, toNumber(value)));
          else if (name == "loop_mode" || name == "loopmode") {
            const std::string mode = lower(value);
            region.loop_mode_set = true;
            if (mode == "one_shot")
              region.loop_mode = LoopMode::kOneShot;
            else if (mode == "loop_continuous")
              region.loop_mode = LoopMode::kContinuous;
            else if (mode == "loop_sustain")
              region.loop_mode = LoopMode::kSustain;
            else
              region.loop_mode = LoopMode::kNoLoop;
          }
          else if (name == "loop_start" || name == "loopstart") {
            region.loop_start = std::max<int64_t>(0, static_cast<int64_t>(toNumber(value)));
            region.loop_start_set = true;
          }
          else if (name == "loop_end" || name == "loopend") {
            region.loop_end = static_cast<int64_t>(toNumber(value)) + 1;
            region.loop_end_set = true;
          }
          else if (name == "rt_decay")
            region.rt_decay = static_cast<float>(std::max(0.0, toNumber(value)));
          else if (applyEnvelope(region, name, value)) { }
          else if (matchNumbered(name, "amplitude_oncc", number) || matchNumbered(name, "amplitude_cc", number))
            addMod(region.amplitude_cc, number, static_cast<float>(toNumber(value) / 100.0));
          else if (matchNumbered(name, "volume_oncc", number) || matchNumbered(name, "gain_oncc", number) ||
                   matchNumbered(name, "gain_cc", number) || matchNumbered(name, "volume_cc", number))
            addMod(region.volume_cc, number, static_cast<float>(toNumber(value)));
          else if (matchNumbered(name, "pan_oncc", number) || matchNumbered(name, "pan_cc", number))
            addMod(region.pan_cc, number, static_cast<float>(toNumber(value) / 100.0));
          else if (name == "default_path" || name == "note_offset" || name == "octave_offset" ||
                   name.rfind("set_cc", 0) == 0 || name.rfind("label_cc", 0) == 0)
            controlOpcode(name, value);
          else
            unsupported(name);
        }

        static void setGate(Region& region, int cc, float value, bool low) {
          if (cc < 0 || cc >= kNumCcs)
            return;
          for (CcRange& gate : region.cc_gates) {
            if (gate.cc == cc) {
              (low ? gate.low : gate.high) = value;
              return;
            }
          }
          CcRange gate;
          gate.cc = cc;
          (low ? gate.low : gate.high) = value;
          region.cc_gates.push_back(gate);
        }

        static void addMod(std::vector<CcMod>& mods, int cc, float amount) {
          if (cc < 0 || cc >= kNumCcs)
            return;
          for (CcMod& mod : mods) {
            if (mod.cc == cc) {
              mod.amount = amount;
              return;
            }
          }
          mods.push_back({ cc, amount });
        }

        bool applyEnvelope(Region& region, const std::string& name, const std::string& value) {
          if (name.rfind("ampeg_", 0) != 0)
            return false;
          struct Stage {
            const char* name;
            float Envelope::* value;
            std::vector<CcMod> EnvelopeCcMods::* mods;
            float scale;
          };
          static const Stage kStages[] = {
            { "delay", &Envelope::delay, &EnvelopeCcMods::delay, 1.0f },
            { "start", &Envelope::start, &EnvelopeCcMods::start, 0.01f },
            { "attack", &Envelope::attack, &EnvelopeCcMods::attack, 1.0f },
            { "hold", &Envelope::hold, &EnvelopeCcMods::hold, 1.0f },
            { "decay", &Envelope::decay, &EnvelopeCcMods::decay, 1.0f },
            { "sustain", &Envelope::sustain, &EnvelopeCcMods::sustain, 0.01f },
            { "release", &Envelope::release, &EnvelopeCcMods::release, 1.0f },
          };
          const std::string rest = name.substr(6);
          for (const Stage& stage : kStages) {
            const std::string stage_name = stage.name;
            if (rest == stage_name) {
              region.ampeg.*stage.value = static_cast<float>(toNumber(value)) * stage.scale;
              return true;
            }
            int number = 0;
            if (matchNumbered(rest, (stage_name + "_oncc").c_str(), number) ||
                matchNumbered(rest, (stage_name + "cc").c_str(), number)) {
              addMod(region.ampeg_cc.*stage.mods, number, static_cast<float>(toNumber(value)) * stage.scale);
              return true;
            }
          }
          return false;
        }

        juce::File main_file_;
        SfzParser::Options options_;
        std::vector<juce::File> roots_;
        SfzParser::Result result_;
        std::map<std::string, std::string> defines_;
        std::set<juce::String> include_stack_;
        std::map<std::string, int> unsupported_;
        std::set<std::string> ignored_headers_;

        Level level_ = Level::kNone;
        bool in_region_ = false;
        OpcodeList global_;
        OpcodeList master_;
        OpcodeList group_;
        OpcodeList region_;
        std::string default_path_;
        int note_offset_ = 0;
        int octave_offset_ = 0;
    };
  } // namespace

  SfzParser::Result SfzParser::parse(const juce::File& file, const Options& options) {
    Parser parser(file, options);
    return parser.run(nullptr);
  }

  SfzParser::Result SfzParser::parseText(const std::string& text, const juce::File& main_file,
                                         const Options& options) {
    Parser parser(main_file, options);
    return parser.run(&text);
  }
} // namespace multisample
} // namespace vital

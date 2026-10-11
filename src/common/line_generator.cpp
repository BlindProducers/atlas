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

#include "line_generator.h"
#include "poly_utils.h"
#include "futils.h"

#include <cmath>
#include <thread>

LineGenerator::BufferRead::BufferRead(BufferRead&& other) noexcept :
    owner_(other.owner_), index_(other.index_), buffer_(other.buffer_) {
  other.owner_ = nullptr;
  other.index_ = -1;
  other.buffer_ = nullptr;
}

LineGenerator::BufferRead& LineGenerator::BufferRead::operator=(BufferRead&& other) noexcept {
  if (this != &other) {
    release();
    owner_ = other.owner_;
    index_ = other.index_;
    buffer_ = other.buffer_;
    other.owner_ = nullptr;
    other.index_ = -1;
    other.buffer_ = nullptr;
  }
  return *this;
}

LineGenerator::BufferRead::~BufferRead() {
  release();
}

void LineGenerator::BufferRead::release() {
  if (owner_ != nullptr && index_ >= 0)
    owner_->buffer_readers_[static_cast<size_t>(index_)].fetch_sub(1, std::memory_order_release);
  owner_ = nullptr;
  index_ = -1;
  buffer_ = nullptr;
}

LineGenerator::LineGenerator(int resolution) : points_(), powers_(), num_points_(2), resolution_(resolution),
                                               shape_mode_(kModeLfo),
                                               loop_(false), smooth_(false), deform_(defaultDeform()),
                                               swing_(defaultSwing()), linear_(true), render_count_(0) {
  for (auto& buffer : buffers_)
    buffer = std::make_unique<vital::mono_float[]>(resolution + kExtraValues);
  initLinear();
}

LineGenerator::BufferRead LineGenerator::acquireBuffer() const {
  for (;;) {
    const int index = published_buffer_.load(std::memory_order_acquire);
    buffer_readers_[static_cast<size_t>(index)].fetch_add(1, std::memory_order_acquire);
    if (published_buffer_.load(std::memory_order_acquire) == index)
      return BufferRead(this, index, buffers_[static_cast<size_t>(index)].get());
    buffer_readers_[static_cast<size_t>(index)].fetch_sub(1, std::memory_order_release);
  }
}

void LineGenerator::setShapeMode(int mode) {
  shape_mode_ = vital::utils::iclamp(mode, 0, kNumShapeModes - 1);
}

void LineGenerator::setDeform(float deform) {
  float new_deform = vital::utils::clamp(deform, 0.0f, 1.0f);
  if (deform_.load(std::memory_order_acquire) == new_deform &&
      !render_pending_.load(std::memory_order_acquire))
    return;

  deform_ = new_deform;
  checkLineIsLinear();
  tryRender();
}

void LineGenerator::setSwing(float swing) {
  float new_swing = vital::utils::clamp(swing, 0.0f, 1.0f);
  if (swing_.load(std::memory_order_acquire) == new_swing &&
      !render_pending_.load(std::memory_order_acquire))
    return;

  swing_ = new_swing;
  checkLineIsLinear();
  tryRender();
}

std::string LineGenerator::shapeModeName(int mode) {
  switch (mode) {
    case kModePerformer: return "performer";
    case kModeStepSequencer: return "step_sequencer";
    case kModeLfo:
    default:
      return "lfo";
  }
}

void LineGenerator::initLinear() {
  points_[0] = { 0.0f, 1.0f };
  points_[1] = { 1.0f, 0.0f };
  powers_[0] = 0.0f;
  powers_[1] = 0.0f;
  num_points_ = 2;
  linear_ = true;
  name_ = "Linear";
  smooth_ = false;
  render();
}

void LineGenerator::initTriangle() {
  points_[0] = { 0.0f, 1.0f };
  points_[1] = { 0.5f, 0.0f };
  points_[2] = { 1.0f, 1.0f };
  powers_[0] = 0.0f;
  powers_[1] = 0.0f;
  powers_[2] = 0.0f;
  num_points_ = 3;
  linear_ = false;
  name_ = "Triangle";
  smooth_ = false;
  render();
}

void LineGenerator::initSquare() {
  num_points_ = 5;
  points_[0] = { 0.0f, 1.0f };
  points_[1] = { 0.0f, 0.0f };
  points_[2] = { 0.5f, 0.0f };
  points_[3] = { 0.5f, 1.0f };
  points_[4] = { 1.0f, 1.0f };

  for (int i = 0; i < num_points_; ++i)
    powers_[i] = 0.0f;

  linear_ = false;
  name_ = "Square";
  smooth_ = false;
  render();
}

void LineGenerator::initSin() {
  points_[0] = { 0.0f, 1.0f };
  points_[1] = { 0.5f, 0.0f };
  points_[2] = { 1.0f, 1.0f };
  powers_[0] = 0.0f;
  powers_[1] = 0.0f;
  powers_[2] = 0.0f;
  num_points_ = 3;
  linear_ = false;
  name_ = "Sin";
  smooth_ = true;
  render();
}

void LineGenerator::initSawUp() {
  num_points_ = 3;
  points_[0] = { 0.0f, 1.0f };
  points_[1] = { 1.0f, 0.0f };
  points_[2] = { 1.0f, 1.0f };
  powers_[0] = 0.0f;
  powers_[1] = 0.0f;
  powers_[2] = 0.0f;
  linear_ = false;
  name_ = "Saw Up";
  smooth_ = false;
  render();
}

void LineGenerator::initSawDown() {
  num_points_ = 3;
  points_[0] = { 0.0f, 0.0f };
  points_[1] = { 1.0f, 1.0f };
  points_[2] = { 1.0f, 0.0f };
  powers_[0] = 0.0f;
  powers_[1] = 0.0f;
  powers_[2] = 0.0f;
  linear_ = false;
  name_ = "Saw Down";
  smooth_ = false;
  render();
}

json LineGenerator::stateToJson() {
  json point_data;
  json power_data;

  for (int i = 0; i < num_points_; ++i) {
    std::pair<float, float> point = points_[i];
    point_data.push_back(point.first);
    point_data.push_back(point.second);
    power_data.push_back(powers_[i]);
  }

  json data;
  data["num_points"] = num_points_;
  data["points"] = point_data;
  data["powers"] = power_data;
  data["name"] = name_;
  data["smooth"] = smooth_;
  data["deform"] = deform_.load(std::memory_order_acquire);
  data["swing"] = swing_.load(std::memory_order_acquire);
  data["mode"] = shape_mode_;
  data["mode_name"] = shapeModeName(shape_mode_);
  return data;
}

bool LineGenerator::isValidJson(json data) {
  if (!data.count("num_points") || !data.count("points") || !data.count("powers"))
    return false;

  json point_data = data["points"];
  json power_data = data["powers"];
  return point_data.is_array() && power_data.is_array();
}

void LineGenerator::jsonToState(json data) {
  num_points_ = data["num_points"];
  json point_data = data["points"];
  json power_data = data["powers"];
  name_ = "";
  if (data.count("name"))
    name_ = data["name"].get<std::string>();

  smooth_ = false;
  if (data.count("smooth"))
    smooth_ = data["smooth"];

  deform_ = defaultDeform();
  if (data.count("deform") && data["deform"].is_number())
    deform_ = vital::utils::clamp(data["deform"].get<float>(), 0.0f, 1.0f);

  swing_ = defaultSwing();
  if (data.count("swing") && data["swing"].is_number())
    swing_ = vital::utils::clamp(data["swing"].get<float>(), 0.0f, 1.0f);

  shape_mode_ = kModeLfo;
  if (data.count("mode") && data["mode"].is_number_integer())
    setShapeMode(data["mode"].get<int>());
  else if (data.count("mode_name") && data["mode_name"].is_string()) {
    std::string mode = data["mode_name"].get<std::string>();
    if (mode == "performer")
      setShapeMode(kModePerformer);
    else if (mode == "step_sequencer")
      setShapeMode(kModeStepSequencer);
  }

  int num_read = kMaxPoints;
  num_read = std::min(num_read, static_cast<int>(power_data.size()));

  for (int i = 0; i < num_read; ++i) {
    points_[i] = std::pair<float, float>(point_data[2 * i], point_data[2 * i + 1]);
    powers_[i] = power_data[i];
  }

  checkLineIsLinear();
  render();
}

float LineGenerator::applySwingToPhase(float phase) const {
  float amount = (swing_.load(std::memory_order_acquire) - defaultSwing()) * 2.0f;
  if (fabsf(amount) < 0.0001f)
    return phase;

  static constexpr int kSwingSteps = 16;
  float step = 1.0f / kSwingSteps;
  float pair_width = 2.0f * step;
  float wrapped = vital::utils::clamp(phase, 0.0f, 1.0f);
  if (wrapped >= 1.0f)
    return 1.0f;

  int pair_index = static_cast<int>(wrapped / pair_width);
  float pair_start = pair_index * pair_width;
  float local = wrapped - pair_start;
  float swing_point = step + amount * step * 0.5f;
  swing_point = vital::utils::clamp(swing_point, step * 0.25f, step * 1.75f);

  float source_local = 0.0f;
  if (local < swing_point)
    source_local = (local / swing_point) * step;
  else
    source_local = step + ((local - swing_point) / (pair_width - swing_point)) * step;

  return vital::utils::clamp(pair_start + source_local, 0.0f, 1.0f);
}

float LineGenerator::applyDeformToTransition(float t) const {
  float deform_amount = (deform_.load(std::memory_order_acquire) - defaultDeform()) * 2.0f;
  if (fabsf(deform_amount) < 0.0001f)
    return t;

  t = vital::utils::clamp(t, 0.0f, 1.0f);
  if (deform_amount < 0.0f) {
    float drive = 1.0f + -deform_amount * 14.0f;
    return powf(t, drive);
  }

  float smooth = smoothTransition(t);
  smooth = smoothTransition(smooth);
  smooth = smoothTransition(smooth);
  return vital::utils::clamp(t + deform_amount * (smooth - t), 0.0f, 1.0f);
}

void LineGenerator::render() {
  bool expected = false;
  while (!render_lock_.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
    expected = false;
    std::this_thread::yield();
  }
  renderLocked();
  render_lock_.store(false, std::memory_order_release);
}

void LineGenerator::tryRender() {
  bool expected = false;
  if (!render_lock_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
    render_pending_.store(true, std::memory_order_release);
    return;
  }
  renderLocked();
  render_lock_.store(false, std::memory_order_release);
}

void LineGenerator::renderLocked() {
  // Cleared before reading deform/swing so a concurrent change re-requests.
  render_pending_.store(false, std::memory_order_release);
  render_count_++;

  const int active_buffer = published_buffer_.load(std::memory_order_acquire);
  const int render_buffer = 1 - active_buffer;
  while (buffer_readers_[static_cast<size_t>(render_buffer)].load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  vital::mono_float* buffer = buffers_[render_buffer].get();

  int point_index = 0;
  std::pair<float, float> last_point = points_[point_index];
  float current_power = 0.0f;
  std::pair<float, float> current_point = points_[point_index];
  if (loop_) {
    last_point = points_[num_points_ - 1];
    last_point.first -= 1.0f;
    current_power = powers_[num_points_ - 1];
  }

  bool use_vital_rasterization = deform_.load(std::memory_order_acquire) == defaultDeform() &&
                                 swing_.load(std::memory_order_acquire) == defaultSwing();
  for (int point = 0; point < num_points_ && use_vital_rasterization; ++point)
    use_vital_rasterization = !isHoldPower(powers_[point]);

  for (int i = 0; i < resolution_; ++i) {
    float x = applySwingToPhase(i / (resolution_ - 1.0f));

    // Vital computes the sample at a point transition before advancing to the
    // next segment. Preserve that exact behavior for legacy/default shapes;
    // Atlas's deform, swing and hold modes need the phase-aware path below.
    if (!use_vital_rasterization) {
      while (x > current_point.first && point_index < num_points_) {
        current_power = powers_[point_index % num_points_];
        point_index++;
        last_point = current_point;
        current_point = points_[point_index % num_points_];
        if (point_index >= num_points_) {
          current_point.first += 1.0f;
          break;
        }
      }
    }

    float t = 1.0f;
    if (current_point.first > last_point.first)
      t = (x - last_point.first) / (current_point.first - last_point.first);

    bool hold = isHoldPower(current_power);
    if (hold)
      t = 0.0f;
    else if (smooth_)
      t = smoothTransition(t);

    t = vital::utils::clamp(vital::futils::powerScale(t, current_power), 0.0f, 1.0f);
    if (!hold)
      t = applyDeformToTransition(t);

    float y = last_point.second + t * (current_point.second - last_point.second);
    buffer[i + 1] = 1.0f - y;

    if (use_vital_rasterization) {
      while (x > current_point.first && point_index < num_points_) {
        current_power = powers_[point_index % num_points_];
        point_index++;
        last_point = current_point;
        current_point = points_[point_index % num_points_];
        if (point_index >= num_points_) {
          current_point.first += 1.0f;
          break;
        }
      }
    }
  }

  if (loop_) {
    buffer[0] = buffer[resolution_];
    buffer[resolution_ + 1] = buffer[1];
    buffer[resolution_ + 2] = buffer[2];
  }
  else {
    buffer[0] = buffer[1];
    buffer[resolution_ + 1] = buffer[resolution_];
    buffer[resolution_ + 2] = buffer[resolution_];
  }

  // Publish only after the complete lookup (including interpolation padding)
  // is ready. The audio thread therefore sees either the old or new shape,
  // never a partially rewritten curve.
  published_buffer_.store(render_buffer, std::memory_order_release);
}

float LineGenerator::valueAtPhase(float phase) {
  const vital::mono_float* buffer = getCubicInterpolationBuffer();
  float scaled_phase = vital::utils::clamp(phase, 0.0f, 1.0f) * resolution_;
  int index = scaled_phase;
  return vital::utils::interpolate(buffer[index + 1], buffer[index + 2], scaled_phase - index);
}

void LineGenerator::checkLineIsLinear() {
  linear_ = !smooth_ && deform_.load(std::memory_order_acquire) == defaultDeform() &&
            swing_.load(std::memory_order_acquire) == defaultSwing() &&
            num_points_ == 2 && powers_[0] == 0.0f &&
            points_[0] == std::pair<float, float>(0.0f, 1.0f) &&
            points_[1] == std::pair<float, float>(1.0f, 0.0f);
}

float LineGenerator::getValueBetweenPoints(float x, int index_from, int index_to) {
  VITAL_ASSERT(index_from >= 0 && index_to < num_points_);

  std::pair<float, float> first = points_[index_from];
  std::pair<float, float> second = points_[index_to];
  float power = powers_[index_from];

  float width = second.first - first.first;
  if (width <= 0.0f)
    return second.second;

  float t = (x - first.first) / width;
  bool hold = isHoldPower(power);
  if (hold)
    t = 0.0f;
  else if (smooth_)
    t = smoothTransition(t);

  t = vital::utils::clamp(vital::futils::powerScale(t, power), 0.0f, 1.0f);
  if (!hold)
    t = applyDeformToTransition(t);
  return t * (second.second - first.second) + first.second;
}

float LineGenerator::getValueAtPhase(float phase) {
  phase = applySwingToPhase(phase);
  for (int i = 0; i < num_points_ - 1; ++i) {
    if (points_[i].first <= phase && points_[i + 1].first >= phase)
      return getValueBetweenPoints(phase, i, i + 1);
  }

  return lastPoint().second;
}

void LineGenerator::addPoint(int index, std::pair<float, float> position) {
  for (int i = num_points_; i > index; --i) {
    points_[i] = points_[i - 1];
    powers_[i] = powers_[i - 1];
  }

  num_points_++;
  points_[index] = position;
  powers_[index] = 0.0f;
  checkLineIsLinear();
}

void LineGenerator::addMiddlePoint(int index) {
  VITAL_ASSERT(index > 0);

  float x = (points_[index - 1].first + points_[index].first) * 0.5f;
  float y = getValueBetweenPoints(x, index - 1, index);
  addPoint(index, { x, y });
}

void LineGenerator::removePoint(int index) {
  num_points_--;
  for (int i = index; i < num_points_; ++i) {
    points_[i] = points_[i + 1];
    powers_[i] = powers_[i + 1];
  }
  checkLineIsLinear();
}

void LineGenerator::flipHorizontal() {
  for (int i = 0; i < (num_points_ + 1) / 2; ++i) {
    float tmp_x = 1.0f - points_[i].first;
    float tmp_y = points_[i].second;
    points_[i].first = 1.0f - points_[num_points_ - i - 1].first;
    points_[i].second = points_[num_points_ - i - 1].second;
    points_[num_points_ - i - 1].first = tmp_x;
    points_[num_points_ - i - 1].second = tmp_y;
  }
  for (int i = 0; i < num_points_ / 2; ++i) {
    float tmp_power = powers_[i];
    powers_[i] = -powers_[num_points_ - i - 2];
    powers_[num_points_ - i - 2] = -tmp_power;
  }
  render();
  checkLineIsLinear();
}

void LineGenerator::flipVertical() {
  for (int i = 0; i < num_points_; ++i)
    points_[i].second = 1.0f - points_[i].second;

  render();
  checkLineIsLinear();
}

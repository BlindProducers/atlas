#pragma once

#include "JuceHeader.h"

// Shared keyboard navigation for parameter, remap and wavetable sliders.
class ParameterSlider : public Slider {
  public:
    // Optional display-unit mapping, shared by keys and accessibility edits.
    std::function<double(double)> toAdjustmentValue;
    std::function<double(double)> fromAdjustmentValue;
    std::function<double()> adjustmentStep;

    double adjustmentValue(double value) const { return toAdjustmentValue ? toAdjustmentValue(value) : value; }
    double sliderValue(double value) const { return fromAdjustmentValue ? fromAdjustmentValue(value) : value; }
    double getAdjustmentStep() const {
      if (adjustmentStep)
        return adjustmentStep();
      return getInterval() > 0.0 ? getInterval() : (getMaximum() - getMinimum()) / 100.0;
    }

    bool keyPressed(const KeyPress& key) override {
      const int code = key.getKeyCode();
      if (code == KeyPress::returnKey) {
        showTextBox();
        return true;
      }
      if ((code == KeyPress::backspaceKey || code == KeyPress::deleteKey) && isDoubleClickReturnEnabled()) {
        setKeyboardValue(getDoubleClickReturnValue());
        return true;
      }
      // Preserve Atlas's existing delay-slider convention.
      if (code == KeyPress::homeKey || code == KeyPress::endKey) {
        setKeyboardValue(code == KeyPress::homeKey ? getMaximum() : getMinimum());
        return true;
      }
      const bool increase = code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::pageUpKey;
      const bool decrease = code == KeyPress::downKey || code == KeyPress::leftKey || code == KeyPress::pageDownKey;
      if (!increase && !decrease)
        return Slider::keyPressed(key);

      const bool continuous = getInterval() <= 0.0;
      const double step = getAdjustmentStep();
      const bool page = code == KeyPress::pageUpKey || code == KeyPress::pageDownKey;
      const double multiplier = page ? 10.0 : (continuous && key.getModifiers().isShiftDown() ? 0.1 : 1.0);
      setKeyboardValue(sliderValue(adjustmentValue(getValue()) + step * multiplier * (increase ? 1.0 : -1.0)));
      return true;
    }

  private:
    void setKeyboardValue(double value) {
      Slider::ScopedDragNotification drag(*this);
      setValue(jlimit(getMinimum(), getMaximum(), value), sendNotificationSync);
      if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
};

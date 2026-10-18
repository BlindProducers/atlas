#pragma once

#include "JuceHeader.h"

// An open popup menu passes Left, and Right on an item without a submenu, to
// the control it was opened from (JUCE's MenuWindow::keyPressed), and a combo
// box's open list does the same. That control sits behind the menu, blocked
// by it, and must not change: the user hears the menu, not the value moving.
inline bool isArrowKeyFromAnOpenMenu(const Component& component, const KeyPress& key) {
  if (!component.isCurrentlyBlockedByAnotherModalComponent())
    return false;
  for (const int code : { KeyPress::leftKey, KeyPress::rightKey, KeyPress::upKey, KeyPress::downKey,
                          KeyPress::pageUpKey, KeyPress::pageDownKey, KeyPress::homeKey, KeyPress::endKey }) {
    if (key.getKeyCode() == code)
      return true;
  }
  return false;
}

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
      if (isArrowKeyFromAnOpenMenu(*this, key))
        return true;
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
      const double previous = getValue();
      setValue(jlimit(getMinimum(), getMaximum(), value), sendNotificationSync);
      // A changed value has already told the screen reader (Slider raises
      // valueChanged); raising it again made NVDA speak every value twice. At
      // either end of the range nothing changes, so say the value again here.
      if (getValue() == previous)
        if (auto* handler = getAccessibilityHandler())
          handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
};

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "JuceHeader.h"

class LfoMsegKeyboardComponent : public Component {
  public:
    std::function<bool(const KeyPress&)> onKeyPressed;
    std::function<String()> getStatusText;

    LfoMsegKeyboardComponent() {
      setWantsKeyboardFocus(true);
      setTitle("LFO Editor");
      setDescription("Keyboard LFO shape editor with time, value, and curve announcements.");
      setHelpText("In LFO mode, Left and Right move in time, Comma and Period select points, Up and Down change a point, and I and K change its curve. In Performer mode, Comma and Period select complete shapes, Up and Down move the whole shape vertically, I and K change every curve in that shape, A inserts the selected shape, and Delete removes it. In Step Sequencer mode, Left and Right select steps, Up and Down set the step value, Delete clears it, and curve commands are ignored. Shift 1 through Shift 0 switch LFOs. Backspace clears the complete LFO.");
    }

    bool keyPressed(const KeyPress& key) override {
      if (onKeyPressed)
        return onKeyPressed(key);
      return Component::keyPressed(key);
    }

    void paint(Graphics& g) override {
      g.fillAll(Colour(0xff20242b));
      g.setColour(Colour(0xffd9dee2));
      g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
      g.setColour(Colours::white);
      g.drawText(getStatusText ? getStatusText() : String("LFO keyboard editor"),
                 getLocalBounds().reduced(12), Justification::centredLeft, true);
    }

    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override {
      class Handler final : public AccessibilityHandler {
        public:
          explicit Handler(LfoMsegKeyboardComponent& component) :
              AccessibilityHandler(component, AccessibilityRole::group), component_(component) { }

          AccessibleState getCurrentState() const override {
            return AccessibilityHandler::getCurrentState().withAccessibleOffscreen();
          }

          String getDescription() const override {
            return component_.getStatusText ? component_.getStatusText() : String();
          }

        private:
          LfoMsegKeyboardComponent& component_;
      };
      return std::make_unique<Handler>(*this);
    }
};


// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../src/plugin/synth_editor.h"

// The editor lists every control group and section at once. Tests reach one
// the way a keyboard user does, by focusing its heading.
namespace editor_navigation {
  inline Component* findHeading(Component& parent, const String& title, const String& kind) {
    if (parent.getTitle() == title && parent.getHelpText() == kind)
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findHeading(*child, title, kind))
        return found;
    }
    return nullptr;
  }

  // A component under `parent` that belongs to the section titled `title`.
  // Bus sections are named "Bus 1 - <title>".
  inline Component* findSectionMember(Component& parent, const String& title) {
    const String section = parent.getProperties()["SectionName"].toString();
    if (section == title || section.endsWith(" - " + title))
      return &parent;
    for (auto* child : parent.getChildren()) {
      if (auto* found = findSectionMember(*child, title))
        return found;
    }
    return nullptr;
  }

  // Kept by name: an editor rebuild replaces the heading components.
  inline String& currentGroup() {
    static String group;
    return group;
  }

  // "Editor group" focuses a group heading. "Editor element" focuses a section
  // heading inside the group chosen last. A group with a single section lists
  // that section's controls directly under the group heading, which is then
  // focused instead. Returns false when there is no such group or section.
  inline bool focusHeading(Component& editor, const String& level, const String& title) {
    Component* target = nullptr;
    if (level == "Editor group") {
      currentGroup() = title;
      target = findHeading(editor, title, "Group heading");
    }
    else if (level == "Editor element") {
      if (auto* group = findHeading(editor, currentGroup(), "Group heading")) {
        target = findHeading(*group, title, "Section heading");
        if (target == nullptr && findSectionMember(*group, title) != nullptr)
          target = group;
      }
    }
    if (target == nullptr)
      return false;

    if (auto* synth_editor = dynamic_cast<SynthEditor*>(&editor))
      synth_editor->ensureComponentVisible(target);
    target->grabKeyboardFocus();
    return true;
  }
}

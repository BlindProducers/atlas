#import <AppKit/AppKit.h>
#include "JuceHeader.h"
#include <dlfcn.h>

namespace {
  thread_local id observedFocusElement = nil;
  thread_local int observedFocusNotifications = 0;
}

// Test-only replacements: simulate an active VoiceOver client, and model its
// response to a layout/focus notification by asking for focus again. Nothing is
// posted to the real screen reader while this bounded probe is active.
extern "C" CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef key, CFStringRef application) {
  if (observedFocusElement != nil && CFEqual(key, CFSTR("voiceOverOnOffKey")) &&
      CFEqual(application, CFSTR("com.apple.universalaccess")))
    return CFRetain(kCFBooleanTrue);
  const auto original = reinterpret_cast<decltype(&CFPreferencesCopyAppValue)>(
      dlsym(RTLD_NEXT, "CFPreferencesCopyAppValue"));
  return original(key, application);
}

extern "C" void NSAccessibilityPostNotificationWithUserInfo(
    id element, NSAccessibilityNotificationName notification,
    NSDictionary<NSAccessibilityNotificationUserInfoKey, id>* userInfo) {
  if (observedFocusElement != nil) {
    if (element == observedFocusElement &&
        ([notification isEqualToString:NSAccessibilityLayoutChangedNotification] ||
         [notification isEqualToString:NSAccessibilityFocusedUIElementChangedNotification])) {
      if (++observedFocusNotifications < 16)
        [element setAccessibilityFocused:YES];
    }
    return;
  }
  const auto original = reinterpret_cast<decltype(&NSAccessibilityPostNotificationWithUserInfo)>(
      dlsym(RTLD_NEXT, "NSAccessibilityPostNotificationWithUserInfo"));
  original(element, notification, userInfo);
}

bool nativeAccessibilityFocusSettles(juce::AccessibilityHandler& handler) {
  @autoreleasepool {
    const juce::ScopedValueSetter<id> observing(observedFocusElement,
        reinterpret_cast<id>(handler.getNativeImplementation()));
    observedFocusNotifications = 0;
    for (int request = 0; request < 8; ++request)
      [observedFocusElement setAccessibilityFocused:YES];
    // A genuine focus transition may emit one focus event, then must settle.
    return observedFocusNotifications <= 1 && handler.hasFocus(false);
  }
}

// VoiceOver moves accessibility focus without using Tab traversal.
void focusNativeAccessibilityElement(juce::AccessibilityHandler& handler) {
  @autoreleasepool {
    id element = reinterpret_cast<id>(handler.getNativeImplementation());
    [element setAccessibilityFocused:YES];
  }
}

juce::StringArray nativeAccessibilityChildTitles(juce::AccessibilityHandler& handler) {
  @autoreleasepool {
    juce::StringArray result;
    id element = reinterpret_cast<id>(handler.getNativeImplementation());
    for (id child in [element accessibilityChildren])
      result.add(juce::String::fromUTF8([[child accessibilityTitle] UTF8String]));
    return result;
  }
}

// Check the actual native properties consumed by VoiceOver. A C++ value
// interface alone did not catch an indicator whose spoken name omitted its load.
bool cpuMeterNativeAccessibilityMatches(juce::AccessibilityHandler& handler) {
  @autoreleasepool {
    id element = reinterpret_cast<id>(handler.getNativeImplementation());
    auto* value = handler.getValueInterface();
    if (element == nil || value == nullptr)
      return false;
    const auto string = [](NSString* text) { return juce::String::fromUTF8([text UTF8String]); };
    const auto title = string([element accessibilityTitle]);
    const auto expected = juce::String(value->getCurrentValue(), 1) + " percent";
    return [[element accessibilityRole] isEqualToString:NSAccessibilityProgressIndicatorRole]
        && title == "CPU Usage, " + expected
        && string([element accessibilityLabel]).isEmpty()
        && string([element accessibilityValue]) == value->getCurrentValueAsString()
        && ![element isAccessibilitySelectorAllowed:@selector(setAccessibilityValue:)]
        && ![element isAccessibilitySelectorAllowed:@selector(accessibilityPerformIncrement)]
        && ![element isAccessibilitySelectorAllowed:@selector(accessibilityPerformDecrement)];
  }
}

#pragma once

// A window that has gone idle still paints whatever it is asked to, at once.
// JUCE woke the message thread on every display refresh while a window was
// open; third_party/patches/juce8-*-idle-vblank.patch stop that while nothing
// waits to be drawn (Windows and macOS), so a repaint has to start the vblanks
// again. Included into macro_modulation_tests.cpp, whose expect it uses.

namespace {
  class PaintCountingWindow : public Component {
    public:
      int paints = 0;

      void paint(Graphics& g) override {
        ++paints;
        g.fillAll(Colours::darkgrey);
      }
  };

  // Runs the message loop until `done` holds or `ms` pass; whether it held.
  template <typename Done>
  bool pumpMessagesUntil(Done done, double ms) {
    const double end = Time::getMillisecondCounterHiRes() + ms;
    while (!done()) {
      if (Time::getMillisecondCounterHiRes() > end)
        return false;
      MessageManager::getInstance()->runDispatchLoopUntil(2);
    }
    return true;
  }

  void pumpMessagesFor(double ms) {
    pumpMessagesUntil([] { return false; }, ms);
  }

  // `engine` picks the peer's renderer (on Windows 0 is software and 1
  // Direct2D, whose paints wait for a vblank in different ways); -1 keeps the default.
  void testIdleWindowPaintsEveryRepaint(int engine = -1) {
    const std::string renderer = engine < 0 ? std::string() : " (renderer " + std::to_string(engine) + ")";
    PaintCountingWindow window;
    window.setSize(320, 200);
    window.addToDesktop(ComponentPeer::windowHasTitleBar);
    if (auto* peer = window.getPeer(); peer != nullptr && engine >= 0) {
      if (engine >= peer->getAvailableRenderingEngines().size())
        return;
      peer->setCurrentRenderingEngine(engine);
    }
    window.setVisible(true);
    expect(pumpMessagesUntil([&] { return window.paints > 0; }, 3000.0), "a new window paints" + renderer);
    // Long enough for every pending paint to be done: the window is idle.
    pumpMessagesFor(500.0);

    double slowest_ms = 0.0;
    for (int i = 0; i < 24; ++i) {
      // Some repaints follow the last at once, the rest come after the window
      // has been idle for longer than a few display refreshes.
      if (i % 3 != 0)
        pumpMessagesFor(i % 3 == 1 ? 60.0 : 250.0);
      const int before = window.paints;
      const double start = Time::getMillisecondCounterHiRes();
      window.repaint((i * 37) % 280, (i * 23) % 160, 40, 40);
      const bool painted = pumpMessagesUntil([&] { return window.paints > before; }, 1000.0);
      expect(painted, "repaint " + std::to_string(i + 1) + " of an idle window is painted" + renderer);
      if (painted)
        slowest_ms = std::max(slowest_ms, Time::getMillisecondCounterHiRes() - start);
    }
    // A display refresh or two, not the second-long wait of a missed one.
    expect(slowest_ms < 500.0, "every repaint is painted within a few display refreshes (slowest " +
                                   std::to_string(slowest_ms) + " ms)" + renderer);
    std::cout << "slowest repaint of an idle window" << renderer << " ms=" << slowest_ms << '\n';

    // Repaints from a timer, as the editor's readouts refresh.
    struct Repainter : Timer {
      Component& target;
      int ticks = 0;
      explicit Repainter(Component& c) : target(c) { }
      void timerCallback() override {
        ++ticks;
        target.repaint();
      }
    } repainter(window);
    const int before_timer = window.paints;
    repainter.startTimerHz(20);
    pumpMessagesFor(1000.0);
    repainter.stopTimer();
    expect(window.paints - before_timer >= repainter.ticks / 2,
           "a 20 Hz timer's repaints are painted (" + std::to_string(window.paints - before_timer) + " paints for " +
               std::to_string(repainter.ticks) + " repaints)" + renderer);

    window.removeFromDesktop();
  }

  // Timing only: what the process's first window costs before Atlas draws
  // anything (the platform's graphics set up for the first time), and a second.
  void firstWindowTiming() {
    for (int pass = 1; pass <= 2; ++pass) {
      PaintCountingWindow window;
      window.setSize(320, 200);
      const double start = Time::getMillisecondCounterHiRes();
      window.addToDesktop(ComponentPeer::windowHasTitleBar);
      const double added = Time::getMillisecondCounterHiRes();
      window.setVisible(true);
      pumpMessagesUntil([&] { return window.paints > 0; }, 5000.0);
      const double painted = Time::getMillisecondCounterHiRes();
      std::cout << "window " << pass << ": peer created ms=" << added - start << " shown and painted ms=" << painted - added
                << '\n';
      window.removeFromDesktop();
    }
  }

  int runWindowRepaintTests() {
    testIdleWindowPaintsEveryRepaint();
#if JUCE_WINDOWS
    // Both of JUCE's Windows renderers: software and Direct2D.
    testIdleWindowPaintsEveryRepaint(0);
    testIdleWindowPaintsEveryRepaint(1);
#endif
    if (failures == 0)
      std::cout << "Atlas window repaint tests passed\n";
    return failures == 0 ? 0 : 1;
  }
}

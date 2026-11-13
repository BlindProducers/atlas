// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace vital {
  // One channel of the Band Reject filter model: the Bandreject type of
  // Alessio Scala's filter JSFX, which recreates Serum 2's from measurements
  // of the plugin. A Chamberlin state variable lowpass below the cutoff and a
  // highpass above it run in parallel, in double precision as the script
  // computes. Each section soft clips the driven input, runs two half steps
  // per sample on it and plays the last three half steps weighted 1/4, 1/2,
  // 1/4. Where the two half steps would be unstable, near Nyquist, a section
  // runs once per sample with the measured coefficients and output gains.
  //
  // The resonance follows Atlas's filters, not the script: the damping is the
  // Digital model's 1/Q with Q from 0.5 to 16 on a cubic curve, so 100% is a
  // sharp notch rather than the script's zero damping, where the sections
  // self-oscillate. At 0% both give the same damping of 2.
  class BandRejectCore {
    public:
      // The script's cutoff knob: 11.4 octaves from 8.1758 Hz, which is MIDI
      // note 0, so a MIDI note over 136.8 is the knob's position.
      static constexpr double kMinCutoff = 8.1758;
      static constexpr double kOctaves = 11.4;
      static constexpr double kSemitones = 136.8;

      // Forgets the signal; the coefficients stay.
      void reset();
      // Zeroes any state too small to be normal, so a silent channel costs
      // nothing on x64, where arithmetic on such values is slow. The script
      // zeroes every such assignment as it makes it, which the output never
      // shows.
      void flushDenormals();
      // Sets both sections' coefficients for a cutoff (a MIDI note), the
      // width (the script's Var, in percent: the sections sit 0.64 + 0.6336
      // Var semitones below and above the cutoff) and the rate the filter
      // runs at. Only needed when one of them changes.
      void setCutoff(double midi, double width_percent, double sample_rate);
      // One sample: the input times the drive gain, with the sections' damping
      // (2 without resonance, down to 1/16).
      double tick(double input, double drive, double damping);

      // Atlas's resonance law: the damping for a resonance from 0 to 1.
      static double damping(double resonance);
      // 2^x, sin x and cos x from IEEE adds and multiplies alone (Taylor
      // series, within a few units of the last place), so arm64 and x64 set
      // the same coefficients, which the C library does not promise. sin and
      // cos take 0 <= x <= 0.8, the range of pi fc / (2 fs) here.
      static double exp2(double x);
      static double sin(double x);
      static double cos(double x);

    private:
      struct Section {
        double low = 0.0, band = 0.0, previous = 0.0;
        // The half steps' coefficient 2 sin(pi fc / (2 fs)).
        double f = 0.0;
        // Whether the once-per-sample regime can be reached at any damping,
        // and its coefficient, damping and output gain.
        bool once = false;
        double f1 = 0.0, k1 = 0.0, gain1 = 0.0;
      };
      static void setSection(Section& section, double knob, double sample_rate, bool highpass);
      template<bool highpass>
      static double run(Section& section, double input, double damping);

      Section low_, high_;
  };
} // namespace vital

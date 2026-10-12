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

#pragma once

#include "processor.h"
#include "circular_queue.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace vital {

  class Feedback;

  // Fixed-capacity open-addressed membership set used while walking the DSP
  // graph. Unlike std::unordered_set it never allocates when a modulation
  // route is connected from the audio callback.
  class FixedProcessorSet {
    public:
      static constexpr size_t kCapacity = 1021;

      FixedProcessorSet() { clear(); }

      void clear() { entries_.fill(nullptr); overflowed_ = false; }
      bool overflowed() const { return overflowed_; }

      bool insert(const Processor* processor) { return tryInsert(processor) > 0; }

      // 1: inserted, 0: already present (or null), -1: table full. Callers must
      // not treat "full" as "present", or large graphs lose dependencies.
      int tryInsert(const Processor* processor) {
        if (processor == nullptr)
          return 0;

        size_t index = hash(processor);
        size_t first_tombstone = kCapacity;
        for (size_t probe = 0; probe < kCapacity; ++probe) {
          const Processor* entry = entries_[index];
          if (entry == processor)
            return 0;
          if (entry == tombstone()) {
            if (first_tombstone == kCapacity)
              first_tombstone = index;
          }
          else if (entry == nullptr) {
            entries_[first_tombstone == kCapacity ? index : first_tombstone] = processor;
            return 1;
          }
          index = (index + 1) % kCapacity;
        }

        if (first_tombstone != kCapacity) {
          entries_[first_tombstone] = processor;
          return 1;
        }
        overflowed_ = true;
        return -1;
      }

      bool count(const Processor* processor) const {
        if (processor == nullptr)
          return false;

        size_t index = hash(processor);
        for (size_t probe = 0; probe < kCapacity; ++probe) {
          const Processor* entry = entries_[index];
          if (entry == processor)
            return true;
          if (entry == nullptr)
            return false;
          index = (index + 1) % kCapacity;
        }
        return false;
      }

      void erase(const Processor* processor) {
        size_t index = hash(processor);
        for (size_t probe = 0; probe < kCapacity; ++probe) {
          const Processor* entry = entries_[index];
          if (entry == processor) {
            entries_[index] = tombstone();
            return;
          }
          if (entry == nullptr)
            return;
          index = (index + 1) % kCapacity;
        }
      }

    private:
      static const Processor* tombstone() {
        return reinterpret_cast<const Processor*>(static_cast<uintptr_t>(1));
      }

      static size_t hash(const Processor* processor) {
        return (reinterpret_cast<uintptr_t>(processor) >> 4) % kCapacity;
      }

      std::array<const Processor*, kCapacity> entries_;
      bool overflowed_ = false;
  };

  class ProcessorRouter : public Processor {
    public:
      ProcessorRouter(int num_inputs = 0, int num_outputs = 0, bool control_rate = false);
      ProcessorRouter(const ProcessorRouter& original);

      virtual ~ProcessorRouter();

      virtual Processor* clone() const override {
        return new ProcessorRouter(*this);
      }

      virtual void process(int num_samples) override;
      virtual void init() override;
      virtual void setSampleRate(int sample_rate) override;
      virtual void setOversampleAmount(int oversample) override;

      virtual void addProcessor(Processor* processor);
      virtual void addProcessorRealTime(Processor* processor);
      virtual void addIdleProcessor(Processor* processor);
      virtual void removeIdleProcessor(Processor* processor);
      virtual void removeProcessor(Processor* processor);

      // Any time new dependencies are added into the ProcessorRouter graph, we
      // should call _connect_ on the destination Processor and source Output.
      void connect(Processor* destination, const Output* source, int index);
      void disconnect(const Processor* destination, const Output* source);
      bool isDownstream(const Processor* first, const Processor* second) const;
      bool areOrdered(const Processor* first, const Processor* second) const;

      virtual bool isPolyphonic(const Processor* processor) const;

      virtual ProcessorRouter* getMonoRouter();
      virtual ProcessorRouter* getPolyRouter();
      virtual void resetFeedbacks(poly_mask reset_mask);

      // This router's copy of a processor it holds, or null. Unlike
      // getLocalProcessor, a processor it doesn't hold adds no entry.
      Processor* findLocalProcessor(const Processor* global_processor) const {
        const auto found = processors_.find(global_processor);
        return found == processors_.end() ? nullptr : found->second.second.get();
      }

      // Uses _other_'s dependency scratch instead of this router's own (about
      // 20 KB). Graph walks clear the scratch first and finish with it before
      // calling into a parent router, so routers may share one as long as
      // their graphs are never changed from two threads at once.
      void shareDependencyScratch(const ProcessorRouter& other);

    protected:
      // When we create a cycle into the ProcessorRouter graph, we must insert
      // a Feedback node and add it here.
      virtual void addFeedback(Feedback* feedback);
      virtual void removeFeedback(Feedback* feedback);

      // Makes sure _processor_ runs in a topologically sorted order in
      // relation to all other Processors in _this_.
      void reorder(Processor* processor);

      // Ensures our local copies of all processors and feedback processors match the master order.
      virtual void updateAllProcessors();

      force_inline bool shouldUpdate() { return local_changes_ != *global_changes_; }

      // Will create local copies of added processors. 
      virtual void createAddedProcessors();

      // Will delete local copies of removed processors. 
      virtual void deleteRemovedProcessors();

      // Returns the ancestor of _processor_ which is a child of _this_.
      // Returns null if _processor_ is not a descendant of _this_.
      const Processor* getContext(const Processor* processor) const;
      void getDependencies(const Processor* processor) const;

      // Returns the processor for this voice from the globally created one.
      Processor* getLocalProcessor(const Processor* global_processor);

      std::shared_ptr<CircularQueue<Processor*>> global_order_;
      std::shared_ptr<CircularQueue<Processor*>> global_reorder_;
      CircularQueue<Processor*> local_order_;
      std::map<const Processor*, std::pair<int, std::unique_ptr<Processor>>> processors_;
      std::map<const Processor*, std::unique_ptr<Processor>> idle_processors_;

      std::shared_ptr<std::vector<const Feedback*>> global_feedback_order_;
      std::vector<Feedback*> local_feedback_order_;
      std::map<const Processor*, std::pair<int, std::unique_ptr<Feedback>>> feedback_processors_;

      std::shared_ptr<int> global_changes_;
      int local_changes_;

      std::shared_ptr<CircularQueue<const Processor*>> dependencies_;
      std::shared_ptr<CircularQueue<const Processor*>> dependencies_visited_;
      std::shared_ptr<CircularQueue<const Processor*>> dependency_inputs_;
      // Hash mirrors of the queues above: membership tests during dependency
      // walks are hot when constructing large graphs, and the queues only
      // offer linear scans.
      std::shared_ptr<FixedProcessorSet> dependencies_set_;
      bool isDependency(const Processor* processor) const {
        return dependencies_set_->count(processor) ||
               (dependencies_set_->overflowed() && dependencies_->contains(processor));
      }
      std::shared_ptr<FixedProcessorSet> dependencies_visited_set_;

      JUCE_LEAK_DETECTOR(ProcessorRouter)
  };
} // namespace vital

#pragma once
// Monophonic note stack (M4): the keys currently held, in the order they were
// pressed. The flute is strictly monophonic with last-note priority: the
// sounding note is always the most recently pressed key that is still held,
// and releasing it falls back to the previous held key (see PLAN "Polyphony").
// Fixed capacity, no allocation: real-time safe.

#include <array>
#include <cstdint>

namespace mf {

class NoteStack {
public:
    static constexpr int kCapacity = 16;

    void clear() { size_ = 0; }
    bool empty() const { return size_ == 0; }
    int size() const { return size_; }
    // The sounding (most recent) note, or -1 when no key is held.
    int top() const { return size_ > 0 ? notes_[static_cast<std::size_t>(size_ - 1)].note : -1; }
    float topVelocity() const { return size_ > 0 ? notes_[static_cast<std::size_t>(size_ - 1)].velocity : 0.0f; }
    bool contains(int note) const { return find(note) >= 0; }

    // Pushes a key. A key that is already held moves to the top (a repeated
    // note-on without note-off). When full, the oldest key is dropped.
    void push(int note, float velocity) {
        remove(note);
        if (size_ == kCapacity) {
            for (int i = 1; i < size_; ++i) notes_[static_cast<std::size_t>(i - 1)] = notes_[static_cast<std::size_t>(i)];
            --size_;
        }
        notes_[static_cast<std::size_t>(size_++)] = {note, velocity};
    }

    // Removes a key; returns true when it was held.
    bool remove(int note) {
        const int i = find(note);
        if (i < 0) return false;
        for (int j = i + 1; j < size_; ++j) notes_[static_cast<std::size_t>(j - 1)] = notes_[static_cast<std::size_t>(j)];
        --size_;
        return true;
    }

private:
    struct Entry {
        int note = 0;
        float velocity = 0.0f;
    };
    int find(int note) const {
        for (int i = size_ - 1; i >= 0; --i)
            if (notes_[static_cast<std::size_t>(i)].note == note) return i;
        return -1;
    }
    std::array<Entry, kCapacity> notes_{};
    int size_ = 0;
};

} // namespace mf

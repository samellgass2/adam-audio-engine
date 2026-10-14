// Pattern sequencer shared by every host (offline renderer, WASM workshop, and
// later the AU's audition mode), so loop-boundary behaviour is tested natively
// and identical in the browser.
//
// Pattern text format ("adam-pattern 1"), one statement per line, '#' comments:
//   name   Soft sustained chords
//   bpm    72
//   length 16                  # loop length in beats
//   note   <start> <dur> <midi> <vel>   # beats, beats, 0-127, 1-127
// Parsing writes into fixed storage (no allocation while rendering).
#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace adam::seq {

struct Event {
    double beat;
    uint8_t note;
    uint8_t velocity;  // 0 = note off
};

class Sequencer {
public:
    static constexpr int kMaxEvents = 4096;

    // Returns the number of notes parsed; 0 on failure (pattern unchanged).
    int load(const char* text) {
        if (!text) return 0;
        int count = 0;
        double length = 0.0, bpm = 120.0, maxEnd = 0.0;
        auto& tmp = scratch_;  // member storage: 64 KB would overflow a WASM stack
        const char* p = text;
        while (*p) {
            const char* eol = std::strchr(p, '\n');
            const size_t len = eol ? static_cast<size_t>(eol - p) : std::strlen(p);
            char line[256];
            const size_t n = std::min(len, sizeof line - 1);
            std::memcpy(line, p, n);
            line[n] = '\0';
            if (char* hash = std::strchr(line, '#')) *hash = '\0';
            char* s = line;
            while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
            if (std::strncmp(s, "bpm", 3) == 0) {
                bpm = std::strtod(s + 3, nullptr);
            } else if (std::strncmp(s, "length", 6) == 0) {
                length = std::strtod(s + 6, nullptr);
            } else if (std::strncmp(s, "note", 4) == 0 && count + 2 <= kMaxEvents) {
                char* q = s + 4;
                const double start = std::strtod(q, &q);
                const double dur = std::strtod(q, &q);
                const long midi = std::strtol(q, &q, 10);
                const long vel = std::strtol(q, &q, 10);
                if (start >= 0 && dur > 0 && midi >= 0 && midi <= 127 && vel >= 1 && vel <= 127) {
                    tmp[count++] = {start, static_cast<uint8_t>(midi), static_cast<uint8_t>(vel)};
                    tmp[count++] = {start + dur, static_cast<uint8_t>(midi), 0};
                    maxEnd = std::max(maxEnd, start + dur);
                }
            }
            if (!eol) break;
            p = eol + 1;
        }
        if (count == 0) return 0;
        if (length <= 0.0) length = maxEnd;
        // clip note-offs to the loop so nothing hangs across the boundary
        for (int i = 0; i < count; ++i)
            if (tmp[i].velocity == 0 && tmp[i].beat > length) tmp[i].beat = length;
        // offs before ons at equal times: a re-struck note releases first
        std::sort(tmp.begin(), tmp.begin() + count, [](const Event& a, const Event& b) {
            if (a.beat != b.beat) return a.beat < b.beat;
            return a.velocity < b.velocity;
        });
        std::copy(tmp.begin(), tmp.begin() + count, events_.begin());
        count_ = count;
        length_ = length;
        patternBpm_ = bpm > 0 ? bpm : 120.0;
        rewind();
        return count / 2;
    }

    void rewind() {
        beat_ = 0.0;
        cur_ = 0;
        next_ = 0;
    }

    // Jump to a musical position (beats), e.g. to start a new layer in phase
    // with a loop that is already running.
    void seek(double beat) {
        beat_ = length_ > 0 ? std::fmod(std::max(0.0, beat), length_) : 0.0;
        spb_ = 0.0;  // the next advance recomputes the sample position
        next_ = 0;
        while (next_ < count_ && events_[next_].beat < beat_) ++next_;
    }

    void setBpm(double bpm) { bpm_ = bpm > 1 ? bpm : 1; }
    double bpm() const { return bpm_; }
    double patternBpm() const { return patternBpm_; }
    void setLoop(bool on) { loop_ = on; }
    void setPlaying(bool on) { playing_ = on; }
    bool playing() const { return playing_; }
    double beat() const { return beat_; }
    double lengthBeats() const { return length_; }
    int eventCount() const { return count_; }

    // Advance `frames` samples. For each due event, calls
    // onEvent(sampleOffset, note, velocity) in time order (velocity 0 = off).
    // Time is an integer sample clock: each event's sample is rounded once
    // from its beat, so nothing drifts across loops. Returns false once a
    // non-looping pattern has finished.
    template <typename F>
    bool advance(int frames, double sampleRate, F&& onEvent) {
        if (!playing_ || count_ == 0) return playing_;
        const double spb = 60.0 * sampleRate / bpm_;  // samples per beat
        if (spb != spb_) {
            // tempo (or rate) changed: keep the musical position
            cur_ = static_cast<int64_t>(std::llround(beat_ * spb));
            spb_ = spb;
        }
        const int64_t loopLen = std::max<int64_t>(1, std::llround(length_ * spb));
        int done = 0;
        while (done < frames) {
            const int64_t blockEnd = cur_ + (frames - done);
            if (next_ < count_) {
                const int64_t t = std::llround(events_[next_].beat * spb);
                if (t < blockEnd && t <= loopLen) {
                    const Event& e = events_[next_++];
                    const int off = done + static_cast<int>(std::max<int64_t>(0, t - cur_));
                    done = off;
                    cur_ = std::max(cur_, t);
                    onEvent(off, e.note, e.velocity);
                    continue;
                }
            }
            if (blockEnd > loopLen || (next_ >= count_ && cur_ >= loopLen)) {
                // loop end inside this block: flush, then wrap or finish
                const int off = done + static_cast<int>(std::max<int64_t>(0, loopLen - cur_));
                while (next_ < count_) {
                    const Event& e = events_[next_++];
                    onEvent(std::min(off, frames - 1), e.note, e.velocity);
                }
                if (!loop_) {
                    playing_ = false;
                    cur_ = loopLen;
                    beat_ = length_;
                    return false;
                }
                done = off;
                cur_ = 0;
                next_ = 0;
                continue;
            }
            cur_ = blockEnd;
            done = frames;
        }
        beat_ = static_cast<double>(cur_) / spb;
        return true;
    }

private:
    std::array<Event, kMaxEvents> events_{};
    std::array<Event, kMaxEvents> scratch_{};
    int count_ = 0;
    int next_ = 0;
    double length_ = 0.0;
    double beat_ = 0.0;
    int64_t cur_ = 0;     // samples since loop start
    double spb_ = 0.0;    // samples per beat at the last advance
    double bpm_ = 120.0;
    double patternBpm_ = 120.0;
    bool loop_ = true;
    bool playing_ = false;
};

}  // namespace adam::seq

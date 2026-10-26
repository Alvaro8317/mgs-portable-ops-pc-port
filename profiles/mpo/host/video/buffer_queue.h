// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from PPSSPP Core/HW/BufferQueue.h @ v1.20.4.
// Copyright (c) 2012- PPSSPP Project (GPL-2.0-or-later).
//
// Fixed-size byte ring with presentation timestamps attached to the byte
// position where each pushed chunk starts. Changes from upstream: storage is
// a std::vector, no savestate code, the consistency assert is gone, and
// clear() also drops the timestamp marks (upstream keeps stale ones).

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace mpo::video {

class BufferQueue {
public:
    explicit BufferQueue(int size = 0x20000) { alloc(size); }

    void alloc(int size) {
        bufQueue_.assign(static_cast<size_t>(size > 0 ? size : 1), 0);
        bufQueueSize_ = static_cast<int>(bufQueue_.size());
        clear();
    }

    void clear() {
        start_ = 0;
        end_ = 0;
        filled_ = 0;
        ptsMarks_.clear();
    }

    int getQueueSize() const { return filled_; }
    int getRemainSize() const { return bufQueueSize_ - filled_; }

    bool push(const uint8_t *buf, int addsize, int64_t pts = 0) {
        const int space = getRemainSize();
        if (space < addsize || addsize < 0) {
            return false;
        }
        savePts(pts);
        if (end_ + addsize <= bufQueueSize_) {
            std::memcpy(bufQueue_.data() + end_, buf, static_cast<size_t>(addsize));
            end_ += addsize;
            if (end_ == bufQueueSize_) {
                end_ = 0;
            }
        } else {
            const int firstSize = bufQueueSize_ - end_;
            std::memcpy(bufQueue_.data() + end_, buf, static_cast<size_t>(firstSize));
            std::memcpy(bufQueue_.data(), buf + firstSize,
                        static_cast<size_t>(addsize - firstSize));
            end_ = addsize - firstSize;
        }
        filled_ += addsize;
        return true;
    }

    // Removes up to `wantedsize` bytes (copies them to `buf` if not null).
    int pop_front(uint8_t *buf, int wantedsize, int64_t *pts = nullptr) {
        if (wantedsize <= 0) {
            return 0;
        }
        int bytesgot = getQueueSize();
        if (wantedsize < bytesgot) {
            bytesgot = wantedsize;
        }
        if (pts != nullptr) {
            *pts = findPts(bytesgot);
        }
        const int firstSize = bufQueueSize_ - start_;
        if (buf) {
            if (bytesgot <= firstSize) {
                std::memcpy(buf, bufQueue_.data() + start_, static_cast<size_t>(bytesgot));
            } else {
                std::memcpy(buf, bufQueue_.data() + start_, static_cast<size_t>(firstSize));
                std::memcpy(buf + firstSize, bufQueue_.data(),
                            static_cast<size_t>(bytesgot - firstSize));
            }
        }
        if (bytesgot <= firstSize) {
            start_ += bytesgot;
        } else {
            start_ = bytesgot - firstSize;
        }
        if (start_ == bufQueueSize_) {
            start_ = 0;
        }
        filled_ -= bytesgot;
        return bytesgot;
    }

    // Copies up to `wantedsize` bytes from the front without removing them.
    int get_front(uint8_t *buf, int wantedsize) const {
        if (wantedsize <= 0) {
            return 0;
        }
        int bytesgot = getQueueSize();
        if (wantedsize < bytesgot) {
            bytesgot = wantedsize;
        }
        const int firstSize = bufQueueSize_ - start_;
        if (bytesgot <= firstSize) {
            std::memcpy(buf, bufQueue_.data() + start_, static_cast<size_t>(bytesgot));
        } else {
            std::memcpy(buf, bufQueue_.data() + start_, static_cast<size_t>(firstSize));
            std::memcpy(buf + firstSize, bufQueue_.data(),
                        static_cast<size_t>(bytesgot - firstSize));
        }
        return bytesgot;
    }

private:
    void savePts(int64_t pts) {
        if (pts != 0) {
            ptsMarks_[static_cast<uint32_t>(end_)] = pts;
        }
    }

    int64_t findPts(std::map<uint32_t, int64_t>::iterator earliest,
                    std::map<uint32_t, int64_t>::iterator latest) {
        int64_t pts = 0;
        // Take the first one, that is the pts of this packet.
        if (earliest != latest) {
            pts = earliest->second;
        }
        ptsMarks_.erase(earliest, latest);
        return pts;
    }

    int64_t findPts(int packetSize) {
        auto earliest = ptsMarks_.lower_bound(static_cast<uint32_t>(start_));
        auto latest = ptsMarks_.lower_bound(static_cast<uint32_t>(start_ + packetSize));
        int64_t pts = findPts(earliest, latest);
        // If it wraps around, we have to look at the other half too.
        if (start_ + packetSize > bufQueueSize_) {
            earliest = ptsMarks_.begin();
            latest =
                ptsMarks_.lower_bound(static_cast<uint32_t>(start_ + packetSize - bufQueueSize_));
            // This also clears the range, so we always call on wrap.
            const int64_t latePts = findPts(earliest, latest);
            if (pts == 0) {
                pts = latePts;
            }
        }
        return pts;
    }

    std::vector<uint8_t> bufQueue_;
    // end_ may be less than start_: the space between end_ and start_ is free.
    int start_ = 0;
    int end_ = 0;
    int filled_ = 0;
    int bufQueueSize_ = 0;
    std::map<uint32_t, int64_t> ptsMarks_;
};

} // namespace mpo::video

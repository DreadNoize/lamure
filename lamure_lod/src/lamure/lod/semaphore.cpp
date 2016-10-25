// Copyright (c) 2014 Bauhaus-Universitaet Weimar
// This Software is distributed under the Modified BSD License, see license.txt.
//
// Virtual Reality and Visualization Research Group 
// Faculty of Media, Bauhaus-Universitaet Weimar
// http://www.uni-weimar.de/medien/vr

#include <lamure/lod/semaphore.h>

#include <iostream>

namespace lamure {
namespace lod {

semaphore::
semaphore()
: signal_count_(0),
  shutdown_(false),
  min_signal_count_(1),
  max_signal_count_(1) {


}

semaphore::
~semaphore() {

}

void semaphore::
wait() {
    {
        std::unique_lock<std::mutex> ulock(mutex_);
        signal_lock_.wait(ulock, [&]{ return signal_count_ >= min_signal_count_ || shutdown_; });
        if (signal_count_ >= min_signal_count_) {
            signal_count_ -= min_signal_count_;
        }
    }

}

void semaphore::
signal(const size_t signal_count) {
    {
        std::lock_guard<std::mutex> ulock(mutex_);
        if (signal_count_+signal_count <= max_signal_count_) {
            signal_count_ += signal_count;
        }
    }
    signal_lock_.notify_all();

}

const size_t semaphore::
num_signals() {
    std::lock_guard<std::mutex> lock(mutex_);
    return signal_count_;
}

void semaphore::
lock() {
    mutex_.lock();
}

void semaphore::
unlock() {
    mutex_.unlock();
}

void semaphore::
shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    signal_lock_.notify_all();
}


} // namespace lod

} // namespace lamure


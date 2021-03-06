/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <chrono>
#include "watchman_time.h"

#ifdef __APPLE__
// We provide our own implementation of these functions
extern "C" {
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* ts);
int pthread_rwlock_timedwrlock(
    pthread_rwlock_t* rwlock,
    const struct timespec* ts);
int pthread_rwlock_timedrdlock(
    pthread_rwlock_t* rwlock,
    const struct timespec* ts);
}
#endif

namespace watchman {

/* Implements the C++14 shared_timed_mutex API using pthread_rwlock_t.
 * This is present because C++11 is our target environment */
class shared_timed_mutex {
  pthread_rwlock_t rwlock_;

 public:
  shared_timed_mutex() {
    pthread_rwlock_init(&rwlock_, nullptr);
  }

  ~shared_timed_mutex() {
    pthread_rwlock_destroy(&rwlock_);
  }

  shared_timed_mutex(const shared_timed_mutex&) = delete;
  shared_timed_mutex& operator=(const shared_timed_mutex&) = delete;

  // Exclusive ownership
  void lock() {
    pthread_rwlock_wrlock(&rwlock_);
  }
  bool try_lock() {
    return pthread_rwlock_trywrlock(&rwlock_) == 0;
  }

  template <class Rep, class Period>
  bool try_lock_for(std::chrono::duration<Rep, Period>& timeout_duration) {
    struct timespec ts = systemClockToTimeSpec(
        std::chrono::system_clock::now() + timeout_duration);
    return pthread_rwlock_timedwrlock(&rwlock_, &ts) == 0;
  }

  void unlock() {
    pthread_rwlock_unlock(&rwlock_);
  }

  // Shared ownership
  void lock_shared() {
    pthread_rwlock_rdlock(&rwlock_);
  }
  bool try_lock_shared() {
    return pthread_rwlock_tryrdlock(&rwlock_) == 0;
  }
  template <class Rep, class Period>
  bool try_lock_shared_for(
      std::chrono::duration<Rep, Period>& timeout_duration) {
    struct timespec ts =
        systemClockToTimeSpec(std::chrono::system_clock::now() + timeout_duration);
    return pthread_rwlock_timedrdlock(&rwlock_, &ts) == 0;
  }
  void unlock_shared() {
    pthread_rwlock_unlock(&rwlock_);
  }
};

/* Implements the C++14 shared_lock API.
 * This is present because C++11 is our target environment */
template <class Mutex>
class shared_lock {
 public:
  typedef Mutex mutex_type;

  // Blocking
  explicit shared_lock(mutex_type& m) : m_(&m), owned_(false) {
    m_->lock_shared();
    owned_ = true;
  }

  void unlock() {
    if (owned_) {
      m_->unlock_shared();
      owned_ = false;
    }
  }

  ~shared_lock() {
    unlock();
  }

  shared_lock(shared_lock const&) = delete;
  shared_lock& operator=(shared_lock const&) = delete;

  void swap(shared_lock& u) noexcept {
    if (this == &u) {
      return;
    }
    std::swap(m_, u.m_);
    std::swap(owned_, u.owned_);
  }

  mutex_type* release() noexcept {
    mutex_type* result = nullptr;
    std::swap(result, m_);
    owned_ = false;
    return result;
  }

  shared_lock(shared_lock&& u) noexcept {
    if (this == &u) {
      return;
    }
    swap(u);
  }

  shared_lock& operator=(shared_lock&& u) noexcept {
    if (this == &u) {
      return *this;
    }
    swap(u);
    return *this;
  }

  bool owns_lock() const noexcept {
    return owned_;
  }

  explicit operator bool() const noexcept {
    return owns_lock();
  }

  mutex_type* mutex() const noexcept {
    return m_;
  }

 private:
  mutex_type* m_;
  bool owned_{false};
};

} // namespace watchman

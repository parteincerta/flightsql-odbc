/*
 * Copyright (C) 2020-2022 Dremio Corporation
 *
 * See "LICENSE" for license information.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <chrono>
#include <boost/optional.hpp>


#include <os/log.h>

namespace driver {
namespace odbcabstraction {


template<typename T>
class BlockingQueue {

  size_t capacity_;
  std::deque<T> buffer_;
  std::chrono::seconds timeout_ = std::chrono::seconds(10);
  std::chrono::time_point<std::chrono::system_clock> last_push_time_;

  std::mutex mtx_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::condition_variable not_closed_;

  std::atomic<bool> closed_{false};
  bool long_time_no_pushes_{false};
  std::vector<std::thread> threads_;
  std::thread notifier_thread_;
  std::atomic<size_t> active_threads_{0};

public:
  typedef std::function<boost::optional<T>(void)> Supplier;

  BlockingQueue(size_t capacity): capacity_(capacity), last_push_time_(std::chrono::system_clock::now()) {
      notifier_thread_ = std::thread([this]() {
        while (!closed_) {
          std::unique_lock<std::mutex> unique_lock(mtx_);
          not_closed_.wait_until(unique_lock, last_push_time_ + timeout_, [this](){ return closed_.load(); });
          os_log(OS_LOG_DEFAULT, "flightsql: BQ::notifier_thread_waked");
          if (last_push_time_ + timeout_ < std::chrono::system_clock::now()) {
            long_time_no_pushes_ = true;
            not_full_.notify_one();
            last_push_time_ = std::chrono::system_clock::now();
            os_log(OS_LOG_DEFAULT, "flightsql: BQ::notifier_thread_notified");
          }
        }
      });
    }

  void AddProducer(Supplier supplier) {
    os_log(OS_LOG_DEFAULT, "flightsql: BQ::AddProducer");
    active_threads_++;
    threads_.emplace_back([=] {
      while (!closed_) {
        auto item = supplier();
        if (!item) break;

        Push(*item);
      }
      os_log(OS_LOG_DEFAULT, "flightsql: BQ::AddProducer exited while");
      std::unique_lock<std::mutex> unique_lock(mtx_);
      active_threads_--;
      not_empty_.notify_all();
    });
  }

  void Push(T item) {
    os_log(OS_LOG_DEFAULT, "flightsql: BQ::Push");
    std::unique_lock<std::mutex> unique_lock(mtx_);
    if (!WaitUntilCanPushOrClosed(unique_lock)) {
      os_log(OS_LOG_DEFAULT, "flightsql: BQ::Push early exit");
      return;
    }

    buffer_.push_back(std::move(item));
    last_push_time_ = std::chrono::system_clock::now();
    long_time_no_pushes_ = false;
    not_empty_.notify_one();
    os_log(OS_LOG_DEFAULT, "flightsql: BQ::Push exit");
  }

  bool Pop(T *result) {
    os_log(OS_LOG_DEFAULT, "flightsql: BQ::Pop");
    std::unique_lock<std::mutex> unique_lock(mtx_);
    if (!WaitUntilCanPopOrClosed(unique_lock)) {
      os_log(OS_LOG_DEFAULT, "flightsql: BQ::Pop early exit false");
      return false;
    }

    *result = std::move(buffer_.front());
    buffer_.pop_front();
    not_full_.notify_one();

    os_log(OS_LOG_DEFAULT, "flightsql: BQ::Pop exit true");
    return true;
  }

  void Close() {
    std::unique_lock<std::mutex> unique_lock(mtx_);

    if (closed_) return;
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
    not_closed_.notify_all();

    unique_lock.unlock();

    for (auto &item: threads_) {
      item.join();
    }
    notifier_thread_.join();
  }

private:
  bool WaitUntilCanPushOrClosed(std::unique_lock<std::mutex> &unique_lock) {
    not_full_.wait(unique_lock, [this]() {
      return closed_ || buffer_.size() < capacity_ || long_time_no_pushes_;
    });
    return !closed_;
  }

  bool WaitUntilCanPopOrClosed(std::unique_lock<std::mutex> &unique_lock) {
    not_empty_.wait(unique_lock, [this]() {
      return closed_ || buffer_.size() != 0 || active_threads_ == 0;
    });

    return !closed_ && buffer_.size() != 0;
  }
};

}
}

#include <chrono>
#include <future>
#include <iostream>
#include <kronos/thread_safe_queue.hpp>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_push_and_pop() {
  kronos::thread_safe_queue<int> queue;

  expect(queue.push(42), "push should succeed");

  auto result = queue.wait_and_pop();

  expect(result.has_value(), "pop should return a value");
  expect(result.value() == 42, "popped value should be 42");

  std::cout << "PASS: push and pop\n";
}

void test_fifo_order() {
  kronos::thread_safe_queue<int> queue;

  queue.push(10);
  queue.push(20);
  queue.push(30);

  auto first = queue.wait_and_pop();
  auto second = queue.wait_and_pop();
  auto third = queue.wait_and_pop();

  expect(first.has_value() && first.value() == 10, "first value should be 10");

  expect(second.has_value() && second.value() == 20,
         "second value should be 20");

  expect(third.has_value() && third.value() == 30, "third value should be 30");

  std::cout << "PASS: FIFO ordering\n";
}

void test_waiter_wakes_on_push() {
  kronos::thread_safe_queue<int> queue;

  std::promise<void> consumer_started;
  auto started_future = consumer_started.get_future();

  std::promise<std::optional<int>> result_promise;
  auto result_future = result_promise.get_future();

  std::thread consumer([&]() {
    consumer_started.set_value();

    auto result = queue.wait_and_pop();

    result_promise.set_value(std::move(result));
  });

  started_future.wait();

  // The consumer should still be blocked because the queue is empty.
  expect(result_future.wait_for(50ms) == std::future_status::timeout,
         "consumer should wait while queue is empty");

  queue.push(99);

  auto result = result_future.get();

  expect(result.has_value(), "consumer should receive a task");
  expect(result.value() == 99, "consumer should receive 99");

  consumer.join();

  std::cout << "PASS: waiting consumer wakes on push\n";
}

void test_waiter_wakes_on_shutdown() {
  kronos::thread_safe_queue<int> queue;

  std::promise<void> consumer_started;
  auto started_future = consumer_started.get_future();

  std::promise<std::optional<int>> result_promise;
  auto result_future = result_promise.get_future();

  std::thread consumer([&]() {
    consumer_started.set_value();

    auto result = queue.wait_and_pop();

    result_promise.set_value(std::move(result));
  });

  started_future.wait();

  expect(result_future.wait_for(50ms) == std::future_status::timeout,
         "consumer should wait while queue is empty");

  queue.shutdown();

  auto result = result_future.get();

  expect(!result.has_value(), "shutdown on empty queue should return nullopt");

  consumer.join();

  std::cout << "PASS: waiting consumer wakes on shutdown\n";
}

void test_shutdown_drains_existing_tasks() {
  kronos::thread_safe_queue<int> queue;

  queue.push(1);
  queue.push(2);

  queue.shutdown();

  auto first = queue.wait_and_pop();
  auto second = queue.wait_and_pop();
  auto third = queue.wait_and_pop();

  expect(first.has_value() && first.value() == 1,
         "first queued task should still be processed");

  expect(second.has_value() && second.value() == 2,
         "second queued task should still be processed");

  expect(!third.has_value(),
         "queue should return nullopt after shutdown and drain");

  std::cout << "PASS: shutdown drains accepted tasks\n";
}

void test_push_rejected_after_shutdown() {
  kronos::thread_safe_queue<int> queue;

  queue.shutdown();

  bool accepted = queue.push(123);

  expect(!accepted, "push should be rejected after shutdown begins");

  std::cout << "PASS: push rejected after shutdown\n";
}

int main() {
  try {
    std::cout << "Running ThreadSafeQueue tests...\n\n";

    test_push_and_pop();
    test_fifo_order();
    test_waiter_wakes_on_push();
    test_waiter_wakes_on_shutdown();
    test_shutdown_drains_existing_tasks();
    test_push_rejected_after_shutdown();

    std::cout << "\nAll ThreadSafeQueue tests passed.\n";

    return 0;

  } catch (const std::exception &e) {
    std::cerr << "\nFAIL: " << e.what() << '\n';
    return 1;
  }
}
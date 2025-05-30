/*
 * Copyright (c) 2023 Lee Howes, Lucian Radu Teodorescu
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "__system_context_replaceability_api.hpp"
#include "stdexec/execution.hpp"
#include "exec/static_thread_pool.hpp"
#if STDEXEC_ENABLE_LIBDISPATCH
#  include "exec/libdispatch_queue.hpp"
#endif

#include <thread>
#include <atomic>

namespace exec::__system_context_default_impl {
  using namespace stdexec::tags;
  using system_context_replaceability::receiver;
  using system_context_replaceability::bulk_item_receiver;
  using system_context_replaceability::storage;
  using system_context_replaceability::system_scheduler;
  using system_context_replaceability::__system_context_backend_factory;

  /// Receiver that calls the callback when the operation completes.
  template <class _Sender>
  struct __operation;

  /*
  Storage needed for a backend operation-state:

  schedule:
  - __recv::__r_ (receiver*) -- 8
  - __recv::__op_ (__operation*) -- 8
  - __operation::__inner_op_ (stdexec::connect_result_t<_Sender, __recv<_Sender>>) -- 56 (when connected with an empty receiver)
  - __operation::__on_heap_ (bool) -- optimized away
  ---------------------
  Total: 72; extra 16 bytes compared to internal operation state.

  extra for bulk:
  - __recv::__r_ (receiver*) -- 8
  - __recv::__op_ (__operation*) -- 8
  - __operation::__inner_op_ (stdexec::connect_result_t<_Sender, __recv<_Sender>>) -- 128 (when connected with an empty receiver & fun)
  - __operation::__on_heap_ (bool) -- optimized away
  - __bulk_functor::__r_ (bulk_item_receiver*) - 8
  ---------------------
  Total: 152; extra 24 bytes compared to internal operation state.

  Using libdispatch backend, the operation sizes are 48 (down from 80) and 128 (down from 160).

  [*] sizes taken on an Apple M2 Pro arm64 arch. They may differ on other architectures, or with different implementations.
  */

  template <class _Sender>
  struct __recv {
    using receiver_concept = stdexec::receiver_t;

    //! The operation state on the frontend.
    receiver* __r_;

    //! The parent operation state that we will destroy when we complete.
    __operation<_Sender>* __op_;

    void set_value() noexcept {
      auto __op = __op_;
      auto __r = __r_;
      __op->__destruct(); // destroys the operation, including `this`.
      __r->set_value();
      // Note: when calling a completion signal, the parent operation might complete, making the
      // static storage passed to this operation invalid. Thus, we need to ensure that we are not
      // using the operation state after the completion signal.
    }

    void set_error(std::exception_ptr __ptr) noexcept {
      auto __op = __op_;
      auto __r = __r_;
      __op->__destruct(); // destroys the operation, including `this`.
      __r->set_error(__ptr);
    }

    void set_stopped() noexcept {
      auto __op = __op_;
      auto __r = __r_;
      __op->__destruct(); // destroys the operation, including `this`.
      __r->set_stopped();
    }

    decltype(auto) get_env() const noexcept {
      auto __o = __r_->try_query<stdexec::inplace_stop_token>();
      stdexec::inplace_stop_token __st = __o ? *__o : stdexec::inplace_stop_token{};
      return stdexec::prop{stdexec::get_stop_token, __st};
    }
  };

  /// Ensure that `__storage` is aligned to `__alignment`. Shrinks the storage, if needed, to match desired alignment.
  inline storage __ensure_alignment(storage __storage, size_t __alignment) noexcept {
    auto __pn = reinterpret_cast<uintptr_t>(__storage.__data);
    if (__pn % __alignment == 0) {
      return __storage;
    } else if (__storage.__size < __alignment) {
      return {nullptr, 0};
    } else {
      auto __new_pn = (__pn + __alignment - 1) & ~(__alignment - 1);
      return {
        reinterpret_cast<void*>(__new_pn),
        static_cast<uint32_t>(__storage.__size - (__new_pn - __pn))};
    }
  }

  template <typename _Sender>
  struct __operation {
    /// The inner operation state, that results out of connecting the underlying sender with the receiver.
    stdexec::connect_result_t<_Sender, __recv<_Sender>> __inner_op_;
    /// True if the operation is on the heap, false if it is in the preallocated space.
    bool __on_heap_;

    /// Try to construct the operation in the preallocated memory if it fits, otherwise allocate a new operation.
    static __operation*
      __construct_maybe_alloc(storage __storage, receiver* __completion, _Sender __sndr) {
      __storage = __ensure_alignment(__storage, alignof(__operation));
      if (__storage.__data == nullptr || __storage.__size < sizeof(__operation)) {
        return new __operation(std::move(__sndr), __completion, true);
      } else {
        return new (__storage.__data) __operation(std::move(__sndr), __completion, false);
      }
    }

    //! Starts the operation that will schedule work on the system scheduler.
    void start() noexcept {
      stdexec::start(__inner_op_);
    }

    /// Destructs the operation; frees any allocated memory.
    void __destruct() {
      if (__on_heap_) {
        delete this;
      } else {
        std::destroy_at(this);
      }
    }

   private:
    __operation(_Sender __sndr, receiver* __completion, bool __on_heap)
      : __inner_op_(stdexec::connect(std::move(__sndr), __recv<_Sender>{__completion, this}))
      , __on_heap_(__on_heap) {
    }
  };

  template <typename _BaseSchedulerContext>
  struct __system_scheduler_generic_impl : system_scheduler {
    __system_scheduler_generic_impl()
      : __pool_scheduler_(__pool_.get_scheduler()) {
    }
   private:
    using __pool_scheduler_t = decltype(std::declval<_BaseSchedulerContext>().get_scheduler());

    /// The underlying thread pool.
    _BaseSchedulerContext __pool_;
    __pool_scheduler_t __pool_scheduler_;

    //! Functor called by the `bulk` operation; sends a `start` signal to the frontend.
    struct __bulk_functor {
      bulk_item_receiver* __r_;

      void operator()(unsigned long __idx) const noexcept {
        __r_->start(static_cast<uint32_t>(__idx));
      }
    };

    using __schedule_operation_t =
      __operation<decltype(stdexec::schedule(std::declval<__pool_scheduler_t>()))>;

    using __bulk_schedule_operation_t = __operation<decltype(stdexec::bulk(
      stdexec::schedule(std::declval<__pool_scheduler_t>()),
      std::declval<uint32_t>(),
      std::declval<__bulk_functor>()))>;

   public:
    void schedule(storage __storage, receiver* __r) noexcept override {
      try {
        auto __sndr = stdexec::schedule(__pool_scheduler_);
        auto __os =
          __schedule_operation_t::__construct_maybe_alloc(__storage, __r, std::move(__sndr));
        __os->start();
      } catch (std::exception& __e) {
        __r->set_error(std::current_exception());
      }
    }

    void
      bulk_schedule(uint32_t __size, storage __storage, bulk_item_receiver* __r) noexcept
      override {
      try {
        auto __sndr =
          stdexec::bulk(stdexec::schedule(__pool_scheduler_), __size, __bulk_functor{__r});
        auto __os =
          __bulk_schedule_operation_t::__construct_maybe_alloc(__storage, __r, std::move(__sndr));
        __os->start();
      } catch (std::exception& __e) {
        __r->set_error(std::current_exception());
      }
    }
  };

  /// Keeps track of the backends for the system context interfaces.
  template <typename _Interface, typename _Impl>
  struct __instance_data {
    /// Gets the current instance; if there is no instance, uses the current factory to create one.
    std::shared_ptr<_Interface> __get_current_instance() {
      // If we have a valid instance, return it.
      __instance_mutex_.lock();
      auto __r = __instance_;
      __instance_mutex_.unlock();
      if (__r) {
        return __r;
      }

      // Otherwise, create a new instance using the factory.
      // Note: we are lazy-loading the instance to avoid creating it if it is not needed.
      auto __new_instance = __factory_.load(std::memory_order_relaxed)();

      // Store the newly created instance.
      __instance_mutex_.lock();
      __instance_ = __new_instance;
      __instance_mutex_.unlock();
      return __new_instance;
    }

    /// Set `__new_factory` as the new factory for `_Interface` and return the old one.
    __system_context_backend_factory<_Interface>
      __set_backend_factory(__system_context_backend_factory<_Interface> __new_factory) {
      // Replace the factory, keeping track of the old one.
      auto __old_factory = __factory_.exchange(__new_factory);
      // Create a new instance with the new factory.
      auto __new_instance = __new_factory();
      // Replace the current instance with the new one.
      __instance_mutex_.lock();
      auto __old_instance = std::exchange(__instance_, __new_instance);
      __instance_mutex_.unlock();
      // Make sure to delete the old instance after releasing the lock.
      __old_instance.reset();
      return __old_factory;
    }

   private:
    std::mutex __instance_mutex_{};
    std::shared_ptr<_Interface> __instance_{nullptr};
    std::atomic<__system_context_backend_factory<_Interface>> __factory_{__default_factory};

    /// The default factory returns an instance of `_Impl`.
    static std::shared_ptr<_Interface> __default_factory() {
      return std::make_shared<_Impl>();
    }
  };

#if STDEXEC_ENABLE_LIBDISPATCH
  using __system_scheduler_impl = __system_scheduler_generic_impl<exec::libdispatch_queue>;
#else
  using __system_scheduler_impl = __system_scheduler_generic_impl<exec::static_thread_pool>;
#endif

  /// The singleton to hold the `system_scheduler` instance.
  inline constinit __instance_data<system_scheduler, __system_scheduler_impl>
    __system_scheduler_singleton{};

} // namespace exec::__system_context_default_impl

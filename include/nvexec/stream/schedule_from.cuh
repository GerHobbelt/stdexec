/*
 * Copyright (c) 2022 NVIDIA Corporation
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

#include "../../stdexec/execution.hpp"
#include <cstddef>
#include <utility>

#include <cuda/std/tuple>

#include "common.cuh"
#include "../detail/variant.cuh"

STDEXEC_PRAGMA_PUSH()
STDEXEC_PRAGMA_IGNORE_EDG(cuda_compile)

namespace nvexec::STDEXEC_STREAM_DETAIL_NS {

  namespace _sched_from {

    template <class Tag, class Storage, class... As>
    __launch_bounds__(1) __global__ void kernel(Storage* storage, As... as) {
      ::new (storage) Storage();
      storage->template emplace<decayed_tuple<Tag, As...>>(Tag(), static_cast<As&&>(as)...);
    }

    template <class CvrefSenderId, class ReceiverId>
    struct receiver_t {
      using Sender = __cvref_t<CvrefSenderId>;
      using Receiver = stdexec::__t<ReceiverId>;
      using Env = typename operation_state_base_t<ReceiverId>::env_t;

      struct __t : stream_receiver_base {
        using __id = receiver_t;
        using storage_t = variant_storage_t<Sender, Env>;

        constexpr static std::size_t memory_allocation_size = sizeof(storage_t);

        operation_state_base_t<ReceiverId>& operation_state_;

        template <class Tag, class... As>
        void complete(Tag, As&&... as) noexcept {
          using tuple_t = decayed_tuple<Tag, As...>;

          // As an optimization, if there are no values to persist to temporary
          // storage, skip it and simply propagate the completion signal.
          if constexpr (sizeof...(As) == 0) {
            operation_state_.propagate_completion_signal(Tag());
          } else {
            // If there are values in the completion channel, we have to construct
            // the temporary storage. If the values are trivially copyable, we launch
            // a kernel and construct the temporary storage on the device to avoid managed
            // memory movements. Otherwise, we construct the temporary storage on the host
            // and prefetch it to the device.
            auto* storage = static_cast<storage_t*>(operation_state_.temp_storage_);
            constexpr bool construct_on_device = trivially_copyable<__decay_t<As>...>;

            if constexpr (!construct_on_device) {
              ::new (storage) storage_t();
              storage->template emplace<tuple_t>(Tag(), static_cast<As&&>(as)...);
            }

            int dev_id{};
            if (cudaError_t status = STDEXEC_DBG_ERR(cudaGetDevice(&dev_id));
                status != cudaSuccess) {
              operation_state_.propagate_completion_signal(stdexec::set_error, std::move(status));
              return;
            }

            int concurrent_managed_access{};
            if (cudaError_t status = STDEXEC_DBG_ERR(cudaDeviceGetAttribute(
                  &concurrent_managed_access, cudaDevAttrConcurrentManagedAccess, dev_id));
                status != cudaSuccess) {
              operation_state_.propagate_completion_signal(stdexec::set_error, std::move(status));
              return;
            }

            cudaStream_t stream = operation_state_.get_stream();

            if (concurrent_managed_access) {
              if (cudaError_t status = STDEXEC_DBG_ERR(
                    cudaMemPrefetchAsync(storage, sizeof(storage_t), dev_id, stream));
                  status != cudaSuccess) {
                operation_state_.propagate_completion_signal(stdexec::set_error, std::move(status));
                return;
              }
            }

            if constexpr (construct_on_device) {
              kernel<Tag, storage_t, __decay_t<As>...><<<1, 1, 0, stream>>>(storage, as...);

              if (cudaError_t status = STDEXEC_DBG_ERR(cudaPeekAtLastError());
                  status != cudaSuccess) {
                operation_state_.propagate_completion_signal(stdexec::set_error, std::move(status));
                return;
              }
            }

            operation_state_.defer_temp_storage_destruction(storage);

            unsigned int index = storage_t::template index_of<tuple_t>::value;

            visit(
              [&](auto& tpl) noexcept {
                ::cuda::std::apply(
                  [&]<class Tag2, class... Bs>(Tag2, Bs&... tas) noexcept {
                    operation_state_.propagate_completion_signal(Tag2(), std::move(tas)...);
                  },
                  tpl);
              },
              *storage,
              index);
          }
        }

        template <class... _Args>
        void set_value(_Args&&... __args) noexcept {
          complete(set_value_t(), static_cast<_Args&&>(__args)...);
        }

        template <class _Error>
        void set_error(_Error&& __err) noexcept {
          complete(set_error_t(), static_cast<_Error&&>(__err));
        }

        void set_stopped() noexcept {
          complete(set_stopped_t());
        }

        auto get_env() const noexcept -> Env {
          return operation_state_.make_env();
        }
      };
    };

    template <class Sender>
    struct source_sender_t : stream_sender_base {
      template <__decays_to<source_sender_t> Self, receiver Receiver>
      static auto connect(Self&& self, Receiver rcvr)
        -> connect_result_t<__copy_cvref_t<Self, Sender>, Receiver> {
        return stdexec::connect(static_cast<Self&&>(self).sndr_, static_cast<Receiver&&>(rcvr));
      }

      auto get_env() const noexcept -> env_of_t<const Sender&> {
        // TODO - this code is not exercised by any test
        return stdexec::get_env(sndr_);
      }

      template <__decays_to<source_sender_t> _Self, class... _Env>
      static auto get_completion_signatures(_Self&&, _Env&&...)
        -> __completion_signatures_of_t<__copy_cvref_t<_Self, Sender>, _Env...> {
        return {};
      }

      Sender sndr_;
    };

    template <class... _Ty>
    using value_completions_t = //
      completion_signatures<set_value_t(__decay_t<_Ty>...)>;

    template <class _Ty>
    using error_completions_t = //
      completion_signatures<set_error_t(__decay_t<_Ty>)>;
  } // namespace _sched_from

  template <class Scheduler, class SenderId>
  struct schedule_from_sender_t {
    using Sender = stdexec::__t<SenderId>;
    using source_sender_th = _sched_from::source_sender_t<Sender>;

    struct __env {
      context_state_t context_state_;

      template <class _Tag>
      Scheduler query(get_completion_scheduler_t<_Tag>) const noexcept {
        return {context_state_};
      }
    };

    struct __t : stream_sender_base {
      using __id = schedule_from_sender_t;
      __env env_;
      source_sender_th sndr_;

      template <class Self, class Receiver>
      using receiver_t = //
        stdexec::__t<_sched_from::receiver_t<__cvref_id<Self, Sender>, stdexec::__id<Receiver>>>;

      template <__decays_to<__t> Self, receiver Receiver>
        requires sender_to<__copy_cvref_t<Self, source_sender_th>, Receiver>
      static auto connect(Self&& self, Receiver rcvr) //
        -> stream_op_state_t<
          __copy_cvref_t<Self, source_sender_th>,
          receiver_t<Self, Receiver>,
          Receiver> {
        return stream_op_state<__copy_cvref_t<Self, source_sender_th>>(
          static_cast<Self&&>(self).sndr_,
          static_cast<Receiver&&>(rcvr),
          [&](operation_state_base_t<stdexec::__id<Receiver>>& stream_provider)
            -> receiver_t<Self, Receiver> {
            return receiver_t<Self, Receiver>{{}, stream_provider};
          },
          self.env_.context_state_);
      }

      auto get_env() const noexcept -> const __env& {
        return env_;
      }

      template <__decays_to<__t> _Self, class... _Env>
      static auto get_completion_signatures(_Self&&, _Env&&...) //
        -> transform_completion_signatures<
          __completion_signatures_of_t<__copy_cvref_t<_Self, Sender>, _Env...>,
          completion_signatures<set_error_t(cudaError_t)>,
          _sched_from::value_completions_t,
          _sched_from::error_completions_t> {
        return {};
      }

      __t(context_state_t context_state, Sender sndr)
        : env_{context_state}
        , sndr_{{}, static_cast<Sender&&>(sndr)} {
      }
    };
  };
} // namespace nvexec::STDEXEC_STREAM_DETAIL_NS

namespace stdexec::__detail {
  template <class _Scheduler, class _SenderId>
  extern __mconst<
    nvexec::STDEXEC_STREAM_DETAIL_NS::schedule_from_sender_t<_Scheduler, __name_of<__t<_SenderId>>>>
    __name_of_v<nvexec::STDEXEC_STREAM_DETAIL_NS::schedule_from_sender_t<_Scheduler, _SenderId>>;
} // namespace stdexec::__detail

STDEXEC_PRAGMA_POP()

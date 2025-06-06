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

// IWYU pragma: always_keep

#include "../../stdexec/__detail/__config.hpp" // IWYU pragma: export

#if !defined(_NVHPC_CUDA) && !defined(__CUDACC__)
#  error The NVIDIA schedulers and utilities require CUDA support
#endif

#define STDEXEC_STREAM_DETAIL_NS _strm

namespace nvexec::STDEXEC_STREAM_DETAIL_NS {
  using namespace stdexec;
} // namespace nvexec::STDEXEC_STREAM_DETAIL_NS

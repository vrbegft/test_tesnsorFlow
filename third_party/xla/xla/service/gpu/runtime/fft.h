/* Copyright 2022 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_RUNTIME_FFT_H_
#define XLA_SERVICE_GPU_RUNTIME_FFT_H_

#include <memory>

#include "xla/mlir/runtime/transforms/custom_call_encoding.h"
#include "xla/runtime/custom_call_registry.h"
#include "xla/service/gpu/runtime3/fft_thunk.h"

namespace xla {
namespace gpu {

// Registers XLA Gpu runtime fft custom calls.
void RegisterFftCustomCalls(runtime::DirectCustomCallRegistry& registry);

// Adds attributes encoding set for fft custom calls
void PopulateFftAttrEncoding(runtime::CustomCallAttrEncodingSet& encoding);

// Keep FftPlanCache for all FFT instances in the executable.
class FftPlans : public runtime::StateVector<std::unique_ptr<FftPlanCache>> {};

}  //  namespace gpu
}  //  namespace xla

#endif  // XLA_SERVICE_GPU_RUNTIME_FFT_H_

/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/nn_ops.cc.

#define EIGEN_USE_THREADS

#include "tensorflow/core/kernels/xent_op.h"

#include "unsupported/Eigen/CXX11/Tensor"  // from @eigen_archive
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/util/bcast.h"
#include "tensorflow/core/util/determinism.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

template <typename Device, typename T>
class SoftmaxXentWithLogitsOp : public OpKernel {
 public:
  explicit SoftmaxXentWithLogitsOp(OpKernelConstruction* context)
      : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    const Tensor& logits_in = context->input(0);
    const Tensor& labels_in = context->input(1);

    TensorShape shape_in = logits_in.shape();

    BCast bcast(BCast::FromShape(logits_in.shape()),
                BCast::FromShape(labels_in.shape()),
                /*fewer_dims_optimization=*/false);
    if (!logits_in.IsSameSize(labels_in)) {
      OP_REQUIRES(context, bcast.IsValid(),
                  errors::InvalidArgument(
                      "logits and labels must be broadcastable: logits_size=",
                      logits_in.shape().DebugString(),
                      " labels_size=", labels_in.shape().DebugString()));
      shape_in = BCast::ToShape(bcast.output_shape());
    }
    OP_REQUIRES(context, TensorShapeUtils::IsMatrix(shape_in),
                errors::InvalidArgument("logits and labels must be either "
                                        "2-dimensional, or broadcasted to be "
                                        "2-dimensional"));

    if (std::is_same<Device, GPUDevice>::value) {
      OP_REQUIRES(context, !OpDeterminismRequired(),
                  errors::Unimplemented(
                      "The GPU implementation of SoftmaxCrossEntropyWithLogits"
                      " that would have been executed is not deterministic."
                      " Note that the Python API uses an alternative,"
                      " deterministic, GPU-accelerated path when determinism is"
                      " enabled."));
    }

    // loss is 1-D (one per example), and size is batch_size.

    Tensor scratch;
    if (std::is_same<Device, CPUDevice>::value) {
      OP_REQUIRES_OK(context,
                     context->allocate_temp(DataTypeToEnum<T>::value,
                                            TensorShape({shape_in.dim_size(0),
                                                         shape_in.dim_size(1)}),
                                            &scratch));
    } else {
      OP_REQUIRES_OK(context,
                     context->allocate_temp(
                         DataTypeToEnum<T>::value,
                         TensorShape({shape_in.dim_size(0), 1}), &scratch));
    }

    Tensor* loss_out = nullptr;
    OP_REQUIRES_OK(context,
                   context->allocate_output(
                       0, TensorShape({shape_in.dim_size(0)}), &loss_out));
    Tensor* back_out = nullptr;
    // Try to reuse the logits_in buffer for the backprop output.
    OP_REQUIRES_OK(context, context->forward_input_or_allocate_output(
                                {0}, 1, shape_in, &back_out));

    if (shape_in.dim_size(0) > 0) {
      functor::XentFunctor<Device, T> functor;
      functor(context->eigen_device<Device>(), shape_in.AsEigenDSizes<2>(),
              BCast::ToIndexArray<2>(bcast.x_bcast()),
              BCast::ToIndexArray<2>(bcast.y_bcast()),
              logits_in.template shaped<T, 2>(bcast.x_reshape()),
              labels_in.template shaped<T, 2>(bcast.y_reshape()),
              scratch.matrix<T>(), loss_out->vec<T>(), back_out->matrix<T>());
    }
  }
};

// Partial specialization for a CPUDevice, that uses the Eigen implementation
// from XentEigenImpl.
namespace functor {
template <typename Device, typename T>
struct XentFunctorBase {
  void operator()(const Device& d,
                  const Eigen::DSizes<Eigen::DenseIndex, 2>& shape,
                  const Eigen::array<Eigen::DenseIndex, 2>& logits_bcast,
                  const Eigen::array<Eigen::DenseIndex, 2>& labels_bcast,
                  typename TTypes<T>::ConstMatrix logits,
                  typename TTypes<T>::ConstMatrix labels,
                  typename TTypes<T>::Matrix scratch,
                  typename TTypes<T>::Vec loss,
                  typename TTypes<T>::Matrix backprop) {
    T* scratch_ptr = scratch.data();
    T* backprop_ptr = backprop.data();

    T* loss_ptr = loss.data();

    int row_size = shape[1];

    if (shape[0] > 0) {
      backprop.device(d) = logits.broadcast(logits_bcast);
      scratch.device(d) = labels.broadcast(labels_bcast);
      auto reductionWorker = [&](int64_t begin, int64_t end) -> void {
        for (int i = begin; i < end; i++) {
          T* this_backprop = backprop_ptr + (i * row_size);
          T* this_logits = backprop_ptr + (i * row_size);
          T* this_labels = scratch_ptr + (i * row_size);
          T max_logits = this_logits[0];

          // calculating max_logits
          for (int j = 1; j < row_size; j++) {
            max_logits = std::max(max_logits, this_logits[j]);
          }

          T sum = T(0);
          T loss_sum = T(0);

          for (int j = 0; j < row_size; j++) {
            // Note that if input is reused than this_logits and this_backprop
            // is same buffer, so after this calculation this_logits should no
            // longer be trusted
            this_backprop[j] = this_logits[j] - max_logits;
            sum = sum + exp(this_backprop[j]);
          }

          // loss calculation
          T log_sum = log(sum);
          for (int j = 0; j < row_size; j++) {
            loss_sum += this_labels[j] * (log_sum - this_backprop[j]);
            this_backprop[j] = (exp(this_backprop[j]) / sum) - this_labels[j];
          }
          loss_ptr[i] = loss_sum;
        }
      };
      const int64_t compute_cycles = 50 * row_size;
      const int64_t input_bytes = sizeof(T) * row_size;
      const int64_t output_bytes = sizeof(T) * row_size;
      const Eigen::TensorOpCost cost(input_bytes, output_bytes, compute_cycles);

      d.parallelFor(shape[0], cost, reductionWorker);
    }
  }
};

template <typename T>
struct XentFunctor<CPUDevice, T> : XentFunctorBase<CPUDevice, T> {};

}  // namespace functor

#define REGISTER_CPU(T)                                         \
  REGISTER_KERNEL_BUILDER(Name("SoftmaxCrossEntropyWithLogits") \
                              .Device(DEVICE_CPU)               \
                              .TypeConstraint<T>("T"),          \
                          SoftmaxXentWithLogitsOp<CPUDevice, T>);
TF_CALL_half(REGISTER_CPU);
TF_CALL_float(REGISTER_CPU);
TF_CALL_double(REGISTER_CPU);
TF_CALL_bfloat16(REGISTER_CPU);

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

#define REGISTER_GPU(T)                                         \
  REGISTER_KERNEL_BUILDER(Name("SoftmaxCrossEntropyWithLogits") \
                              .Device(DEVICE_GPU)               \
                              .TypeConstraint<T>("T"),          \
                          SoftmaxXentWithLogitsOp<GPUDevice, T>);

TF_CALL_GPU_NUMBER_TYPES(REGISTER_GPU);

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

}  // namespace tensorflow

#include "chainerx/cuda/cuda_device.h"

#include <cstdint>
#include <memory>

#include <cudnn.h>

#include "chainerx/array.h"
#include "chainerx/axes.h"
#include "chainerx/backend_util.h"
#include "chainerx/cuda/cuda_set_device_scope.h"
#include "chainerx/cuda/cudnn.h"
#include "chainerx/device.h"
#include "chainerx/dtype.h"
#include "chainerx/error.h"
#include "chainerx/macro.h"
#include "chainerx/routines/creation.h"
#include "chainerx/scalar.h"
#include "chainerx/shape.h"

namespace chainerx {
namespace cuda {
namespace {

// TODO(sonots): Support other than 4, 5 dimensional arrays by reshaping into 4-dimensional arrays as Chainer does.
cudnnBatchNormMode_t GetBatchNormMode(const Axes& axis) {
    if (axis.ndim() == 1 && axis[0] == 0) {  // (1, channels, (depth, )height, width)
        return CUDNN_BATCHNORM_PER_ACTIVATION;
    }
    if ((axis.ndim() == 3 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3) ||
        (axis.ndim() == 4 && axis[0] == 0 && axis[1] == 2 && axis[2] == 3 && axis[3] == 4)) {  // (1, channels, (1, )1, 1)
        // TODO(hvy): Consider CUDNN_BATCHNORM_SPATIAL_PERSISTENT if we can afford to check for overflow, with or without blocking.
        return CUDNN_BATCHNORM_SPATIAL;
    }
    throw DimensionError{"Invalid axis for BatchNorm using cuDNN ", axis, ". Expected 1, 3 or 4 dimensions."};
}

// Helper function to update the running mean and running variance.
void UpdateRunning(const Array& running, const Array& running_updated) {
    CHAINERX_ASSERT(running.IsContiguous());
    CHAINERX_ASSERT(running_updated.IsContiguous());
    CHAINERX_ASSERT(&running.device() == &running_updated.device());
    CHAINERX_ASSERT(
            (running.dtype() == running_updated.dtype()) ==
            (internal::GetRawOffsetData(running) == internal::GetRawOffsetData(running_updated)));

    if (running.dtype() == running_updated.dtype()) {
        // Assume that running already holds the updated values.
        return;
    }

    // The running values must be written back.
    const Array& running_casted_back = running_updated.AsType(running.dtype());
    Device& device = running.device();
    device.MemoryCopyFrom(
            internal::GetRawOffsetData(running), internal::GetRawOffsetData(running_casted_back), running.GetNBytes(), device);
}

// Derives a secondary tensor descriptor for the batch normalization parameters.
cuda_internal::CudnnTensorDescriptor DeriveBatchNormTensorDescriptor(
        const cuda_internal::CudnnTensorDescriptor& x_desc, cudnnBatchNormMode_t mode) {
    cuda_internal::CudnnTensorDescriptor derive_desc{};
    CheckCudnnError(cudnnDeriveBNTensorDescriptor(*derive_desc, *x_desc, mode));
    return derive_desc;
}

class CudaBatchNormForwardBackward : public chainerx::GenericBatchNormForwardBackward {
public:
    explicit CudaBatchNormForwardBackward(
            cuda_internal::CudnnHandle& cudnn_handle,
            const Array& running_mean,
            const Array& running_var,
            Scalar eps,
            Scalar decay,
            const Axes& axis)
        : GenericBatchNormForwardBackward{running_mean, running_var, eps, decay, axis}, cudnn_handle_{cudnn_handle} {
        if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
            throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
        }
        if (!running_mean.IsContiguous()) {
            throw DeviceError{"Running mean must to be contiguous for cuDNN to update it in-place."};
        }
        if (!running_var.IsContiguous()) {
            throw DeviceError{"Running variance must to be contiguous for cuDNN to update it in-place."};
        }
    }

    Array Forward(const Array& x, const Array& gamma, const Array& beta) override {
        if (CHAINERX_DEBUG) {
            Shape reduced_shape = internal::ReduceShape(x.shape(), axis(), true);
            CHAINERX_ASSERT(gamma.shape() == reduced_shape);
            CHAINERX_ASSERT(beta.shape() == reduced_shape);

            int64_t reduced_total_size = reduced_shape.GetTotalSize();
            CHAINERX_ASSERT(running_mean().GetTotalSize() == reduced_total_size);
            CHAINERX_ASSERT(running_var().GetTotalSize() == reduced_total_size);

            CHAINERX_ASSERT(&x.device() == &gamma.device());
            CHAINERX_ASSERT(&x.device() == &beta.device());
            CHAINERX_ASSERT(&x.device() == &running_mean().device());
            CHAINERX_ASSERT(&x.device() == &running_var().device());

            CHAINERX_ASSERT(GetKind(x.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(gamma.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(beta.dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(running_mean().dtype()) == DtypeKind::kFloat);
            CHAINERX_ASSERT(GetKind(running_var().dtype()) == DtypeKind::kFloat);
        }

        Device& device = x.device();
        Dtype dtype = x.dtype();

        CudaSetDeviceScope scope{device.index()};

        Array x_cont = internal::AsContiguous(x);
        cuda_internal::CudnnTensorDescriptor x_desc{x_cont};
        cudnnBatchNormMode_t mode = GetBatchNormMode(axis());

        // Let cuDNN decide the parameter dtype based on the input and batch normalization mode.
        cuda_internal::CudnnTensorDescriptor gamma_beta_mean_var_desc = DeriveBatchNormTensorDescriptor(x_desc, mode);
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();

        Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
        Array beta_casted_cont = internal::AsContiguous(beta, gamma_beta_mean_var_dtype);

        CHAINERX_ASSERT(running_mean().IsContiguous());
        CHAINERX_ASSERT(running_var().IsContiguous());

        // Convert parameter dtypes if they do not match the dtype expected by cuDNN.
        const Array& running_mean_casted =
                running_mean().dtype() != gamma_beta_mean_var_dtype ? running_mean().AsType(gamma_beta_mean_var_dtype) : running_mean();
        const Array& running_var_casted =
                running_var().dtype() != gamma_beta_mean_var_dtype ? running_var().AsType(gamma_beta_mean_var_dtype) : running_var();

        Array out = EmptyLike(x, device);
        Array x_mean = EmptyLike(gamma_casted_cont, device);
        Array x_inv_std = EmptyLike(gamma_casted_cont, device);

        cudnn_handle_.Call(
                cudnnBatchNormalizationForwardTraining,
                mode,
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                *x_desc,
                internal::GetRawOffsetData(x_cont),
                *x_desc,
                internal::GetRawOffsetData(out),
                *gamma_beta_mean_var_desc,
                internal::GetRawOffsetData(gamma_casted_cont),
                internal::GetRawOffsetData(beta_casted_cont),
                1.0 - static_cast<double>(decay()),
                internal::GetRawOffsetData(running_mean_casted),
                internal::GetRawOffsetData(running_var_casted),
                static_cast<double>(eps()),
                internal::GetRawOffsetData(x_mean),
                internal::GetRawOffsetData(x_inv_std));

        // When data type of parameters is converted, say, from fp16
        // to fp32, the values of fp32 arrays of running_mean and
        // running_var updated by batchNormalizationForwardTraining
        // must be explicitly written back to their original fp16 arrays.
        UpdateRunning(running_mean(), running_mean_casted);
        UpdateRunning(running_var(), running_var_casted);

        SetForwardResults(std::move(x_cont), gamma, std::move(x_mean), std::move(x_inv_std), beta.dtype());

        return out;
    }

    std::array<Array, 3> Backward(const Array& gout) override {
        const Array& x_cont = this->x();
        const Array& gamma = this->gamma();
        const Array& x_mean = this->x_mean();
        const Array& x_inv_std = this->x_inv_std();
        if (CHAINERX_DEBUG) {
            Shape reduced_shape = internal::ReduceShape(x_cont.shape(), axis(), true);
            CHAINERX_ASSERT(reduced_shape == gamma.shape());
            CHAINERX_ASSERT(x_cont.shape() == gout.shape());

            CHAINERX_ASSERT(internal::GetArrayBody(x_mean) != nullptr);
            CHAINERX_ASSERT(internal::GetArrayBody(x_inv_std) != nullptr);

            CHAINERX_ASSERT(&x_cont.device() == &gamma.device());
            CHAINERX_ASSERT(&x_cont.device() == &gout.device());
            CHAINERX_ASSERT(&x_cont.device() == &x_mean.device());
            CHAINERX_ASSERT(&x_cont.device() == &x_inv_std.device());

            CHAINERX_ASSERT(x_cont.IsContiguous());
        }

        Device& device = x_cont.device();
        Dtype dtype = x_cont.dtype();

        CudaSetDeviceScope scope{device.index()};

        Array gout_cont = internal::AsContiguous(gout);
        Array gx = EmptyLike(x_cont, device);

        cuda_internal::CudnnTensorDescriptor x_desc{x_cont};
        cudnnBatchNormMode_t mode = GetBatchNormMode(axis());

        cuda_internal::CudnnTensorDescriptor gamma_beta_mean_var_desc = DeriveBatchNormTensorDescriptor(x_desc, mode);
        Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();
        Shape gamma_beta_mean_var_shape = internal::ReduceShape(x_cont.shape(), axis(), true);

        Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
        Array ggamma = Empty(gamma_beta_mean_var_shape, gamma_beta_mean_var_dtype, device);
        Array gbeta = Empty(gamma_beta_mean_var_shape, gamma_beta_mean_var_dtype, device);
        CHAINERX_ASSERT(gamma_beta_mean_var_dtype == x_mean.dtype());
        CHAINERX_ASSERT(gamma_beta_mean_var_dtype == x_inv_std.dtype());
        CHAINERX_ASSERT(x_mean.IsContiguous());
        CHAINERX_ASSERT(x_inv_std.IsContiguous());

        cudnn_handle_.Call(
                cudnnBatchNormalizationBackward,
                mode,
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<1>(dtype),
                cuda_internal::GetCudnnCoefficientPtr<0>(dtype),
                *x_desc,
                internal::GetRawOffsetData(x_cont),
                *x_desc,
                internal::GetRawOffsetData(gout_cont),
                *x_desc,
                internal::GetRawOffsetData(gx),
                *gamma_beta_mean_var_desc,
                internal::GetRawOffsetData(gamma_casted_cont),
                internal::GetRawOffsetData(ggamma),
                internal::GetRawOffsetData(gbeta),
                static_cast<double>(eps()),
                internal::GetRawOffsetData(x_mean),
                internal::GetRawOffsetData(x_inv_std));

        if (ggamma.dtype() != gamma.dtype()) {
            ggamma = ggamma.AsType(gamma.dtype());
        }
        if (gbeta.dtype() != beta_dtype()) {
            gbeta = gbeta.AsType(beta_dtype());
        }

        return {std::move(gx), std::move(ggamma), std::move(gbeta)};
    }

private:
    cuda_internal::CudnnHandle& cudnn_handle_;
};

}  // namespace

std::unique_ptr<BatchNormForwardBackward> CudaDevice::GetBatchNormForwardBackward(
        const Array& running_mean, const Array& running_var, Scalar eps, Scalar decay, const Axes& axis) {
    return std::make_unique<CudaBatchNormForwardBackward>(cudnn_handle(), running_mean, running_var, eps, decay, axis);
}

Array CudaDevice::FixedBatchNorm(
        const Array& x, const Array& gamma, const Array& beta, const Array& mean, const Array& var, Scalar eps, const Axes& axis) {
    if (static_cast<double>(eps) < CUDNN_BN_MIN_EPSILON) {
        throw CudnnError{"Minimum allowed epsilon is ", CUDNN_BN_MIN_EPSILON, " but found ", eps, "."};
    }

    CudaSetDeviceScope scope{index()};

    if (CHAINERX_DEBUG) {
        Shape reduced_shape = internal::ReduceShape(x.shape(), axis, true);
        CHAINERX_ASSERT(gamma.shape() == reduced_shape);
        CHAINERX_ASSERT(beta.shape() == reduced_shape);
        CHAINERX_ASSERT(mean.shape() == reduced_shape);
        CHAINERX_ASSERT(var.shape() == reduced_shape);

        CHAINERX_ASSERT(&x.device() == &gamma.device());
        CHAINERX_ASSERT(&x.device() == &beta.device());
        CHAINERX_ASSERT(&x.device() == &mean.device());
        CHAINERX_ASSERT(&x.device() == &var.device());

        CHAINERX_ASSERT(GetKind(x.dtype()) == DtypeKind::kFloat);
        CHAINERX_ASSERT(GetKind(gamma.dtype()) == DtypeKind::kFloat);
        CHAINERX_ASSERT(GetKind(beta.dtype()) == DtypeKind::kFloat);
        CHAINERX_ASSERT(GetKind(mean.dtype()) == DtypeKind::kFloat);
        CHAINERX_ASSERT(GetKind(var.dtype()) == DtypeKind::kFloat);
    }

    Array x_cont = internal::AsContiguous(x);
    cuda_internal::CudnnTensorDescriptor x_desc{x_cont};
    cudnnBatchNormMode_t mode = GetBatchNormMode(axis);

    cuda_internal::CudnnTensorDescriptor gamma_beta_mean_var_desc = DeriveBatchNormTensorDescriptor(x_desc, mode);
    Dtype gamma_beta_mean_var_dtype = gamma_beta_mean_var_desc.GetDtype();

    Array gamma_casted_cont = internal::AsContiguous(gamma, gamma_beta_mean_var_dtype);
    Array beta_casted_cont = internal::AsContiguous(beta, gamma_beta_mean_var_dtype);
    Array mean_casted_cont = internal::AsContiguous(mean, gamma_beta_mean_var_dtype);
    Array var_casted_cont = internal::AsContiguous(var, gamma_beta_mean_var_dtype);

    Array out = EmptyLike(x, x.device());

    cudnn_handle_.Call(
            cudnnBatchNormalizationForwardInference,
            GetBatchNormMode(axis),
            cuda_internal::GetCudnnCoefficientPtr<1>(x.dtype()),
            cuda_internal::GetCudnnCoefficientPtr<0>(x.dtype()),
            *x_desc,
            internal::GetRawOffsetData(x_cont),
            *x_desc,
            internal::GetRawOffsetData(out),
            *gamma_beta_mean_var_desc,
            internal::GetRawOffsetData(gamma_casted_cont),
            internal::GetRawOffsetData(beta_casted_cont),
            internal::GetRawOffsetData(mean_casted_cont),
            internal::GetRawOffsetData(var_casted_cont),
            static_cast<double>(eps));

    return out;
}

}  // namespace cuda
}  // namespace chainerx

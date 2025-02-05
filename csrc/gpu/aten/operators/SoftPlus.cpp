#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/TensorIterator.h>

#include <utils/DPCPP.h>
#include "Loops.h"
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

using namespace xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {

template <typename scalar_t, typename accscalar_t>
struct softplus_out_functor {
  scalar_t operator()(accscalar_t a) const {
    return scalar_t(
        a * b > t
            ? a
            : Numerics<accscalar_t>::log1p(Numerics<accscalar_t>::exp(a * b)) /
                b);
  }

  softplus_out_functor(accscalar_t b, accscalar_t t) : b(b), t(t) {}

 private:
  accscalar_t b;
  accscalar_t t;
};

Tensor& softplus_out(
    const Tensor& self,
    const Scalar& beta,
    const Scalar& threshold,
    Tensor& out) {
  auto iter = TensorIterator::unary_op(out, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::BFloat16,
      at::ScalarType::Half,
      iter.dtype(),
      "softplus_forward",
      [&]() {
        using accscalar_t = acc_type<scalar_t>;
        auto b = beta.to<accscalar_t>();
        auto t = threshold.to<accscalar_t>();
        softplus_out_functor<scalar_t, accscalar_t> f(b, t);
        dpcpp_kernel_for_tensor_iter(iter, f);
      });

  return out;
}

Tensor softplus(
    const Tensor& self,
    const Scalar& beta,
    const Scalar& threshold) {
  Tensor out = at::empty({0}, self.options());
  return at::AtenIpexTypeXPU::softplus_out(self, beta, threshold, out);
}

template <typename scalar_t>
struct softplus_backward_out_functor {
  scalar_t operator()(scalar_t grad_output_data, scalar_t output_data) const {
    scalar_t beta_out = b * output_data;
    scalar_t exp_bo = Numerics<scalar_t>::exp(beta_out);
    return beta_out > t ? grad_output_data
                        : grad_output_data * exp_bo / (exp_bo + 1);
  }

  softplus_backward_out_functor(scalar_t b, scalar_t t) : b(b), t(t) {}

 private:
  scalar_t b;
  scalar_t t;
};

Tensor& softplus_backward_out(
    const Tensor& grad_output,
    const Tensor& self,
    const Scalar& beta,
    const Scalar& threshold,
    Tensor& grad_input) {
  auto iter =
      TensorIterator::borrowing_binary_op(grad_input, grad_output, self);
  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.dtype(),
      "softplus_backward",
      [&]() {
        auto b = beta.to<scalar_t>();
        auto t = threshold.to<scalar_t>();
        softplus_backward_out_functor<scalar_t> f(b, t);
        dpcpp_kernel_for_tensor_iter(iter, f);
      });

  return grad_input;
}

} // namespace AtenIpexTypeXPU
} // namespace at

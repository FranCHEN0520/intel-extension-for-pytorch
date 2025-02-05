#include <ATen/ATen.h>
#include <ATen/Context.h>

#include <oneDNN/oneDNN.h>
#include <utils/DPCPP.h>
#include "comm/ATDispatch.h"
#include "comm/RegistrationDeclarations.h"

#include "Loops.h"

namespace at {
namespace AtenIpexTypeXPU {

template <typename scalar_t>
struct leaky_relu_out_functor {
  scalar_t operator()(scalar_t x) const {
    x = (x >= 0) ? x : x * negval;
    return x;
  }

  leaky_relu_out_functor(scalar_t negval) : negval(negval) {}

 private:
  scalar_t negval;
};

Tensor& leaky_relu_out(
    const Tensor& self,
    const Scalar& negative_slope,
    Tensor& out) {
  auto iter = TensorIterator::unary_op(out, self);

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.dtype(),
      "LeakyReLU",
      [&]() {
        auto negval = negative_slope.to<scalar_t>();
        leaky_relu_out_functor<scalar_t> f(negval);
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return out;
}

template <typename scalar_t>
struct leaky_relu_backward_out_functor {
  scalar_t operator()(scalar_t grad_output, scalar_t x) const {
    if (x > 0)
      return grad_output;
    else
      return grad_output * negval;
  }

  leaky_relu_backward_out_functor(scalar_t negval) : negval(negval) {}

 private:
  scalar_t negval;
};

Tensor& leaky_relu_backward_out(
    const Tensor& grad_output,
    const Tensor& self,
    const Scalar& negative_slope,
    bool self_is_result,
    Tensor& grad_input) {
  auto iter = TensorIterator::binary_op(grad_input, grad_output, self);

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      iter.dtype(),
      "LeakyReLU_backward",
      [&]() {
        auto negval = negative_slope.to<scalar_t>();
        leaky_relu_backward_out_functor<scalar_t> f(negval);
        dpcpp_kernel_for_tensor_iter(iter, f);
      });
  return grad_input;
}

} // namespace AtenIpexTypeXPU

} // namespace at

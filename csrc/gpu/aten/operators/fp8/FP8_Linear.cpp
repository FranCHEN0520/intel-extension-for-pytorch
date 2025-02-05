#include <ATen/WrapDimUtilsMulti.h>
#include <ATen/native/Resize.h>
#include "../BlasImpl.h"
#include "cast.h"
#include "utils/CustomOperatorRegistration.h"

namespace at {
namespace AtenIpexTypeXPU {

Tensor fp8_gemm(
    const Tensor& m1_ /*act*/,
    int64_t in_format,
    int64_t input_meta_index,
    const Tensor& m2_ /*wei*/,
    int64_t wei_format,
    int64_t weight_meta_index,
    const Tensor& m3 /*bias*/,
    const Tensor& scale,
    const Tensor& scale_inv,
    const Tensor& amax_history) {
  // TODO: simulation with fp32/bf16 input, need to remove it after hardware
  // support FP8.
  Tensor m1 = at::empty(m1_.sizes().vec(), m1_.options().dtype(at::kBFloat16));
  Tensor m2 = at::empty(m2_.sizes().vec(), m2_.options().dtype(at::kBFloat16));
  fp8_dequantize_op(
      m1_, m1, in_format, scale_inv[input_meta_index].data_ptr<float>());
  fp8_dequantize_op(
      m2_, m2, wei_format, scale_inv[weight_meta_index].data_ptr<float>());

  auto result = at::linear(m1, m2, m3.to(m1.scalar_type()));

  return result;
}

Tensor fp8_gemm_backward(
    const Tensor& m1 /*grad_out*/,
    int64_t m1_format,
    int64_t m1_meta_index,
    const Tensor& m2 /*act, wei*/,
    int64_t grad_format,
    int64_t grad_meta_index,
    const Tensor& scale,
    const Tensor& scale_inv,
    const Tensor& amax_history) {
  std::vector<int64_t> result_shape;
  if (m1.dim() == 2) {
    result_shape = {m1.size(0), m2.size(1)};
  } else if (m1.dim() == 3) {
    if (m2.dim() == 2) {
      result_shape = {m1.size(0) * m1.size(1), m2.size(1)};
    } else {
      result_shape = {m1.size(0), m1.size(1), m2.size(2)};
    }
  } else {
    TORCH_CHECK(false, "linear only support for 2D and 3D tensors!\n");
  }
  Tensor result = at::empty(result_shape, m1.options());
  if (m1.dim() == 3 && m2.dim() == 2) {
    xpu::oneDNN::matmul(
        result,
        m1.reshape({m1.sizes()[0] * m1.sizes()[1], m1.sizes()[2]}),
        m2,
        at::Tensor(),
        true,
        Attr());
    return result.view_symint(
        {m1.sizes()[0], m1.sizes()[1], result.sym_size(1)});
  }
  xpu::oneDNN::matmul(result, m1, m2, at::Tensor(), true, Attr());
  return result;
}

} // namespace AtenIpexTypeXPU
} // namespace at

namespace {
IPEX_LIBRARY_FRAGMENT() {
  IPEX_OP_REGISTER_DISPATCH(
      "fp8_gemm.xpu", at::AtenIpexTypeXPU::fp8_gemm, c10::DispatchKey::XPU);
  IPEX_OP_REGISTER_DISPATCH(
      "fp8_gemm_backward.xpu",
      at::AtenIpexTypeXPU::fp8_gemm_backward,
      c10::DispatchKey::XPU);
}
} // namespace

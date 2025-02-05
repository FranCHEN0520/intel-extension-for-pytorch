#include <ATen/ATen.h>
#include <ATen/ExpandUtils.h>
#include <ATen/MatrixRef.h>
#include <ATen/cpp_custom_type_hack.h>
#include <ATen/record_function.h>
#include <dnnl.hpp>
#include <oneDNN/oneDNN.h>
#include <torch/autograd.h>
#include <torch/custom_class.h>
#include "comm/ATDispatch.h"
#include "comm/RegistrationDeclarations.h"

#include "comm/ParamUtils.h"

using namespace dnnl;
using namespace at::native;
using namespace xpu::dpcpp;
using namespace xpu::oneDNN;
using namespace torch::autograd;

namespace at {
namespace AtenIpexTypeXPU {

Tensor pixel_shuffle(const Tensor& self, int64_t upscale_factor) {
  auto smf = self.suggest_memory_format();
  AT_ASSERTM(
      self.dim() == 4,
      "pixel_shuffle expects 4D input, but got input with sizes ",
      self.sizes());
  int64_t b = self.size(0);
  int64_t c = self.size(1);
  int64_t h = self.size(2);
  int64_t w = self.size(3);
  int64_t upscale_factor_squared = upscale_factor * upscale_factor;
  AT_ASSERTM(
      c % upscale_factor_squared == 0,
      "pixel_shuffle expects input channel to be divisible by square of "
      "upscale_factor, but got input with sizes ",
      self.sizes(),
      ", upscale_factor=",
      upscale_factor,
      ", and self.size(1)=",
      c,
      " is not divisible by ",
      upscale_factor_squared);
  int64_t oc = c / upscale_factor_squared;
  int64_t oh = h * upscale_factor;
  int64_t ow = w * upscale_factor;

  auto input_reshaped =
      self.reshape({b, oc, upscale_factor, upscale_factor, h, w});
  return input_reshaped
      .permute(
          {0 /* b */,
           1 /* oc */,
           4 /* h */,
           2 /* 1st upscale_factor */,
           5 /* w */,
           3 /* 2nd upscale_factor */})
      .reshape({b, oc, oh, ow})
      .contiguous(smf);
}

TORCH_LIBRARY_IMPL(aten, XPU, m) {
  m.impl("pixel_shuffle", TORCH_FN(AtenIpexTypeXPU::pixel_shuffle));
}
} // namespace AtenIpexTypeXPU
} // namespace at

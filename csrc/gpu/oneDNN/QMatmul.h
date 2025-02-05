#pragma once

#include <ATen/ATen.h>
#include <ATen/record_function.h>

#include <oneDNN/Runtime.h>
#include <runtime/Utils.h>
#include <tensor/Tensor.h>
#include <utils/LRUCache.h>
#include "Attr.h"
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>

using namespace dnnl;
using namespace xpu::dpcpp;
using namespace at::AtenIpexTypeXPU;

namespace xpu {
namespace oneDNN {
static inline void quantized_matmul(
    Tensor& result,
    const Tensor& mat1,
    const Tensor& mat2,
    const Tensor& b_raw,
    bool m2_trans,
    Attr attr) {
  size_t dims = result.dim();
  TORCH_CHECK(
      dims == 2 || dims == 3,
      "oneDNN matmul only works with 2D or 3D, got ",
      dims);
  TORCH_CHECK(
      dims == mat1.dim() && dims == mat2.dim(),
      "oneDNN input matrixes must have the same ranks");
  TORCH_CHECK(result.defined(), "oneDNN matmul result should be defined");

  at::Device curDevice = at::Device(at::kXPU, current_device());
  auto engine = GpuEngineManager::Instance().get_engine(curDevice);
  auto strm = GpuStreamManager::Instance().get_stream();

  Tensor m1 = xpu::oneDNN::is_onednn_matmul_strides(mat1)
      ? mat1
      : contiguous_if_needed(mat1);
  Tensor m2 = xpu::oneDNN::is_onednn_matmul_strides(mat2)
      ? mat2
      : contiguous_if_needed(mat2);
  Tensor dst = xpu::oneDNN::is_onednn_matmul_strides(result, true)
      ? result
      : contiguous_if_needed(result);

  int64_t m = dst.size(-2);
  int64_t n = dst.size(-1);
  int64_t k = m1.size(-1);
  int64_t mb = 1;

  if (dims == 3) {
    mb = dst.size(0);
    TORCH_CHECK(
        mb == m1.size(0) && mb == m2.size(0),
        "batch size mismatch, dst mb: ",
        mb,
        "m1 mb",
        m1.size(0),
        " m2 mb: ",
        m2.size(0));
  }

  // validate bias and make it compatible with oneDNN implementation
  bool with_bias = false;
  Tensor b = b_raw;
  if (b.defined()) {
    with_bias = true;
    if (b.dim() == 1) {
      TORCH_CHECK(
          b.size(0) == n || b.size(0) == 1,
          "matmul supports [n] or [1] when bias dim is 1 ...");
      if (b.size(0) == 0) {
        with_bias = false;
      } else if (m1.dim() == 3) {
        b = b.expand({mb, m, n}).contiguous();
      } else if (m1.dim() == 2) {
        b = b.expand({1, n}).contiguous();
      }
    } else if (b.dim() == 2) {
      TORCH_CHECK(
          (b.size(0) == m && b.size(1) == n) ||
              (b.size(0) == 1 && b.size(1) == n) ||
              (b.size(0) == m && b.size(1) == 1) ||
              (b.size(0) == 1 && b.size(1) == 1),
          "matmul supports [m, n] or [1, n] or [m, 1] or [1, 1] when bias dim is 2 ...");
      if (b.size(0) == 1 && b.size(1) == 1)
        b = b.expand({1, n}).contiguous();
    } else if (b.dim() == 3) {
      TORCH_CHECK(
          are_expandable({mb, m, n}, b.sizes()),
          "matmul bias must be expandable to:",
          dst.sizes(),
          " but got:",
          b.sizes());
      b = b.expand({mb, m, n}).contiguous();
    } else if (b.dim() == 0) {
      TORCH_CHECK(
          b.numel() == 1, "matmul supports 1 numel when bias dim is [] ...");
      if (m1.dim() == 3) {
        b = b.expand({mb, m, n}).contiguous();
      } else {
        b = b.expand({1, n}).contiguous();
      }
    } else {
      TORCH_CHECK(0, "unsupported bias dim in matmul ...");
    }
  }

  // bias is fused in post-op for quantized path
  b = b.contiguous(); // avoid reorder 2 times

  // ipex matmul support both ab/ba shape for m2 tensor, we don't check any more

  auto m1_usr_dt = get_onednn_dtype(m1);
  auto m2_usr_dt = get_onednn_dtype(m2);
  auto dst_usr_dt = get_onednn_dtype(dst);

  auto m1_dt = m1_usr_dt;
  auto m2_dt = m2_usr_dt;
  auto dst_dt = dst_usr_dt;
  memory::data_type bias_dt;

  memory::desc m1_md, m1_usr_md, m1_any_md;
  memory::desc m2_md, m2_usr_md, m2_any_md;
  memory::desc dst_md, dst_usr_md, dst_any_md;
  memory::desc b_md;

  // STEP1: create memory desc
  memory::dims m1_dims, m2_dims, dst_dims, bias_dims;
  memory::dims m1_strides, m2_strides, dst_strides, bias_strides;
  if (dims == 2) {
    m1_dims = {m, k};
    m2_dims = {k, n};
    dst_dims = {m, n};

    m1_strides = {m1.stride(0), m1.stride(1)};
    if (m2_trans) {
      m2_strides = {m2.stride(0), m2.stride(1)};
    } else {
      m2_strides = {m2.stride(1), m2.stride(0)};
    }
    dst_strides = {dst.stride(0), dst.stride(1)};
  } else {
    m1_dims = {mb, m, k};
    m2_dims = {mb, k, n};
    dst_dims = {mb, m, n};

    m1_strides = {m1.stride(0), m1.stride(1), m1.stride(2)};
    if (m2_trans) {
      m2_strides = {m2.stride(0), m2.stride(1), m2.stride(2)};
    } else {
      m2_strides = {m2.stride(0), m2.stride(2), m2.stride(1)};
    }
    dst_strides = {dst.stride(0), dst.stride(1), dst.stride(2)};
  }

  if (with_bias) {
    bias_dims = get_onednn_dims(b);
    bias_dt = get_onednn_dtype(b);
    bias_strides = get_onednn_strides(b);
  }

  std::unordered_map<int, memory> args;

  post_ops po;
  attr.extract_post_ops(po, dst);
  auto is_onednn_layout_suggested = using_onednn_layout_for_matmul(m1);
  bool m1_need_zp = (m1.q_zero_point() != 0);
  // wgh should never have zero point
  bool wgh_is_per_channel =
      (m2.is_quantized() && (m2.qscheme() != kPerTensorAffine));

  lru_key_t key_primitive;
#ifdef USE_PRIMITIVE_CACHE
  create_key(
      key_primitive,
      m1_dims,
      m2_dims,
      dst_dims,
      bias_dims,
      m1_dt,
      m2_dt,
      dst_dt,
      bias_dt,
      m1_strides,
      m2_strides,
      dst_strides,
      bias_strides,
      dims,
      is_onednn_layout_suggested,
      m1_need_zp,
      wgh_is_per_channel,
      attr);
#endif

  // STEP3: create primitive
#ifdef USE_PRIMITIVE_CACHE
  bool load_from_cache = find_key<dnnl::matmul>(key_primitive);
#else
  bool load_from_cache = false;
#endif

  dnnl::matmul matmul_p;
  dnnl::matmul::primitive_desc matmul_pd;

  if (load_from_cache) {
    // load primitive from cache
    matmul_p = fetch_m<dnnl::matmul>(key_primitive);
    auto matmul_pd_t = matmul_p.get_primitive_desc();
    matmul_pd = dnnl::matmul::primitive_desc(
        const_cast<dnnl_primitive_desc_t>(matmul_pd_t));
  } else {
    if (is_onednn_layout_suggested && dims == 2) {
      m1_md = memory::desc(m1_dims, m1_dt, memory::format_tag::any);
      m2_md = memory::desc(m2_dims, m2_dt, memory::format_tag::any);
      dst_md = memory::desc(dst_dims, dst_dt, memory::format_tag::any);
    } else {
      m1_md = memory::desc(m1_dims, m1_dt, m1_strides);
      m2_md = memory::desc(m2_dims, m2_dt, m2_strides);
      dst_md = memory::desc(dst_dims, dst_dt, dst_strides);
    }
    // STEP2: creat attribute
    primitive_attr pattr;
    pattr.set_post_ops(po);

#ifdef USE_SCRATCHPAD_MODE
    pattr.set_scratchpad_mode(dnnl::scratchpad_mode::user);
#endif

    Tensor m2_sc;
    memory::desc m2_sc_md =
        memory::desc({1}, memory::data_type::f32, memory::format_tag::x);
    memory m2_sc_m, m2_zp_m;
    if (m2.is_quantized()) {
      if (!wgh_is_per_channel) {
        pattr.set_scales_mask(DNNL_ARG_WEIGHTS, 0);
      } else {
        pattr.set_scales_mask(DNNL_ARG_WEIGHTS, 1 << 1);
      }
    }

    Tensor m1_sc;
    memory::desc m1_sc_md =
        memory::desc({1}, memory::data_type::f32, memory::format_tag::x);
    memory m1_sc_m, m1_zp_m;
    if (m1.is_quantized()) {
      int mask_ac = 0;
      pattr.set_scales_mask(DNNL_ARG_SRC, mask_ac);
      if (m1_need_zp) {
        pattr.set_zero_points_mask(DNNL_ARG_SRC, mask_ac);
      }
    }

    if (with_bias) {
      b_md = memory::desc(bias_dims, bias_dt, bias_strides);
      matmul_pd =
          matmul::primitive_desc(engine, m1_md, m2_md, b_md, dst_md, pattr);
    } else {
      matmul_pd = matmul::primitive_desc(engine, m1_md, m2_md, dst_md, pattr);
    }

#ifdef USE_PRIMITIVE_CACHE
    matmul_p = create_and_fetch_m<dnnl::matmul>(key_primitive, matmul_pd);
#else
    matmul_p = dnnl::matmul(matmul_pd);
#endif
  }

  m1_usr_md = memory::desc(m1_dims, m1_usr_dt, m1_strides);
  m2_usr_md = memory::desc(m2_dims, m2_usr_dt, m2_strides);
  dst_usr_md = memory::desc(dst_dims, dst_usr_dt, dst_strides);
  // STEP4: create memory
  auto m1_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(m1);
  auto m1_usr_m = m1_ctx.is_plain()
      ? dpcpp_onednn_memory(m1_usr_md, engine, m1.data_ptr())
      : dpcpp_onednn_memory({m1_ctx.meta()}, engine, m1.data_ptr());

  auto m2_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(m2);
  auto m2_usr_m = m2_ctx.is_plain()
      ? dpcpp_onednn_memory(m2_usr_md, engine, m2.data_ptr())
      : dpcpp_onednn_memory({m2_ctx.meta()}, engine, m2.data_ptr());

  auto dst_ctx = at::AtenIpexTypeXPU::DPCPPTensorContext::get_tensor_ctx(dst);
  auto dst_usr_m = dst_ctx.is_plain()
      ? dpcpp_onednn_memory(dst_usr_md, engine, dst.data_ptr())
      : dpcpp_onednn_memory({dst_ctx.meta()}, engine, dst.data_ptr());

  auto expected_m1_md = matmul_pd.src_desc();
  auto expected_m2_md = matmul_pd.weights_desc();
  auto expected_dst_md = matmul_pd.dst_desc();

  memory m1_m = m1_usr_m, m2_m = m2_usr_m, dst_m = dst_usr_m;
  Tensor m1_, m2_, dst_;

  auto weight_cache_optimization = [&]() {
    bool onoff = false;
    onoff |= is_onednn_layout_suggested;
    onoff &= c10::InferenceMode::is_enabled();
    return onoff;
  }();

#ifdef USE_SCRATCHPAD_MODE
  int scratchpad_size = matmul_pd.scratchpad_desc().get_size();
  Tensor scratchpad_tensor = at::AtenIpexTypeXPU::empty(
      {scratchpad_size}, m1.options().dtype(at::kByte), c10::nullopt);
  auto scratchpad_memory = dpcpp_onednn_memory(
      matmul_pd.scratchpad_desc(), engine, scratchpad_tensor.data_ptr());
  args.insert({DNNL_ARG_SCRATCHPAD, scratchpad_memory});
#endif

  // reorder cases
  // case1: master weight support to reorder data type
  // case2: block format support to reorder format
  if (m1_usr_m.get_desc() != expected_m1_md) {
    m1_ = empty_opaque_tensor(expected_m1_md, m1.options(), c10::nullopt);
    m1_m = dpcpp_onednn_memory(expected_m1_md, engine, m1_.data_ptr());
    xpu::oneDNN::reorder(m1, m1_);
  }

  if (m2_usr_m.get_desc() != expected_m2_md) {
    m2_ = empty_opaque_tensor(expected_m2_md, m2.options(), c10::nullopt);
    m2_m = dpcpp_onednn_memory(expected_m2_md, engine, m2_.data_ptr());
    auto m2_onednn_matmul_shape_compatible = m2_trans ? m2 : m2.t();
    xpu::oneDNN::reorder(m2_onednn_matmul_shape_compatible, m2_);

    if (weight_cache_optimization) {
      auto ctx_ =
          at::AtenIpexTypeXPU::DPCPPTensorContext::release_tensor_ctx(m2_);
      // assume oneDNN.matmul.weight is the permution of torch.nn.Linear.weight
      ctx_.set_aten_meta(
          {m2_onednn_matmul_shape_compatible.sizes().vec(),
           m2_onednn_matmul_shape_compatible.strides().vec()});
      at::AtenIpexTypeXPU::DPCPPTensorContext::set_tensor_ctx(
          m2, std::move(ctx_));
    }
  }

  // bias add for gen12hp platform
  if (dst_usr_m.get_desc() != expected_dst_md) {
    dst_ = empty_opaque_tensor(expected_dst_md, dst.options(), c10::nullopt);
    dst_m = dpcpp_onednn_memory(expected_dst_md, engine, dst_.data_ptr());
    if (attr.with_sum())
      xpu::oneDNN::reorder(dst, dst_);
  }
  if (attr.with_binary())
    attr.construct_post_binary(matmul_pd, po, args);

  args.insert({DNNL_ARG_SRC, m1_m});
  args.insert({DNNL_ARG_WEIGHTS, m2_m});
  args.insert({DNNL_ARG_DST, dst_m});
  if (b.defined()) {
    auto b_m = dpcpp_onednn_memory(b_md, engine, b.data_ptr());
    args.insert({DNNL_ARG_BIAS, b_m});
  }

  // Add scale/zp md
  Tensor m2_sc;
  memory m2_sc_m, m2_zp_m;
  memory::desc m2_sc_md =
      memory::desc({1}, memory::data_type::f32, memory::format_tag::x);
  if (m2.is_quantized()) {
    if (m2.qscheme() == kPerTensorAffine) {
      std::tie(m2_sc_m, m2_zp_m) = xpu::oneDNN::q_get_sc_zp_gpu_mem(m2, engine);
    } else {
      m2_sc = m2.q_per_channel_scales().to(at::kFloat);
      m2_sc_m = dpcpp_onednn_memory(m2_sc_md, engine, m2_sc.data_ptr());
      // See [Note: Per-channel quantization mask setting]
    }
    args.insert({DNNL_ARG_ATTR_SCALES | DNNL_ARG_WEIGHTS, m2_sc_m});
  }

  Tensor m1_sc;
  memory::desc m1_sc_md =
      memory::desc({1}, memory::data_type::f32, memory::format_tag::x);
  memory m1_sc_m, m1_zp_m;
  if (m1.is_quantized()) {
    int mask_ac = 0;
    std::tie(m1_sc_m, m1_zp_m) = xpu::oneDNN::q_get_sc_zp_gpu_mem(m1, engine);
    args.insert({DNNL_ARG_ATTR_SCALES | DNNL_ARG_SRC, m1_sc_m});
    if (m1_need_zp) {
      args.insert({DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_SRC, m1_zp_m});
    }
  }

  DPCPP_ONEDNN_EXEC(matmul_p, strm, args);
  if (is_onednn_layout_suggested && dst_m != dst_usr_m && dims == 2) {
    auto blk_ctx = DPCPPTensorContext::release_tensor_ctx(dst_);
    DPCPPTensorContext::set_tensor_ctx(dst, std::move(blk_ctx));
  }

  if (!dst.is_same(result))
    result.copy_(dst);
}

} // namespace oneDNN
} // namespace xpu

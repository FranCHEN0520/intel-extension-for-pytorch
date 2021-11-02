#pragma once

#include <ATen/core/Tensor.h>
#include <utils/DPCPP.h>

using namespace at;

namespace xpu {
namespace dpcpp {

inline bool is_channels_last(at::MemoryFormat fmt) {
  if (
#ifdef USE_CHANNELS_LAST_1D
      at::MemoryFormat::ChannelsLast1d == fmt ||
#endif
      at::MemoryFormat::ChannelsLast == fmt ||
      at::MemoryFormat::ChannelsLast3d == fmt) {
    return true;
  }
  return false;
}

inline bool is_smf_channels_last(const Tensor& t) {
  const auto ndim = t.ndimension();
  if (
#ifdef USE_CHANNELS_LAST_1D
      3 != ndim &&
#endif
      4 != ndim && 5 != ndim) {
    // channels last only supports 3D, 4D, 5D tensor
    return false;
  }

  return is_channels_last(t.suggest_memory_format());
}

inline MemoryFormat get_cl_tag_by_ndim(const int64_t ndim) {
  TORCH_CHECK(
      3 == ndim || 4 == ndim || 5 == ndim,
      "ndim must be 3, 4 or 5 when get cl tag");
  if (3 == ndim) {
#ifdef USE_CHANNELS_LAST_1D
    return at::MemoryFormat::ChannelsLast1d;
#else
    // if doesn't enable channels last 1d
    return at::MemoryFormat::Contiguous;
#endif
  } else if (5 == ndim) {
    return at::MemoryFormat::ChannelsLast3d;
  } else {
    return at::MemoryFormat::ChannelsLast;
  }
}

} // namespace dpcpp
} // namespace xpu

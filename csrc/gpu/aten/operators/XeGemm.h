#pragma once

#include <ATen/ATen.h>
#include <ATen/record_function.h>
#include <oneDNN/oneDNN.h>
#include <runtime/Utils.h>
#include <utils/oneMKLUtils.h>
#include "comm/ATDispatch.h"
#include "comm/RegistrationDeclarations.h"
#include "utils/ComputeEngine.h"
#include "xetla/hgemm.h"

using namespace xpu::xetla;

#define RECORD_FUNCTION_IMPL(F, M, N, K)            \
  char str__[100];                                  \
  sprintf(str__, "%s(%d, %d, %d)", "" #F, M, N, K); \
  RECORD_FUNCTION(str__, c10::ArrayRef<c10::IValue>({}));

class HGEMM_XETLA final {
 public:
  enum EpilogueType {
    BIAS = 0,
    RES_ADD,
    RELU,
    GELU,
    RES_MUL,
    SILU,
  };

 private:
  enum {
    MAX_EPILOGUES = 4,
  };
  c10::MaybeOwned<Tensor> c_, a_, b_;
  SmallVector<c10::MaybeOwned<Tensor>, MAX_EPILOGUES> epilogue_tensors_;
  EpilogueType epilogue_types_[MAX_EPILOGUES];
  float epilogue_params_[MAX_EPILOGUES];
  int num_epilogues_ = 0;
  bool is_a_row_major_;
  bool is_a_col_major_;
  bool is_b_row_major_;
  bool is_b_col_major_;
  bool valid_ = false;
  int m_, n_, k_;
  float alpha_ = 1.0f;

 public:
  HGEMM_XETLA() = default;
  bool valid() const {
    return valid_;
  }
  HGEMM_XETLA& add_alpha(const float alpha) {
    alpha_ = alpha;
    return *this;
  }
  HGEMM_XETLA& add_matrix_c(const Tensor& c) {
    c_ = c10::MaybeOwned<Tensor>::borrowed(c);
    return *this;
  }
  HGEMM_XETLA& add_matrix_a(const Tensor& a) {
    a_ = c10::MaybeOwned<Tensor>::borrowed(a);
    return *this;
  }
  HGEMM_XETLA& add_matrix_b(const Tensor& b) {
    b_ = c10::MaybeOwned<Tensor>::borrowed(b);
    return *this;
  }
  HGEMM_XETLA& add_epilogue(
      const Tensor& t,
      const EpilogueType eptype,
      const float x = 1.0) {
    epilogue_tensors_.push_back(c10::MaybeOwned<Tensor>::borrowed(t));
    epilogue_params_[num_epilogues_] = x;
    epilogue_types_[num_epilogues_++] = eptype;
    return *this;
  }
  HGEMM_XETLA& build() {
#define __CHECK(X) \
  if (!(X))        \
    return *this;
    __CHECK(
        a_->scalar_type() == kHalf && b_->scalar_type() == kHalf &&
        c_->scalar_type() == kHalf);

    DeviceId curDevID;
    AT_DPCPP_CHECK(dpcppGetDevice(&curDevID));
    __CHECK(Settings::I().has_2d_block_array(curDevID));

    using scalar_t =
        decltype(c10::impl::ScalarTypeToCPPType<ScalarType::Half>::t);
    __CHECK(reinterpret_cast<uint64_t>(c_->data_ptr<scalar_t>()) % 8 == 0);
    __CHECK(reinterpret_cast<uint64_t>(a_->data_ptr<scalar_t>()) % 8 == 0);
    __CHECK(reinterpret_cast<uint64_t>(b_->data_ptr<scalar_t>()) % 8 == 0);
    auto a_for_gemm = a_->flatten(0, -2);
    auto c_for_gemm = c_->flatten(0, -2);
    auto a_sizes = a_for_gemm.sizes();
    auto b_sizes = b_->sizes();
    auto c_sizes = c_for_gemm.sizes();
    m_ = a_sizes[0];
    k_ = a_sizes[1];
    n_ = b_sizes[1];
    __CHECK(k_ % 4 == 0 && n_ % 4 == 0);
    __CHECK((m_ == 1028) || (!(m_ > 1000 && n_ > 8192)));
    __CHECK(
        b_->dim() == 2 && b_sizes[0] == k_ && c_sizes[0] == m_ &&
        c_sizes[1] == n_);

    is_a_row_major_ = a_->is_contiguous();
    is_a_col_major_ = a_->transpose(0, 1).is_contiguous();
    is_b_row_major_ = b_->is_contiguous();
    is_b_col_major_ = b_->transpose(0, 1).is_contiguous();
    __CHECK(is_a_row_major_);
    __CHECK(is_b_row_major_);

    // check the input tensor
    xpu::COMPUTE_ENG real_eng =
        choose_compute_eng(xpu::COMPUTE_ENG::XETLA, *a_, *b_);
    __CHECK(real_eng == xpu::COMPUTE_ENG::XETLA);

    for (int i = 0; i < num_epilogues_; i++) {
      auto eptensor = epilogue_tensors_[i];
      // check all epilogues
      xpu::COMPUTE_ENG real_eng =
          choose_compute_eng(xpu::COMPUTE_ENG::XETLA, *eptensor);
      __CHECK(real_eng == xpu::COMPUTE_ENG::XETLA);

      switch (epilogue_types_[i]) {
        case BIAS: {
          __CHECK(
              eptensor->is_contiguous() && eptensor->scalar_type() == kHalf);
          __CHECK(eptensor->numel() == n_);
        } break;
        case RES_MUL:
        case RES_ADD: {
          __CHECK(
              eptensor->is_contiguous() && eptensor->scalar_type() == kHalf);
          auto eptensor_for_gemm = eptensor->flatten(0, -2);
          auto epsizes = eptensor_for_gemm.sizes();
          __CHECK(epsizes[0] == m_ && epsizes[1] == n_);
        } break;
        case RELU:
        case GELU:
        case SILU: {
        } break;
      }
    }
    valid_ = true;
    return *this;
#undef __CHECK
  }

  xpu::xetla::GemmStatus run() {
    using scalar_t =
        decltype(c10::impl::ScalarTypeToCPPType<ScalarType::Half>::t);
    auto& q = dpcppGetCurrentQueue();

    xpu::xetla::GemmStatus status;

    if (num_epilogues_ == 0) {
      RECORD_FUNCTION_IMPL(hgemm_common, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_common(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          is_b_row_major_);
    } else if (num_epilogues_ == 1 && epilogue_types_[0] == RES_ADD) {
      if (alpha_ == 1.0f) {
        RECORD_FUNCTION_IMPL(hgemm_res, m_, n_, k_)
        status = hgemm_res(
            q,
            reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(
                epilogue_tensors_[0]->data_ptr<scalar_t>()),
            m_,
            n_,
            k_,
            epilogue_params_[0],
            is_b_row_major_);
      } else {
        RECORD_FUNCTION_IMPL(hgemm_addmm, m_, n_, k_)
        status = hgemm_addmm(
            q,
            reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(
                epilogue_tensors_[0]->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
            reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
            m_,
            n_,
            k_,
            alpha_,
            epilogue_params_[0],
            is_b_row_major_);
      }
    } else if (
        num_epilogues_ == 2 && epilogue_types_[0] == RES_ADD &&
        epilogue_types_[1] == RES_ADD) {
      RECORD_FUNCTION_IMPL(hgemm_res_res, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_res_res(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[1]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          epilogue_params_[1],
          is_b_row_major_);
    } else if (num_epilogues_ == 1 && epilogue_types_[0] == BIAS) {
      RECORD_FUNCTION_IMPL(hgemm_bias, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_bias(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          is_b_row_major_);
    } else if (
        num_epilogues_ == 2 && epilogue_types_[0] == BIAS &&
        epilogue_types_[1] == RES_ADD) {
      RECORD_FUNCTION_IMPL(hgemm_bias_res, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_bias_res(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[1]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          epilogue_params_[1],
          is_b_row_major_);
    } else if (
        num_epilogues_ == 3 && epilogue_types_[0] == BIAS &&
        epilogue_types_[1] == RES_ADD && epilogue_types_[2] == RES_ADD) {
      RECORD_FUNCTION_IMPL(hgemm_bias_res_res, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_bias_res_res(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[1]->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[2]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          epilogue_params_[1],
          epilogue_params_[2],
          is_b_row_major_);
    } else if (
        num_epilogues_ == 2 && epilogue_types_[0] == BIAS &&
        epilogue_types_[1] == RELU) {
      RECORD_FUNCTION_IMPL(hgemm_bias_relu, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_bias_relu(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          is_b_row_major_);
    } else if (
        num_epilogues_ == 2 && epilogue_types_[0] == BIAS &&
        epilogue_types_[1] == GELU) {
      RECORD_FUNCTION_IMPL(hgemm_bias_gelu, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_bias_gelu(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          epilogue_params_[0],
          is_b_row_major_);
    } else if (num_epilogues_ == 1 && epilogue_types_[0] == RES_MUL) {
      RECORD_FUNCTION_IMPL(hgemm_resmul, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_resmul(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(
              epilogue_tensors_[0]->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          is_b_row_major_);
    } else if (num_epilogues_ == 1 && epilogue_types_[0] == SILU) {
      RECORD_FUNCTION_IMPL(hgemm_silu, m_, n_, k_)
      TORCH_CHECK(alpha_ == 1.0f);
      status = hgemm_silu(
          q,
          reinterpret_cast<sycl::half*>(c_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(a_->data_ptr<scalar_t>()),
          reinterpret_cast<sycl::half*>(b_->data_ptr<scalar_t>()),
          m_,
          n_,
          k_,
          is_b_row_major_);
    } else {
      TORCH_CHECK(false, "No mateched policy");
    }
    return status;
  };

  inline Tensor matmul_resize(const Tensor& a, const Tensor& output) {
    auto output_ = output.flatten(0, -2);
    int n = output_.sizes()[1];
    auto sizes = a.sym_sizes().vec();
    sizes[sizes.size() - 1] = n;
    return output.view_symint(sizes);
  }

  inline std::vector<std::tuple<int, int>> hgemm_split_m(
      const int m,
      const int n) {
    std::vector<std::tuple<int, int>> res;
    constexpr int slice_n = 4096;
    if (m > 4096 && n >= 4096) {
      for (int start_idx = 0; start_idx < m; start_idx += slice_n) {
        int remaining = m - start_idx;
        int len = slice_n < remaining ? slice_n : remaining;
        res.push_back(std::make_tuple(start_idx, len));
      }
    } else {
      res.push_back(std::make_tuple(0, m));
    }
    return res;
  }
};

inline Tensor matmul_resize(const Tensor& a, const Tensor& output) {
  auto output_ = output.flatten(0, -2);
  int n = output_.sizes()[1];
  auto sizes = a.sym_sizes().vec();
  sizes[sizes.size() - 1] = n;
  return output.view_symint(sizes);
}

inline std::vector<std::tuple<int, int>> hgemm_split_m(
    const int m,
    const int n) {
  std::vector<std::tuple<int, int>> res;
  constexpr int slice_n = 4096;
  if (m > 4096 && n >= 4096) {
    for (int start_idx = 0; start_idx < m; start_idx += slice_n) {
      int remaining = m - start_idx;
      int len = slice_n < remaining ? slice_n : remaining;
      res.push_back(std::make_tuple(start_idx, len));
    }
  } else {
    res.push_back(std::make_tuple(0, m));
  }
  return res;
}

#undef RECORD_FUNCTION_IMPL

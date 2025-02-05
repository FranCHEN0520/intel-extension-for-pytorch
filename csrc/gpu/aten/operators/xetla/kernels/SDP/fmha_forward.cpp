
/*
Fused Multi-Head Attention Forward

This is an implementation of the Flash Attention algorithm
(see: Dao et al., https://arxiv.org/pdf/2205.14135v2.pdf)
*/

#include <utils/DPCPP.h>
#include <limits>
#include "../../../comm/AccumulateType.h"
#include "../../mha.h"
#include "fmha_forward_policy.h"
#include "fmha_utils.h"
// Set to 1 to get raw output, not permuted
#define _RAW_OUTPUT 1

namespace gpu::xetla {

namespace fmha {

template <
    typename fmha_policy,
    typename scalar_t,
    bool kUseAlibi,
    bool kUseBias,
    bool kIsCausal,
    bool kSeqLast,
    bool kIsTraining,
    bool kIsDropout>
class fmha_forward_t {
 public:
  using accum_t = float;
  static constexpr accum_t kNegInfinity = INFINITY * -1;

  struct arguments_t {
    // Input tensors
    scalar_t* Q_ptr; // [B, F, N, H] - query
    scalar_t* K_ptr; // [B, T, N, H] - key
    scalar_t* V_ptr; // [B, T, N, H] - value
    scalar_t* A_ptr = nullptr; // [B, N, 1, T] - Alibi
    scalar_t* B_ptr = nullptr; // [B, 1, F, T] - bias
    uint8_t* Dp_ptr = nullptr; // [B, N, F, T] - dropout mask
    // Output tensor
    scalar_t* O_ptr; // raw: [B, F, N, H]; permute: [B, N, F, H] - output
    accum_t* L_ptr; // logsum softmax: [B, N, F]
    // Dimension size
    uint32_t uB;
    uint32_t uN;
    uint32_t uH;
    uint32_t uF;
    uint32_t uT;
    // Softmax scale is the reciprocal square root of head size by default
    accum_t sm_scale;

    // Dropout scale is computed from dropout prob
    accum_t dp_prob;
    accum_t dp_scale;
    uint32_t uAT;
    uint32_t uMT;
    uint64_t seed;
    uint64_t offset;

    inline arguments_t() = default;
    inline arguments_t(
        scalar_t* query,
        scalar_t* key,
        scalar_t* value,
        scalar_t* alibi,
        scalar_t* bias,
        uint8_t* dropout,
        scalar_t* out,
        accum_t* logsumsoftmax,
        uint32_t num_batches,
        uint32_t num_heads,
        uint32_t head_size,
        uint32_t num_queries,
        uint32_t num_keys,
        accum_t sm_scale,
        accum_t dropout_prob,
        uint32_t alibi_padded_block_size,
        uint32_t attn_mask_padded_block_size,
        uint64_t seed_t,
        uint64_t offset_t)
        : Q_ptr(query),
          K_ptr(key),
          V_ptr(value),
          A_ptr(alibi),
          B_ptr(bias),
          Dp_ptr(dropout),
          O_ptr(out),
          L_ptr(logsumsoftmax),
          uB(num_batches),
          uN(num_heads),
          uH(head_size),
          uF(num_queries),
          uT(num_keys),
          sm_scale(sm_scale),
          dp_prob(dropout_prob),
          dp_scale(1.f / (1.f - dropout_prob)),
          uAT(alibi_padded_block_size),
          uMT(attn_mask_padded_block_size),
          seed(seed_t),
          offset(offset_t) {}
  };

 private:
  // -------------------- // Compute policy // -------------------- //
  static constexpr uint32_t accum_step = fmha_policy::accum_step;
  static constexpr uint32_t stages = fmha_policy::stages;
  static constexpr uint32_t sync_freq = fmha_policy::sync_freq;

  using compute_attr = group::compute_attr_t<scalar_t, scalar_t, accum_t>;
  using perf_tuning_knob =
      group::perf_tuning_knob_t<accum_step, stages, sync_freq>;
  using compute_policy = group::
      compute_policy_default_xmx<compute_attr, perf_tuning_knob, gpu_arch::Xe>;
  // ---------------- // Tile shape and Threads // ---------------- //
  static constexpr uint32_t kBr = fmha_policy::kBr;
  static constexpr uint32_t kBc = fmha_policy::kBc;
  static constexpr uint32_t kHm = fmha_policy::kHm;
  static constexpr uint32_t kSgBr = fmha_policy::kSgBr;
  static constexpr uint32_t kSgBc = fmha_policy::kSgBc;
  static constexpr uint32_t kSgHm = fmha_policy::kSgHm;

  using tile_shape_BrBc = group::tile_shape_t<kBc, kBr, kSgBc, kSgBr>;
  using tile_shape_BrHm = group::tile_shape_t<kHm, kBr, kSgHm, kSgBr>;

  static constexpr uint32_t wg_size_x = tile_shape_BrBc::wg_size_x;
  static constexpr uint32_t wg_size_y = tile_shape_BrBc::wg_size_y;
  using work_group_t = typename tile_shape_BrBc::work_group_t;
  static constexpr uint32_t wg_size = work_group_t::size;

  static_assert(
      kHm / kSgHm == kBc / kSgBc,
      "wg_size_x must be the same between Hm and Bc");
  static_assert(wg_size <= 32, "The number of threads should be less than 32!");

  // --------------------- // Memory desc // ---------------------- //
  // suffix: L -> local; T -> transpose
  using mem_desc_Qi_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::global>;
  using mem_desc_Qi_L_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::local>;
  using mem_desc_Kj_T_t =
      mem_desc_t<scalar_t, mem_layout::col_major, mem_space::global>;
  using mem_desc_Pij_L_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::local>;
  using mem_desc_Vj_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::global>;
  using mem_desc_Bij_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::global>;
  using mem_desc_Oi_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::global>;
  using mem_desc_Ai_t =
      mem_desc_t<scalar_t, mem_layout::row_major, mem_space::global>;

  using mem_desc_Li_t =
      mem_desc_t<accum_t, mem_layout::row_major, mem_space::global>;

  // ------------------- // Slm and nbarrier // ------------------- //
  static constexpr uint32_t slm_size_Qi = kBr * kHm * sizeof(scalar_t);
  static constexpr uint32_t slm_size_Pij = kBr * kBc * sizeof(scalar_t);
  static constexpr uint32_t slm_size_softmax =
      (wg_size_x > 1) ? wg_size * kSgBr * sizeof(accum_t) : 0;
  // Slm addr to store inermediate results
  static constexpr uint32_t Qi_slm = 0;
  static constexpr uint32_t Pij_slm = Qi_slm + slm_size_Qi;
  static constexpr uint32_t softmax_slm = Pij_slm + slm_size_Pij;

  static constexpr uint32_t nbarrier_cnt = (wg_size_x > 1) ? wg_size_y : 0;
  using brgemm_Sij_t = group::brgemm_t<
      compute_policy,
      tile_shape_BrBc,
      mem_desc_Qi_L_t,
      mem_desc_Kj_T_t>;
  using matAccSij_t = typename brgemm_Sij_t::matAcc_t;
  using dropout_t = dropout_fwd_t<matAccSij_t::tile_elems>;

  // ======================== // Context // ======================= //

  /// @brief Used to store variables in the flash mha loops
  struct context_t {
    // thread id
    work_group_t g;
    uint32_t sg_idx;
    uint32_t sg_idy;
    // nbarrier
    xetla_nbarrier_t<wg_size_x, wg_size_x> nbarrier;
    // softmax statistics
    xetla_vector<accum_t, kSgBr> softmax_m;
    xetla_vector<accum_t, kSgBr> softmax_l;
    // mem desc variables
    mem_desc_Qi_t mem_desc_Qi;
    mem_desc_Qi_L_t mem_desc_Qi_L;
    mem_desc_Kj_T_t mem_desc_Kj_T;
    mem_desc_Pij_L_t mem_desc_Pij_L;
    mem_desc_Vj_t mem_desc_Vj;
    mem_desc_Bij_t mem_desc_Bij;
    mem_desc_Oi_t mem_desc_Oi;
    mem_desc_Ai_t mem_desc_Ai;
    mem_desc_Li_t mem_desc_Li;
    dropout_t dropout_op;

    inline context_t() = default;

    /// @brief Initialize invariant variables in the flash mha loop
    inline void init_context(xetla_exec_item<3>& ei, arguments_t& args) {
      // thread id
      uint32_t sg_id = ei.get_local_linear_id();
      g.init(sg_id);
      sg_idx = sg_id % wg_size_x;
      sg_idy = sg_id / wg_size_x;
      // nbarrier
      nbarrier.init_nbarrier(sg_idy, nbarrier_role::producer_consumer);
      // softmax statistics
      softmax_m = kNegInfinity;
      softmax_l = 0.f;

      // mem desc variables
      uint32_t gid = ei.get_group(0);
      uint32_t batch_id = gid / args.uN; // get batch idx
      uint32_t head_id = gid % args.uN; // get head idx

      if constexpr (kSeqLast) { // 2d mem: [F, BxNxH]
        // startF
        int32_t start_y = ei.get_group(1) * kBr;
        uint32_t end_y = start_y + kBr;
        // boundaryF
        uint32_t boundary_y = args.uF;
        end_y = end_y > boundary_y ? boundary_y : end_y;

        // XXX: code-gen crash
        uint32_t b_stride = args.uN * args.uH;
        int32_t start_acc = batch_id * b_stride + head_id * args.uH;
        uint32_t end_x = start_acc + args.uH;

        mem_desc_Qi.init(
            args.Q_ptr,
            {end_x, end_y, b_stride * args.uB},
            {start_acc, start_y});
        mem_desc_Oi.init(
            args.O_ptr,
            {end_x, end_y, b_stride * args.uB},
            {start_acc, start_y});
      } else { // 2d mem: [BxF, NxH]
        // startF
        int32_t start_y = batch_id * args.uF + ei.get_group(1) * kBr;
        uint32_t end_y = start_y + kBr;
        // boundaryF
        uint32_t boundary_y = (batch_id + 1) * args.uF;
        end_y = end_y > boundary_y ? boundary_y : end_y;

        int32_t start_acc = head_id * args.uH;

        mem_desc_Qi.init(
            args.Q_ptr,
            {args.uH * args.uN, end_y, args.uH * args.uN},
            {start_acc, start_y});
        mem_desc_Oi.init(
            args.O_ptr,
            {args.uH * args.uN, end_y, args.uH * args.uN},
            {start_acc, start_y});
      }

      int32_t start_x_ml = ei.get_group(1) * kBr + sg_idy * kSgBr;
      int32_t start_y_ml = gid;
      mem_desc_Li.init(
          args.L_ptr,
          {args.uF, args.uB * args.uN, args.uF},
          {start_x_ml, start_y_ml});
      mem_desc_Qi_L.init(Qi_slm, {kHm, kBr, kHm}, {0, 0});
      mem_desc_Pij_L.init(Pij_slm, {kBc, kBr, kBc}, {0, 0});
    }

    /// @brief Update variables for each flash mha loop
    inline void update_context(
        xetla_exec_item<3>& ei,
        arguments_t& args,
        uint32_t startT) {
      uint32_t gid = ei.get_group(0);
      uint32_t batch_id = gid / args.uN; // get batch idx
      uint32_t head_id = gid % args.uN; // get head idx

      // TODO: what's this startT for

      if constexpr (kSeqLast) {
        int32_t start_x = startT;
        uint32_t end_x = start_x + kBc;
        uint32_t boundary_x = args.uT;
        end_x = end_x > boundary_x ? boundary_x : end_x;

        // XXX: code-gen crash
        uint32_t b_stride = args.uN * args.uH;
        int32_t start_acc = batch_id * b_stride + head_id * args.uH;
        uint32_t end_y = start_acc + args.uH;

        mem_desc_Kj_T.init(
            args.K_ptr,
            {end_x, end_y, b_stride * args.uB},
            {start_x, start_acc});
        mem_desc_Vj.init(
            args.V_ptr,
            {end_y, end_x, b_stride * args.uB},
            {start_acc, start_x});
      } else {
        int32_t start_x = batch_id * args.uT + startT;
        uint32_t end_x = start_x + kBc;
        uint32_t boundary_x = (batch_id + 1) * args.uT;
        end_x = end_x > boundary_x ? boundary_x : end_x;

        int32_t start_acc = head_id * args.uH;

        mem_desc_Kj_T.init(
            args.K_ptr,
            {end_x, args.uH * args.uN, args.uH * args.uN},
            {start_x, start_acc});
        mem_desc_Vj.init(
            args.V_ptr,
            {args.uH * args.uN, end_x, args.uH * args.uN},
            {start_acc, start_x});
      }

      // B, N, 1, T
      // gid * T + startT
      if constexpr (kUseAlibi) {
        int32_t batch_start = gid * args.uAT;
        int32_t start_x = batch_start + startT;
        uint32_t end_x = startT + kBc;
        uint32_t boundary_x = args.uT;
        end_x = end_x > boundary_x ? boundary_x : end_x;
        end_x += batch_start;

        mem_desc_Ai.init(
            args.A_ptr, {end_x, 1, args.uAT * args.uN * args.uB}, {start_x, 0});
      }

      if constexpr (kUseBias && !kIsCausal) {
        int32_t start_x = startT;
        uint32_t end_x = start_x + kBc;
        uint32_t boundary_x = args.uMT;
        end_x = end_x > boundary_x ? boundary_x : end_x;

        int32_t start_y = batch_id * args.uF + ei.get_group(1) * kBr;
        uint32_t end_y = start_y + kBr;
        uint32_t boundary_y = (batch_id + 1) * args.uF;
        end_y = end_y > boundary_y ? boundary_y : end_y;

        mem_desc_Bij.init(
            args.B_ptr, {end_x, end_y, args.uMT}, {start_x, start_y});
      }

      // B, 1, 1, T
      // batch_id * T + startT
      if constexpr (kUseBias && kIsCausal) {
        int32_t batch_start = batch_id * args.uMT;
        int32_t start_x = batch_start + startT;
        uint32_t end_x = startT + kBc;
        uint32_t boundary_x = args.uT;
        end_x = end_x > boundary_x ? boundary_x : end_x;
        end_x += batch_start;

        mem_desc_Bij.init(
            args.B_ptr, {end_x, 1, args.uMT * args.uB}, {start_x, 0});
      }

      if constexpr (kIsDropout) {
        int coord_y = ei.get_group(1) * kBr + sg_idy * kSgBr;
        int coord_x = startT + sg_idx * kSgBc;
        uint64_t sg_subseq = uint64_t(coord_y) << 32 | uint64_t(coord_x);
        uint32_t threshold = uint32_t(args.dp_prob * float(4294967296));
        dropout_op.init(
            args.seed, sg_subseq, args.offset, threshold, args.dp_scale);
      }
    }
  };

  context_t ctx;

  // ======================= // gemm_Sij // ======================= //
  // Define kernel to compute Sij = Qi x Kj.T
  /// @brief gemm_Sij is used to compute Sij = Qi x Kj.T
  /// # [Br,H] x [H,Bc] = [Br,Bc]
  inline void gemm_Sij(matAccSij_t& matAccSij, arguments_t& args) {
    using brgemm_args_t = typename brgemm_Sij_t::arguments_t;

    // Gemm to comput Sij
    brgemm_Sij_t brgemm;
    uint32_t loop_count = (args.uH + accum_step - 1) / accum_step;
    brgemm_args_t brgemm_args(ctx.mem_desc_Qi_L, ctx.mem_desc_Kj_T, loop_count);
    brgemm(ctx.g, matAccSij, brgemm_args, 0, /* nbarrier_base */ nbarrier_cnt);

    // Multiply by softmax scaling factor
    // bmm * alpha
    matAccSij.reg *= args.sm_scale;

    // + beta * alibi
    if constexpr (kUseAlibi) {
      using alibi_op_t = subgroup::bias_add_op_t<scalar_t, gpu_arch::Xe>;
      using alibi_args_t = typename alibi_op_t::arguments_t;

      int32_t tile_offset_x = ctx.sg_idx * kSgBc;
      int32_t tile_offset_y = 0;
      ctx.mem_desc_Ai.update_coord(tile_offset_x, tile_offset_y);

      alibi_op_t alibi_op;
      alibi_args_t alibi_args(ctx.mem_desc_Ai.base, ctx.mem_desc_Ai.shape);
      alibi_op(matAccSij, ctx.mem_desc_Ai.coord, alibi_args);
    }

    // Add attn_mask if needed
    if constexpr (kUseBias && !kIsCausal) {
      using bias_op_t = subgroup::
          elemwise_reduce_op_t<reduce_op::sum, scalar_t, gpu_arch::Xe>;
      using bias_args_t = typename bias_op_t::arguments_t;

      int32_t tile_offset_x = ctx.sg_idx * kSgBc;
      int32_t tile_offset_y = ctx.sg_idy * kSgBr;
      ctx.mem_desc_Bij.update_coord(tile_offset_x, tile_offset_y);

      bias_op_t bias_op;
      bias_args_t bias_args(ctx.mem_desc_Bij.base, ctx.mem_desc_Bij.shape);
      bias_op(matAccSij, ctx.mem_desc_Bij.coord, bias_args);
    }

    // Add attn_mask if needed
    if constexpr (kUseBias && kIsCausal) {
      using bias_op_t = subgroup::bias_add_op_t<scalar_t, gpu_arch::Xe>;
      using bias_args_t = typename bias_op_t::arguments_t;

      int32_t tile_offset_x = ctx.sg_idx * kSgBc;
      int32_t tile_offset_y = 0;
      ctx.mem_desc_Bij.update_coord(tile_offset_x, tile_offset_y);

      bias_op_t bias_op;
      bias_args_t bias_args(ctx.mem_desc_Bij.base, ctx.mem_desc_Bij.shape);
      bias_op(matAccSij, ctx.mem_desc_Bij.coord, bias_args);
    }
  }
  // ======================= // gemm_Oi // ======================= //
  // Define kernel to compute Oi += Pij x Vj
  using brgemm_Oi_t = group::brgemm_t<
      compute_policy,
      tile_shape_BrHm,
      mem_desc_Pij_L_t,
      mem_desc_Vj_t>;
  using matAccOi_t = typename brgemm_Oi_t::matAcc_t;

  /// @brief gemm_Oi is used to compute Oi += Pij x Vj
  /// # [Br,Bc] x [Bc,H] = [Br,Hm]
  inline void gemm_Oi(
      matAccOi_t& matAccOi,
      arguments_t& args,
      uint32_t startT) {
    using brgemm_args_t = typename brgemm_Oi_t::arguments_t;

    uint32_t remainT = args.uT - startT;
    uint32_t boundary_k = remainT > kBc ? kBc : remainT;
    uint32_t loop_count = (boundary_k + accum_step - 1) / accum_step;

    // Gemm to comput Oi
    brgemm_Oi_t brgemm;
    brgemm_args_t brgemm_args(ctx.mem_desc_Pij_L, ctx.mem_desc_Vj, loop_count);
    brgemm(ctx.g, matAccOi, brgemm_args, 0, /* nbarrier_base */ nbarrier_cnt);
  }

  // ====================== // apply_mask // ====================== //

  /// @brief apply mask to matAccSij.
  inline void apply_mask(
      matAccSij_t& matAccSij,
      arguments_t& args,
      uint32_t startF,
      uint32_t startT) {
    using tile_mask = tile_mask_t<matAccSij_t>;

    uint32_t sg_startT = startT + ctx.sg_idx * kSgBc;
    uint32_t remainT = std::max(int(args.uT) - int(sg_startT), 0);
    if (remainT < kSgBc) {
      tile_mask::padding_mask(matAccSij, remainT);
    }

    if constexpr (kIsCausal) {
      uint32_t sg_startF = startF + ctx.sg_idy * kSgBr;
      if (sg_startT + kSgBc > sg_startF) {
        tile_mask::causal_mask(matAccSij, sg_startT, sg_startF);
      }
    }
  }
  // ====================== // softmax_fwd // ===================== //

  /// @brief softmax_fwd is used to do softmax.
  inline void softmax_fwd(
      matAccSij_t& matAccSij,
      matAccOi_t& matAccOi,
      arguments_t& args) {
    using wg_row_max_t =
        group_row_reduce_t<matAccSij_t, wg_size_x, reduce_op::max>;
    using wg_row_sum_t =
        group_row_reduce_t<matAccSij_t, wg_size_x, reduce_op::sum>;

    // init slm address for group reducer
    uint32_t reducer_slm =
        softmax_slm + ctx.sg_idy * wg_size_x * kSgBr * sizeof(accum_t);

    // compute new m
    wg_row_max_t wg_row_max(ctx.sg_idx, ctx.sg_idy, reducer_slm);
    xetla_vector<accum_t, kSgBr> m_new = wg_row_max(matAccSij);
    m_new = xetla_max<accum_t, kSgBr>(m_new, ctx.softmax_m);

    if constexpr (wg_size_x > 1)
      ctx.nbarrier.arrive();

    // correct old l
    ctx.softmax_l *= xetla_exp<accum_t, kSgBr>(ctx.softmax_m - m_new);
    // compute Pij
    subgroup::tile_broadcast_op<subgroup::tile_minus, matAccSij_t>(
        matAccSij, m_new);
    // matAccSij.reg = xetla_exp<accum_t>(matAccSij.reg);

    matAccSij_t mat_zeros(0);
    constexpr int elems = matAccSij_t::tile_desc::tile_elems;
    // xetla_mask<elems> mask = matAccSij->reg < (INFINITY * -1) ||
    // matAccSij->reg > INFINITY;
    xetla_mask<elems> mask = matAccSij.reg < -65400.f;
    (matAccSij.reg)
        .xetla_merge(mat_zeros.reg, xetla_exp<accum_t>(matAccSij.reg), mask);

    if constexpr (wg_size_x > 1)
      ctx.nbarrier.wait();

    // compute new l
    wg_row_sum_t wg_row_sum(ctx.sg_idx, ctx.sg_idy, reducer_slm);
    xetla_vector<accum_t, kSgBr> l_new = wg_row_sum(matAccSij);
    l_new += ctx.softmax_l;

    // rescale operands of matmuls
    subgroup::tile_broadcast_op<subgroup::tile_div, matAccSij_t>(
        matAccSij, l_new);
    xetla_vector<accum_t, kSgBr> o_scale = l_new / ctx.softmax_l;
    subgroup::tile_broadcast_op<subgroup::tile_div, matAccOi_t>(
        matAccOi, o_scale);
    // update m and l for the next step
    ctx.softmax_m = m_new;
    ctx.softmax_l = l_new;

    if constexpr (kIsTraining) {
      // TODO: save m and l to global
    }

    if constexpr (kIsDropout) {
      matAccSij.reg = ctx.dropout_op.template process<float>(matAccSij.reg);
    }

    // save Pij to local memory
    using epilogue_t = group::epilogue_t<
        group::epilogue_policy_default<result_overwrite, gpu_arch::Xe>,
        tile_shape_BrBc,
        mem_desc_Pij_L_t>;
    epilogue_t epilogue;
    epilogue(ctx.g, matAccSij, ctx.mem_desc_Pij_L);
    xetla_fence<memory_kind::shared_local>();
    if constexpr (wg_size_x > 1)
      ctx.nbarrier.arrive_wait();
  }

  // ==================== // store_logsumsoftmax // ====================== //

  /// @brief store logsumsoftmax to global memory. [B,N,F]
  inline void store_for_backward(const arguments_t& args) {
    // save m and l to global
    if constexpr (!kIsTraining) {
      return;
    }
    using store_desc =
        subgroup::tile_desc_t<kSgBr, 1, kSgBr, 1, reg_layout::tiled>;
    using store_tile_t = subgroup::tile_t<accum_t, store_desc>;
    // Note: use block_2d store as only block_2d supports boundary check
    using store_payload_t = subgroup::mem_payload_t<
        accum_t,
        store_desc,
        msg_type::block_2d,
        mem_layout::row_major,
        mem_space::global>;
    store_tile_t mat_store;
    store_payload_t store_payload(ctx.mem_desc_Li);
    mat_store.reg = ctx.softmax_m + sycl::ext::intel::esimd::log(ctx.softmax_l);
    if (ctx.sg_idx == 0) {
      subgroup::tile_store(mat_store, store_payload);
    }
  }
  // == == == == == == == == == == // raw_store_Oi // ====================== //

  /// @brief store raw Oi to global memory. [B,F,N,H]
  inline void raw_store_Oi(matAccOi_t& matAccOi, arguments_t& args) {
    using epilogue_t = group::epilogue_t<
        group::epilogue_policy_default<result_overwrite, gpu_arch::Xe>,
        tile_shape_BrHm,
        mem_desc_Oi_t>;
    epilogue_t epilogue;
    epilogue(ctx.g, matAccOi, ctx.mem_desc_Oi);
  }

  // ================== // permute_store_Oi // ==================== //

  /// @brief permuted store Oi to global memory. [B,N,F,H]
  inline void permute_store_Oi(
      xetla_exec_item<3>& ei,
      matAccOi_t& matAccOi,
      arguments_t& args) {
    uint32_t b = ei.get_group(0) / args.uN;
    uint32_t n = ei.get_group(0) % args.uN;
    uint32_t f = ctx.sg_idy * kSgBr + ei.get_group(1) * kBr;
    uint32_t h = ctx.sg_idx * kSgHm;

    // Because Hm is greater than uH
    if (h >= args.uH)
      return;

    xetla_tdescriptor transpose_tdecs;
    xetla_vector<scalar_t, kSgHm> v_out;

    uint32_t height = args.uB * args.uN * args.uF;
    uint32_t offset_height = b * args.uN * args.uF + f * args.uN + n;

    xetla_fill_tdesc<scalar_t, kSgHm, 1, 1>(
        transpose_tdecs.xetla_format<uint32_t>(),
        args.O_ptr,
        args.uH,
        height,
        args.uH,
        h,
        offset_height);

    for (uint32_t i = 0; i < kSgBr && (f + i < args.uF); ++i) {
      // load data from matAccOi
      auto v_acc = matAccOi.reg.xetla_select<kSgHm, 1>(i * kSgHm);
      v_out = xetla_cvt<scalar_t, accum_t, kSgHm>(v_acc);

      xetla_tstore_global<
          scalar_t,
          kSgHm,
          cache_hint::write_back,
          cache_hint::write_back>(transpose_tdecs, v_out);
      xetla_update_tdesc_offsety(
          transpose_tdecs.xetla_format<uint32_t>(), args.uN);
    }
  }
  // ====================== // preload_Qi // ====================== //

  /// @brief preload_Qi is used to load Qi from global to local memory.
  inline void preload_Qi(arguments_t& args) {
    using matQi_tile_desc_t = typename brgemm_Oi_t::matAcc_tile_desc_t;
    using matQi_t = subgroup::tile_t<scalar_t, matQi_tile_desc_t>;
    using matQi_load_t = subgroup::mem_payload_t<
        scalar_t,
        matQi_tile_desc_t,
        subgroup::msg_type_v<matQi_tile_desc_t, mem_desc_Qi_t::space>,
        mem_desc_Qi_t::layout,
        mem_desc_Qi_t::space,
        gpu_arch::Xe>;
    using matQi_store_t = subgroup::mem_payload_t<
        scalar_t,
        matQi_tile_desc_t,
        subgroup::msg_type_v<matQi_tile_desc_t, mem_desc_Qi_L_t::space>,
        mem_desc_Qi_L_t::layout,
        mem_desc_Qi_L_t::space,
        gpu_arch::Xe>;

    int32_t tile_offset_x = ctx.sg_idx * kSgHm;
    int32_t tile_offset_y = ctx.sg_idy * kSgBr;

    mem_desc_Qi_t mem_desc_Qi_load(ctx.mem_desc_Qi);
    mem_desc_Qi_L_t mem_desc_Qi_store(ctx.mem_desc_Qi_L);

    mem_desc_Qi_load.update_coord(tile_offset_x, tile_offset_y);
    mem_desc_Qi_store.update_coord(tile_offset_x, tile_offset_y);

    matQi_t matQi;
    matQi_load_t matQi_load(mem_desc_Qi_load);
    subgroup::tile_load(matQi, matQi_load);

    matQi_store_t matQi_store(mem_desc_Qi_store);
    subgroup::tile_store(matQi, matQi_store);

    xetla_fence<memory_kind::shared_local>();
    if constexpr (wg_size_x > 1)
      ctx.nbarrier.arrive_wait();
  }

 public:
  /// @brief Gets named_barrier id consumption count.
  /// Users query and get a named_barrier id consumption count in compile time.
  /// @return The count of named barriers required.
  inline static constexpr uint32_t get_barrier_count() {
    constexpr uint32_t barrier_count_Sij = brgemm_Sij_t::barrier_count;
    constexpr uint32_t barrier_count_Oi = brgemm_Oi_t::barrier_count;
    constexpr uint32_t count =
        std::max(barrier_count_Sij, barrier_count_Oi) + nbarrier_cnt;
    static_assert(
        count <= 32, "The named_barrier count should be less than 32!");
    return count;
  }

  /// @brief Gets local memory size consumption.
  /// Users query and get a local memory consumption size in compile time.
  /// @return The size of local memory required.
  inline static constexpr uint32_t get_slm_size() {
    constexpr uint32_t size = slm_size_Qi + slm_size_Pij + slm_size_softmax;
    static_assert(
        size <= (128 * 1024),
        "The local memory size should be less than 128KB!");
    return size;
  };

  /// @brief Helper function to get the nd_range under the Fmha policy.
  /// @return Expected nd_range.
  static sycl::nd_range<3> get_nd_range(
      uint32_t total_batches,
      uint32_t num_queries) {
    // local range
    sycl::range<3> local_range = sycl::range<3>{1, wg_size_y, wg_size_x};
    // group range
    uint32_t group_range_m = (num_queries + kBr - 1) / kBr;
    sycl::range<3> group_range =
        sycl::range<3>{total_batches, group_range_m, 1};
    return sycl::nd_range<3>{group_range * local_range, local_range};
  };
  // ================= // Entry of the functor // ================= //

  inline KERNEL_FUNC void operator()(
      xetla_exec_item<3>& ei,
      arguments_t& args) {
    // allocate slm and nbarrier resource
    xetla_local_init<get_slm_size()>();
    xetla_nbarrier_init<get_barrier_count()>();

    // initialize context for flash mha loops
    ctx.init_context(ei, args);
    // preload Qi to local memory
    preload_Qi(args);
    // initialize matAccOi for accumulate the output
    matAccOi_t matAccOi(0);

    uint32_t startF = ei.get_group(1) /* 0 */ * kBr /* 64 */;
    uint32_t endF = std::min(startF + kBr, args.uF);

    // iterate through the keys
    for (uint32_t startT = 0; startT < args.uT; startT += kBc) {
      if constexpr (kIsCausal) {
        if (startT >= endF)
          break;
      }
      // update context for current loop
      ctx.update_context(ei, args, startT);
      // compute Sij
      matAccSij_t matAccSij(0);
      gemm_Sij(matAccSij, args);
      // apply mask
      apply_mask(matAccSij, args, startF, startT);
      // softmax
      softmax_fwd(matAccSij, matAccOi, args);
      // compute Oi
      gemm_Oi(matAccOi, args, startT);
    }

    // Store output to global
#if _RAW_OUTPUT
    raw_store_Oi(matAccOi, args);
    store_for_backward(args);
#else
    permute_store_Oi(ei, matAccOi, args);
    store_for_backward(args);
#endif
  }
}; // fmha_forward_t

template <typename fmha_forward_op_t, typename T>
struct FmhaForwardKernelFunctor {
  SYCL_ESIMD_KERNEL void operator()(sycl::nd_item<3> item) const {
    // exec item
    xetla_exec_item<3> ei(item);

    // init fmha forward op and arguments
    fmha_forward_op_t fmha_fwd_op;
    using accscalar_t = fmha_forward_op_t::accum_t;
    typename fmha_forward_op_t::arguments_t args(
        query,
        key,
        value,
        alibi,
        bias,
        dropout,
        out,
        (accscalar_t*)log_sumexp,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        (accscalar_t)alpha,
        (accscalar_t)dropout_prob,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        seed_t,
        offset_t);

    // call the functor
    fmha_fwd_op(ei, args);
  }
  FmhaForwardKernelFunctor(
      T* query_,
      T* key_,
      T* value_,
      T* alibi_,
      T* bias_,
      uint8_t* dropout_,
      T* out_,
      void* log_sumexp_,
      uint32_t num_batches_,
      uint32_t num_heads_,
      uint32_t head_size_,
      uint32_t num_queries_,
      uint32_t num_keys_,
      float alpha_,
      float dropout_prob_,
      uint32_t alibi_padded_block_size_,
      uint32_t attn_mask_padded_block_size_,
      uint64_t seed_t_,
      uint64_t offset_t_)
      : query(query_),
        key(key_),
        value(value_),
        alibi(alibi_),
        bias(bias_),
        dropout(dropout_),
        out(out_),
        log_sumexp(log_sumexp_),
        num_batches(num_batches_),
        num_heads(num_heads_),
        head_size(head_size_),
        num_queries(num_queries_),
        num_keys(num_keys_),
        alpha(alpha_),
        dropout_prob(dropout_prob_),
        alibi_padded_block_size(alibi_padded_block_size_),
        attn_mask_padded_block_size(attn_mask_padded_block_size_),
        seed_t(seed_t_),
        offset_t(offset_t_) {}

 private:
  T* query;
  T* key;
  T* value;
  T* alibi;
  T* bias;
  uint8_t* dropout;
  T* out;
  void* log_sumexp;
  uint32_t num_batches;
  uint32_t num_heads;
  uint32_t head_size;
  uint32_t num_queries;
  uint32_t num_keys;
  float alpha;
  float dropout_prob;
  uint32_t alibi_padded_block_size;
  uint32_t attn_mask_padded_block_size;
  uint64_t seed_t;
  uint64_t offset_t;
};

// The launcher of fmha forward kernel
template <
    typename fmha_policy,
    typename T,
    bool kUseAlibi,
    bool kUseBias,
    bool kIsCausal,
    bool kSeqLast,
    bool kIsTraining,
    bool kIsDropout>
void xetla_fmha_forward_kernel(
    sycl::queue& q,
    T* query,
    T* key,
    T* value,
    T* alibi,
    T* bias,
    uint8_t* dropout,
    T* out,
    void* log_sumexp,
    float alpha,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    uint64_t seed_t,
    uint64_t offset_t) {
#ifdef SDP_DBG
  printf(
      "B, N, F, T, H: %d, %d, %d, %d, %d, UseAlibi: %d, UseBias: %d, IsCausal: %d, IsTraining: %d, IsDropout: %d, alibi @ 0x%llx, uAT %d, uMT %d, dropout_prob %f, kSeqLast %d\n",
      num_batches,
      num_heads,
      num_queries,
      num_keys,
      head_size,
      kUseAlibi,
      kUseBias,
      kIsCausal,
      kIsTraining,
      kIsDropout,
      (unsigned long long)alibi,
      alibi_padded_block_size,
      attn_mask_padded_block_size,
      dropout_prob,
      kSeqLast);
#endif
  // fmha forward kernel
  using fmha_forward_op_t = fmha_forward_t<
      fmha_policy,
      T,
      kUseAlibi,
      kUseBias,
      kIsCausal,
      kSeqLast,
      kIsTraining,
      kIsDropout>;

  sycl::nd_range<3> NdRange =
      fmha_forward_op_t::get_nd_range(num_batches * num_heads, num_queries);

  auto cgf = DPCPP_Q_CGF(cgh) {
    FmhaForwardKernelFunctor<fmha_forward_op_t, T> kfn(
        query,
        key,
        value,
        alibi,
        bias,
        dropout,
        out,
        log_sumexp,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        alpha,
        dropout_prob,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        seed_t,
        offset_t);
    cgh.parallel_for<decltype(kfn)>(NdRange, kfn);
  };
  DPCPP_Q_SUBMIT(q, cgf);
}

} // namespace fmha

#define CALL_IMPL_FUNC(P)          \
  fmha::xetla_fmha_forward_kernel< \
      P,                           \
      T,                           \
      kUseAlibi,                   \
      kUseBias,                    \
      kIsCausal,                   \
      kSeqLast,                    \
      kIsTraining,                 \
      kIsDropout>(                 \
      q,                           \
      query,                       \
      key,                         \
      value,                       \
      alibi,                       \
      bias,                        \
      dropout,                     \
      out,                         \
      log_sumexp,                  \
      alpha,                       \
      dropout_prob,                \
      num_batches,                 \
      num_heads,                   \
      head_size,                   \
      num_queries,                 \
      num_keys,                    \
      alibi_padded_block_size,     \
      attn_mask_padded_block_size, \
      seed_t,                      \
      offset_t)
/// @brief Main execution function for flash mha forward.

template <
    typename T,
    bool kUseAlibi = false,
    bool kUseBias = false,
    bool kIsCausal = false,
    bool kSeqLast = false,
    bool kIsTraining = false,
    bool kIsDropout = false>
void fmha_forward_kernel_policy(
    sycl::queue& q,
    T* query,
    T* key,
    T* value,
    T* alibi,
    T* bias,
    uint8_t* dropout,
    T* out,
    void* log_sumexp, // [bs * numhead * q_len]
    float alpha,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    uint64_t seed_t,
    uint64_t offset_t) {
  // Xetla dropout requires the same policy for fwd and bwd
  if (kIsTraining && kIsDropout) {
    if (head_size <= 64) {
      CALL_IMPL_FUNC(fmha_policy_64x64x64);
    } else if (head_size <= 128) {
      CALL_IMPL_FUNC(fmha_policy_128x128x128);
    } else if (head_size <= 256) {
      CALL_IMPL_FUNC(fmha_policy_128x128x256);
    } else {
      std::cout << "No policy available for current head_size " << head_size
                << "\n";
      return;
    }
  } else {
    if (head_size <= 64) {
      CALL_IMPL_FUNC(fmha_policy_64x128x64);
    } else if (head_size <= 128) {
      CALL_IMPL_FUNC(fmha_policy_64x128x128);
    } else if (head_size <= 256) {
      if (num_keys < 64) {
        CALL_IMPL_FUNC(fmha_policy_8x256x256);
      } else if (num_keys < 128) {
        CALL_IMPL_FUNC(fmha_policy_64x128x256);
      } else {
        CALL_IMPL_FUNC(fmha_policy_64x256x256);
      }
    } else {
      std::cout << "No policy available for current head_size " << head_size
                << "\n";
      return;
    }
  }
}

#undef CALL_IMPL_FUNC

template <typename T, bool... Bs>
void dispatch_fmha_forward(
    sycl::queue& q,
    T* query,
    T* key,
    T* value,
    T* alibi,
    T* attn_mask,
    uint8_t* dropout_mask,
    T* out,
    void* log_sumexp,
    float softmax_scale,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    uint64_t seed_t,
    uint64_t offset_t) {
  fmha_forward_kernel_policy<T, Bs...>(
      q,
      query,
      key,
      value,
      alibi,
      attn_mask,
      dropout_mask,
      out,
      log_sumexp,
      softmax_scale,
      dropout_prob,
      num_batches,
      num_heads,
      head_size,
      num_queries,
      num_keys,
      alibi_padded_block_size,
      attn_mask_padded_block_size,
      seed_t,
      offset_t);
}

// dispatch different conditions
template <typename T, bool... Bs, typename... Ts>
void dispatch_fmha_forward(
    sycl::queue& q,
    T* query,
    T* key,
    T* value,
    T* alibi,
    T* attn_mask,
    uint8_t* dropout_mask,
    T* out,
    void* log_sumexp,
    float softmax_scale,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    uint64_t seed_t,
    uint64_t offset_t,
    bool b,
    Ts... ts) {
  if (b) {
    dispatch_fmha_forward<T, Bs..., true>(
        q,
        query,
        key,
        value,
        alibi,
        attn_mask,
        dropout_mask,
        out,
        log_sumexp,
        softmax_scale,
        dropout_prob,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        seed_t,
        offset_t,
        ts...);
  } else {
    dispatch_fmha_forward<T, Bs..., false>(
        q,
        query,
        key,
        value,
        alibi,
        attn_mask,
        dropout_mask,
        out,
        log_sumexp,
        softmax_scale,
        dropout_prob,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        seed_t,
        offset_t,
        ts...);
  }
}

template <typename T>
void fmha_forward_kernel_impl(
    sycl::queue& q,
    T* query,
    T* key,
    T* value,
    T* alibi,
    T* attn_mask,
    uint8_t* dropout_mask,
    T* out,
    void* log_sumexp,
    float softmax_scale,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    bool is_causal,
    bool seq_last,
    bool is_training,
    bool is_dropout,
    uint64_t seed_t,
    uint64_t offset_t) {
  bool is_attn_mask = attn_mask ? true : false;
  bool is_alibi = alibi ? true : false;

  dispatch_fmha_forward<T>(
      q,
      query,
      key,
      value,
      alibi,
      attn_mask,
      (uint8_t*)dropout_mask,
      out,
      log_sumexp,
      softmax_scale,
      dropout_prob,
      num_batches,
      num_heads,
      head_size,
      num_queries,
      num_keys,
      alibi_padded_block_size,
      attn_mask_padded_block_size,
      seed_t,
      offset_t,
      is_alibi,
      is_attn_mask,
      is_causal,
      seq_last,
      is_training,
      is_dropout);
}

// dispatch datatype
void fmha_forward_kernel(
    XetlaType xeType,
    sycl::queue& q,
    void* query,
    void* key,
    void* value,
    void* alibi,
    void* attn_mask,
    void* dropout,
    void* out,
    void* log_sumexp,
    float alpha,
    float beta,
    float dropout_prob,
    uint32_t num_batches,
    uint32_t num_heads,
    uint32_t head_size,
    uint32_t num_queries,
    uint32_t num_keys,
    uint32_t alibi_padded_block_size,
    uint32_t attn_mask_padded_block_size,
    bool is_causal,
    bool seq_last,
    bool is_training,
    bool is_dropout,
    uint64_t seed_t,
    uint64_t offset_t) {
  if (xeType == XetlaType::fp16) {
    fmha_forward_kernel_impl<fp16>(
        q,
        (fp16*)query,
        (fp16*)key,
        (fp16*)value,
        (fp16*)alibi,
        (fp16*)attn_mask,
        (uint8_t*)dropout,
        (fp16*)out,
        log_sumexp,
        alpha,
        dropout_prob,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        is_causal,
        seq_last,
        is_training,
        is_dropout,
        seed_t,
        offset_t);
  } else {
    fmha_forward_kernel_impl<bf16>(
        q,
        (bf16*)query,
        (bf16*)key,
        (bf16*)value,
        (bf16*)alibi,
        (bf16*)attn_mask,
        (uint8_t*)dropout,
        (bf16*)out,
        log_sumexp,
        alpha,
        dropout_prob,
        num_batches,
        num_heads,
        head_size,
        num_queries,
        num_keys,
        alibi_padded_block_size,
        attn_mask_padded_block_size,
        is_causal,
        seq_last,
        is_training,
        is_dropout,
        seed_t,
        offset_t);
  }
}
} // namespace gpu::xetla

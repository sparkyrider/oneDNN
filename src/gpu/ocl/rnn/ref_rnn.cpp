/*******************************************************************************
* Copyright 2019-2023 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

// General architecture
//
// for diff states, we have n_states + 1 as we have n_states diff
// to propagate to the previous iteration and 1 states to propagate
// to the previous layer
// index 0 is dh for cell(t-1, l) to consume
// index 1 is dc for cell(t-1, l) to consume
// index 2 is dh for cell(t, l-1) to consume
// this indexing enables to have the same indexing for states in elemwise
// function
// only the cell execution function should be impacted

#include "gpu/ocl/rnn/ref_rnn.hpp"

#include "common/c_types_map.hpp"
#include "common/dnnl_traits.hpp"
#include "common/gemm_utils.hpp"
#include "common/math_utils.hpp"
#include "common/type_helpers.hpp"
#include "gpu/gemm/gpu_gemm.hpp"
#include "gpu/getenv_utils.hpp"
#include "gpu/gpu_primitive_attr.hpp"

static inline bool is_ws_print_enabled() {
    return get_verbose_dev_mode(dnnl::impl::verbose_t::debuginfo) >= 5;
}

#define DPRINT(fmt, ...) \
    do { \
        if (get_verbose_dev_mode(verbose_t::debuginfo) >= 2) { \
            printf(fmt, __VA_ARGS__); \
            fflush(nullptr); \
        } \
    } while (0)
#define WS_PRINT(c, s, w) \
    do { \
        if (is_ws_print_enabled()) { ws_print(c, s, w); } \
    } while (0)

namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

using namespace dnnl::impl::utils;
using namespace dnnl::impl::math;
using namespace prop_kind;
using namespace alg_kind;
using namespace rnn_utils;
using namespace dnnl::impl::memory_tracking::names;

#define AOC array_offset_calculator

static status_t init_conf(rnn_conf_t &conf, const rnn_pd_t *rnn_pd,
        const rnn_utils::conf_t &rnn, const memory_desc_wrapper &src_layer_d,
        const memory_desc_wrapper &src_iter_d,
        const memory_desc_wrapper &src_iter_c_d,
        const memory_desc_wrapper &weights_layer_d,
        const memory_desc_wrapper &weights_iter_d,
        const memory_desc_wrapper &bias_d,
        const memory_desc_wrapper &dst_layer_d,
        const memory_desc_wrapper &dst_iter_d,
        const memory_desc_wrapper &dst_iter_c_d,
        const memory_desc_wrapper &diff_src_layer_d,
        const memory_desc_wrapper &diff_src_iter_d,
        const memory_desc_wrapper &diff_src_iter_c_d,
        const memory_desc_wrapper &diff_weights_layer_d,
        const memory_desc_wrapper &diff_weights_iter_d,
        const memory_desc_wrapper &diff_bias_d,
        const memory_desc_wrapper &diff_dst_layer_d,
        const memory_desc_wrapper &diff_dst_iter_d,
        const memory_desc_wrapper &diff_dst_iter_c_d,
        const memory_desc_wrapper &ws_d, rnn_offsets_t &off) {

    using namespace rnn_utils;

    conf.src_dt = src_layer_d.data_type();
    conf.wei_dt = weights_layer_d.data_type();
    conf.acc_dt = rnn.acc_data_type;
    conf.aux_dt = rnn.aux_data_type;
    conf.diff_dt = rnn.diff_data_type;
    conf.input_dt = rnn.input_data_type;
    conf.output_dt = rnn.output_data_type;
    conf.dst_dt = rnn.dst_data_type;

    conf.is_fwd = rnn.is_fwd;
    conf.n_layer = rnn.n_layer;
    conf.n_dir = rnn.n_dir;
    conf.n_iter = rnn.n_iter;
    conf.n_iter_scratch_gates = rnn.n_iter_scratch_gates;
    conf.n_gates = rnn.n_gates;
    conf.n_bias = rnn.n_bias;
    conf.n_states = rnn.n_states;
    conf.n_weights_input = weights_layer_d.dims()[2];
    conf.n_weights_state = weights_iter_d.dims()[2];
    conf.batch = rnn.mb;
    conf.slc = rnn.slc;
    conf.sic = rnn.sic;
    conf.dhc = rnn.dhc;
    conf.dlc = rnn.dlc;
    conf.wic = nstl::max(conf.slc, nstl::max(conf.sic, conf.dhc));

    conf.n_parts_weights_iter = rnn.n_parts_weights_iter;
    conf.n_parts_weights_layer = rnn.n_parts_weights_layer;

    conf.with_bias = rnn_pd->with_bias();
    conf.with_src_iter = rnn_pd->with_src_iter();
    conf.with_src_iter_c = rnn_pd->with_src_iter_c();
    conf.with_dst_iter = rnn_pd->with_dst_iter();
    conf.with_dst_iter_c = rnn_pd->with_dst_iter_c();
    conf.is_lbr = rnn.is_lbr;
    conf.is_vanilla_gru = rnn.is_vanilla_gru;
    conf.copy_bias = rnn.copy_bias;
    conf.is_int8 = rnn.is_int8;
    conf.is_training = rnn.is_training;

    conf.states_ws_ld = rnn.states_ws_ld;
    conf.scratch_diff_states_ld = rnn.scratch_diff_states_ld;
    conf.gates_ws_ld = rnn.gates_ws_ld;
    conf.scratch_gates_ld = rnn.scratch_gates_ld;

    conf.src_layer_ndims = src_layer_d.ndims();
    conf.src_iter_ndims = src_iter_d.ndims();
    if (conf.with_src_iter_c) conf.src_iter_c_ndims = src_iter_c_d.ndims();
    conf.weights_layer_ndims = weights_layer_d.ndims();
    conf.weights_iter_ndims = weights_iter_d.ndims();
    conf.dst_layer_ndims = dst_layer_d.ndims();
    conf.dst_iter_ndims = dst_iter_d.ndims();
    if (conf.with_dst_iter_c) conf.dst_iter_c_ndims = dst_iter_c_d.ndims();
    conf.bias_ndims = bias_d.ndims();

    gpu::set_offsets(src_layer_d, off.src_layer_off);
    gpu::set_offsets(src_iter_d, off.src_iter_off);
    if (conf.with_src_iter_c)
        gpu::set_offsets(src_iter_c_d, off.src_iter_c_off);
    gpu::set_offsets(weights_layer_d, off.weights_layer_off);
    gpu::set_offsets(weights_iter_d, off.weights_iter_off);
    gpu::set_offsets(bias_d, off.bias_off);
    gpu::set_offsets(dst_layer_d, off.dst_layer_off);
    gpu::set_offsets(dst_iter_d, off.dst_iter_off);
    if (conf.with_dst_iter_c)
        gpu::set_offsets(dst_iter_c_d, off.dst_iter_c_off);

    if (!conf.is_fwd) {
        conf.diff_src_layer_ndims = diff_src_layer_d.ndims();
        conf.diff_src_iter_ndims = diff_src_iter_d.ndims();
        if (conf.with_src_iter_c)
            conf.diff_src_iter_c_ndims = diff_src_iter_c_d.ndims();
        conf.diff_weights_layer_ndims = diff_weights_layer_d.ndims();
        conf.diff_weights_iter_ndims = diff_weights_iter_d.ndims();
        conf.diff_dst_layer_ndims = diff_dst_layer_d.ndims();
        conf.diff_dst_iter_ndims = diff_dst_iter_d.ndims();
        if (conf.with_dst_iter_c)
            conf.diff_dst_iter_c_ndims = diff_dst_iter_c_d.ndims();
        conf.diff_bias_ndims = diff_bias_d.ndims();

        gpu::set_offsets(diff_src_layer_d, off.diff_src_layer_off);
        gpu::set_offsets(diff_src_iter_d, off.diff_src_iter_off);
        if (conf.with_src_iter_c)
            gpu::set_offsets(diff_src_iter_c_d, off.diff_src_iter_c_off);
        gpu::set_offsets(diff_weights_layer_d, off.diff_weights_layer_off);
        gpu::set_offsets(diff_weights_iter_d, off.diff_weights_iter_off);
        gpu::set_offsets(diff_bias_d, off.diff_bias_off);
        gpu::set_offsets(diff_dst_layer_d, off.diff_dst_layer_off);
        gpu::set_offsets(diff_dst_iter_d, off.diff_dst_iter_off);
        if (conf.with_dst_iter_c)
            gpu::set_offsets(diff_dst_iter_c_d, off.diff_dst_iter_c_off);
    }

    rnn_utils::set_workspace_offsets(rnn, conf.ws_gates_offset,
            conf.ws_states_offset, conf.ws_c_state_offset,
            conf.ws_grid_comp_offset, conf.ws_bias_offset);

    conf.cell_kind = rnn_pd->cell_kind();
    conf.activation_kind = rnn_pd->activation_kind();
    conf.direction_kind = rnn_pd->direction();

    conf.wei_qparam_mask = rnn_pd->attr()->rnn_weights_qparams_.mask_;
    conf.is_testmode = rnn.is_testmode;

    return status::success;
}

static status_t init_kernel_ctx(compute::kernel_ctx_t &kernel_ctx,
        const rnn_conf_t &conf, const rnn_offsets_t &off, int subgroup_size) {

    kernel_ctx.add_option("-cl-std=CL2.0");

    kernel_ctx.define_int("IS_FWD", conf.is_fwd);
    kernel_ctx.define_int("IS_TRAINING", conf.is_training);
    kernel_ctx.define_int("WITH_BIAS", conf.with_bias);
    kernel_ctx.define_int("WITH_SRC_ITER", conf.with_src_iter);
    kernel_ctx.define_int("WITH_SRC_ITER_C", conf.with_src_iter_c);
    kernel_ctx.define_int("WITH_DST_ITER", conf.with_dst_iter);
    kernel_ctx.define_int("WITH_DST_ITER_C", conf.with_dst_iter_c);
    kernel_ctx.define_int("IS_LBR", conf.is_lbr);

    kernel_ctx.define_int(
            "ELEMWISE_BWD_BATCH_BLOCK", conf.elemwise_bwd_batch_block);
    kernel_ctx.define_int("NEED_BIAS_ATOMIC_REDUCE",
            !conf.is_fwd && conf.elemwise_bwd_batch_block < conf.batch);
    kernel_ctx.define_int("VANILLA_RNN", alg_kind::vanilla_rnn);
    kernel_ctx.define_int("VANILLA_LSTM", alg_kind::vanilla_lstm);
    kernel_ctx.define_int("VANILLA_GRU", alg_kind::vanilla_gru);
    kernel_ctx.define_int("LBR_GRU", alg_kind::lbr_gru);
    kernel_ctx.define_int("CELL_KIND", conf.cell_kind);

    kernel_ctx.define_int("ELTWISE_RELU", alg_kind::eltwise_relu);
    kernel_ctx.define_int("ELTWISE_TANH", alg_kind::eltwise_tanh);
    kernel_ctx.define_int("ELTWISE_LOGISTIC", alg_kind::eltwise_logistic);
    kernel_ctx.define_int("ACTIVATION_KIND", conf.activation_kind);

    kernel_ctx.define_int("WS_GATES", rnn_utils::gates);
    kernel_ctx.define_int("WS_STATES", rnn_utils::states);
    kernel_ctx.define_int("WS_C_STATES", rnn_utils::c_states);
    kernel_ctx.define_int("WS_BIAS", rnn_utils::bias);

    kernel_ctx.define_int("L2R", dnnl_unidirectional_left2right);
    kernel_ctx.define_int("R2L", dnnl_unidirectional_right2left);
    kernel_ctx.define_int("CONCAT", dnnl_bidirectional_concat);
    kernel_ctx.define_int("SUM", dnnl_bidirectional_sum);
    kernel_ctx.define_int("DIRECTION_KIND", conf.direction_kind);

    kernel_ctx.define_int("N_PARTS_WEI_ST", conf.n_parts_weights_iter);
    kernel_ctx.define_int("N_PARTS_WEI_I", conf.n_parts_weights_layer);

    kernel_ctx.define_int("SUBGROUP_SIZE", subgroup_size);

    def_block_offsets(
            off.src_layer_off, kernel_ctx, "SRC_L", conf.src_layer_ndims);
    def_block_offsets(
            off.src_iter_off, kernel_ctx, "SRC_I", conf.src_iter_ndims);
    if (conf.with_src_iter_c)
        def_block_offsets(off.src_iter_c_off, kernel_ctx, "SRC_I_C",
                conf.src_iter_c_ndims);
    def_block_offsets(off.weights_layer_off, kernel_ctx, "WEI_L",
            conf.weights_layer_ndims);
    def_block_offsets(
            off.weights_iter_off, kernel_ctx, "WEI_I", conf.weights_iter_ndims);
    def_block_offsets(
            off.dst_layer_off, kernel_ctx, "DST_L", conf.dst_layer_ndims);
    def_block_offsets(
            off.dst_iter_off, kernel_ctx, "DST_I", conf.dst_iter_ndims);
    if (conf.with_dst_iter_c)
        def_block_offsets(off.dst_iter_c_off, kernel_ctx, "DST_I_C",
                conf.dst_iter_c_ndims);
    def_block_offsets(off.bias_off, kernel_ctx, "BIAS", conf.bias_ndims);
    kernel_ctx.define_int("N_BIAS", conf.n_bias);

    if (!conf.is_fwd) {
        def_block_offsets(off.diff_src_layer_off, kernel_ctx, "DIFF_SRC_L",
                conf.diff_src_layer_ndims);
        def_block_offsets(off.diff_src_iter_off, kernel_ctx, "DIFF_SRC_I",
                conf.diff_src_iter_ndims);
        if (conf.with_src_iter_c)
            def_block_offsets(off.diff_src_iter_c_off, kernel_ctx,
                    "DIFF_SRC_I_C", conf.diff_src_iter_c_ndims);
        def_block_offsets(off.diff_weights_layer_off, kernel_ctx, "DIFF_WEI_L",
                conf.diff_weights_layer_ndims);
        def_block_offsets(off.diff_weights_iter_off, kernel_ctx, "DIFF_WEI_I",
                conf.diff_weights_iter_ndims);
        def_block_offsets(off.diff_dst_layer_off, kernel_ctx, "DIFF_DST_L",
                conf.diff_dst_layer_ndims);
        def_block_offsets(off.diff_dst_iter_off, kernel_ctx, "DIFF_DST_I",
                conf.diff_dst_iter_ndims);
        if (conf.with_dst_iter_c)
            def_block_offsets(off.diff_dst_iter_c_off, kernel_ctx,
                    "DIFF_DST_I_C", conf.diff_dst_iter_c_ndims);
        def_block_offsets(off.diff_bias_off, kernel_ctx, "DIFF_BIAS",
                conf.diff_bias_ndims);
    }

    if (conf.src_dt == data_type::f16) {
        kernel_ctx.set_data_type(data_type::f16);
    } else
        kernel_ctx.set_data_type(data_type::f32);

    def_data_type(kernel_ctx, conf.src_dt, "WS_STATE");
    def_data_type(kernel_ctx, conf.src_dt, "SRC");
    def_data_type(kernel_ctx, conf.wei_dt, "WEI");
    def_data_type(kernel_ctx, conf.acc_dt, "ACC");
    def_data_type(kernel_ctx, conf.aux_dt, "AUX");
    def_data_type(kernel_ctx, conf.dst_dt, "DST");
    def_data_type(kernel_ctx, conf.input_dt, "INPUT");
    def_data_type(kernel_ctx, conf.output_dt, "OUTPUT");
    def_data_type(kernel_ctx, conf.diff_dt, "DIFF");

    kernel_ctx.define_int("IS_INT8", conf.is_int8);
    kernel_ctx.define_int("COPY_BIAS", conf.copy_bias);
    kernel_ctx.define_int("WEI_QPARAM_MASK", conf.wei_qparam_mask);
    kernel_ctx.define_int("IS_TESTMODE", conf.is_testmode);
    if (is_ws_print_enabled()) kernel_ctx.define_int("DEBUGPRINT", true);

    return status::success;
}

template <prop_kind_t aprop>
inline status_t init_conf(rnn_conf_t &conf, const rnn_utils::conf_t &rnn,
        const rnn_pd_t *rnn_pd, rnn_offsets_t &off) {

    const memory_desc_wrapper fakedesc = rnn_pd->src_md(0);
    return init_conf(conf, rnn_pd, rnn, rnn_pd->src_md(0), rnn_pd->src_md(1),
            rnn_pd->src_md(2), rnn_pd->weights_md(0), rnn_pd->weights_md(1),
            rnn_pd->weights_md(2), rnn_pd->dst_md(0), rnn_pd->dst_md(1),
            rnn_pd->dst_md(2), fakedesc, fakedesc, fakedesc, fakedesc, fakedesc,
            fakedesc, fakedesc, fakedesc, fakedesc, rnn_pd->workspace_md(0),
            off);
}

template <>
inline status_t init_conf<prop_kind::backward>(rnn_conf_t &conf,
        const rnn_utils::conf_t &rnn, const rnn_pd_t *rnn_pd,
        rnn_offsets_t &off) {
    return init_conf(conf, rnn_pd, rnn, rnn_pd->src_md(0), rnn_pd->src_md(1),
            rnn_pd->src_md(2), rnn_pd->weights_md(0), rnn_pd->weights_md(1),
            rnn_pd->weights_md(2), rnn_pd->dst_md(0), rnn_pd->dst_md(1),
            rnn_pd->dst_md(2), rnn_pd->diff_src_md(0), rnn_pd->diff_src_md(1),
            rnn_pd->diff_src_md(2), rnn_pd->diff_weights_md(0),
            rnn_pd->diff_weights_md(1), rnn_pd->diff_weights_md(2),
            rnn_pd->diff_dst_md(0), rnn_pd->diff_dst_md(1),
            rnn_pd->diff_dst_md(2), rnn_pd->workspace_md(0), off);
}

template <>
status_t _ref_rnn_common_t<prop_kind::forward>::pd_t::set_default_params() {
    using namespace format_tag;
    if (src_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(src_layer_md_, tnc));
    if (dst_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(dst_layer_md_, tnc));

    // Optional parameters
    if ((!types::is_zero_md(&src_iter_md_))
            && (src_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(src_iter_md_, ldnc));
    if ((!types::is_zero_md(&src_iter_c_md_))
            && (src_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(src_iter_c_md_, ldnc));
    if ((!types::is_zero_md(&bias_md_))
            && (bias_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(bias_md_, ldgo));
    if ((!types::is_zero_md(&dst_iter_md_))
            && (dst_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(dst_iter_md_, ldnc));
    if ((!types::is_zero_md(&dst_iter_c_md_))
            && (dst_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(dst_iter_c_md_, ldnc));

    return status::success;
}

template <>
status_t _ref_rnn_common_t<prop_kind::backward>::pd_t::set_default_params() {
    using namespace format_tag;
    int arch_ld = is_xe_hpc ? 128 : 64;
    if (src_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(src_layer_md_, tnc));
    if (weights_layer_md_.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(weights_layer_md_, ldgoi));
        CHECK(rnn_utils::set_good_strides(arch_ld, weights_layer_md_, ldgoi));
    }
    if (dst_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(dst_layer_md_, tnc));

    if (weights_iter_md_.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(weights_iter_md_, ldgoi));
        CHECK(rnn_utils::set_good_strides(arch_ld, weights_iter_md_, ldgoi));
    }

    if (diff_src_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(diff_src_layer_md_, tnc));
    if (diff_weights_layer_md_.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(diff_weights_layer_md_, ldigo));
        CHECK(rnn_utils::set_good_strides(
                arch_ld, diff_weights_layer_md_, ldigo));
    }
    if (diff_weights_iter_md_.format_kind == format_kind::any) {
        CHECK(memory_desc_init_by_tag(diff_weights_iter_md_, ldigo));
        CHECK(rnn_utils::set_good_strides(
                arch_ld, diff_weights_iter_md_, ldigo));
    }
    if (diff_dst_layer_md_.format_kind == format_kind::any)
        CHECK(memory_desc_init_by_tag(diff_dst_layer_md_, tnc));

    // Optional parameters
    if ((!types::is_zero_md(&src_iter_md_))
            && (src_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(src_iter_md_, ldnc));
    if ((!types::is_zero_md(&src_iter_c_md_))
            && (src_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(src_iter_c_md_, ldnc));
    if ((!types::is_zero_md(&bias_md_))
            && (bias_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(bias_md_, ldgo));
    if ((!types::is_zero_md(&dst_iter_md_))
            && (dst_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(dst_iter_md_, ldnc));
    if ((!types::is_zero_md(&dst_iter_c_md_))
            && (dst_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(dst_iter_c_md_, ldnc));

    if ((!types::is_zero_md(&diff_src_iter_md_))
            && (diff_src_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(diff_src_iter_md_, ldnc));
    if ((!types::is_zero_md(&diff_src_iter_c_md_))
            && (diff_src_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(diff_src_iter_c_md_, ldnc));
    if ((!types::is_zero_md(&diff_bias_md_))
            && (diff_bias_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(diff_bias_md_, ldgo));
    if ((!types::is_zero_md(&diff_dst_iter_md_))
            && (diff_dst_iter_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(diff_dst_iter_md_, ldnc));
    if ((!types::is_zero_md(&diff_dst_iter_c_md_))
            && (diff_dst_iter_c_md_.format_kind == format_kind::any))
        CHECK(memory_desc_init_by_tag(diff_dst_iter_c_md_, ldnc));

    return status::success;
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::pd_t::init(engine_t *engine) {
    using namespace prop_kind;
    using namespace utils;
    using namespace rnn_utils;
    using namespace format_tag;

    assert(engine->kind() == engine_kind::gpu);
    auto *compute_engine
            = utils::downcast<const compute::compute_engine_t *>(engine);

    is_xe_hpc = compute_engine->is_xe_hpc();
    subgroup_size = compute_engine->device_info()->max_subgroup_size();
    max_eus_per_wg = compute_engine->device_info()->max_eus_per_wg();
    auto eu_count = compute_engine->device_info()->eu_count();

    const alg_kind_t cell_kind = this->desc()->cell_kind;

    data_type_t src_layer_dt = this->desc()->src_layer_desc.data_type;
    data_type_t weights_iter_dt = this->desc()->weights_iter_desc.data_type;
    data_type_t weights_layer_dt = this->desc()->weights_layer_desc.data_type;
    data_type_t bias_dt = this->desc()->bias_desc.data_type;

    bool src_is_u8 = src_layer_dt == data_type::u8;
    bool src_is_f16 = src_layer_dt == data_type::f16;
    if (src_is_u8 && !src_is_f16)
        acc_data_t = data_type::s32;
    else if (!src_is_u8 && src_is_f16)
        acc_data_t = data_type::f16;
    else if (!src_is_u8 && !src_is_f16)
        acc_data_t = data_type::f32;
    src_type = src_layer_dt;
    weights_type = weights_layer_dt;

    bool ok = true
            && one_of(cell_kind, alg_kind::vanilla_rnn, alg_kind::vanilla_lstm,
                    alg_kind::lbr_gru, alg_kind::vanilla_gru)
            && !this->is_lstm_peephole() && !this->is_lstm_projection()
            && IMPLICATION(aprop == prop_kind::forward,
                    one_of(this->desc()->prop_kind, forward_training,
                            forward_inference))
            && IMPLICATION(aprop == backward,
                    one_of(this->desc()->prop_kind, backward))
            && IMPLICATION(
                    src_type == data_type::bf16, bias_dt == data_type::f32)
            && src_layer_dt == src_type
            && ((aprop == prop_kind::forward && src_layer_dt == data_type::u8
                        && weights_layer_dt == data_type::s8
                        && cell_kind == alg_kind::vanilla_lstm)
                    || (aprop == prop_kind::forward
                            && one_of(src_layer_dt, data_type::f16,
                                    data_type::f32, data_type::bf16)
                            && weights_layer_dt == src_layer_dt)
                    || (aprop == prop_kind::backward
                            && one_of(weights_layer_dt, data_type::f32,
                                    data_type::bf16)
                            && weights_layer_dt == src_layer_dt))
            && weights_iter_dt == weights_layer_dt
            && everyone_is(weights_type, weights_iter_dt, weights_layer_dt)
            && this->set_default_params() == status::success
            && this->with_bias()
            && IMPLICATION(
                    src_type == data_type::f16 || src_type == data_type::u8,
                    this->desc()->prop_kind == forward_inference)
            && compute_engine->mayiuse(compute::device_ext_t::intel_subgroups)
            && IMPLICATION(src_type == data_type::f16,
                    true
                            && compute_engine->mayiuse(
                                    compute::device_ext_t::khr_fp16)
                            && compute_engine->mayiuse(compute::device_ext_t::
                                            intel_subgroups_short));
    if (!ok) return status::unimplemented;

    init_rnn_conf(rnn_conf, *this->desc(), this->src_md(0), this->src_md(1),
            this->weights_md(0), this->weights_md(1), this->dst_md(0),
            is_xe_hpc);

    if (rnn_conf.is_int8) {
        auto has_trivial_strides = [](const memory_desc_wrapper &md) {
            return md.is_dense(true);
        };
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->src_layer_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->src_iter_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->src_iter_c_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->dst_layer_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->dst_iter_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
        VCONDCHECK(create, dispatch, rnn,
                has_trivial_strides(this->desc()->dst_iter_c_desc),
                status::unimplemented, VERBOSE_NONTRIVIAL_STRIDE);
    }

    init_test_mode(rnn_conf, *this->attr());

    // Check that only supported attr have been passed.
    primitive_attr_t::skip_mask_t attr_mask
            = primitive_attr_t::skip_mask_t::rnn_tparams;
    if (weights_layer_dt == data_type::s8)
        attr_mask = attr_mask | primitive_attr_t::skip_mask_t::rnn_data_qparams
                | primitive_attr_t::skip_mask_t::rnn_weights_qparams;
    ok = ok && this->attr()->has_default_values(attr_mask);

    // TODO: implement something like check layout consistency
    switch (aprop) {
        case (prop_kind::forward): break;
        case (prop_kind::backward):
            ok = ok && utils::one_of(this->desc()->prop_kind, backward);
            break;
        default: ok = false;
    }
    if (!ok) return status::unimplemented;

    // Set weights descriptors to desired format
    memory_desc_t new_weights_layer_md = *this->weights_md(0);
    CHECK(set_expected_desc(rnn_conf, new_weights_layer_md, false));

    if (this->weights_layer_md_.format_kind == format_kind::any) {
        this->weights_layer_md_ = new_weights_layer_md;
    } else if (this->weights_layer_md_.format_kind == format_kind::rnn_packed) {
        if (dnnl::impl::operator!=(
                    this->weights_layer_md_, new_weights_layer_md))
            return status::unimplemented;
    }

    memory_desc_t new_weights_iter_md = *this->weights_md(1);
    CHECK(set_expected_desc(rnn_conf, new_weights_iter_md, true));
    if (this->weights_iter_md_.format_kind == format_kind::any) {
        this->weights_iter_md_ = new_weights_iter_md;
    } else if (this->weights_iter_md_.format_kind == format_kind::rnn_packed) {
        if (dnnl::impl::operator!=(this->weights_iter_md_, new_weights_iter_md))
            return status::unimplemented;
    }

    // Check dimensions consistency
    int ls_multiplier
            = (this->direction() == dnnl_bidirectional_concat) ? 2 : 1;

    ok = ok && (ls_multiplier * this->DHC() == this->DLC())
            && ((ls_multiplier * this->SLC()) == this->DLC()
                    || (this->L() == 1))
            && (this->SIC() == this->DHC() || (this->T() == 1));
    if (!ok) return status::unimplemented;

    set_rnn_conf(rnn_conf, *this->desc(), this->weights_md(0),
            this->weights_md(1), this->diff_weights_md(0),
            this->diff_weights_md(1));

    size_t workspace_size = get_workspace_size(rnn_conf);

    // initialize the workspace_pd if needed
    if (rnn_conf.use_workspace) {
        dims_t ws_dims = {(dim_t)workspace_size};
        CHECK(memory_desc_init_by_tag(
                this->ws_md_, 1, ws_dims, data_type::u8, x));
    }

    rnn_conf.acc_data_type = acc_data_t;
    rnn_conf.acc_data_type_elsz = (int)types::data_type_size(acc_data_t);
    status_t status = init_conf<aprop>(conf, rnn_conf, this, this->off);
    if (status != status::success) { return status; }

    int batch = rnn_conf.mb;
    int n_gates = rnn_conf.n_gates;
    int slc = rnn_conf.slc;
    int sic = rnn_conf.sic;
    int dhc = rnn_conf.dhc;

    auto fpmath_mode = this->attr()->fpmath_mode_;

    auto max_elemwise_threads = utils::div_up(batch * dhc, subgroup_size);
    auto max_elemwise_threads_per_eu
            = utils::div_up(max_elemwise_threads, eu_count);
    auto preferred_threads_per_eu = 4;
    conf.elemwise_bwd_batch_block = dev_getenv("bwd_batch_block",
            std::min(8,
                    utils::rnd_up_pow2(max_elemwise_threads_per_eu
                            / preferred_threads_per_eu)));

    // The inputs of create_gemm_pd describe a gemm in column major.
    // Below, we have to transpose the a and b descriptor to describe
    // the GEMM as a row major problem.
    int threads_per_eu = 0;
    auto create_gemm_pd
            = [&](std::shared_ptr<primitive_desc_t> &gemm_pd, int m, int n,
                      int k, int lda, int ldb, int ldc, data_type_t a_dt,
                      data_type_t b_dt, data_type_t c_dt, bool is_B_trans,
                      float beta) -> status_t {
        memory_desc_t a_md, b_md, c_md;
        CHECK(create_2d_desc(&b_md, k, m, a_dt, transpose::notrans, lda));
        CHECK(create_2d_desc(&a_md, n, k, b_dt,
                is_B_trans ? transpose::trans : transpose::notrans, ldb));
        CHECK(create_2d_desc(&c_md, n, m, c_dt, transpose::notrans, ldc));

        primitive_attr_t attr;
        CHECK(attr.post_ops_.append_sum(beta));
        CHECK(attr.set_fpmath_mode(fpmath_mode));
        status_t status = dnnl::impl::create_gemm_pd(gemm_pd, engine, &a_md,
                &b_md, &c_md, &glob_zero_md, c_dt, &attr);
        if (threads_per_eu == 0)
            CHECK(gemm_pd->query(
                    query::preferred_gpu_threads_per_eu, 0, &threads_per_eu));
        else if (get_verbose_dev_mode(verbose_t::debuginfo) > 1) {
            auto t = 0;
            CHECK(gemm_pd->query(query::preferred_gpu_threads_per_eu, 0, &t));
            if (t != threads_per_eu)
                printf("[WARNING] GEMM grf modes are inconsistent");
        }
        return status;
    };

    int layer_merged_size
            = rnn_conf.merge_gemm_layer ? batch * rnn_conf.n_iter : batch;
    int iter_merged_size
            = rnn_conf.merge_gemm_iter ? batch * rnn_conf.n_iter : batch;

    int gemm_iter_fwd_beta = this->is_lbr() ? 0.0 : 1.0;
    int gemm_iter_bwd_beta = this->is_lbr() ? 1.0f : 0.0f;
    switch (aprop) {
        case prop_kind::forward:
            CHECK(create_gemm_pd(gemm_layer_fwd_pd_, n_gates * dhc,
                    layer_merged_size, slc, rnn_conf.weights_layer_ld,
                    rnn_conf.states_ws_ld, rnn_conf.scratch_gates_ld,
                    weights_type, src_type, rnn_conf.acc_data_type, false,
                    0.0));
            if (rnn_conf.is_vanilla_gru) {
                CHECK(create_gemm_pd(gemm_iter_fwd_pd_, (n_gates - 1) * dhc,
                        batch, sic, rnn_conf.weights_iter_ld,
                        rnn_conf.states_ws_ld, rnn_conf.scratch_gates_ld,
                        weights_type, src_type, rnn_conf.acc_data_type, false,
                        gemm_iter_fwd_beta));
                CHECK(create_gemm_pd(gemm_iter_fwd_2_pd_, dhc, batch, sic,
                        rnn_conf.weights_iter_ld, rnn_conf.states_ws_ld,
                        rnn_conf.scratch_gates_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, false, gemm_iter_fwd_beta));
            } else {
                CHECK(create_gemm_pd(gemm_iter_fwd_pd_, n_gates * dhc, batch,
                        sic, rnn_conf.weights_iter_ld, rnn_conf.states_ws_ld,
                        rnn_conf.gates_ws_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, false, gemm_iter_fwd_beta));
            }
            break;
        case prop_kind::backward:
            if (rnn_conf.is_vanilla_gru) {
                CHECK(create_gemm_pd(gemm_iter_bwd_pd_, sic, batch,
                        (n_gates - 1) * dhc, rnn_conf.weights_iter_ld,
                        rnn_conf.scratch_gates_ld,
                        rnn_conf.scratch_diff_states_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, false, 1.0f));
                CHECK(create_gemm_pd(gemm_iter_bwd_2_pd_, sic, batch, dhc,
                        rnn_conf.weights_iter_ld, rnn_conf.scratch_gates_ld,
                        rnn_conf.scratch_diff_states_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, false, 0.0f));
                CHECK(create_gemm_pd(gemm_diff_wei_iter_pd_,
                        (n_gates - 1) * dhc, sic, iter_merged_size,
                        rnn_conf.scratch_gates_ld, rnn_conf.states_ws_ld,
                        rnn_conf.diff_weights_iter_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, true, 1.0f));
                CHECK(create_gemm_pd(gemm_diff_wei_iter_2_pd_, dhc, sic,
                        iter_merged_size, rnn_conf.scratch_gates_ld,
                        rnn_conf.states_ws_ld, rnn_conf.diff_weights_iter_ld,
                        weights_type, src_type, rnn_conf.acc_data_type, true,
                        1.0f));
            } else {
                CHECK(create_gemm_pd(gemm_iter_bwd_pd_, sic, batch,
                        n_gates * dhc, rnn_conf.weights_iter_ld,
                        rnn_conf.scratch_gates_ld,
                        rnn_conf.scratch_diff_states_ld, weights_type, src_type,
                        rnn_conf.acc_data_type, false, gemm_iter_bwd_beta));
                CHECK(create_gemm_pd(gemm_diff_wei_iter_pd_, n_gates * dhc, sic,
                        iter_merged_size, rnn_conf.scratch_gates_ld,
                        rnn_conf.states_ws_ld, rnn_conf.diff_weights_iter_ld,
                        weights_type, src_type, rnn_conf.acc_data_type, true,
                        1.0f));
            }
            CHECK(create_gemm_pd(gemm_layer_bwd_pd_, slc, layer_merged_size,
                    n_gates * dhc, rnn_conf.weights_layer_ld,
                    rnn_conf.scratch_gates_ld, rnn_conf.scratch_diff_states_ld,
                    weights_type, src_type, rnn_conf.acc_data_type, false,
                    0.0f));
            CHECK(create_gemm_pd(gemm_diff_wei_layer_pd_, n_gates * dhc, slc,
                    layer_merged_size, rnn_conf.scratch_gates_ld,
                    rnn_conf.states_ws_ld, rnn_conf.diff_weights_layer_ld,
                    weights_type, src_type, rnn_conf.acc_data_type, true,
                    1.0f));
            break;
        default: assert(!"unknown prop_kind"); return status::invalid_arguments;
    }

    // Fwd operations are not well optimized for larger grf mode
    if (!rnn_conf.is_fwd)
        CHECK(ocl_attr.set_gpu_attr(gpu_primitive_attr_t(threads_per_eu)));
    init_scratchpad(rnn_conf.use_workspace ? 0 : workspace_size);
    return status::success;
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::init(engine_t *engine) {
    using namespace rnn_utils;
    auto assign_funcs = [](gemm_t &g, weights_assign_t &p) {
        g = &class_name::gemm_primitive;
        p = &class_name::assign_weights;
    };

    assign_funcs(gemm_iter_func, weights_iter_assign_func);
    assign_funcs(gemm_layer_func, weights_layer_assign_func);

    switch (pd()->cell_kind()) {
        case dnnl_vanilla_lstm:
            cell_func = &class_name::cell_execution;
            elemwise_common = pd()->src_type == data_type::u8
                            && pd()->weights_type == data_type::s8
                    ? &class_name::lstm_elemwise_u8s8
                    : &class_name::lstm_elemwise;
            break;
        case dnnl_vanilla_rnn:
            cell_func = &class_name::cell_execution;
            elemwise_common = &class_name::rnn_elemwise;
            break;
        case dnnl_vanilla_gru:
            cell_func = &class_name::cell_execution_gru;
            elemwise_gru = &class_name::gru_elemwise;
            break;
        case dnnl_lbr_gru:
            cell_func = &class_name::cell_execution_gru_lbr;
            elemwise_gru_lbr = &class_name::gru_lbr_elemwise;
            break;
        default: break;
    }

    grid_computation = &class_name::linear_execution;

    rnn_utils::set_workspace_offsets(pd()->rnn_conf, ws_gates_offset_,
            ws_states_offset_, ws_c_states_offset_, ws_grid_comp_offset_,
            ws_bias_offset_);
    int max_nparts = (pd()->cell_kind() == alg_kind::vanilla_gru) ? 2 : 1;
    int wei_offsets_iter_sz = pd()->L() * pd()->D() * max_nparts;
    int wei_offsets_layer_sz = pd()->L() * pd()->D();

    wei_layer_offset_ptr
            = (size_t *)malloc(sizeof(size_t) * wei_offsets_layer_sz, 64);
    wei_iter_offset_ptr
            = (size_t *)malloc(sizeof(size_t) * wei_offsets_iter_sz, 64);

    compute::kernel_ctx_t kernel_ctx(&pd()->ocl_attr);
    status_t status = init_kernel_ctx(
            kernel_ctx, pd()->conf, pd()->off, pd()->subgroup_size);
    CHECK(status);

    std::vector<const char *> kernel_names = {"ref_rnn_bias_prepare",
            "ref_rnn_copy_init_layer", "ref_rnn_copy_init_iter",
            "ref_rnn_copy_res_layer", "ref_rnn_copy_res_iter", "ref_rnn_ws_set",
            "ref_rnn_elemwise_fwd", "ref_rnn_elemwise_bwd"};
    if (is_ws_print_enabled()) {
        kernel_names.emplace_back("ref_rnn_ws_print");
    }

    std::vector<compute::kernel_t> kernels;
    status = create_kernels(engine, &kernels, kernel_names, kernel_ctx);
    CHECK(status);

    bias_prepare_kernel_ = kernels[0];
    copy_init_layer_kernel_ = kernels[1];
    copy_init_iter_kernel_ = kernels[2];
    copy_res_layer_kernel_ = kernels[3];
    copy_res_iter_kernel_ = kernels[4];
    ws_set_kernel_ = kernels[5];
    elemwise_fwd_kernel_ = kernels[6];
    elemwise_bwd_kernel_ = kernels[7];
    if (is_ws_print_enabled()) ws_print_kernel_ = kernels[9];

    bool gemm_ok = true;

    switch (aprop) {
        case prop_kind::forward:
            gemm_ok = true
                    && utils::everyone_is(status::success,
                            create_nested_primitive(gemm_layer_fwd_,
                                    pd()->gemm_layer_fwd_pd_, engine),
                            create_nested_primitive(gemm_iter_fwd_,
                                    pd()->gemm_iter_fwd_pd_, engine),
                            pd()->conf.is_vanilla_gru
                                    ? create_nested_primitive(gemm_iter_fwd_2_,
                                            pd()->gemm_iter_fwd_2_pd_, engine)
                                    : status::success);
            break;
        case prop_kind::backward:
            gemm_ok = true
                    && utils::everyone_is(status::success,
                            create_nested_primitive(gemm_layer_bwd_,
                                    pd()->gemm_layer_bwd_pd_, engine),
                            create_nested_primitive(gemm_iter_bwd_,
                                    pd()->gemm_iter_bwd_pd_, engine),
                            create_nested_primitive(gemm_diff_wei_layer_,
                                    pd()->gemm_diff_wei_layer_pd_, engine),
                            create_nested_primitive(gemm_diff_wei_iter_,
                                    pd()->gemm_diff_wei_iter_pd_, engine),
                            pd()->conf.is_vanilla_gru
                                    ? create_nested_primitive(gemm_iter_bwd_2_,
                                            pd()->gemm_iter_bwd_2_pd_, engine)
                                    : status::success,
                            pd()->conf.is_vanilla_gru ? create_nested_primitive(
                                    gemm_diff_wei_iter_2_,
                                    pd()->gemm_diff_wei_iter_2_pd_, engine)
                                                      : status::success);
            break;
        default: assert(!"unknown prop_kind"); return status::invalid_arguments;
    }

    if (!gemm_ok) return status::runtime_error;

    return status::success;
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::init_res_storage(
        engine_t *engine, gpu_resource_t *r) const {
    if (pd()->rnn_conf.is_int8 && pd()->rnn_conf.copy_bias) {
        size_t size = pd()->rnn_conf.n_gates * pd()->rnn_conf.dhc
                * sizeof(float); // G * O * sizeof(float);
        memory_storage_t *tmp_mem_storage_ptr = nullptr;
        CHECK(engine->create_memory_storage(&tmp_mem_storage_ptr, size));
        // copy bias to memory storage
        std::unique_ptr<memory_storage_t> tmp_mem_storage(tmp_mem_storage_ptr);
        void *scales_ptr = nullptr;
        CHECK(tmp_mem_storage->map_data(&scales_ptr, nullptr,
                sizeof(float) * pd()->rnn_conf.n_gates * pd()->rnn_conf.dhc));
        utils::array_copy((float *)scales_ptr,
                pd()->attr()->rnn_weights_qparams_.scales_,
                pd()->rnn_conf.n_gates * pd()->rnn_conf.dhc);
        CHECK(tmp_mem_storage->unmap_data(scales_ptr, nullptr));
        r->add_memory_storage(SCALES_, std::move(tmp_mem_storage));
    }

    // Prepare testmode scales defined by attributes. Doesn't introduce
    // primitive state, because it is a constant memory -- will not be
    // changed during execution.
    // TODO: add the testmode scales to ws
    if (pd()->rnn_conf.is_testmode && pd_->attr()->rnn_tparams_.scales_) {
        size_t size = pd()->rnn_conf.tm_ngates
                * sizeof(*pd_->attr()->rnn_tparams_.scales_);
        memory_storage_t *tmp_mem_storage_ptr = nullptr;
        CHECK(engine->create_memory_storage(&tmp_mem_storage_ptr, size));

        std::unique_ptr<memory_storage_t> tmp_mem_storage(tmp_mem_storage_ptr);
        void *tm_scales_ptr = nullptr;
        CHECK(tmp_mem_storage->map_data(&tm_scales_ptr, nullptr,
                sizeof(float) * pd()->attr()->rnn_tparams_.ngates_));
        utils::array_copy((float *)tm_scales_ptr,
                pd()->attr()->rnn_tparams_.scales_,
                pd()->attr()->rnn_tparams_.ngates_);
        CHECK(tmp_mem_storage->unmap_data(tm_scales_ptr, nullptr));
        r->add_memory_storage(TM_SCALES_, std::move(tmp_mem_storage));
    }
    return status::success;
}

template <prop_kind_t aprop>
gemm_sig((_ref_rnn_common_t<aprop>::gemm_primitive)) {

    // FIXME: This should be created once per execute() instead of creating
    // memory before each gemm call. Each cell type (+prop kind) might have
    // different number of GEMMs.
    bool is_lbr = this->pd()->is_lbr();
    bool is_vanilla_gru = this->pd()->rnn_conf.is_vanilla_gru;
    bool is_training = !this->pd()->rnn_conf.is_fwd;

    memory_t *weights {nullptr};

    // These memory storages provide a mechanism to reuse existing memory
    // storage with an offset. These memory storages don't own attached memory
    std::unique_ptr<memory_storage_t> gemm_A_;
    std::unique_ptr<memory_storage_t> gemm_B_;
    std::unique_ptr<memory_storage_t> gemm_C_;
    std::unique_ptr<memory_storage_t> workspace;
    std::unique_ptr<memory_storage_t> scratchpad_gates;
    std::unique_ptr<memory_storage_t> scratchpad_cell;
    std::unique_ptr<memory_storage_t> scratchpad_dhG1;
    std::unique_ptr<memory_storage_t> scratchpad_diff;

    scratchpad_gates
            = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_gates);

    if (pd()->rnn_conf.use_workspace) {
        workspace = ((aprop == prop_kind::forward)
                        ? ctx.output(DNNL_ARG_WORKSPACE)
                        : ctx.input(DNNL_ARG_WORKSPACE))
                            ->memory_storage()
                            ->clone();
    } else {
        workspace = ctx.get_scratchpad_grantor().get_memory_storage(
                key_rnn_space);
    }

    if (is_lbr || (is_vanilla_gru && gemm_kind == gemm_diff_wei_iter_2)) {
        scratchpad_cell
                = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_cell);
    }

    if (is_vanilla_gru && gemm_kind == gemm_iter_bwd_2) {
        scratchpad_dhG1 = ctx.get_scratchpad_grantor().get_memory_storage(
                key_rnn_diff_ht);
    }

    if (is_training) {
        scratchpad_diff = ctx.get_scratchpad_grantor().get_memory_storage(
                key_rnn_diff_states);
    }

    switch (gemm_kind) {
        case gemm_iter_fwd:
        case gemm_layer_fwd:
        case gemm_iter_fwd_2:
            weights = (gemm_kind == gemm_layer_fwd)
                    ? ctx.input(DNNL_ARG_WEIGHTS_LAYER)
                    : ctx.input(DNNL_ARG_WEIGHTS_ITER);
            gemm_A_ = weights->memory_storage()->clone();
            gemm_B_ = workspace->clone();
            if (is_lbr && gemm_kind == gemm_iter_fwd) {
                gemm_C_ = scratchpad_cell->clone();
            } else {
                gemm_C_ = scratchpad_gates->clone();
            }
            break;
        case gemm_iter_bwd:
        case gemm_iter_bwd_2:
        case gemm_layer_bwd:
            weights = (gemm_kind == gemm_layer_bwd)
                    ? ctx.input(DNNL_ARG_WEIGHTS_LAYER)
                    : ctx.input(DNNL_ARG_WEIGHTS_ITER);
            gemm_A_ = weights->memory_storage()->clone();
            if (is_lbr && gemm_kind == gemm_iter_bwd) {
                gemm_B_ = scratchpad_cell->clone();
            } else {
                gemm_B_ = scratchpad_gates->clone();
            }
            gemm_C_ = (gemm_kind == gemm_iter_bwd_2) ? scratchpad_dhG1->clone()
                                                     : scratchpad_diff->clone();
            break;
        case gemm_diff_wei_iter:
        case gemm_diff_wei_layer:
            weights = (gemm_kind == gemm_diff_wei_iter)
                    ? ctx.output(DNNL_ARG_DIFF_WEIGHTS_ITER)
                    : ctx.output(DNNL_ARG_DIFF_WEIGHTS_LAYER);
            if (is_lbr && gemm_kind == gemm_diff_wei_iter) {
                gemm_A_ = scratchpad_cell->clone();
            } else {
                gemm_A_ = scratchpad_gates->clone();
            }
            gemm_B_ = workspace->clone();
            gemm_C_ = weights->memory_storage()->clone();
            break;
        case gemm_diff_wei_iter_2:
            weights = ctx.output(DNNL_ARG_DIFF_WEIGHTS_ITER);

            gemm_A_ = scratchpad_gates->clone();
            gemm_B_ = scratchpad_cell->clone();
            gemm_C_ = weights->memory_storage()->clone();
            break;
        default: assert(!"unknown gemm_kind");
    }

    gemm_A_->set_offset(off_a);
    gemm_B_->set_offset(off_b);
    gemm_C_->set_offset(off_c);

    // We flip A and B here since the GEMM API is row major but the
    // RNN code describes GEMM in column major fashion
    gemm_exec_args_t gemm_args;
    gemm_args.a = gemm_B_.get();
    gemm_args.b = gemm_A_.get();
    gemm_args.c = gemm_C_.get();

    auto gemm_ctx = gemm_exec_ctx_t(ctx, gemm_args);

    std::unique_ptr<nested_scratchpad_t> ns;
    const auto init_gemm_nested_scratchpad
            = [&](const std::shared_ptr<primitive_t> &gemm, int key) {
                  ns = utils::make_unique<nested_scratchpad_t>(ctx, key, gemm);
                  gemm_ctx.set_scratchpad_grantor(ns->grantor());
              };

    switch (gemm_kind) {
        case gemm_iter_fwd:
            init_gemm_nested_scratchpad(gemm_iter_fwd_, key_gemm_iter_fwd);
            CHECK(gpu_gemm(gemm_iter_fwd_)->execute(gemm_ctx));
            break;
        case gemm_iter_fwd_2:
            init_gemm_nested_scratchpad(gemm_iter_fwd_2_, key_gemm_iter_fwd_2);
            CHECK(gpu_gemm(gemm_iter_fwd_2_)->execute(gemm_ctx));
            break;
        case gemm_layer_fwd:
            init_gemm_nested_scratchpad(gemm_layer_fwd_, key_gemm_layer_fwd);
            CHECK(gpu_gemm(gemm_layer_fwd_)->execute(gemm_ctx));
            break;
        case gemm_iter_bwd:
            init_gemm_nested_scratchpad(gemm_iter_bwd_, key_gemm_iter_bwd);
            CHECK(gpu_gemm(gemm_iter_bwd_)->execute(gemm_ctx));
            break;
        case gemm_iter_bwd_2:
            init_gemm_nested_scratchpad(gemm_iter_bwd_2_, key_gemm_iter_bwd_2);
            CHECK(gpu_gemm(gemm_iter_bwd_2_)->execute(gemm_ctx));
            break;
        case gemm_layer_bwd:
            init_gemm_nested_scratchpad(gemm_layer_bwd_, key_gemm_layer_bwd);
            CHECK(gpu_gemm(gemm_layer_bwd_)->execute(gemm_ctx));
            break;
        case gemm_diff_wei_iter:
            init_gemm_nested_scratchpad(
                    gemm_diff_wei_iter_, key_gemm_diff_wei_iter);
            CHECK(gpu_gemm(gemm_diff_wei_iter_)->execute(gemm_ctx));
            break;
        case gemm_diff_wei_layer:
            init_gemm_nested_scratchpad(
                    gemm_diff_wei_layer_, key_gemm_diff_wei_layer);
            CHECK(gpu_gemm(gemm_diff_wei_layer_)->execute(gemm_ctx));
            break;
        case gemm_diff_wei_iter_2:
            init_gemm_nested_scratchpad(
                    gemm_diff_wei_iter_2_, key_gemm_diff_wei_iter_2);
            CHECK(gpu_gemm(gemm_diff_wei_iter_2_)->execute(gemm_ctx));
            break;
        default: assert(!"unknown gemm_kind"); return status::runtime_error;
    }
    return status::success;
}

//*************** Grid computations strategy: linear ***************//
template <prop_kind_t aprop>
grid_execution_sig((_ref_rnn_common_t<aprop>::linear_execution)) {
    const conf_t &rnn = pd()->rnn_conf;
    data_type_t src_t = pd()->src_type;
    int n_layer = rnn.n_layer;
    int n_dir = rnn.n_dir;
    int n_iter = rnn.n_iter;

    if (aprop == prop_kind::backward && pd()->diff_weights_overwrite()) {
        compute::compute_stream_t *compute_stream
                = utils::downcast<compute::compute_stream_t *>(ctx.stream());
        auto zero = [&](const memory_storage_t &data, int arg_id) {
            auto mdw = memory_desc_wrapper(pd()->arg_md(arg_id));
            return compute_stream->fill(data, 0, mdw.size(),
                    compute_stream->ctx().get_deps(),
                    compute_stream->ctx().get_deps());
        };

        CHECK(zero(diff_bias, DNNL_ARG_DIFF_BIAS));
        CHECK(zero(diff_weights_layer, DNNL_ARG_DIFF_WEIGHTS_LAYER));
        CHECK(zero(diff_weights_iter, DNNL_ARG_DIFF_WEIGHTS_ITER));
    }

    // Grid Computation for RNN with a cell execution call
    for (int dir = 0; dir < n_dir; dir++) {
        for (int j = 0; j < n_layer; j++) {
            int lay = (aprop == prop_kind::forward) ? j : n_layer - j - 1;

            // offsets for fwd rnn gemm grid computation
            cl_ulong offset_ws_layer, offset_wei_layer, offset_ws_iter;
            // offsets for bwd rnn gemm grid computation
            cl_ulong offset_diff_wei_iter, offset_diff_wei_lay,
                    offset_scratch_diff_lay;

            set_offsets_fwd_gemm(rnn, dir, lay, src_t, wei_layer_offset_ptr,
                    ws_states_offset_, offset_ws_layer, offset_wei_layer,
                    offset_ws_iter);
            if (aprop == prop_kind::backward) {
                int start_diff_src_iter_idx = 0;
                set_offsets_bwd_gemm(rnn, start_diff_src_iter_idx, dir, lay,
                        offset_diff_wei_iter, offset_diff_wei_lay,
                        offset_scratch_diff_lay);
            }

            if (aprop == prop_kind::forward && rnn.merge_gemm_layer) {
                CHECK(gemm_primitive(engine, ctx, wei_layer, offset_wei_layer,
                        workspace.ws(), offset_ws_layer, scratch_gates, 0,
                        gemm_layer_fwd));
            }

            for (int i = 0; i < n_iter; i++) {
                int iter = (aprop == prop_kind::forward) ? i : n_iter - i - 1;
                CHECK((this->*cell_func)(engine, ctx, dir, lay, iter,
                        &offset_wei_layer, wei_iter_offset_ptr, bias, workspace,
                        scratch_gates, scratch_cell, scratch_diff_states,
                        scratch_dhG1, wei_layer, wei_iter, diff_weights_layer,
                        diff_weights_iter, diff_bias, scales, tm_scales));
            }

            if (aprop == prop_kind::backward && rnn.merge_gemm_layer) {
                CHECK(gemm_primitive(engine, ctx, wei_layer, offset_wei_layer,
                        scratch_gates, 0, scratch_diff_states,
                        offset_scratch_diff_lay, gemm_layer_bwd));
                CHECK(gemm_primitive(engine, ctx, scratch_gates, 0,
                        workspace.ws(), offset_ws_layer, diff_weights_layer,
                        offset_diff_wei_lay, gemm_diff_wei_layer));
            }

            if (aprop == prop_kind::backward && rnn.merge_gemm_iter) {
                CHECK(gemm_primitive(engine, ctx, scratch_gates, 0,
                        workspace.ws(), offset_ws_iter, diff_weights_iter,
                        offset_diff_wei_iter, gemm_diff_wei_iter));
            }
        }
    }
    return status::success;
}
//********* GRID computations strategy: utility functions **********//

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::bias_prepare(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream, int n_layer, int n_dir,
        int n_bias, int n_gates, int dhc, const memory_storage_t &ws_bias,
        const memory_storage_t &scales, const memory_storage_t &wei_layer,
        const memory_storage_t &wei_iter, const memory_storage_t &bias) const {

    float data_shift = pd()->attr()->rnn_data_qparams_.shift_;
    float data_scale = pd()->attr()->rnn_data_qparams_.scale_;

    compute::kernel_arg_list_t arg_list;
    arg_list.append(ws_bias);
    arg_list.append(scales);
    arg_list.append(wei_layer);
    arg_list.append(wei_iter);
    arg_list.append(bias);
    arg_list.append(dhc);
    arg_list.append(n_layer);
    arg_list.append(n_dir);
    arg_list.append(n_bias);
    arg_list.append(data_shift);
    arg_list.append(data_scale);

    constexpr int stride_idx = 1, dim_idx = 3;
    arg_list.append(pd()->off.weights_layer_off[stride_idx][0]);
    arg_list.append(pd()->off.weights_layer_off[dim_idx][0]);
    arg_list.append(pd()->off.weights_iter_off[stride_idx][0]);
    arg_list.append(pd()->off.weights_iter_off[dim_idx][0]);
    rnn_utils::append_strides(
            arg_list, pd()->off.bias_off, 4, pd()->conf.bias_ndims);

    return parallel_for(ctx,
            compute::nd_range_t({dhc, n_bias, n_layer * n_dir}),
            bias_prepare_kernel_, arg_list);
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::copy_init_layer(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream, bool lr, bool rl, int batch,
        int dhc, int slc, int n_iter, int n_layer, int n_dir, int n_states,
        int states_ws_ld, int scratch_diff_states_ld,
        const memory_storage_t &ws_states,
        const memory_storage_t &scratch_diff_states,
        const memory_storage_t &input,
        const memory_storage_t &diff_dst_layer) const {

    if (aprop == prop_kind::forward) {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws_states);
        arg_list.append(input);
        arg_list.append(scratch_diff_states);
        arg_list.append((cl_int)lr);
        arg_list.append((cl_int)rl);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(slc);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);
        rnn_utils::append_strides(arg_list, pd()->off.src_layer_off, 1,
                pd()->conf.src_layer_ndims);

        return parallel_for(ctx,
                compute::nd_range_t(get_nd_range({slc, batch, n_iter})),
                copy_init_layer_kernel_, arg_list);
    } else {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws_states);
        arg_list.append(diff_dst_layer);
        arg_list.append(scratch_diff_states);
        arg_list.append((cl_int)0);
        arg_list.append((cl_int)0);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(slc);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);
        rnn_utils::append_strides(arg_list, pd()->off.diff_dst_layer_off, 2,
                pd()->conf.diff_dst_layer_ndims);

        return parallel_for(ctx,
                compute::nd_range_t(get_nd_range({dhc, batch, n_iter})),
                copy_init_layer_kernel_, arg_list);
    }
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::copy_init_iter(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream, int batch, int dhc, int sic,
        int n_iter, int n_layer, int n_dir, int n_states, int states_ws_ld,
        int scratch_diff_states_ld, const workspace_t &ws,
        const memory_storage_t &scratch_diff_states,
        const memory_storage_t &firstit_states,
        const memory_storage_t &firstit_c_states,
        const memory_storage_t &diff_dst_iter,
        const memory_storage_t &diff_dst_iter_c, const float shift,
        const float scale, const bool quantize) const {

    if (aprop == prop_kind::forward) {
        int max_d = std::max(dhc, sic);
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws.states());
        arg_list.append(ws.c_states());
        arg_list.append(firstit_states);
        arg_list.append(firstit_c_states);
        arg_list.append(scratch_diff_states);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(sic);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);

        rnn_utils::append_strides(
                arg_list, pd()->off.src_iter_off, 4, pd()->conf.src_iter_ndims);
        if (pd()->conf.with_src_iter_c)
            rnn_utils::append_strides(arg_list, pd()->off.src_iter_c_off, 4,
                    pd()->conf.src_iter_c_ndims);

        arg_list.append(shift);
        arg_list.append(scale);
        arg_list.append((int)quantize);
        return parallel_for(ctx,
                compute::nd_range_t({max_d, batch, n_layer * n_dir}),
                copy_init_iter_kernel_, arg_list);
    } else {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws.states());
        arg_list.append(ws.c_states());
        arg_list.append(diff_dst_iter);
        arg_list.append(diff_dst_iter_c);
        arg_list.append(scratch_diff_states);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(sic);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);

        rnn_utils::append_strides(arg_list, pd()->off.diff_dst_iter_off, 4,
                pd()->conf.diff_dst_iter_ndims);
        if (pd()->conf.with_dst_iter_c)
            rnn_utils::append_strides(arg_list, pd()->off.diff_dst_iter_c_off,
                    4, pd()->conf.diff_dst_iter_c_ndims);

        return parallel_for(ctx,
                compute::nd_range_t({dhc, batch, n_layer * n_dir}),
                copy_init_iter_kernel_, arg_list);
    }
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::copy_res_layer(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream, bool lr, bool rl, int batch,
        int dhc, int slc, int n_iter, int n_layer, int n_dir, int n_states,
        int states_ws_ld, int scratch_diff_states_ld,
        const memory_storage_t &scratch_diff_states,
        const memory_storage_t &dst_last_layer,
        const memory_storage_t &diff_src_layer,
        const memory_storage_t &ws_states, const float shift, const float scale,
        const bool dequantize) const {

    if (aprop == prop_kind::forward) {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws_states);
        arg_list.append(dst_last_layer);
        arg_list.append(scratch_diff_states);
        arg_list.append((cl_int)lr);
        arg_list.append((cl_int)rl);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(slc);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);

        rnn_utils::append_strides(arg_list, pd()->off.dst_layer_off, 3,
                pd()->conf.dst_layer_ndims);

        arg_list.append(shift);
        arg_list.append(scale);
        arg_list.append((int)dequantize);
        return parallel_for(ctx, get_nd_range({dhc, batch, n_iter}),
                copy_res_layer_kernel_, arg_list);
    } else {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws_states);
        arg_list.append(diff_src_layer);
        arg_list.append(scratch_diff_states);
        arg_list.append((cl_int)lr);
        arg_list.append((cl_int)rl);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(slc);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);
        rnn_utils::append_strides(arg_list, pd()->off.diff_src_layer_off, 3,
                pd()->conf.diff_src_layer_ndims);

        return parallel_for(ctx, get_nd_range({slc, batch, n_iter}),
                copy_res_layer_kernel_, arg_list);
    }
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::copy_res_iter(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream, int batch, int dhc, int sic,
        int n_iter, int n_layer, int n_dir, int n_states, int states_ws_ld,
        int scratch_diff_states_ld, const memory_storage_t &scratch_diff_states,
        const memory_storage_t &dst_last_iter,
        const memory_storage_t &dst_last_iter_c,
        const memory_storage_t &diff_src_iter,
        const memory_storage_t &diff_src_iter_c, const workspace_t &ws,
        const float shift, const float scale, const bool dequantize) const {

    if (aprop == prop_kind::forward) {
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws.states());
        arg_list.append(ws.c_states());
        arg_list.append(dst_last_iter);
        arg_list.append(dst_last_iter_c);
        arg_list.append(scratch_diff_states);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(sic);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);

        rnn_utils::append_strides(
                arg_list, pd()->off.dst_iter_off, 4, pd()->conf.dst_iter_ndims);
        if (pd()->conf.with_dst_iter_c)
            rnn_utils::append_strides(arg_list, pd()->off.dst_iter_c_off, 4,
                    pd()->conf.dst_iter_c_ndims);

        arg_list.append(shift);
        arg_list.append(scale);
        arg_list.append((int)dequantize);
        return parallel_for(ctx,
                compute::nd_range_t({dhc, batch, n_layer * n_dir}),
                copy_res_iter_kernel_, arg_list);
    } else {
        int max_d = std::max(dhc, sic);
        compute::kernel_arg_list_t arg_list;
        arg_list.append(ws.states());
        arg_list.append(ws.c_states());
        arg_list.append(diff_src_iter);
        arg_list.append(diff_src_iter_c);
        arg_list.append(scratch_diff_states);

        arg_list.append(batch);
        arg_list.append(dhc);
        arg_list.append(sic);
        arg_list.append(n_iter);
        arg_list.append(n_layer);
        arg_list.append(n_dir);
        arg_list.append(n_states);
        arg_list.append(states_ws_ld);
        arg_list.append(scratch_diff_states_ld);

        rnn_utils::append_strides(arg_list, pd()->off.diff_src_iter_off, 4,
                pd()->conf.diff_src_iter_ndims);
        if (pd()->conf.with_src_iter_c)
            rnn_utils::append_strides(arg_list, pd()->off.diff_src_iter_c_off,
                    4, pd()->conf.diff_src_iter_c_ndims);

        return parallel_for(ctx,
                compute::nd_range_t({max_d, batch, n_layer * n_dir}),
                copy_res_iter_kernel_, arg_list);
    }
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::ws_set(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream,
        const memory_storage_t &workspace_, const cl_ulong ws_offset,
        const int ws_part, const float val, const size_t size) const {
    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, workspace_);
    arg_list.set(1, ws_offset);
    arg_list.set(2, val);
    arg_list.set(3, ws_part);
    auto nd_range = compute::nd_range_t({size});

    return parallel_for(ctx, nd_range, ws_set_kernel_, arg_list);
}

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::ws_print(const exec_ctx_t &ctx,
        compute::compute_stream_t *compute_stream,
        const workspace_t &workspace_) const {
    // This is only for use in DNNL_DEV_MODE
    assert(is_dev_mode());
    if (!is_dev_mode()) return status::runtime_error;

    compute::kernel_arg_list_t arg_list;
    arg_list.append(workspace_.gates());
    arg_list.append(workspace_.states());
    arg_list.append(workspace_.c_states());
    arg_list.append(workspace_.bias());
    arg_list.append(workspace_.grid_comp());

    arg_list.append(pd()->conf.batch);
    arg_list.append(pd()->conf.n_layer);
    arg_list.append(pd()->conf.n_dir);
    arg_list.append(pd()->conf.n_iter);
    arg_list.append(pd()->conf.n_bias);
    arg_list.append(pd()->conf.dhc);
    arg_list.append(pd()->conf.n_gates);
    arg_list.append(pd()->conf.states_ws_ld);
    arg_list.append(pd()->conf.gates_ws_ld);
    arg_list.append(pd()->conf.wic);

    auto nd_range = compute::nd_range_t({1});

    return parallel_for(ctx, nd_range, ws_print_kernel_, arg_list);
}

template <prop_kind_t aprop>
weights_assign_sig((_ref_rnn_common_t<aprop>::assign_weights)) {
    assert(md->format_kind == format_kind::blocked);
    AOC<size_t, 3> weights(weights_, rnn.n_layer, rnn.n_dir, n_parts);
    const auto &blk = md->format_desc.blocking;

    for (int i = 0; i < rnn.n_layer; i++) {
        for (int d = 0; d < rnn.n_dir; d++) {
            size_t offset_weights = 0;
            for (int p = 0; p < n_parts; p++) {
                weights(i, d, p) = OFF3(i, rnn.n_layer, d, rnn.n_dir,
                                           offset_weights, ld * nld)
                        * types::data_type_size(wei_t);
                offset_weights += gates_per_part[p] * blk.strides[3];
            }
        }
    }
}

//********************* Execution function *********************//

template <prop_kind_t aprop>
status_t _ref_rnn_common_t<aprop>::execute_(const exec_ctx_t &ctx) const {

    engine_t *engine = ctx.stream()->engine();
    auto *compute_stream
            = utils::downcast<compute::compute_stream_t *>(ctx.stream());

    auto rnn_pd = this->pd();

    const conf_t &rnn = this->pd()->rnn_conf;

    int n_layer = rnn.n_layer;
    int n_dir = rnn.n_dir;
    int n_states = rnn.n_states;
    int n_iter = rnn.n_iter;
    int n_gates = rnn.n_gates;
    int n_bias = rnn.n_bias;
    int batch = rnn.mb;
    int slc = rnn.slc;
    int sic = rnn.sic;
    int dhc = rnn.dhc;
    int dlc = rnn.dlc;
    int n_parts_weights_iter = rnn.n_parts_weights_iter;
    int n_parts_weights_layer = rnn.n_parts_weights_layer;

    bool is_fwd = rnn.is_fwd;
    bool is_vanilla_gru = rnn.is_vanilla_gru;

    auto &src_layer_native_ = CTX_IN_STORAGE(DNNL_ARG_SRC_LAYER);
    auto &src_iter_native_ = CTX_IN_STORAGE(DNNL_ARG_SRC_ITER);
    auto &src_c_iter_native_ = CTX_IN_STORAGE(DNNL_ARG_SRC_ITER_C);
    auto &wei_layer_native_ = CTX_IN_STORAGE(DNNL_ARG_WEIGHTS_LAYER);
    auto &wei_iter_native_ = CTX_IN_STORAGE(DNNL_ARG_WEIGHTS_ITER);
    auto &bias_native_ = CTX_IN_STORAGE(DNNL_ARG_BIAS);

    auto &dst_last_layer_native_ = is_fwd ? CTX_OUT_STORAGE(DNNL_ARG_DST_LAYER)
                                          : CTX_IN_STORAGE(DNNL_ARG_DST_LAYER);
    auto &dst_last_iter_native_ = is_fwd ? CTX_OUT_STORAGE(DNNL_ARG_DST_ITER)
                                         : CTX_IN_STORAGE(DNNL_ARG_DST_ITER);
    auto &dst_last_iter_c_native_ = is_fwd
            ? CTX_OUT_STORAGE(DNNL_ARG_DST_ITER_C)
            : CTX_IN_STORAGE(DNNL_ARG_DST_ITER_C);

    auto &diff_dst_layer_native_ = CTX_IN_STORAGE(DNNL_ARG_DIFF_DST_LAYER);
    auto &diff_dst_iter_native_ = CTX_IN_STORAGE(DNNL_ARG_DIFF_DST_ITER);
    auto &diff_dst_iter_c_native_ = CTX_IN_STORAGE(DNNL_ARG_DIFF_DST_ITER_C);

    auto scratch_workspace
            = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_space);
    auto &workspace_ = rnn.is_training ? is_fwd
                    ? CTX_OUT_STORAGE(DNNL_ARG_WORKSPACE)
                    : CTX_IN_STORAGE(DNNL_ARG_WORKSPACE)
                                       : *scratch_workspace;
    const auto &workspace
            = workspace_t(workspace_, pd()->conf, pd()->rnn_conf, pd()->off);

    auto scratchpad_gates
            = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_gates);
    auto &scratch_gates = *scratchpad_gates;

    empty_memory_storage_t empty_mem;
    auto scratchpad_cell
            = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_cell);

    auto &scratch_cell = this->pd()->is_lbr() || is_vanilla_gru
            ? *scratchpad_cell
            : empty_mem;

    auto scratchpad_diff_states
            = ctx.get_scratchpad_grantor().get_memory_storage(
                    key_rnn_diff_states);
    auto &scratch_diff_states = is_fwd ? empty_mem : *scratchpad_diff_states;

    auto scratchpad_dhG1
            = ctx.get_scratchpad_grantor().get_memory_storage(key_rnn_diff_ht);
    auto &scratch_dhG1
            = (!is_fwd && is_vanilla_gru) ? *scratchpad_dhG1 : empty_mem;

    auto &diff_src_layer_native_ = CTX_OUT_STORAGE(DNNL_ARG_DIFF_SRC_LAYER);
    auto &diff_src_iter_native_ = CTX_OUT_STORAGE(DNNL_ARG_DIFF_SRC_ITER);
    auto &diff_src_iter_c_native_ = CTX_OUT_STORAGE(DNNL_ARG_DIFF_SRC_ITER_C);

    auto &diff_weights_layer_native_
            = CTX_OUT_STORAGE(DNNL_ARG_DIFF_WEIGHTS_LAYER);
    auto &diff_weights_iter_native_
            = CTX_OUT_STORAGE(DNNL_ARG_DIFF_WEIGHTS_ITER);
    auto &diff_bias_native_ = CTX_OUT_STORAGE(DNNL_ARG_DIFF_BIAS);

    DPRINT("\n%s\n", "+++++++++++++++");
    DPRINT(" aprop = %d\n", (int)aprop);
    DPRINT("%s\n", "+++++++++++++++");
    DPRINT("  n_layer         = %d\n", n_layer);
    DPRINT("  n_dir           = %d\n", n_dir);
    DPRINT("  n_iter          = %d\n", n_iter);
    DPRINT("  n_gates         = %d\n", n_gates);
    DPRINT("  n_bias          = %d\n", n_bias);
    DPRINT("  n_states        = %d\n", n_states);
    DPRINT("  n_weights_layer = %lld\n", (long long)rnn_pd->SLC());
    DPRINT("  n_weights_iter  = %lld\n", (long long)rnn_pd->SIC());
    DPRINT("  batch           = %d\n", batch);
    DPRINT("  slc             = %d\n", slc);
    DPRINT("  sic             = %d\n", sic);
    DPRINT("  dhc             = %d\n", dhc);
    DPRINT("  dlc             = %d\n", dlc);
    DPRINT("%s\n", "+++++++++++++++");
    DPRINT("  is_fwd          = %s\n", is_fwd ? "yes" : "no");
    DPRINT("  is_vanilla_gru  = %s\n", is_vanilla_gru ? "yes" : "no");
    DPRINT("  use_workspace   = %s\n", rnn.use_workspace ? "yes" : "no");
    DPRINT("%s\n", "+++++++++++++++");
    DPRINT("  with_src_iter   = %s\n", rnn_pd->with_src_iter() ? "yes" : "no");
    DPRINT("  with_src_iter_c = %s\n",
            rnn_pd->with_src_iter_c() ? "yes" : "no");
    DPRINT("  with_bias       = %s\n", rnn_pd->with_bias() ? "yes" : "no");
    DPRINT("  with_dst_iter   = %s\n", rnn_pd->with_dst_iter() ? "yes" : "no");
    DPRINT("  with_dst_iter_c = %s\n",
            rnn_pd->with_dst_iter_c() ? "yes" : "no");
    DPRINT("%s\n", "+++++++++++++++");

#if WS_NAN_FILLING
    if (rnn.is_fwd) {
        DPRINT("DEBUG ws NaN filling: (offset, size) states: %ld %ld c_states: "
               "%ld %ld gates: %ld %ld\n",
                ws_states_offset_, rnn.ws_states_size, ws_c_states_offset_,
                rnn.ws_c_states_size, ws_gates_offset_, rnn.ws_gates_size);

        ws_set(compute_stream, workspace_, ws_states_offset_, rnn_utils::states,
                NAN, rnn.ws_states_size / rnn.ws_states_elsz);
        if (rnn_pd->with_src_iter_c()) {
            ws_set(compute_stream, workspace_, ws_c_states_offset_,
                    rnn_utils::c_states, NAN,
                    rnn.ws_c_states_size / sizeof(float));
        }
        ws_set(compute_stream, workspace_, ws_gates_offset_, rnn_utils::gates,
                NAN, rnn.ws_gates_size / rnn.ws_gates_elsz);
        ws_set(compute_stream, workspace_, ws_bias_offset_, rnn_utils::bias,
                NAN, rnn.ws_bias_size / rnn.ws_bias_elsz);
    }
#endif

    DPRINT("\n%s(%d) WS before bias prepare\n\n", __FUNCTION__, __LINE__);
    WS_PRINT(ctx, compute_stream, workspace);

    // TODO: implement without copies
    bool is_lr = !one_of(rnn.exec_dir, r2l, r2l);
    bool is_rl = !one_of(rnn.exec_dir, l2r, l2r);

    // XXX: this function is used for calculating offsets for buffers
    (this->*weights_iter_assign_func)(rnn, rnn_pd->weights_md(1),
            wei_iter_offset_ptr, n_parts_weights_iter, rnn.parts_weights_iter,
            wei_iter_native_, rnn.weights_iter_ld, rnn.weights_iter_nld,
            pd()->weights_type);
    (this->*weights_layer_assign_func)(rnn, rnn_pd->weights_md(0),
            wei_layer_offset_ptr, n_parts_weights_layer,
            rnn.parts_weights_layer, wei_layer_native_, rnn.weights_layer_ld,
            rnn.weights_layer_nld, pd()->weights_type);

    const memory_storage_t *scales_buf = nullptr;
    if (pd()->rnn_conf.is_int8 && pd()->rnn_conf.copy_bias) {
        scales_buf = &CTX_GPU_RES_STORAGE(SCALES_);
    }

    // bias prepare if needed
    if (rnn.copy_bias) {
        CHECK(bias_prepare(ctx, compute_stream, n_layer, n_dir, n_bias, n_gates,
                dhc, workspace.bias(), *scales_buf, wei_layer_native_,
                wei_iter_native_, bias_native_));
    }
    DPRINT("\n%s(%d) WS before copy init\n\n", __FUNCTION__, __LINE__);
    WS_PRINT(ctx, compute_stream, workspace);

    float shift = (pd()->attr()->rnn_data_qparams_.shift_);
    float scale = (pd()->attr()->rnn_data_qparams_.scale_);

    // we first need to copy the initial states and input into ws
    CHECK(copy_init_layer(ctx, compute_stream, is_lr, is_rl, batch, dhc, slc,
            n_iter, n_layer, n_dir, n_states, rnn.states_ws_ld,
            rnn.scratch_diff_states_ld, workspace.states(), scratch_diff_states,
            src_layer_native_, diff_dst_layer_native_));
    const bool quantize = pd()->with_src_iter()
            && pd()->src_md(1)->data_type == data_type::f32 && rnn.is_int8;
    CHECK(copy_init_iter(ctx, compute_stream, batch, dhc, sic, n_iter, n_layer,
            n_dir, n_states, rnn.states_ws_ld, rnn.scratch_diff_states_ld,
            workspace, scratch_diff_states, src_iter_native_,
            src_c_iter_native_, diff_dst_iter_native_, diff_dst_iter_c_native_,
            shift, scale, quantize));

    DPRINT("\n%s(%d) WS before grid\n\n", __FUNCTION__, __LINE__);
    WS_PRINT(ctx, compute_stream, workspace);

    const memory_storage_t *tm_scales_buf = nullptr;
    if (pd()->rnn_conf.is_testmode && pd_->attr()->rnn_tparams_.scales_) {
        tm_scales_buf = &CTX_GPU_RES_STORAGE(TM_SCALES_);
    }

    // run the execution on the grid
    CHECK((this->*grid_computation)(engine, ctx, bias_native_, workspace,
            scratch_gates, scratch_cell, scratch_diff_states, scratch_dhG1,
            wei_layer_native_, wei_iter_native_, diff_weights_layer_native_,
            diff_weights_iter_native_, diff_bias_native_, scales_buf,
            tm_scales_buf));

    DPRINT("\n%s(%d) WS before copy res\n\n", __FUNCTION__, __LINE__);
    WS_PRINT(ctx, compute_stream, workspace);

    // Finally we copy the results to the result buffers

    const bool dequantize_l
            = pd()->dst_md(0)->data_type == data_type::f32 && rnn.is_int8;
    CHECK(copy_res_layer(ctx, compute_stream, is_lr, is_rl, batch, dhc, slc,
            n_iter, n_layer, n_dir, n_states, rnn.states_ws_ld,
            rnn.scratch_diff_states_ld, scratch_diff_states,
            dst_last_layer_native_, diff_src_layer_native_, workspace.states(),
            shift, scale, dequantize_l));
    const bool dequantize_i = pd()->with_dst_iter()
            && pd()->dst_md(1)->data_type == data_type::f32 && rnn.is_int8;
    CHECK(copy_res_iter(ctx, compute_stream, batch, dhc, sic, n_iter, n_layer,
            n_dir, n_states, rnn.states_ws_ld, rnn.scratch_diff_states_ld,
            scratch_diff_states, dst_last_iter_native_, dst_last_iter_c_native_,
            diff_src_iter_native_, diff_src_iter_c_native_, workspace, shift,
            scale, dequantize_i));

    return status::success;
};
// Fix for MSVS warning C4661.
template <>
cell_execution_sig(ref_rnn_fwd_t::cell_execution);
template <>
cell_execution_sig(ref_rnn_bwd_t::cell_execution);
template <>
cell_execution_sig(ref_rnn_fwd_t::cell_execution_gru);
template <>
cell_execution_sig(ref_rnn_bwd_t::cell_execution_gru);
template <>
cell_execution_sig(ref_rnn_fwd_t::cell_execution_gru_lbr);
template <>
cell_execution_sig(ref_rnn_bwd_t::cell_execution_gru_lbr);
template <>
elemwise_sig(ref_rnn_fwd_t::rnn_elemwise);
template <>
elemwise_sig(ref_rnn_bwd_t::rnn_elemwise);
template <>
elemwise_sig(ref_rnn_fwd_t::lstm_elemwise);
template <>
elemwise_sig(ref_rnn_bwd_t::lstm_elemwise);
template <>
elemwise_sig(ref_rnn_fwd_t::lstm_elemwise_u8s8);
template <>
elemwise_sig(ref_rnn_bwd_t::lstm_elemwise_u8s8);
template <>
elemwise_sig_gru_lbr(ref_rnn_fwd_t::gru_lbr_elemwise);
template <>
elemwise_sig_gru_lbr(ref_rnn_bwd_t::gru_lbr_elemwise);
template <>
elemwise_sig_gru(ref_rnn_fwd_t::gru_elemwise);
template <>
elemwise_sig_gru(ref_rnn_bwd_t::gru_elemwise);

template struct _ref_rnn_common_t<prop_kind::forward>;
template struct _ref_rnn_common_t<prop_kind::backward>;

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl

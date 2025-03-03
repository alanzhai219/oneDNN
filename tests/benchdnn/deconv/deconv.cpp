/*******************************************************************************
* Copyright 2018-2022 Intel Corporation
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

#include <algorithm>
#include <utility>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils/parallel.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "binary/binary.hpp"
#include "deconv/deconv.hpp"
#include "prelu/prelu.hpp"

namespace deconv {

int transpose_data_wei(
        const prb_t *prb, const dnn_mem_t &wei, const dnn_mem_t &wei_tr) {
    benchdnn_parallel_nd(prb->g, prb->oc / prb->g, prb->ic / prb->g, prb->kd,
            prb->kh, prb->kw,
            [&](int64_t g, int64_t oc, int64_t ic, int64_t kd, int64_t kh,
                    int64_t kw) {
                int64_t ch_idx
                        = (g * prb->ic / prb->g + ic) * prb->oc / prb->g + oc;
                int64_t idx = ((ch_idx * prb->kd + kd) * prb->kh + kh) * prb->kw
                        + kw;
                ((float *)wei_tr)[idx]
                        = ((float *)wei)[wei_off_f(prb, g, oc, ic, kd, kh, kw)];
            });

    return OK;
}

double get_non_zero_trust_percent(const prb_t *prb, data_kind_t kind) {
    auto negative_to_zero = [&]() {
        using pk = attr_t::post_ops_t::kind_t;
        const auto &po = prb->attr.post_ops;
        int count = 0;

        // Check for all post-ops that convert negative to zero
        std::vector<pk> non_neg_po {pk::ABS, pk::BRELU};
        std::vector<pk> non_neg_alpha_0_po {
                pk::CLIP, pk::CLIP_V2, pk::ELU, pk::RELU};
        for (int i = 0; i < po.len(); ++i) {
            const auto &e = po.entry[i];
            if (!e.is_eltwise_kind()) continue;

            auto k = e.kind;
            auto alpha = e.eltwise.alpha;

            count += std::any_of(non_neg_po.cbegin(), non_neg_po.cend(),
                    [k](const pk alg) { return alg == k; });
            count += std::any_of(non_neg_alpha_0_po.cbegin(),
                    non_neg_alpha_0_po.cend(), [k, alpha](const pk alg) {
                        return alg == k && alpha == 0;
                    });
        }
        // Check for u8 dst
        count += prb->cfg[DST].dt == dnnl_u8;
        // Check for physically padded area in the output
        count += prb->od > prb->id || prb->oh > prb->ih || prb->ow > prb->iw;

        return !!count;
    };

    double trust = 0.3; /* why? */
    switch (kind) {
        case SRC: trust /= prb->sd * prb->sh * prb->sw; break;
        case WEI:
            trust /= 1. * prb->kd * prb->kh * prb->kw
                    / MIN3(prb->kd * prb->kh * prb->kw,
                            prb->id * prb->ih * prb->iw,
                            prb->od * prb->oh * prb->ow);
            break;
        case BIA:
            trust = 0.8 * prb->cfg[DST].f_sparsity; /* why? */
            break;
        case DST: trust /= (1.f + negative_to_zero()); break;
        default: assert(!"unsupported data kind");
    }

    return trust;
}

bool need_src_init(const prb_t *prb) {
    return !(prb->dir == BWD_D);
}

bool need_wei_init(const prb_t *prb) {
    return !(prb->dir & FLAG_BWD && prb->dir & FLAG_WEI);
}

bool need_bia_init(const prb_t *prb) {
    return need_wei_init(prb);
}

bool need_dst_init(const prb_t *prb) {
    return !(prb->dir & FLAG_FWD)
            || (prb->attr.post_ops.find(attr_t::post_ops_t::SUM) >= 0);
}

int fill_src(
        const prb_t *prb, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *res) {
    const bool check_reorder
            = (is_bench_mode(CORR)) && (mem_dt.dt() != mem_fp.dt());
    dnn_mem_t extra_mem;
    if (check_reorder) {
        extra_mem = dnn_mem_t(mem_dt.md_, dnnl_f32, tag::abx, get_cpu_engine());
    }
    dnn_mem_t &mem_00 = check_reorder ? extra_mem : mem_fp;

    // Use dense filling for small problems.
    int src_nelems_mask = powf(2.f, prb->ndims) - 1;
    src_nelems_mask -= 1; // remove minibatch as independent dimension
    auto src_nelems = prb->desc_nelems(DNNL_ARG_SRC, src_nelems_mask);
    if (prb->has_groups) src_nelems /= prb->g; // groups are also independent

    const auto &c = prb->cfg[SRC];
    const int range = c.f_max - c.f_min + 1;
    const float sparsity = src_nelems < 100 ? 1.f : c.f_sparsity;

    benchdnn_parallel_nd(prb->mb, prb->ic, prb->id, prb->ih, prb->iw,
            [&](int64_t mb, int64_t ic, int64_t id, int64_t ih, int64_t iw) {
                const int64_t gen
                        = 101 * id + 103 * ih + 107 * iw + 109 * mb + 113 * ic;
                const bool non_base = flip_coin(gen, sparsity);
                float value = non_base ? c.f_min + gen * c.f_step % range
                                       : c.f_base;

                maybe_zero_point(
                        prb->attr, value, prb->src_zp, ic, DNNL_ARG_SRC, true);

                ((float *)mem_00)[src_off_f(prb, mb, 0, ic, id, ih, iw)]
                        = round_to_nearest_representable(mem_dt.dt(), value);
            });

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (check_reorder) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        int rc = std::memcmp((void *)mem_fp, (void *)mem_00, mem_00.size());
        if (rc != 0) {
            res->state = FAILED;
            SAFE(FAIL, WARN);
        }
    }

    return OK;
}

int fill_wei(
        const prb_t *prb, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *res) {
    const bool wino_s8 = prb->alg == WINO && prb->cfg[WEI].dt == dnnl_s8;
    const bool is_def_zp = prb->attr.zero_points.is_def(DNNL_ARG_SRC);
    const bool diff_data_type = mem_dt.dt() != mem_fp.dt();

    dnnl_data_type_t dt_check = dnnl_s8;
#if DNNL_AARCH64
    /* Note for x64:
    Both data types of src and weight are s8, oneDNN addds 128 to one of the s8
    input to make it of type u8 instead, as explained in
    https://oneapi-src.github.io/oneDNN/dev_guide_int8_computations.html or
    doc/advanced/int8_computations.md
    It is because `VPDPBUSD` instruction uses the combination of s8 and u8 as
    input.

    Note for AArch64:
    Because dot product instructions of AArch64 "SDOT" receives s8 input
    for both src and weight, the addition (and its counterpart of subtraction)
    is not required for AArch64.
    */
    if (res->impl_name.find("jit", 0) == 0) dt_check = dnnl_u8;
#endif

    const bool wei_x8x8
            = prb->cfg[WEI].dt == dnnl_s8 && prb->cfg[SRC].dt == dt_check;
    const bool check_reorder = (is_bench_mode(CORR)) && diff_data_type
            && !wino_s8 && !wei_x8x8 && is_def_zp;

    dnn_mem_t extra_mem;
    if (check_reorder) {
        extra_mem = dnn_mem_t(mem_dt.md_, dnnl_f32, tag::abx, get_cpu_engine());
    }
    dnn_mem_t &mem_00 = check_reorder ? extra_mem : mem_fp;

    const auto &c = prb->cfg[WEI];
    const int range = c.f_max - c.f_min + 1;

    benchdnn_parallel_nd(prb->g, prb->oc / prb->g, prb->ic / prb->g, prb->kd,
            prb->kh, prb->kw,
            [&](int64_t g, int64_t oc, int64_t ic, int64_t kd, int64_t kh,
                    int64_t kw) {
                const int64_t gen = 113 * g + 127 * kd + 131 * kh + 137 * kw
                        + 139 * oc + 149 * ic + 151;
                const bool non_base = flip_coin(gen, c.f_sparsity);
                const float value = non_base ? c.f_min + gen * c.f_step % range
                                             : c.f_base;
                ((float *)mem_00)[wei_off_f(prb, g, oc, ic, kd, kh, kw)]
                        = value;
            });

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (check_reorder) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        int rc = std::memcmp((void *)mem_fp, (void *)mem_00, mem_00.size());
        if (rc != 0) {
            res->state = FAILED;
            SAFE(FAIL, WARN);
        }
    }
    if ((wei_x8x8 || !is_def_zp) && is_cpu()) {
        // Check that s8 -> s8_comp exists in the library since users may have
        // already quantized data.
        dnn_mem_t mem_fp_s8(mem_fp.md_, dnnl_s8, get_cpu_engine());
        dnn_mem_t mem_dt_s8(mem_dt.md_, dnnl_s8, get_test_engine());
        SAFE(mem_fp_s8.reorder(mem_fp), WARN);
        SAFE(mem_dt_s8.reorder(mem_fp_s8), WARN);
        SAFE(mem_dt.size() == mem_dt_s8.size() ? OK : FAIL, WARN);
        int rc = std::memcmp((void *)mem_dt, (void *)mem_dt_s8, mem_dt.size());
        SAFE(rc == 0 ? OK : FAIL, WARN);
    }
    return OK;
}

int fill_bia(
        const prb_t *prb, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *res) {
    const bool check_reorder
            = (is_bench_mode(CORR)) && (mem_dt.dt() != mem_fp.dt());
    dnn_mem_t extra_mem;
    if (check_reorder)
        extra_mem = dnn_mem_t(mem_dt.md_, dnnl_f32, tag::x, get_cpu_engine());
    dnn_mem_t &mem_00 = check_reorder ? extra_mem : mem_fp;

    const size_t nelems = mem_00.nelems();
    if (nelems == 0) return OK;

    const auto &c = prb->cfg[BIA];
    const int range = c.f_max - c.f_min + 1;

    for (size_t i = 0; i < nelems; ++i) {
        const int gen = (int)(151 * i);
        const bool non_base = flip_coin(gen, c.f_sparsity);
        const float value
                = non_base ? c.f_min + gen * c.f_step % range : c.f_base;

        ((float *)mem_00)[i] = value;
    }

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (check_reorder) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        int rc = std::memcmp((void *)mem_fp, (void *)mem_00, mem_00.size());
        if (rc != 0) {
            res->state = FAILED;
            SAFE(FAIL, WARN);
        }
    }
    return OK;
}

int fill_dst_with_params(const prb_t *prb, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        dnnl_data_type_t dt, double sparsity, int min, int max, int base,
        int step, res_t *res) {
    const bool check_reorder
            = (is_bench_mode(CORR)) && (mem_dt.dt() != mem_fp.dt());
    dnn_mem_t extra_mem;
    if (check_reorder) {
        extra_mem = dnn_mem_t(mem_dt.md_, dnnl_f32, tag::abx, get_cpu_engine());
    }

    dnn_mem_t &mem_00 = check_reorder ? extra_mem : mem_fp;
    const int range = max - min + 1;

    benchdnn_parallel_nd(prb->mb, prb->oc, prb->od, prb->oh, prb->ow,
            [&](int64_t mb, int64_t oc, int64_t od, int64_t oh, int64_t ow) {
                const int64_t gen
                        = 157 * od + 163 * oh + 167 * ow + 173 * mb + 179 * oc;
                const bool non_base = flip_coin(gen, sparsity);
                const float value = non_base ? min + gen * step % range : base;

                ((float *)mem_00)[dst_off_f(prb, mb, 0, oc, od, oh, ow)]
                        = value;
            });

    SAFE(mem_dt.reorder(mem_00), WARN);
    if (check_reorder) {
        SAFE(mem_fp.reorder(mem_dt), WARN);
        int rc = std::memcmp((void *)mem_fp, (void *)mem_00, mem_00.size());
        if (rc != 0) {
            res->state = FAILED;
            SAFE(FAIL, WARN);
        }
    }

    return OK;
}

int fill_dst(
        const prb_t *prb, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *res) {
    auto dst_dt = mem_dt.dt();
    int sum_ind = prb->attr.post_ops.find(attr_t::post_ops_t::kind_t::SUM);
    auto sum_dt = (sum_ind != -1) ? prb->attr.post_ops.entry[sum_ind].sum.dt
                                  : dnnl_data_type_undef;
    bool diff_sum_dst_types
            = sum_dt != dnnl_data_type_undef && sum_dt != dst_dt;
    bool sum_dt_is_int8 = sum_dt == dnnl_s8 || sum_dt == dnnl_u8;

    const auto &c = prb->cfg[DST];
    float f_min = c.f_min;
    float f_max = c.f_max;
    if (diff_sum_dst_types && sum_dt_is_int8) {
        f_min = lowest_dt(sum_dt);
        f_max = max_dt(sum_dt);
    }

    // Change mem dt to sum dt, so we can save sum data properly.
    if (diff_sum_dst_types) { mem_dt.set_dt(sum_dt); }

    fill_dst_with_params(prb, mem_dt, mem_fp, sum_dt, c.f_sparsity, f_min,
            f_max, c.f_base, c.f_step, res);

    // Return dst data type back.
    if (diff_sum_dst_types) { mem_dt.set_dt(dst_dt); }
    return OK;
}

dnnl_status_t init_pd(dnnl_engine_t engine, const prb_t *prb,
        dnnl_primitive_desc_t &dpd, res_t *res, dir_t dir,
        const_dnnl_primitive_desc_t hint) {
    dnnl_deconvolution_desc_t cd;

    auto src_d = dnn_mem_t::init_md(
            prb->ndims, prb->src_dims().data(), prb->cfg[SRC].dt, prb->stag);
    auto wei_d = dnn_mem_t::init_md(prb->ndims + prb->has_groups,
            prb->wei_dims().data(), prb->cfg[WEI].dt, prb->wtag);
    auto bia_d = dnn_mem_t::init_md(
            1, prb->bia_dims().data(), prb->cfg[BIA].dt, tag::any);
    auto dst_d = dnn_mem_t::init_md(
            prb->ndims, prb->dst_dims().data(), prb->cfg[DST].dt, prb->dtag);

    dnnl_alg_kind_t alg = dnnl_deconvolution_direct;
    if (prb->alg == WINO) alg = dnnl_deconvolution_winograd;

    switch (prb->dir) {
        case FWD_D:
        case FWD_B:
        case FWD_I:
            DNN_SAFE_STATUS(dnnl_dilated_deconvolution_forward_desc_init(&cd,
                    prb->dir == FWD_I ? dnnl_forward_inference
                                      : dnnl_forward_training,
                    alg, &src_d, &wei_d, prb->dir == FWD_B ? &bia_d : nullptr,
                    &dst_d, prb->strides().data(), prb->dilations().data(),
                    prb->padding().data(), prb->padding_r().data()));
            break;
        case BWD_D:
            DNN_SAFE_STATUS(dnnl_dilated_deconvolution_backward_data_desc_init(
                    &cd, alg, &src_d, &wei_d, &dst_d, prb->strides().data(),
                    prb->dilations().data(), prb->padding().data(),
                    prb->padding_r().data()));
            break;
        case BWD_W:
        case BWD_WB:
            DNN_SAFE_STATUS(
                    dnnl_dilated_deconvolution_backward_weights_desc_init(&cd,
                            alg, &src_d, &wei_d,
                            prb->dir == BWD_W ? nullptr : &bia_d, &dst_d,
                            prb->strides().data(), prb->dilations().data(),
                            prb->padding().data(), prb->padding_r().data()));
            break;
        default: DNN_SAFE_STATUS(dnnl_invalid_arguments);
    }

    DNN_SAFE_STATUS(cd.accum_data_type == prb->cfg[ACC].dt
                    ? dnnl_success
                    : dnnl_unimplemented);

    attr_args_t attr_args;
    attr_args.prepare_output_scales(prb->attr, prb->scales, prb->oc);
    attr_args.prepare_post_ops_mds(
            prb->attr, prb->ndims, prb->dst_dims().data());
    auto dnnl_attr = make_benchdnn_dnnl_wrapper(
            create_dnnl_attr(prb->attr, attr_args));

    return dnnl_primitive_desc_create(&dpd, &cd, dnnl_attr, engine, nullptr);
}

int init_prim_ref(
        benchdnn_dnnl_wrapper_t<dnnl_primitive_t> &prim_ref, const prb_t *prb) {
    if (!(is_bench_mode(CORR) && is_gpu() && fast_ref_gpu)) return OK;

    // Create a new copy of prb to avoid potentially corrupting the test by
    // modifying prb in place.
    // DIRECT algorithm is used to prevent fallback  to the slow benchdnn
    // reference implementation.
    auto cpu_attr = prb->attr;
    update_cpu_ref_attrs(cpu_attr);
    prb_t prb_cpu {*prb, prb->dir, conf_f32, tag::abx, tag::abx, tag::abx,
            DIRECT, cpu_attr, prb->mb};
    dnnl_primitive_desc_t pd_ref_ {};
    init_pd(get_cpu_engine(), &prb_cpu, pd_ref_, nullptr, prb->dir, nullptr);
    auto pd_ref = make_benchdnn_dnnl_wrapper(pd_ref_);

    dnnl_primitive_t prim_ref_ {};
    if (pd_ref) {
        if (query_impl_info(pd_ref) == "ref:any") return OK;
        DNN_SAFE(dnnl_primitive_create(&prim_ref_, pd_ref), WARN);
        BENCHDNN_PRINT(5, "CPU reference oneDNN implementation: %s\n",
                query_impl_info(pd_ref).c_str());
    }
    prim_ref.reset(prim_ref_);
    return OK;
}

void skip_unimplemented_prb(const prb_t *prb, res_t *res) {
    skip_unimplemented_data_type(
            {prb->cfg[SRC].dt, prb->cfg[WEI].dt, prb->cfg[DST].dt}, prb->dir,
            res);
    skip_unimplemented_sum_po(prb->attr, res);

    // GPU supports only post ops and all but x8s8bf16 cfg
    if (is_gpu()) {
        const bool is_x8s8bf16_cfg
                = prb->cfg[WEI].dt == dnnl_s8 && prb->cfg[DST].dt == dnnl_bf16;
        const bool fwd_ok = !is_x8s8bf16_cfg;
        if (!fwd_ok) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }
    } else if (is_cpu()) {
        if (prb->attr.oscale.runtime) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
        }
    }
}

void skip_invalid_prb(const prb_t *prb, res_t *res) {}

void setup_cmp(compare::compare_t &cmp, const prb_t *prb, data_kind_t kind,
        const args_t &ref_args) {
    const bool compare_with_norm = (prb->alg & WINO);
    cmp.set_norm_validation_mode(compare_with_norm);

    float trh = prb->cfg[kind].eps;
    if ((prb->alg & WINO) && (prb->dir & FLAG_WEI)) {
        // This is an empirical equation derived by observing growth error with
        // increasing 'k' dimension in gemm of winograd
        const float log_const = log10(0.125 * prb->mb * prb->oh * prb->ow);
        trh = prb->cfg[kind].eps * (MAX2(1, pow(10, 0.4 * log_const)));
    }
    cmp.set_threshold(trh);

    const float zpp = (1.f - get_non_zero_trust_percent(prb, kind)) * 100.f;
    cmp.set_zero_trust_percent(zpp);
}

int doit(const prb_t *prb, res_t *res) {
    if (bench_mode == LIST) return res->state = LISTED, OK;

    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim;
    SAFE(init_prim(prim, init_pd, prb, res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    auto const_pd = query_pd(prim);

    if (check_mem_size(const_pd) != OK) {
        return res->state = SKIPPED, res->reason = NOT_ENOUGH_RAM, OK;
    }

    const auto &src_md = prb->dir == BWD_D
            ? query_md(const_pd, DNNL_ARG_DIFF_SRC)
            : query_md(const_pd, DNNL_ARG_SRC);
    const auto &wei_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd, DNNL_ARG_WEIGHTS);
    const auto &bia_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd, DNNL_ARG_BIAS);
    const auto &dst_md = prb->dir & FLAG_BWD
            ? query_md(const_pd, DNNL_ARG_DIFF_DST)
            : query_md(const_pd, DNNL_ARG_DST);
    const auto &scratchpad_md = query_md(const_pd, DNNL_ARG_SCRATCHPAD);
    auto wei_tr_md = wei_md;

    const bool with_groups = true;
    std::swap(wei_tr_md.dims[with_groups + 0], wei_tr_md.dims[with_groups + 1]);

    const auto fp = dnnl_f32;
    const auto src_tag = tag::abx;
    const auto wei_tag = tag::abx;

    // Use CPU prim as the reference in GPU testing to reduce testing time.
    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim_ref;
    SAFE(init_prim_ref(prim_ref, prb), WARN);

    const auto &test_engine = get_test_engine();
    const auto &ref_engine = get_cpu_engine();

    dnn_mem_t src_dt(src_md, test_engine);
    dnn_mem_t wei_dt(wei_md, test_engine);
    dnn_mem_t dst_dt(dst_md, test_engine);
    dnn_mem_t bia_dt(bia_md, test_engine);
    dnn_mem_t scratchpad_dt(scratchpad_md, test_engine);
    std::vector<dnn_mem_t> binary_po_fp, binary_po_dt;
    std::vector<int> binary_po_args;
    SAFE(binary::setup_binary_po(
                 const_pd, binary_po_args, binary_po_dt, binary_po_fp),
            WARN);
    std::vector<dnn_mem_t> prelu_po_fp, prelu_po_dt;
    std::vector<int> prelu_po_args;
    SAFE(prelu::setup_prelu_po(
                 const_pd, prelu_po_args, prelu_po_fp, prelu_po_dt),
            WARN);

    dnn_mem_t src_fp(src_md, fp, src_tag, ref_engine);
    dnn_mem_t wei_fp(wei_md, fp, wei_tag, ref_engine);
    dnn_mem_t dst_fp(dst_md, fp, src_tag, ref_engine);
    dnn_mem_t wei_tr_fp(wei_tr_md, fp, wei_tag, ref_engine);
    dnn_mem_t bia_fp(bia_md, fp, tag::x, ref_engine);
    dnn_mem_t scratchpad_fp;

    if (prim_ref)
        scratchpad_fp = dnn_mem_t(
                query_md(query_pd(prim_ref), DNNL_ARG_SCRATCHPAD), ref_engine);

    dnn_mem_t src_zero_points_m;
    dnn_mem_t dst_zero_points_m;
    dnn_mem_t scales;

    /* fill memory + reorders <-> */
    if (need_dst_init(prb)) SAFE(fill_dst(prb, dst_dt, dst_fp, res), WARN);
    if (need_src_init(prb)) SAFE(fill_src(prb, src_dt, src_fp, res), WARN);
    if (need_wei_init(prb)) {
        SAFE(fill_wei(prb, wei_dt, wei_fp, res), WARN);
        SAFE(transpose_data_wei(prb, wei_fp, wei_tr_fp), WARN);
    }
    if (need_bia_init(prb)) SAFE(fill_bia(prb, bia_dt, bia_fp, res), WARN);

    args_t args, ref_args;

    if (prb->dir & FLAG_FWD) {
        maybe_prepare_runtime_zero_points(src_zero_points_m, prb->attr,
                DNNL_ARG_SRC, prb->ic, prb->src_zp);
        maybe_prepare_runtime_zero_points(dst_zero_points_m, prb->attr,
                DNNL_ARG_DST, prb->oc, prb->dst_zp);
        maybe_prepare_runtime_scales(
                scales, prb->attr.oscale, prb->oc, prb->scales);

        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_BIAS, bia_dt);
        args.set(DNNL_ARG_DST, dst_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);
        args.set(binary_po_args, binary_po_dt);
        args.set(prelu_po_args, prelu_po_dt);
        args.set(DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_SRC, src_zero_points_m);
        args.set(DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_DST, dst_zero_points_m);
        args.set(DNNL_ARG_ATTR_OUTPUT_SCALES, scales);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_BIAS, bia_fp);
            ref_args.set(DNNL_ARG_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);
            ref_args.set(binary_po_args, binary_po_fp);
            ref_args.set(prelu_po_args, prelu_po_fp);

            check_correctness(
                    prb, {DST}, args, ref_args, setup_cmp, res, prim_ref);
        }
    } else if (prb->dir == BWD_D) {
        args.set(DNNL_ARG_DIFF_DST, dst_dt);
        args.set(DNNL_ARG_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_DIFF_SRC, src_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_DIFF_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_DIFF_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);

            check_correctness(
                    prb, {SRC}, args, ref_args, setup_cmp, res, prim_ref);
        }
    } else if (prb->dir & FLAG_BWD && prb->dir & FLAG_WEI) {
        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_DIFF_DST, dst_dt);
        args.set(DNNL_ARG_DIFF_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_DIFF_BIAS, bia_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_DIFF_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_DIFF_BIAS, bia_fp);
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);

            check_correctness(
                    prb, {WEI, BIA}, args, ref_args, setup_cmp, res, prim_ref);
        }
    } else {
        SAFE(FAIL, CRIT);
    }

    return measure_perf(res, prim, args);
}

} // namespace deconv

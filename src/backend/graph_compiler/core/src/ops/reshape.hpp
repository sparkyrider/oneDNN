/*******************************************************************************
 * Copyright 2020-2023 Intel Corporation
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
#ifndef BACKEND_GRAPH_COMPILER_CORE_SRC_OPS_RESHAPE_HPP
#define BACKEND_GRAPH_COMPILER_CORE_SRC_OPS_RESHAPE_HPP

#include <vector>
#include <compiler/ir/graph/graph.hpp>
#include <compiler/ir/graph/traits.hpp>

namespace dnnl {
namespace impl {
namespace graph {
namespace gc {
namespace ops {
// the dynamic reshape op. The first input is the tensor to reshape. The second
// is the target shape. We currently only support the case when the second input
// is constant
class dynamic_reshape_op : public sc_op,
                           public op_traits::auto_copyable_t,
                           public op_traits::may_quantize_t,
                           public op_traits::constant_optimizable_t {
public:
    dynamic_reshape_op(const std::vector<graph_tensor_ptr> &ins,
            const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs);
    void query_format(context_ptr ctx,
            std::vector<std::vector<format_stride_pair>> &supported_ins,
            std::vector<std::vector<format_stride_pair>> &supported_outs)
            override;
    ir_module_ptr get_func(context_ptr ctx) override;
    sc_op_ptr constant_optimize(sc_graph_t &graph) override;
};

// the static reshape op. The first input is the tensor to reshape. The second
// is the target tensor. Shape info is included in attributes.
class static_reshape_op : public sc_op,
                          public op_traits::auto_copyable_t,
                          public op_traits::constant_optimizable_t {
public:
    static_reshape_op(const std::vector<graph_tensor_ptr> &ins,
            const std::vector<graph_tensor_ptr> &outs, const any_map_t &attrs);
    void query_format(context_ptr ctx,
            std::vector<std::vector<format_stride_pair>> &supported_ins,
            std::vector<std::vector<format_stride_pair>> &supported_outs)
            override;
    ir_module_ptr get_func(context_ptr ctx) override;
    sc_op_ptr constant_optimize(sc_graph_t &graph) override;
};
} // namespace ops
} // namespace gc
} // namespace graph
} // namespace impl
} // namespace dnnl

#endif

/*******************************************************************************
* Copyright 2021-2022 Arm Ltd. and affiliates
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

#ifndef CPU_AARCH64_ACL_UTILS_HPP
#define CPU_AARCH64_ACL_UTILS_HPP

#include <mutex>

#include "oneapi/dnnl/dnnl_types.h"

#include "common/dnnl_thread.hpp"
#include "common/memory_tracking.hpp"
#include "common/primitive.hpp"
#include "common/utils.hpp"

#include "arm_compute/runtime/NEON/NEFunctions.h"
#include "arm_compute/runtime/Scheduler.h"

namespace dnnl {
namespace impl {
namespace cpu {
namespace aarch64 {

namespace acl_utils {

arm_compute::DataType get_acl_data_t(const dnnl_data_type_t dt);
arm_compute::ActivationLayerInfo get_acl_act(const primitive_attr_t &attr);
arm_compute::ActivationLayerInfo get_acl_act(const eltwise_desc_t &ed);
bool acl_act_ok(alg_kind_t eltwise_activation);

// Convert a memory desc to an arm_compute::TensorInfo. Note that memory desc
// must be blocking format, plain, dense and have no zero dimensions.
status_t tensor_info(arm_compute::TensorInfo &info, const memory_desc_t &md);
status_t tensor_info(
        arm_compute::TensorInfo &info, const memory_desc_wrapper &md);

// Insert a dimension of size 1 at the index dim_i of TensorInfo
status_t insert_singleton_dimension(arm_compute::TensorInfo &ti, size_t dim_i);

// Reorder the logical dimensions of the memory descriptors (mds) by stride so
// that accessing the tensor elements in the natural order is dense. Note, this
// does not reorder the data, it just reorders the logical indices. The
// permutation is common to all mds, so the function returns when it cannot find
// a dimension with a common smallest stride. Returns the number of dimensions
// that we managed to reorder to be dense.
int reorder_dimensions_by_stride(std::vector<memory_desc_t *> permuted_mds,
        std::vector<const memory_desc_t *> mds);

// Logs a custom 'info' line describing an unsupported case
#define LOG_ACL_UNSUPPORTED(msg) \
    do { \
        if (get_verbose() >= 2) \
            printf("onednn_verbose,cpu,acl,unsupported: %s\n", (msg)); \
    } while (0)

// Returns unimplemented if error code x is NOT OK
#define ACL_CHECK_VALID(x) \
    do { \
        arm_compute::Status s = x; \
        if (s.error_code() != arm_compute::ErrorCode::OK) { \
            LOG_ACL_UNSUPPORTED(s.error_description().c_str()); \
            return dnnl::impl::status::unimplemented; \
        } \
    } while (0)

// Returns unimplemented on condition x == true
#define ACL_CHECK_SUPPORT(x, msg) \
    do { \
        if (x) { \
            LOG_ACL_UNSUPPORTED(msg); \
            return dnnl::impl::status::unimplemented; \
        } \
    } while (0)

} // namespace acl_utils

} // namespace aarch64
} // namespace cpu
} // namespace impl
} // namespace dnnl

#endif // CPU_AARCH64_ACL_UTILS_HPP

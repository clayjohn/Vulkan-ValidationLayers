/* Copyright (c) 2018-2024 The Khronos Group Inc.
 * Copyright (c) 2018-2024 Valve Corporation
 * Copyright (c) 2018-2024 LunarG, Inc.
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
 */

#pragma once

#include <vector>
#include <mutex>

#include "external/inplace_function.h"
#include "gpu/core/gpu_state_tracker.h"
#include "gpu/descriptor_validation/gpuav_descriptor_set.h"
#include "gpu/resources/gpu_resources.h"
#include "generated/vk_object_types.h"
#include "gpu/shaders/gpu_shaders_constants.h"

// We pull in most the core state tracking files
// gpuav_subclasses.h should NOT be included by any other header file
#include "state_tracker/buffer_state.h"
#include "state_tracker/image_state.h"
#include "state_tracker/cmd_buffer_state.h"
#include "state_tracker/sampler_state.h"
#include "state_tracker/ray_tracing_state.h"

namespace gpuav {

class Validator;

struct DescSetState {
    uint32_t num;
    std::shared_ptr<DescriptorSet> state;
    BindingVariableMap binding_req;
    // State that will be used by the GPU-AV shader instrumentation
    // For update-after-bind, this will be set during queue submission
    // Otherwise it will be set when the DescriptorSet is bound.
    std::shared_ptr<DescriptorSet::State> gpu_state;
    std::shared_ptr<DescriptorSet::State> output_state;
};

struct DescBindingInfo {
    VkBuffer bindless_state_buffer;
    VmaAllocation bindless_state_buffer_allocation;
    // Hold a buffer for each descriptor set
    // Note: The index here is from vkCmdBindDescriptorSets::firstSet
    std::vector<DescSetState> descriptor_set_buffers;
};

// Used for draws/dispatch/traceRays indirect
struct CmdIndirectState {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint32_t draw_count;
    uint32_t stride;
    VkBuffer count_buffer;
    VkDeviceSize count_buffer_offset;
    VkDeviceAddress indirectDeviceAddress;
};

class CommandBuffer : public gpu_tracker::CommandBuffer {
  public:
    // per vkCmdBindDescriptorSet() state
    std::vector<DescBindingInfo> di_input_buffer_list;
    VkBuffer current_bindless_buffer = VK_NULL_HANDLE;
    uint32_t draw_index = 0;
    uint32_t compute_index = 0;
    uint32_t trace_rays_index = 0;

    CommandBuffer(Validator &gpuav, VkCommandBuffer handle, const VkCommandBufferAllocateInfo *pCreateInfo,
                  const vvl::CommandPool *pool);
    ~CommandBuffer();

    bool PreProcess() final;
    void PostProcess(VkQueue queue, const Location &loc) final;
    [[nodiscard]] bool ValidateBindlessDescriptorSets();

    const VkDescriptorSetLayout &GetInstrumentationDescriptorSetLayout() const {
        assert(instrumentation_desc_set_layout_ != VK_NULL_HANDLE);
        return instrumentation_desc_set_layout_;
    }

    // Bindings: {error output buffer}
    const VkDescriptorSet &GetValidationCmdCommonDescriptorSet() const {
        assert(validation_cmd_desc_set_ != VK_NULL_HANDLE);
        return validation_cmd_desc_set_;
    }

    const VkDescriptorSetLayout &GetValidationCmdCommonDescriptorSetLayout() const {
        assert(validation_cmd_desc_set_layout_ != VK_NULL_HANDLE);
        return validation_cmd_desc_set_layout_;
    }

    uint32_t GetValidationErrorBufferDescSetIndex() const { return 0; }

    const VkBuffer &GetErrorOutputBuffer() const {
        assert(error_output_buffer_.buffer != VK_NULL_HANDLE);
        return error_output_buffer_.buffer;
    }

    VkDeviceSize GetCmdErrorsCountsBufferByteSize() const { return 8192 * sizeof(uint32_t); }

    const VkBuffer &GetCmdErrorsCountsBuffer() const {
        assert(cmd_errors_counts_buffer_.buffer != VK_NULL_HANDLE);
        return cmd_errors_counts_buffer_.buffer;
    }

    const gpu::DeviceMemoryBlock &GetBdaRangesSnapshot() const { return bda_ranges_snapshot_; }

    void ClearCmdErrorsCountsBuffer() const;

    void Destroy() final;
    void Reset() final;

    gpu::GpuResourcesManager gpu_resources_manager;
    // Using stdext::inplace_function over std::function to allocate memory in place
    using ErrorLoggerFunc =
        stdext::inplace_function<bool(Validator &gpuav, const uint32_t *error_record, const LogObjectList &objlist), 128>;
    std::vector<ErrorLoggerFunc> per_command_error_loggers;

  private:
    void AllocateResources();
    void ResetCBState();
    bool NeedsPostProcess();

    VkDeviceSize GetBdaRangesBufferByteSize() const;
    [[nodiscard]] bool UpdateBdaRangesBuffer();

    Validator &state_;

    VkDescriptorSetLayout instrumentation_desc_set_layout_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout validation_cmd_desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet validation_cmd_desc_set_ = VK_NULL_HANDLE;
    VkDescriptorPool validation_cmd_desc_pool_ = VK_NULL_HANDLE;

    // Buffer storing GPU-AV errors
    gpu::DeviceMemoryBlock error_output_buffer_ = {};
    // Buffer storing an error count per validated commands.
    // Used to limit the number of errors a single command can emit.
    gpu::DeviceMemoryBlock cmd_errors_counts_buffer_ = {};
    // Buffer storing a snapshot of buffer device address ranges
    gpu::DeviceMemoryBlock bda_ranges_snapshot_ = {};
    uint32_t bda_ranges_snapshot_version_ = 0;
};

class Queue : public gpu_tracker::Queue {
  public:
    Queue(Validator &state, VkQueue q, uint32_t family_index, uint32_t queue_index, VkDeviceQueueCreateFlags flags,
          const VkQueueFamilyProperties &qfp);

  protected:
    vvl::PreSubmitResult PreSubmit(std::vector<vvl::QueueSubmission> &&submissions) override;
};

class Buffer : public vvl::Buffer {
  public:
    Buffer(ValidationStateTracker &dev_data, VkBuffer buff, const VkBufferCreateInfo *pCreateInfo, DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

class BufferView : public vvl::BufferView {
  public:
    BufferView(const std::shared_ptr<vvl::Buffer> &bf, VkBufferView bv, const VkBufferViewCreateInfo *ci,
               VkFormatFeatureFlags2KHR buf_ff, DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

class ImageView : public vvl::ImageView {
  public:
    ImageView(const std::shared_ptr<vvl::Image> &image_state, VkImageView iv, const VkImageViewCreateInfo *ci,
              VkFormatFeatureFlags2KHR ff, const VkFilterCubicImageViewImageFormatPropertiesEXT &cubic_props,
              DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

class Sampler : public vvl::Sampler {
  public:
    Sampler(const VkSampler s, const VkSamplerCreateInfo *pci, DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

class AccelerationStructureKHR : public vvl::AccelerationStructureKHR {
  public:
    AccelerationStructureKHR(VkAccelerationStructureKHR as, const VkAccelerationStructureCreateInfoKHR *ci,
                             std::shared_ptr<vvl::Buffer> &&buf_state, DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

class AccelerationStructureNV : public vvl::AccelerationStructureNV {
  public:
    AccelerationStructureNV(VkDevice device, VkAccelerationStructureNV as, const VkAccelerationStructureCreateInfoNV *ci,
                            DescriptorHeap &desc_heap_);

    void Destroy() final;
    void NotifyInvalidate(const NodeList &invalid_nodes, bool unlink) final;

    DescriptorHeap &desc_heap;
    const DescriptorId id;
};

namespace glsl {

struct DescriptorSetRecord {
    VkDeviceAddress layout_data;
    VkDeviceAddress in_data;
    VkDeviceAddress out_data;
};

struct BindlessStateBuffer {
    VkDeviceAddress global_state;
    DescriptorSetRecord desc_sets[kDebugInputBindlessMaxDescSets];
};

}  // namespace glsl

}  // namespace gpuav

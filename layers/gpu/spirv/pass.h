/* Copyright (c) 2024 LunarG, Inc.
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

#include <stdint.h>
#include <spirv/unified1/spirv.hpp>
#include "function_basic_block.h"

namespace gpuav {
namespace spirv {

class Module;
struct Variable;
struct BasicBlock;

// Info we know is the same regardless what pass is consuming the CreateFunctionCall()
struct InjectionData {
    uint32_t stage_info_id;
    uint32_t inst_position_id;
};

// Common helpers for all passes
class Pass {
  public:
    void Run();

    // Finds (and creates if needed) decoration and returns the OpVariable it points to
    const Variable& GetBuiltinVariable(uint32_t built_in);

    // Returns the ID for OpCompositeConstruct it creates
    uint32_t GetStageInfo(Function& function, BasicBlockIt target_block_it, InstructionIt& target_inst_it);

    const Instruction* GetDecoration(uint32_t id, spv::Decoration decoration);
    const Instruction* GetMemeberDecoration(uint32_t id, uint32_t member_index, spv::Decoration decoration);

    // Generate SPIR-V needed to help convert things to be uniformly uint32_t
    // If no inst_it is passed in, any new instructions will be added to end of the Block
    uint32_t ConvertTo32(uint32_t id, BasicBlock& block, InstructionIt* inst_it = nullptr);
    uint32_t CastToUint32(uint32_t id, BasicBlock& block, InstructionIt* inst_it = nullptr);

  protected:
    Pass(Module& module, bool conditional_function_check)
        : module_(module), conditional_function_check_(conditional_function_check) {}
    Module& module_;

    BasicBlockIt InjectConditionalFunctionCheck(Function* function, BasicBlockIt block_it, InstructionIt inst_it,
                                                const InjectionData& injection_data);
    void InjectFunctionCheck(BasicBlockIt block_it, InstructionIt* inst_it, const InjectionData& injection_data);

    // Each pass decides if the instruction should needs to have its function check injected
    virtual bool AnalyzeInstruction(const Function& function, const Instruction& inst) = 0;
    // A callback from the function injection logic.
    // Each pass creates a OpFunctionCall and returns its result id.
    // If |inst_it| is not null, it will update it to instruction post OpFunctionCall
    virtual uint32_t CreateFunctionCall(BasicBlock& block, InstructionIt* inst_it, const InjectionData& injection_data) = 0;
    // clear values incase multiple injections are made
    virtual void Reset() = 0;

    // If this is false, we assume through other means (such as robustness) we won't crash on bad values and go
    //     PassFunction(original_value)
    //     value = original_value;
    //
    // Otherwise, we will have wrap the checks to be safe
    // For OpStore we will just ignore the store if it is invalid, example:
    // Before:
    //     bda.data[index] = value;
    // After:
    //    if (isValid(bda.data, index)) {
    //         bda.data[index] = value;
    //    }
    //
    // For OpLoad we replace the value with Zero (via Phi node) if it is invalid, example
    // Before:
    //     int X = bda.data[index];
    //     int Y = bda.data[X];
    // After:
    //    if (isValid(bda.data, index)) {
    //         int X = bda.data[index];
    //    } else {
    //         int X = 0;
    //    }
    //    if (isValid(bda.data, X)) {
    //         int Y = bda.data[X];
    //    } else {
    //         int Y = 0;
    //    }
    const bool conditional_function_check_;

    // As various things are modifiying the instruction streams, we need to get back to where we were.
    // Every pass needs to set this in AnalyzeInstruction()
    const Instruction* target_instruction_ = nullptr;

  private:
    InstructionIt FindTargetInstruction(BasicBlock& block) const;
};

}  // namespace spirv
}  // namespace gpuav
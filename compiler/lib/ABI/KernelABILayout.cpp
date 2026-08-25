// SPDX-License-Identifier: MIT
#include "cgra/ABI/KernelABILayout.h"

#include <nlohmann/json.hpp>

namespace cgra::abi {

std::string toJson(const KernelABILayout& layout, const KernelSignature* signature,
                   const KernelInvocation* invocation) {
  nlohmann::json root = {{"schema", "cgra.kernel_abi.layout.v1"},
                         {"scratchpad",
                          {{"depth", layout.scratchpadDepth},
                           {"user_word_limit", layout.userScratchpadLimit},
                           {"output_region_base", layout.outputRegionBase},
                           {"output_region_end", layout.scratchpadDepth}}},
                         {"inputs", nlohmann::json::array()},
                         {"outputs", nlohmann::json::array()}};
  if (signature)
    root["kernel"] = signature->kernelName;
  if (invocation)
    root["trip_count"] = invocation->tripCount;
  for (const auto& input : layout.inputs)
    root["inputs"].push_back(
        {{"id", input.input}, {"specialized_constant", input.specializedConstant}});
  for (const auto& output : layout.outputs)
    root["outputs"].push_back({{"id", output.output},
                               {"scratchpad_word_address", output.scratchpadAddress},
                               {"abi_store_node", output.abiStoreNode},
                               {"address_constant", output.addressConstant}});
  return root.dump(2) + "\n";
}
} // namespace cgra::abi

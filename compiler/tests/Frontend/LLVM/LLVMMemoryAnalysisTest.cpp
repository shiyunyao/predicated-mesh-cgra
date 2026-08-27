// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>

namespace cgra::ir {
class DFGTestAccess {
public:
  static void setMemoryEdgeKind(DFG& dfg, EdgeId edge, MemoryDepKind kind) {
    std::get<MemoryEdgeInfo>(dfg.edges_.at(dfg.edgeIndices_.at(edge)).info).dependence = kind;
  }

  static void setEdgeDistance(DFG& dfg, EdgeId edge, std::uint32_t distance) {
    dfg.edges_.at(dfg.edgeIndices_.at(edge)).distance = distance;
  }

  static void eraseEdge(DFG& dfg, EdgeId edge) {
    dfg.edges_.erase(dfg.edges_.begin() + static_cast<std::ptrdiff_t>(dfg.edgeIndices_.at(edge)));
    rebuildEdges(dfg);
  }

  static void appendMemoryEdge(DFG& dfg, NodeId source, NodeId destination, MemoryDepKind kind,
                               std::uint32_t distance) {
    Edge edge;
    edge.id = dfg.edges_.empty() ? 0 : std::ranges::max(dfg.edges_, {}, &Edge::id).id + 1;
    edge.src = source;
    edge.dst = destination;
    edge.distance = distance;
    edge.info = MemoryEdgeInfo{kind};
    dfg.appendEdge(std::move(edge));
  }

  static void setConstantBindingBits(DFG& dfg, NodeId node, std::uint32_t operand,
                                     std::uint64_t bits) {
    const auto binding = std::ranges::find_if(dfg.bindings_, [&](const auto& item) {
      return item.node == node && item.operand == operand;
    });
    if (binding == dfg.bindings_.end())
      throw std::runtime_error("constant binding not found");
    const auto* reference = std::get_if<ConstantRef>(&binding->source);
    if (!reference)
      throw std::runtime_error("binding is not a constant");
    dfg.constants_.at(dfg.constantIndices_.at(reference->value)).bits = bits;
  }

  static void setExternalBinding(DFG& dfg, NodeId node, std::uint32_t operand,
                                 ExternalValueId external) {
    const auto binding = std::ranges::find_if(dfg.bindings_, [&](const auto& item) {
      return item.node == node && item.operand == operand;
    });
    if (binding == dfg.bindings_.end())
      throw std::runtime_error("external binding not found");
    binding->source = ExternalValueRef{external};
  }

private:
  static void rebuildEdges(DFG& dfg) {
    dfg.edgeIndices_.clear();
    for (auto& incoming : dfg.incoming_)
      incoming.clear();
    for (auto& outgoing : dfg.outgoing_)
      outgoing.clear();
    for (std::size_t index = 0; index < dfg.edges_.size(); ++index) {
      const auto& edge = dfg.edges_[index];
      dfg.edgeIndices_.emplace(edge.id, index);
      dfg.incoming_.at(dfg.nodeIndices_.at(edge.dst)).push_back(edge.id);
      dfg.outgoing_.at(dfg.nodeIndices_.at(edge.src)).push_back(edge.id);
    }
  }
};
} // namespace cgra::ir

namespace {

using cgra::frontend::llvm_frontend::LLVMFrontendOptions;
using cgra::frontend::llvm_frontend::LLVMFrontendResult;

void expect(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::unique_ptr<llvm::Module> parse(const std::string& text, llvm::LLVMContext& context) {
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(text, diagnostic, context);
  if (!module) {
    diagnostic.print("cgra-llvm-memory-tests", llvm::errs());
    throw std::runtime_error("failed to parse LLVM fixture");
  }
  return module;
}

LLVMFrontendResult lower(const std::string& text, const std::string& function,
                         llvm::LLVMContext& context) {
  auto module = parse(text, context);
  LLVMFrontendOptions options;
  options.functionName = function;
  auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  if (result.ok()) {
    const auto verification =
        cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result);
    expect(verification.ok(),
           "memory frontend result must pass independent verification: " + verification.format());
  }
  return result;
}

std::size_t countOpcode(const cgra::ir::DFG& dfg, cgra::ir::Opcode opcode) {
  std::size_t count = 0;
  for (const auto& node : dfg.nodes())
    count += node.opcode == opcode;
  return count;
}

std::size_t countMemoryEdge(const cgra::ir::DFG& dfg, cgra::ir::MemoryDepKind kind,
                            std::uint32_t distance) {
  std::size_t count = 0;
  for (const auto& edge : dfg.edges()) {
    if (!std::holds_alternative<cgra::ir::MemoryEdgeInfo>(edge.info))
      continue;
    const auto& memory = std::get<cgra::ir::MemoryEdgeInfo>(edge.info);
    count += memory.dependence == kind && edge.distance == distance;
  }
  return count;
}

std::optional<cgra::ir::EdgeId>
findMemoryEdge(const cgra::ir::DFG& dfg, cgra::ir::MemoryDepKind kind, std::uint32_t distance) {
  for (const auto& edge : dfg.edges()) {
    const auto* memory = std::get_if<cgra::ir::MemoryEdgeInfo>(&edge.info);
    if (memory && memory->dependence == kind && edge.distance == distance)
      return edge.id;
  }
  return std::nullopt;
}

std::optional<cgra::ir::NodeId> findNode(const cgra::ir::DFG& dfg, cgra::ir::Opcode opcode) {
  for (const auto& node : dfg.nodes())
    if (node.opcode == opcode)
      return node.id;
  return std::nullopt;
}

template <typename Mutator>
void expectVerifierRejects(const std::string& text, const std::string& function, Mutator mutate,
                           const std::string& message) {
  llvm::LLVMContext context;
  auto module = parse(text, context);
  LLVMFrontendOptions options;
  options.functionName = function;
  auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(result.ok(), "corruption fixture must lower before mutation: " + result.message);
  mutate(result);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result).ok(),
         message);
}

const char* kLoadOnly = R"IR(
target datalayout = "e-p:64:64"
define void @load_only(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kVectorAddNoAlias = R"IR(
target datalayout = "e-p:64:64"
define void @vector_add(i32* noalias %A, i32* noalias %B, i32* noalias %C) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %a.addr = getelementptr i32, i32* %A, i32 %i
  %b.addr = getelementptr i32, i32* %B, i32 %i
  %c.addr = getelementptr i32, i32* %C, i32 %i
  %a = load i32, i32* %a.addr, align 4
  %b = load i32, i32* %b.addr, align 4
  %sum = add i32 %a, %b
  store i32 %sum, i32* %c.addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kRawDistance1 = R"IR(
target datalayout = "e-p:64:64"
define void @raw_d1(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 1, %entry ], [ %inc, %loop ]
  %previous = sub i32 %i, 1
  %read.addr = getelementptr i32, i32* %A, i32 %previous
  %write.addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %read.addr, align 4
  %next = add i32 %value, 1
  store i32 %next, i32* %write.addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 5
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kRawDistance2 = R"IR(
target datalayout = "e-p:64:64"
define void @raw_d2(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 2, %entry ], [ %inc, %loop ]
  %previous = sub i32 %i, 2
  %read.addr = getelementptr i32, i32* %A, i32 %previous
  %write.addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %read.addr, align 4
  %next = add i32 %value, 1
  store i32 %next, i32* %write.addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 6
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kInvariantStore = R"IR(
target datalayout = "e-p:64:64"
define void @invariant_store(i32* %A, i32 %value) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store i32 %value, i32* %A, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kVaryingStore = R"IR(
target datalayout = "e-p:64:64"
define void @varying_store(i32* %A, i32 %value) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i32 %i
  store i32 %value, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kSameAddressRaw = R"IR(
target datalayout = "e-p:64:64"
define void @same_raw(i32* %A, i32 %value) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store i32 %value, i32* %A, align 4
  %read = load i32, i32* %A, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kSameAddressWar = R"IR(
target datalayout = "e-p:64:64"
define void @same_war(i32* %A, i32 %value) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %read = load i32, i32* %A, align 4
  store i32 %value, i32* %A, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kTwoInvariantStores = R"IR(
target datalayout = "e-p:64:64"
define void @same_waw(i32* %A, i32 %first, i32 %second) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store i32 %first, i32* %A, align 4
  store i32 %second, i32* %A, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kConstantOffset = R"IR(
target datalayout = "e-p:64:64"
define void @constant_offset(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i32 1
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kStrideTwo = R"IR(
target datalayout = "e-p:64:64"
define void @stride_two(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %scaled = mul i32 %i, 2
  %addr = getelementptr i32, i32* %A, i32 %scaled
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kAffineOffsetStride = R"IR(
target datalayout = "e-p:64:64"
define void @affine_offset_stride(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %scaled = mul i32 %i, 2
  %index = add i32 %scaled, 3
  %addr = getelementptr i32, i32* %A, i32 %index
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kImplicitGEPScaleOffset = R"IR(
target datalayout = "e-p:64:64"
define void @implicit_gep_scale_offset([2 x i32]* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr [2 x i32], [2 x i32]* %A, i32 %i, i32 1
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kOversizedGEPOffset = R"IR(
target datalayout = "e-p:64:64"
define void @oversized_gep_offset(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i64 4294967296
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kMayAlias = R"IR(
target datalayout = "e-p:64:64"
define void @may_alias(i32* %A, i32* %B) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %a.addr = getelementptr i32, i32* %A, i32 %i
  %b.addr = getelementptr i32, i32* %B, i32 %i
  %value = load i32, i32* %a.addr, align 4
  store i32 %value, i32* %b.addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kCrossBlockLoadThenStore = R"IR(
target datalayout = "e-p:64:64"
define void @cross_block_load_store(i32* %A, i32 %value) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
  %read = load i32, i32* %A, align 4
  br label %body
body:
  store i32 %value, i32* %A, align 4
  br label %latch
latch:
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %header, label %exit
exit:
  ret void
}
)IR";

const char* kCrossBlockStoreThenLoad = R"IR(
target datalayout = "e-p:64:64"
define void @cross_block_store_load(i32* %A, i32 %value) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
  store i32 %value, i32* %A, align 4
  br label %body
body:
  %read = load i32, i32* %A, align 4
  br label %latch
latch:
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %header, label %exit
exit:
  ret void
}
)IR";

const char* kVolatileLoad = R"IR(
target datalayout = "e-p:64:64"
define void @volatile_load(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i32 %i
  %value = load volatile i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kSubwordLoad = R"IR(
target datalayout = "e-p:64:64"
define void @subword(i16* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %value = load i16, i16* %A, align 2
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kWideFloatLoad = R"IR(
target datalayout = "e-p:64:64"
define void @wide_float(double* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr double, double* %A, i32 %i
  %value = load double, double* %addr, align 8
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kUnalignedLoad = R"IR(
target datalayout = "e-p:64:64"
define void @unaligned(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %value = load i32, i32* %A, align 1
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kNonAffineLoad = R"IR(
target datalayout = "e-p:64:64"
define void @non_affine(i32* %A, i32 %x) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %index = mul i32 %i, %x
  %addr = getelementptr i32, i32* %A, i32 %index
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kSymbolicAffine = R"IR(
target datalayout = "e-p:64:64"
define void @symbolic_affine(i32* %A, i32 %outer) {
entry:
  %row = mul i32 %outer, 64
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %index = add i32 %row, %i
  %addr = getelementptr i32, i32* %A, i32 %index
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kDynamicLoadedIndexStore = R"IR(
target datalayout = "e-p:64:64"
define void @dynamic_loaded_index(i32* noalias %indices, i32* noalias %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %index.addr = getelementptr i32, i32* %indices, i32 %i
  %index = load i32, i32* %index.addr, align 4
  %addr = getelementptr i32, i32* %A, i32 %index
  store i32 %i, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kDynamicMemoryLiveOut = R"IR(
target datalayout = "e-p:64:64"
define i32 @memory_liveout(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %addr = getelementptr i32, i32* %A, i32 %i
  %value = load i32, i32* %addr, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %value, %loop ]
  ret i32 %out
}
)IR";

const char* kAtomicLoad = R"IR(
target datalayout = "e-p:64:64"
define void @atomic_load(i32* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %value = load atomic i32, i32* %A seq_cst, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kNonDefaultAddressSpace = R"IR(
target datalayout = "e-p:64:64"
define void @non_default_address_space(i32 addrspace(1)* %A) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %value = load i32, i32 addrspace(1)* %A, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kPointerSelect = R"IR(
target datalayout = "e-p:64:64"
define void @pointer_select(i32* %A, i32* %B, i1 %choose) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %address = select i1 %choose, i32* %A, i32* %B
  %value = load i32, i32* %address, align 4
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kPathSensitiveMemoryOrder = R"IR(
target datalayout = "e-p:64:64"
define void @path_sensitive(i32* %A, i32* %B, i32 %choose) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %condition = icmp ne i32 %choose, 0
  br i1 %condition, label %true, label %false
true:
  store i32 1, i32* %A, align 4
  br label %merge
false:
  store i32 2, i32* %B, align 4
  br label %merge
merge:
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kNoAliasOppositeArmStores = R"IR(
target datalayout = "e-p:64:64"
define void @noalias_opposite_arms(i32* noalias %A, i32* noalias %B, i32 %choose) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %condition = icmp ne i32 %choose, 0
  br i1 %condition, label %true, label %false
true:
  store i32 1, i32* %A, align 4
  br label %merge
false:
  store i32 2, i32* %B, align 4
  br label %merge
merge:
  %inc = add i32 %i, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

void runSemanticCases() {
  llvm::LLVMContext context;

  const auto loadOnly = lower(kLoadOnly, "load_only", context);
  expect(loadOnly.ok(), "single affine Load must lower: " + loadOnly.message);
  expect(countOpcode(*loadOnly.dfg, cgra::ir::Opcode::Load) == 1, "one Generic Load");
  expect(countMemoryEdge(*loadOnly.dfg, cgra::ir::MemoryDepKind::RAW, 0) == 0 &&
             countMemoryEdge(*loadOnly.dfg, cgra::ir::MemoryDepKind::WAR, 0) == 0 &&
             countMemoryEdge(*loadOnly.dfg, cgra::ir::MemoryDepKind::WAW, 0) == 0,
         "Load-only loop has no memory ordering edge");

  const auto vectorAdd = lower(kVectorAddNoAlias, "vector_add", context);
  expect(vectorAdd.ok(), "noalias vector add must lower: " + vectorAdd.message);
  expect(countOpcode(*vectorAdd.dfg, cgra::ir::Opcode::Load) == 2, "two Generic Loads");
  expect(countOpcode(*vectorAdd.dfg, cgra::ir::Opcode::Store) == 1, "one Generic Store");
  expect(vectorAdd.provenance.memoryDependences.empty(), "NoAlias arrays need no MemoryEdge");

  const auto raw1 = lower(kRawDistance1, "raw_d1", context);
  expect(raw1.ok(), "distance-one affine RAW must lower: " + raw1.message);
  expect(countMemoryEdge(*raw1.dfg, cgra::ir::MemoryDepKind::RAW, 1) == 1,
         "Store A[i] to Load A[i-1] is RAW distance one");

  const auto raw2 = lower(kRawDistance2, "raw_d2", context);
  expect(raw2.ok(), "distance-two affine RAW must lower: " + raw2.message);
  expect(countMemoryEdge(*raw2.dfg, cgra::ir::MemoryDepKind::RAW, 2) == 1,
         "Store A[i] to Load A[i-2] is RAW distance two");

  const auto invariant = lower(kInvariantStore, "invariant_store", context);
  expect(invariant.ok(), "invariant Store must lower: " + invariant.message);
  expect(countMemoryEdge(*invariant.dfg, cgra::ir::MemoryDepKind::WAW, 1) == 1,
         "invariant Store has one self-WAW distance one");

  const auto varying = lower(kVaryingStore, "varying_store", context);
  expect(varying.ok(), "varying Store must lower: " + varying.message);
  expect(varying.provenance.memoryDependences.empty(),
         "Store A[i] must not receive a false self-WAW");

  const auto sameRaw = lower(kSameAddressRaw, "same_raw", context);
  expect(sameRaw.ok(), "same-address Store then Load must lower: " + sameRaw.message);
  expect(countMemoryEdge(*sameRaw.dfg, cgra::ir::MemoryDepKind::RAW, 0) == 1,
         "Store before Load on one address has RAW distance zero");
  expect(countMemoryEdge(*sameRaw.dfg, cgra::ir::MemoryDepKind::WAR, 1) == 1,
         "invariant Store/Load pair carries reverse WAR distance one");

  const auto sameWar = lower(kSameAddressWar, "same_war", context);
  expect(sameWar.ok(), "same-address Load then Store must lower: " + sameWar.message);
  expect(countMemoryEdge(*sameWar.dfg, cgra::ir::MemoryDepKind::WAR, 0) == 1,
         "Load before Store on one address has WAR distance zero");
  expect(countMemoryEdge(*sameWar.dfg, cgra::ir::MemoryDepKind::RAW, 1) == 1,
         "invariant Load/Store pair carries reverse RAW distance one");

  const auto sameWaw = lower(kTwoInvariantStores, "same_waw", context);
  expect(sameWaw.ok(), "same-address Stores must lower: " + sameWaw.message);
  expect(countMemoryEdge(*sameWaw.dfg, cgra::ir::MemoryDepKind::WAW, 0) == 1,
         "ordered same-address Stores have WAW distance zero");
  expect(countMemoryEdge(*sameWaw.dfg, cgra::ir::MemoryDepKind::WAW, 1) == 3,
         "two invariant Stores have self and reverse WAW distance-one ordering");

  const auto mayAlias = lower(kMayAlias, "may_alias", context);
  expect(mayAlias.ok(), "MayAlias pair must lower conservatively: " + mayAlias.message);
  expect(countMemoryEdge(*mayAlias.dfg, cgra::ir::MemoryDepKind::WAR, 0) == 1,
         "Load before Store gets WAR distance zero");
  expect(countMemoryEdge(*mayAlias.dfg, cgra::ir::MemoryDepKind::RAW, 1) == 1,
         "MayAlias pair gets reverse RAW distance one");

  const auto crossBlockWar = lower(kCrossBlockLoadThenStore, "cross_block_load_store", context);
  expect(crossBlockWar.ok(), "cross-block Load then Store must lower: " + crossBlockWar.message);
  expect(crossBlockWar.metadata && crossBlockWar.metadata->loopShape == "linear_multiblock",
         "cross-block memory fixture must use the linear multi-block path");
  expect(countMemoryEdge(*crossBlockWar.dfg, cgra::ir::MemoryDepKind::WAR, 0) == 1 &&
             countMemoryEdge(*crossBlockWar.dfg, cgra::ir::MemoryDepKind::RAW, 1) == 1,
         "cross-block program order must preserve WAR d0 and reverse RAW d1");

  const auto crossBlockRaw = lower(kCrossBlockStoreThenLoad, "cross_block_store_load", context);
  expect(crossBlockRaw.ok(), "cross-block Store then Load must lower: " + crossBlockRaw.message);
  expect(crossBlockRaw.metadata && crossBlockRaw.metadata->loopShape == "linear_multiblock",
         "reverse cross-block memory fixture must use the linear multi-block path");
  expect(countMemoryEdge(*crossBlockRaw.dfg, cgra::ir::MemoryDepKind::RAW, 0) == 1 &&
             countMemoryEdge(*crossBlockRaw.dfg, cgra::ir::MemoryDepKind::WAR, 1) == 1,
         "cross-block program order must preserve RAW d0 and reverse WAR d1");

  const auto affine = lower(kAffineOffsetStride, "affine_offset_stride", context);
  expect(affine.ok(), "A[2*i+3] must lower as real Generic address arithmetic: " + affine.message);
  expect(affine.provenance.memoryAccesses.size() == 1 &&
             affine.provenance.memoryAccesses.front().offsetWords == 3 &&
             affine.provenance.memoryAccesses.front().strideWords == 2,
         "DataLayout/SCEV analysis must preserve affine offset three and stride two");
  expect(countOpcode(*affine.dfg, cgra::ir::Opcode::Mul) == 1 &&
             countOpcode(*affine.dfg, cgra::ir::Opcode::Add) >= 3,
         "A[2*i+3] must retain its Mul/Add address dataflow in the Generic DFG");

  const auto implicit = lower(kImplicitGEPScaleOffset, "implicit_gep_scale_offset", context);
  expect(implicit.ok(),
         "DataLayout-implied scale plus field offset must lower: " + implicit.message);
  expect(implicit.provenance.memoryAccesses.size() == 1 &&
             implicit.provenance.memoryAccesses.front().offsetWords == 1 &&
             implicit.provenance.memoryAccesses.front().strideWords == 2 &&
             countOpcode(*implicit.dfg, cgra::ir::Opcode::Mul) == 1,
         "implicit GEP scale two and offset one must be real Generic address dataflow");

  const auto symbolic = lower(kSymbolicAffine, "symbolic_affine", context);
  expect(symbolic.ok(), "selected-loop invariant offset must lower: " + symbolic.message);
  expect(symbolic.provenance.memoryAccesses.size() == 1 &&
             symbolic.provenance.memoryAccesses.front().addressMode == "symbolic_affine" &&
             symbolic.provenance.memoryAccesses.front().strideBytes == 4,
         "outer-loop/function-entry terms must remain symbolic with an exact selected-loop stride");

  const auto dynamic = lower(kDynamicLoadedIndexStore, "dynamic_loaded_index", context);
  expect(dynamic.ok(), "loaded-index address dataflow must lower conservatively: " + dynamic.message);
  expect(std::ranges::any_of(dynamic.provenance.memoryAccesses, [](const auto& access) {
           return access.kind == "store" && access.addressMode == "dynamic";
         }),
         "indirect Store must retain dynamic address provenance");
  expect(countMemoryEdge(*dynamic.dfg, cgra::ir::MemoryDepKind::WAW, 1) == 1,
         "dynamic Store must conservatively order successive iterations");
}

void runNegativeCases() {
  llvm::LLVMContext context;
  const auto volatileResult = lower(kVolatileLoad, "volatile_load", context);
  expect(!volatileResult.ok(), "volatile Load must be rejected");
  expect(volatileResult.status ==
             cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedMemoryOperation,
         "volatile Load uses structured memory rejection");

  const auto subword = lower(kSubwordLoad, "subword", context);
  expect(subword.ok() && subword.provenance.memoryAccesses.size() == 1 &&
             subword.provenance.memoryAccesses.front().accessWidthBits == 16 &&
             subword.dfg->node(subword.provenance.memoryAccesses.front().memoryNode).resultType ==
                 cgra::ir::ValueType::i16(),
         "typed memory frontend must preserve a naturally aligned i16 Load");

  const auto wideFloat = lower(kWideFloatLoad, "wide_float", context);
  expect(wideFloat.ok() && wideFloat.provenance.memoryAccesses.size() == 1 &&
             wideFloat.provenance.memoryAccesses.front().accessWidthBits == 64 &&
             wideFloat.provenance.memoryAccesses.front().strideBytes == 8 &&
             wideFloat.dfg->node(wideFloat.provenance.memoryAccesses.front().memoryNode)
                     .resultType == cgra::ir::ValueType::floating(64),
         "typed memory frontend must retain f64 width and byte stride");

  const auto unaligned = lower(kUnalignedLoad, "unaligned", context);
  expect(!unaligned.ok() &&
             unaligned.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedMemoryAlignment,
         "unaligned i32 memory access must be rejected");

  const auto oversized = lower(kOversizedGEPOffset, "oversized_gep_offset", context);
  expect(!oversized.ok() &&
             oversized.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedNonAffineAddress,
         "GEP word offsets wider than Generic i32 must be rejected before narrowing");

  const auto nonAffine = lower(kNonAffineLoad, "non_affine", context);
  expect(nonAffine.ok() && nonAffine.provenance.memoryAccesses.size() == 1 &&
             nonAffine.provenance.memoryAccesses.front().addressMode == "dynamic",
         "runtime-stride GEP must lower as dynamic address dataflow");

  const auto memoryLiveOut = lower(kDynamicMemoryLiveOut, "memory_liveout", context);
  expect(!memoryLiveOut.ok() && memoryLiveOut.status ==
                                    cgra::frontend::llvm_frontend::LLVMFrontendStatus::
                                        MemoryWithABIScalarLiveOutUnsupportedV0,
         "dynamic memory plus ABI scalar LiveOut must be rejected in V0");

  const auto atomic = lower(kAtomicLoad, "atomic_load", context);
  expect(!atomic.ok() &&
             atomic.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedMemoryOperation,
         "atomic Load must receive the structured T018 memory rejection");

  const auto addressSpace = lower(kNonDefaultAddressSpace, "non_default_address_space", context);
  expect(!addressSpace.ok() &&
             addressSpace.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedMemoryAddressSpace,
         "non-default address space must be rejected explicitly");

  const auto pointerSelect = lower(kPointerSelect, "pointer_select", context);
  expect(!pointerSelect.ok() &&
             pointerSelect.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedPointerBase,
         "pointer Select must not be treated as a scratchpad base");

  const auto pathSensitive = lower(kPathSensitiveMemoryOrder, "path_sensitive", context);
  expect(!pathSensitive.ok() && pathSensitive.status ==
                                    cgra::frontend::llvm_frontend::LLVMFrontendStatus::
                                        UnsupportedPathSensitiveMemoryOrder,
         "mutually exclusive MayAlias Stores require path-sensitive memory ordering; status=" +
             std::string(cgra::frontend::llvm_frontend::toString(pathSensitive.status)) +
             " message=" + pathSensitive.message);

  const auto oppositeArmStores = lower(kNoAliasOppositeArmStores, "noalias_opposite_arms", context);
  expect(!oppositeArmStores.ok() &&
             oppositeArmStores.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedIfSideEffect,
         "NoAlias removes memory ordering, but two Store predicate polarities remain outside V0");
}

void runVerifierCorruptionCases() {
  expectVerifierRejects(
      kRawDistance1, "raw_d1",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::RAW, 1);
        expect(edge.has_value(), "RAW d1 edge exists before corruption");
        cgra::ir::DFGTestAccess::setEdgeDistance(*result.dfg, *edge, 0);
      },
      "verifier must reject RAW distance one changed to zero");

  expectVerifierRejects(
      kRawDistance2, "raw_d2",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::RAW, 2);
        expect(edge.has_value(), "RAW d2 edge exists before corruption");
        cgra::ir::DFGTestAccess::setEdgeDistance(*result.dfg, *edge, 1);
      },
      "verifier must reject RAW distance two changed to one");

  expectVerifierRejects(
      kRawDistance1, "raw_d1",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::RAW, 1);
        expect(edge.has_value(), "RAW edge exists before kind corruption");
        cgra::ir::DFGTestAccess::setMemoryEdgeKind(*result.dfg, *edge,
                                                   cgra::ir::MemoryDepKind::WAR);
      },
      "verifier must reject a corrupted dependence kind");

  expectVerifierRejects(
      kRawDistance1, "raw_d1",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::RAW, 1);
        expect(edge.has_value(), "RAW edge exists before removal");
        cgra::ir::DFGTestAccess::eraseEdge(*result.dfg, *edge);
      },
      "verifier must reject a missing affine RAW edge");

  expectVerifierRejects(
      kMayAlias, "may_alias",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::RAW, 1);
        expect(edge.has_value(), "conservative reverse edge exists before removal");
        cgra::ir::DFGTestAccess::eraseEdge(*result.dfg, *edge);
      },
      "verifier must reject a missing conservative reverse distance-one edge");

  expectVerifierRejects(
      kLoadOnly, "load_only",
      [](auto& result) {
        const auto load = findNode(*result.dfg, cgra::ir::Opcode::Load);
        expect(load.has_value(), "Load node exists before spurious-edge corruption");
        cgra::ir::DFGTestAccess::appendMemoryEdge(*result.dfg, *load, *load,
                                                  cgra::ir::MemoryDepKind::RAW, 0);
      },
      "verifier must reject a spurious Load-to-Load edge");

  expectVerifierRejects(
      kVectorAddNoAlias, "vector_add",
      [](auto& result) {
        const auto load = findNode(*result.dfg, cgra::ir::Opcode::Load);
        const auto store = findNode(*result.dfg, cgra::ir::Opcode::Store);
        expect(load && store, "NoAlias fixture memory nodes exist");
        cgra::ir::DFGTestAccess::appendMemoryEdge(*result.dfg, *load, *store,
                                                  cgra::ir::MemoryDepKind::WAR, 0);
      },
      "verifier must reject a spurious dependence between NoAlias arrays");

  expectVerifierRejects(
      kInvariantStore, "invariant_store",
      [](auto& result) {
        const auto edge = findMemoryEdge(*result.dfg, cgra::ir::MemoryDepKind::WAW, 1);
        expect(edge.has_value(), "invariant Store self-WAW exists before removal");
        cgra::ir::DFGTestAccess::eraseEdge(*result.dfg, *edge);
      },
      "verifier must reject a missing invariant Store self-WAW");

  expectVerifierRejects(
      kVaryingStore, "varying_store",
      [](auto& result) {
        const auto store = findNode(*result.dfg, cgra::ir::Opcode::Store);
        expect(store.has_value(), "varying Store exists before false self-WAW insertion");
        cgra::ir::DFGTestAccess::appendMemoryEdge(*result.dfg, *store, *store,
                                                  cgra::ir::MemoryDepKind::WAW, 1);
      },
      "verifier must reject a false self-WAW on Store A[i]");

  expectVerifierRejects(
      kConstantOffset, "constant_offset",
      [](auto& result) {
        const auto& access = result.provenance.memoryAccesses.front();
        cgra::ir::DFGTestAccess::setConstantBindingBits(*result.dfg, access.addressProvider, 1, 0);
      },
      "verifier must reject GEP word offset +1 changed to +0");

  expectVerifierRejects(
      kStrideTwo, "stride_two",
      [](auto& result) {
        const auto multiply = findNode(*result.dfg, cgra::ir::Opcode::Mul);
        expect(multiply.has_value(), "stride-two multiply exists before corruption");
        cgra::ir::DFGTestAccess::setConstantBindingBits(*result.dfg, *multiply, 1, 1);
      },
      "verifier must reject GEP stride two changed to one");

  expectVerifierRejects(
      kImplicitGEPScaleOffset, "implicit_gep_scale_offset",
      [](auto& result) {
        const auto scale = std::ranges::find_if(
            result.provenance.nodes, [](const auto& node) { return node.opcode == "GEP_SCALE"; });
        expect(scale != result.provenance.nodes.end(),
               "implicit GEP scale node exists before corruption");
        cgra::ir::DFGTestAccess::setConstantBindingBits(*result.dfg, scale->node, 1, 1);
      },
      "verifier must reject an implicit GEP word scale changed from two to one");

  expectVerifierRejects(
      kImplicitGEPScaleOffset, "implicit_gep_scale_offset",
      [](auto& result) {
        const auto offset = std::ranges::find_if(
            result.provenance.nodes, [](const auto& node) { return node.opcode == "GEP_OFFSET"; });
        expect(offset != result.provenance.nodes.end(),
               "implicit GEP offset node exists before corruption");
        cgra::ir::DFGTestAccess::setConstantBindingBits(*result.dfg, offset->node, 1, 0);
      },
      "verifier must reject an implicit GEP field offset changed from one to zero");

  expectVerifierRejects(
      kMayAlias, "may_alias",
      [](auto& result) {
        const auto store =
            std::ranges::find_if(result.provenance.memoryAccesses,
                                 [](const auto& access) { return access.kind == "store"; });
        const auto external = std::ranges::find_if(
            result.provenance.externals, [](const auto& item) { return item.valueName == "A"; });
        expect(store != result.provenance.memoryAccesses.end() &&
                   external != result.provenance.externals.end(),
               "base corruption fixture has Store B and External A");
        cgra::ir::DFGTestAccess::setExternalBinding(*result.dfg, store->addressProvider, 0,
                                                    external->external);
      },
      "verifier must reject Store base B changed to A");
}

} // namespace

int main() {
  try {
    runSemanticCases();
    runNegativeCases();
    runVerifierCorruptionCases();
    std::cout << "LLVM memory analysis tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

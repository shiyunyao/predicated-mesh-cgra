// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/FrontendInvocationValidation.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace cgra::ir {
class DFGTestAccess {
public:
  static void setEdgeDistance(DFG& dfg, EdgeId edge, std::uint32_t distance) {
    dfg.edges_[edge].distance = distance;
  }
  static void setEdgeOperand(DFG& dfg, EdgeId edge, std::uint32_t operand) {
    std::get<DataEdgeInfo>(dfg.edges_[edge].info).dstOperand = operand;
  }
  static void setEdgeSource(DFG& dfg, EdgeId edge, NodeId source) { dfg.edges_[edge].src = source; }
  static void setNodeOpcode(DFG& dfg, NodeId node, Opcode opcode) {
    dfg.nodes_[dfg.nodeIndices_.at(node)].opcode = opcode;
  }
  static void setEdgeDestination(DFG& dfg, EdgeId edge, NodeId destination) {
    dfg.edges_[edge].dst = destination;
  }
  static void clearBoundary(DFG& dfg, EdgeId edge) {
    std::get<DataEdgeInfo>(dfg.edges_[edge].info).boundary.reset();
  }
  static void setBoundaryOffset(DFG& dfg, EdgeId edge, std::uint32_t offset) {
    auto& boundary = std::get<DataEdgeInfo>(dfg.edges_[edge].info).boundary;
    if (boundary && !boundary->values.empty())
      boundary->values.front().iterationOffset = offset;
  }
  static void setBoundaryConstant(DFG& dfg, EdgeId edge, ConstantId constant) {
    auto& boundary = std::get<DataEdgeInfo>(dfg.edges_[edge].info).boundary;
    if (boundary && !boundary->values.empty())
      boundary->values.front().value = ConstantRef{constant};
  }
  static void appendDuplicateEdge(DFG& dfg, EdgeId edgeId) {
    auto duplicate = dfg.edges_[edgeId];
    duplicate.id = static_cast<EdgeId>(dfg.edges_.size());
    dfg.appendEdge(std::move(duplicate));
  }
  static NodeId appendDuplicateNode(DFG& dfg, NodeId nodeId) {
    auto duplicate = dfg.node(nodeId);
    duplicate.id = std::ranges::max(dfg.nodes_, {}, &Node::id).id + 1;
    const auto duplicateId = dfg.appendNode(std::move(duplicate));
    std::vector<Edge> incoming;
    for (const auto& edge : dfg.edges_)
      if (edge.dst == nodeId)
        incoming.push_back(edge);
    EdgeId nextEdge = dfg.edges_.empty() ? 0 : std::ranges::max(dfg.edges_, {}, &Edge::id).id + 1;
    for (auto edge : incoming) {
      edge.id = nextEdge++;
      edge.dst = duplicateId;
      dfg.appendEdge(std::move(edge));
    }
    std::vector<OperandBinding> bindings;
    for (const auto& binding : dfg.bindings_)
      if (binding.node == nodeId) {
        auto copy = binding;
        copy.node = duplicateId;
        bindings.push_back(std::move(copy));
      }
    for (auto& binding : bindings)
      dfg.appendBinding(std::move(binding));
    return duplicateId;
  }
  static void eraseEdge(DFG& dfg, EdgeId edgeId) {
    const auto position = dfg.edgeIndices_.at(edgeId);
    dfg.edges_.erase(dfg.edges_.begin() + static_cast<std::ptrdiff_t>(position));
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
  static void eraseNode(DFG& dfg, NodeId nodeId) {
    std::vector<EdgeId> incident;
    for (const auto& edge : dfg.edges_)
      if (edge.src == nodeId || edge.dst == nodeId)
        incident.push_back(edge.id);
    for (const auto edge : incident)
      eraseEdge(dfg, edge);
    std::erase_if(dfg.bindings_, [&](const auto& binding) { return binding.node == nodeId; });
    std::erase_if(dfg.liveOuts_, [&](const auto& liveOut) { return liveOut.source == nodeId; });
    dfg.liveOutIndices_.clear();
    for (std::size_t index = 0; index < dfg.liveOuts_.size(); ++index)
      dfg.liveOutIndices_.emplace(dfg.liveOuts_[index].id, index);
    const auto position = dfg.nodeIndices_.at(nodeId);
    dfg.nodes_.erase(dfg.nodes_.begin() + static_cast<std::ptrdiff_t>(position));
    dfg.incoming_.erase(dfg.incoming_.begin() + static_cast<std::ptrdiff_t>(position));
    dfg.outgoing_.erase(dfg.outgoing_.begin() + static_cast<std::ptrdiff_t>(position));
    dfg.nodeIndices_.clear();
    for (std::size_t index = 0; index < dfg.nodes_.size(); ++index)
      dfg.nodeIndices_.emplace(dfg.nodes_[index].id, index);
  }
  static void setBindingExternal(DFG& dfg, NodeId node, std::uint32_t operand,
                                 ExternalValueId external) {
    const auto binding = std::ranges::find_if(dfg.bindings_, [&](const auto& item) {
      return item.node == node && item.operand == operand;
    });
    binding->source = ExternalValueRef{external};
  }
  static void appendExternalBinding(DFG& dfg, NodeId node, std::uint32_t operand,
                                    ExternalValueId external) {
    dfg.appendBinding({node, operand, ExternalValueRef{external}});
  }
  static void clearLiveOuts(DFG& dfg) {
    dfg.liveOuts_.clear();
    dfg.liveOutIndices_.clear();
  }
};
} // namespace cgra::ir

namespace {

const char* kScalarAdd = R"IR(
define i32 @kernel(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = add i32 %x, %x
  %cmp = icmp ult i32 %iv.next, 4
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
)IR";

const char* kChain = R"IR(
define i32 @chain(i32 %x, i32 %z) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %a = add i32 %x, %x
  %b = mul i32 %a, %z
  %c = xor i32 %b, %x
  %cmp = icmp ult i32 %iv.next, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %c, %loop ]
  ret i32 %result
}
)IR";

const char* kDataPhi = R"IR(
define i32 @reduction(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %next, %loop ]
  %iv.next = add i32 %iv, 1
  %next = add i32 %sum, %x
  %cmp = icmp ult i32 %iv.next, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
}
)IR";

const char* kExternalDataPhi = R"IR(
define i32 @external_reduction(i32 %seed) {
entry:
  br label %loop
loop:
  %sum = phi i32 [ %seed, %entry ], [ %next, %loop ]
  %next = add i32 %sum, 1
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
}
)IR";

const char* kPreheaderExternal = R"IR(
define i32 @preheader_external(i32 %x) {
entry:
  %seed = add i32 %x, 2
  br label %loop
loop:
  %sum = phi i32 [ %seed, %entry ], [ %next, %loop ]
  %next = add i32 %sum, 1
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
}
)IR";

const char* kRepeatedPhiUse = R"IR(
define i32 @repeated_phi(i32 %seed) {
entry:
  br label %loop
loop:
  %sum = phi i32 [ %seed, %entry ], [ %next, %loop ]
  %double = add i32 %sum, %sum
  %next = add i32 %double, 1
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %next, %loop ]
  ret i32 %result
}
)IR";

const char* kInductionDataUse = R"IR(
define i32 @iv_data(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = add i32 %x, %iv
  %cmp = icmp ult i32 %iv.next, 3
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
)IR";

const char* kThreeIncomingPhi = R"IR(
define i32 @three_incoming(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %next, %loop ], [ 1, %entry ]
  %next = add i32 %iv, %x
  %cmp = icmp ult i32 %next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %next
}
)IR";

const char* kPhiToPhi = R"IR(
define i32 @phi_to_phi(i32 %x) {
entry:
  br label %loop
loop:
  %a = phi i32 [ 0, %entry ], [ %b, %loop ]
  %b = phi i32 [ 1, %entry ], [ %a, %loop ]
  %next = add i32 %a, %b
  %cmp = icmp ult i32 %next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %next
}
)IR";

const char* kPointerPhi = R"IR(
define i32 @pointer_phi(i32* %base) {
entry:
  br label %loop
loop:
  %ptr = phi i32* [ %base, %entry ], [ %ptr, %loop ]
  %iv = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = add i32 %iv, 1
  %cmp = icmp ult i32 %next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %next
}
)IR";

const char* kFloatPhi = R"IR(
define i32 @float_phi(float %x) {
entry:
  br label %loop
loop:
  %sum = phi float [ 0.0, %entry ], [ %next, %loop ]
  %next = fadd float %sum, %x
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %inc
}
)IR";

const char* kVectorPhi = R"IR(
define i32 @vector_phi(<2 x i32> %x) {
entry:
  br label %loop
loop:
  %sum = phi <2 x i32> [ zeroinitializer, %entry ], [ %next, %loop ]
  %next = add <2 x i32> %sum, %x
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %cmp = icmp ult i32 %inc, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %inc
}
)IR";

const char* kRawPhiLiveOut = R"IR(
define i32 @raw_phi_liveout(i32 %x) {
entry:
  br label %loop
loop:
  %sum = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = add i32 %sum, %x
  %cmp = icmp ult i32 %next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  ret i32 %sum
}
)IR";

const char* kMemory = R"IR(
define i32 @memory(i32* %p) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %v = load i32, i32* %p
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %v, %loop ]
  ret i32 %result
}
)IR";

const char* kFloat = R"IR(
define float @float_loop(float %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = fadd float %x, %x
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi float [ %y, %loop ]
  ret float %result
}
)IR";

const char* kFusedMultiplyAdd = R"IR(
declare float @llvm.fmuladd.f32(float, float, float)

define float @fma_loop(float %a, float %b, float %c) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = call float @llvm.fmuladd.f32(float %a, float %b, float %c)
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi float [ %y, %loop ]
  ret float %result
}
)IR";

const char* kPureHelper = R"IR(
define internal i32 @absolute(i32 %x) {
entry:
  %negative = icmp slt i32 %x, 0
  %negated = sub i32 0, %x
  %value = select i1 %negative, i32 %negated, i32 %x
  ret i32 %value
}

define i32 @pure_helper_loop(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = call i32 @absolute(i32 %x)
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
)IR";

const char* kUndefOperand = R"IR(
define i32 @undef_operand(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = add i32 %x, undef
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
)IR";

const char* kAmbiguous = R"IR(
define i32 @two_loops(i32 %x) {
entry:
  br label %a
a:
  %ai = phi i32 [ 0, %entry ], [ %an, %a ]
  %an = add i32 %ai, 1
  %ac = icmp ult i32 %an, 2
  br i1 %ac, label %a, label %b
b:
  %bi = phi i32 [ 0, %a ], [ %bn, %b ]
  %bn = add i32 %bi, 1
  %bc = icmp ult i32 %bn, 2
  br i1 %bc, label %b, label %exit
exit:
  ret i32 %x
}
)IR";

const char* kConstant = R"IR(
define i32 @constant(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %y = add i32 %x, 11
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %y, %loop ]
  ret i32 %result
}
)IR";

const char* kAllIntegerOps = R"IR(
define i32 @all_integer_ops(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %a = add i32 %x, %x
  %b = sub i32 %a, 1
  %c = mul i32 %b, %x
  %d = and i32 %c, %x
  %e = or i32 %d, %x
  %f = xor i32 %e, %x
  %g = shl i32 %f, 1
  %h = lshr i32 %g, 1
  %i = ashr i32 %h, 1
  %cmp = icmp ult i32 %iv.next, 2
  br i1 %cmp, label %loop, label %exit
exit:
  %result = phi i32 [ %i, %loop ]
  ret i32 %result
}
)IR";

const char* kDirectSelect = R"IR(
define i32 @direct_select(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %p = icmp ult i32 %x, %y
  %v = select i1 %p, i32 %x, i32 %y
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %v, %loop ]
  ret i32 %out
}
)IR";

const char* kPredicateSSA = R"IR(
define i32 @predicate_ssa(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %inc = add i32 %iv, 1
  %p = icmp ult i32 %x, %y
  %not.p = xor i1 %p, true
  %wide = zext i1 %not.p to i32
  %v = select i1 %not.p, i32 %wide, i32 %x
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %v, %loop ]
  ret i32 %out
}
)IR";

const char* kDiamond = R"IR(
define i32 @diamond(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %x, %y
  br i1 %p, label %then, label %else
then:
  %a = add i32 %x, %x
  br label %merge
else:
  %b = add i32 %y, %y
  br label %merge
merge:
  %v = phi i32 [ %a, %then ], [ %b, %else ]
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %v, %merge ]
  ret i32 %out
}
)IR";

const char* kTriangle = R"IR(
define i32 @triangle(i32 %x, i32 %y) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %x, %y
  br i1 %p, label %then, label %merge
then:
  %a = add i32 %x, %x
  br label %merge
merge:
  %v = phi i32 [ %a, %then ], [ %x, %cond ]
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  %out = phi i32 [ %v, %merge ]
  ret i32 %out
}
)IR";

const char* kPredicatedStore = R"IR(
define void @predicated_store(i32 %limit, i32* %out) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %iv, %limit
  br i1 %p, label %then, label %else
then:
  %v = add i32 %iv, 1
  store i32 %v, i32* %out
  br label %merge
else:
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kFalseArmStore = R"IR(
define void @false_arm_store(i32 %limit, i32* %out) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %iv, %limit
  br i1 %p, label %then, label %else
then:
  br label %merge
else:
  store i32 %iv, i32* %out
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kTwoFalseArmStores = R"IR(
define void @two_false_arm_stores(i32 %limit, i32 %value, i32* %a, i32* %b) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %iv, %limit
  br i1 %p, label %then, label %else
then:
  br label %merge
else:
  store i32 %iv, i32* %a
  store i32 %value, i32* %b
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kPredicatedLoad = R"IR(
define i32 @predicated_load(i32 %x, i32* %ptr) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  br label %cond
cond:
  %p = icmp ult i32 %x, 5
  br i1 %p, label %then, label %else
then:
  %loaded = load i32, i32* %ptr
  br label %merge
else:
  %v0 = add i32 %x, 0
  br label %merge
merge:
  %v = phi i32 [ %loaded, %then ], [ %v0, %else ]
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  ret i32 %v
}
)IR";

const char* kMultipleBranches = R"IR(
define i32 @multiple_branches(i32 %x) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge2 ]
  %p = icmp ult i32 %x, 3
  br i1 %p, label %then, label %else
then:
  %q = icmp ult i32 %x, 2
  br i1 %q, label %a, label %b
a:
  br label %merge1
b:
  br label %merge1
merge1:
  br label %merge2
else:
  br label %merge2
merge2:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  ret i32 %x
}
)IR";

const char* kUnsafeSpeculation = R"IR(
define i32 @unsafe_speculation(i32 %x, i32 %d) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %p = icmp ult i32 %x, 3
  br i1 %p, label %then, label %else
then:
  %a = sdiv i32 %x, %d
  br label %merge
else:
  %b = add i32 %x, 1
  br label %merge
merge:
  %v = phi i32 [ %a, %then ], [ %b, %else ]
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  ret i32 %v
}
)IR";

const char* kMultipleStores = R"IR(
define void @multiple_stores(i32 %x, i32* %a, i32* %b) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %p = icmp ult i32 %x, 3
  br i1 %p, label %then, label %else
then:
  store i32 %x, i32* %a
  store i32 %x, i32* %b
  br label %merge
else:
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kGEPStore = R"IR(
define void @gep_store(i32 %x, i32* %out) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %p = icmp ult i32 %x, 3
  br i1 %p, label %then, label %else
then:
  %addr = getelementptr i32, i32* %out, i32 1
  store i32 %x, i32* %addr
  br label %merge
else:
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 2
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kConditionalRecurrence = R"IR(
define i32 @conditional_recurrence(i32 %seed, i32 %x) {
entry:
  br label %loop
loop:
  %state = phi i32 [ %seed, %entry ], [ %next, %merge ]
  %p = icmp ult i32 %state, %x
  br i1 %p, label %then, label %else
then:
  %a = add i32 %state, 1
  br label %merge
else:
  %b = add i32 %state, 2
  br label %merge
merge:
  %next = phi i32 [ %a, %then ], [ %b, %else ]
  %done = icmp ult i32 %state, 4
  br i1 %done, label %loop, label %exit
exit:
  ret i32 %next
}
)IR";

const char* kSameAddressArmStores = R"IR(
target datalayout = "e-p:64:64"
define void @same_address_arm_stores(i32* %out, i32 %x, i32 %y, i32 %limit) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %addr = getelementptr i32, i32* %out, i32 %iv
  %p = icmp ult i32 %iv, %limit
  br i1 %p, label %then, label %else
then:
  store i32 %x, i32* %addr, align 4
  br label %merge
else:
  store i32 %y, i32* %addr, align 4
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, 4
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

const char* kRedundantArmLoadStores = R"IR(
target datalayout = "e-p:64:64"
define void @redundant_arm_load_stores(i32* %in, i32* %out, i32 %limit) {
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %inc, %merge ]
  %in.addr = getelementptr i32, i32* %in, i32 %iv
  %value = load i32, i32* %in.addr, align 4
  %p = icmp sgt i32 %value, 0
  br i1 %p, label %then, label %else
then:
  %in.addr.again = getelementptr i32, i32* %in, i32 %iv
  %value.again = load i32, i32* %in.addr.again, align 4
  %out.then = getelementptr i32, i32* %out, i32 %iv
  store i32 %value.again, i32* %out.then, align 4
  br label %merge
else:
  %out.else = getelementptr i32, i32* %out, i32 %iv
  store i32 0, i32* %out.else, align 4
  br label %merge
merge:
  %inc = add i32 %iv, 1
  %done = icmp ult i32 %inc, %limit
  br i1 %done, label %loop, label %exit
exit:
  ret void
}
)IR";

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::unique_ptr<llvm::Module> parse(const char* text, llvm::LLVMContext& context);

std::string linearLoopIR(std::size_t blockCount, std::string functionName, std::string blockPrefix,
                         std::string valuePrefix, bool reorderText = false,
                         bool dataRecurrence = true) {
  if (blockCount < 2)
    throw std::invalid_argument("linear loop fixture requires at least two blocks");
  const auto header = blockPrefix + "_header";
  const auto latch = blockPrefix + "_latch";
  std::vector<std::string> blocks{header};
  for (std::size_t index = 1; index + 1 < blockCount; ++index)
    blocks.push_back(blockPrefix + "_body" + std::to_string(index));
  blocks.push_back(latch);

  std::vector<std::string> definitions(blockCount);
  definitions[0] = header + ":\n  %" + valuePrefix + "_iv = phi i32 [ 0, %entry ], [ %" +
                   valuePrefix + "_next, %" + latch + " ]\n  %" + valuePrefix +
                   "_v0 = add i32 %x, %" +
                   (dataRecurrence ? valuePrefix + "_iv" : std::string("x")) + "\n  br label %" +
                   blocks[1] + "\n";
  for (std::size_t index = 1; index + 1 < blockCount; ++index) {
    definitions[index] = blocks[index] + ":\n  %" + valuePrefix + "_v" + std::to_string(index) +
                         " = add i32 %" + valuePrefix + "_v" + std::to_string(index - 1) +
                         ", 1\n  br label %" + blocks[index + 1] + "\n";
  }
  const auto last = blockCount - 1;
  definitions[last] = latch + ":\n  %" + valuePrefix + "_v" + std::to_string(last) +
                      " = add i32 %" + valuePrefix + "_v" + std::to_string(last - 1) + ", 1\n  %" +
                      valuePrefix + "_next = add i32 %" + valuePrefix + "_iv, 1\n  %" +
                      valuePrefix + "_done = icmp ult i32 %" + valuePrefix + "_next, 4\n  br i1 %" +
                      valuePrefix + "_done, label %" + header + ", label %exit\n";

  std::ostringstream ir;
  ir << "define i32 @" << functionName << "(i32 %x) {\nentry:\n  br label %" << header << "\n";
  if (reorderText) {
    ir << definitions[0] << definitions[last];
    for (std::size_t index = 1; index < last; ++index)
      ir << definitions[index];
  } else {
    for (const auto& definition : definitions)
      ir << definition;
  }
  ir << "exit:\n  %out = phi i32 [ %" << valuePrefix << "_v" << last << ", %" << latch
     << " ]\n  ret i32 %out\n}\n";
  return ir.str();
}

const char* kLinearPretest = R"IR(
define i32 @linear_pretest(i32 %x) {
entry:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ %next, %body ]
  %done = icmp ult i32 %iv, 4
  br i1 %done, label %body, label %exit
body:
  %y = add i32 %x, %iv
  %next = add i32 %iv, 1
  br label %header
exit:
  ret i32 %x
}
)IR";

const char* kLinearNonHeaderPhi = R"IR(
define i32 @linear_nonheader_phi(i32 %x) {
entry:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ %next, %body ]
  br label %body
body:
  %hidden = phi i32 [ %x, %header ]
  %y = add i32 %hidden, %iv
  %next = add i32 %iv, 1
  %done = icmp ult i32 %next, 4
  br i1 %done, label %header, label %exit
exit:
  %out = phi i32 [ %y, %body ]
  ret i32 %out
}
)IR";

const char* kLinearNoPreheader = R"IR(
define void @linear_no_preheader(i1 %choose) {
entry:
  br i1 %choose, label %header, label %alternate
alternate:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ 7, %alternate ], [ %next, %body ]
  %done = icmp ult i32 %iv, 4
  br i1 %done, label %body, label %exit
body:
  %data = add i32 %iv, 2
  %next = add i32 %iv, 1
  br label %header
exit:
  ret void
}
)IR";

const char* kLinearSwitchTerminator = R"IR(
define void @linear_switch() {
entry:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ %next, %body ]
  br label %body
body:
  %next = add i32 %iv, 1
  switch i32 %next, label %exit [ i32 0, label %header ]
exit:
  ret void
}
)IR";

const char* kLinearTwoExits = R"IR(
define void @linear_two_exits(i1 %stop) {
entry:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ %next, %latch ]
  %done = icmp ult i32 %iv, 4
  br i1 %done, label %body, label %exit0
body:
  br i1 %stop, label %exit1, label %latch
latch:
  %next = add i32 %iv, 1
  br label %header
exit0:
  ret void
exit1:
  ret void
}
)IR";

const char* kLinearInternalDiamond = R"IR(
define void @linear_internal_diamond(i1 %condition) {
entry:
  br label %header
header:
  %iv = phi i32 [ 0, %entry ], [ %next, %latch ]
  %done = icmp ult i32 %iv, 4
  br i1 %done, label %body, label %exit
body:
  br i1 %condition, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  br label %latch
latch:
  %next = add i32 %iv, 1
  br label %header
exit:
  ret void
}
)IR";

cgra::frontend::llvm_frontend::LinearLoopStatus linearAnalysisStatus(const char* text,
                                                                     const char* functionName) {
  llvm::LLVMContext context;
  auto module = parse(text, context);
  auto* function = module->getFunction(functionName);
  llvm::DominatorTree dominatorTree(*function);
  llvm::LoopInfo loopInfo(dominatorTree);
  expect(!loopInfo.empty(), "linear analysis fixture must contain a natural loop");
  return cgra::frontend::llvm_frontend::discoverLinearLoopRegion(**loopInfo.begin()).status;
}

std::string semanticFingerprint(const cgra::ir::DFG& dfg) {
  std::ostringstream output;
  for (const auto& node : dfg.nodes()) {
    output << cgra::ir::toString(node.opcode) << ':' << node.resultType.toString() << ':';
    for (const auto& operand : node.operandTypes)
      output << operand.toString() << ',';
    output << ';';
  }
  for (const auto& edge : dfg.edges()) {
    output << static_cast<unsigned>(edge.kind()) << ':' << edge.src << ':' << edge.dst << ':'
           << edge.distance;
    if (const auto* data = std::get_if<cgra::ir::DataEdgeInfo>(&edge.info))
      output << ':' << data->dstOperand << ':' << data->boundary.has_value();
    output << ';';
  }
  for (const auto& binding : dfg.externalBindings())
    output << "B:" << binding.node << ':' << binding.operand << ':' << binding.source.index()
           << ';';
  for (const auto& constant : dfg.constants())
    output << "C:" << constant.type.toString() << ':' << constant.bits << ';';
  for (const auto& liveOut : dfg.liveOuts())
    output << "L:" << liveOut.type.toString() << ':' << liveOut.source << ';';
  return output.str();
}

void testLinearMultiBlockLowering() {
  for (const auto blockCount : {2U, 3U, 4U, 5U}) {
    llvm::LLVMContext context;
    const auto function = "linear_" + std::to_string(blockCount);
    const auto text = linearLoopIR(blockCount, function, "cfg", "ssa");
    auto module = parse(text.c_str(), context);
    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    options.functionName = function;
    auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    expect(result.ok(), result.message.c_str());
    expect(result.metadata && result.metadata->loopShape == "linear_multiblock",
           "multi-block loop must report its structural shape");
    expect(result.metadata->loopBlockCount == blockCount,
           "linear loop metadata must preserve block count");
    expect(result.provenance.linearLoop &&
               result.provenance.linearLoop->orderedBlocks.size() == blockCount,
           "linear loop must publish CFG-derived block order");
    const auto report =
        cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result);
    expect(report.ok(), report.format().c_str());

    const auto crossBlock = std::ranges::find_if(result.dfg->edges(), [&](const auto& edge) {
      if (edge.kind() != cgra::ir::Edge::Kind::Data || edge.distance != 0)
        return false;
      const auto source = std::ranges::find_if(
          result.provenance.nodes, [&](const auto& item) { return item.node == edge.src; });
      const auto destination = std::ranges::find_if(
          result.provenance.nodes, [&](const auto& item) { return item.node == edge.dst; });
      return source != result.provenance.nodes.end() &&
             destination != result.provenance.nodes.end() &&
             source->basicBlock != destination->basicBlock;
    });
    expect(crossBlock != result.dfg->edges().end(),
           "linear fixture must contain a distance-zero cross-block SSA edge");

    if (blockCount == 3) {
      auto missingEdge = result;
      cgra::ir::DFGTestAccess::eraseEdge(*missingEdge.dfg, crossBlock->id);
      expect(
          !cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, missingEdge).ok(),
          "verifier must reject a missing cross-block SSA edge");

      auto wrongOperand = result;
      const auto crossBlockOperand = std::get<cgra::ir::DataEdgeInfo>(crossBlock->info).dstOperand;
      cgra::ir::DFGTestAccess::setEdgeOperand(*wrongOperand.dfg, crossBlock->id,
                                              crossBlockOperand + 1);
      expect(
          !cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, wrongOperand).ok(),
          "verifier must reject a wrong cross-block destination operand");

      auto missingBodyNode = result;
      cgra::ir::DFGTestAccess::eraseNode(*missingBodyNode.dfg, crossBlock->dst);
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, missingBodyNode)
                  .ok(),
             "verifier must reject a dropped body-block semantic node");

      auto externalizedBodyValue = result;
      cgra::ir::DFGTestAccess::eraseEdge(*externalizedBodyValue.dfg, crossBlock->id);
      cgra::ir::DFGTestAccess::appendExternalBinding(
          *externalizedBodyValue.dfg, crossBlock->dst, crossBlockOperand,
          externalizedBodyValue.dfg->externalValues().front().id);
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options,
                                                                  externalizedBodyValue)
                  .ok(),
             "verifier must reject an in-loop cross-block value externalized as a live-in");

      const auto recurrence = std::ranges::find_if(result.dfg->edges(), [](const auto& edge) {
        return edge.kind() == cgra::ir::Edge::Kind::Data && edge.distance == 1;
      });
      expect(recurrence != result.dfg->edges().end(),
             "linear fixture must retain its header-PHI recurrence edge");
      auto missingRecurrence = result;
      cgra::ir::DFGTestAccess::eraseEdge(*missingRecurrence.dfg, recurrence->id);
      expect(
          !cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, missingRecurrence)
               .ok(),
          "verifier must reject a missing cross-block recurrence edge");

      auto wrongRecurrenceSource = result;
      const auto replacement =
          recurrence->src == crossBlock->src ? crossBlock->dst : crossBlock->src;
      cgra::ir::DFGTestAccess::setEdgeSource(*wrongRecurrenceSource.dfg, recurrence->id,
                                             replacement);
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options,
                                                                  wrongRecurrenceSource)
                  .ok(),
             "verifier must reject a recurrence sourced from the wrong latch value");

      auto badOrder = result;
      std::reverse(badOrder.provenance.linearLoop->orderedBlocks.begin(),
                   badOrder.provenance.linearLoop->orderedBlocks.end());
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, badOrder).ok(),
             "verifier must reject corrupted linear CFG order");

      auto badBlock = result;
      badBlock.provenance.nodes.front().basicBlock = "wrong.block";
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, badBlock).ok(),
             "verifier must reject wrong node block provenance");

      auto fakeBranchNode = result;
      const auto duplicate = cgra::ir::DFGTestAccess::appendDuplicateNode(
          *fakeBranchNode.dfg, fakeBranchNode.dfg->nodes().front().id);
      const llvm::BranchInst* termination = nullptr;
      for (const auto& block : *module->getFunction(function)) {
        const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
        if (branch && branch->isConditional()) {
          termination = branch;
          break;
        }
      }
      expect(termination != nullptr, "linear fixture must contain a termination branch");
      fakeBranchNode.provenance.nodes.push_back(
          {duplicate, function, termination->getParent()->getName().str(), 0, "br", termination});
      expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, fakeBranchNode)
                  .ok(),
             "verifier must reject a fake Generic node for the LLVM branch terminator");
    }
  }

  llvm::LLVMContext renamedContext;
  auto renamed = parse(linearLoopIR(4, "renamed_function", "random_block", "random_value").c_str(),
                       renamedContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions renamedOptions;
  renamedOptions.functionName = "renamed_function";
  const auto renamedResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*renamed, renamedOptions);
  expect(renamedResult.ok(), "renamed linear loop must lower");

  llvm::LLVMContext reorderedContext;
  auto reordered =
      parse(linearLoopIR(4, "layout_changed", "bb", "q", true).c_str(), reorderedContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions reorderedOptions;
  reorderedOptions.functionName = "layout_changed";
  const auto reorderedResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*reordered, reorderedOptions);
  expect(reorderedResult.ok(), "textually reordered linear loop must lower");
  expect(semanticFingerprint(*renamedResult.dfg) == semanticFingerprint(*reorderedResult.dfg),
         "function/block/SSA renaming and textual layout must preserve Generic semantics");

  for (const auto seed : {0U, 1U, 7U, 19U, 42U}) {
    llvm::LLVMContext context;
    const auto blocks = 2U + seed % 4U;
    const auto function = "property_" + std::to_string(seed);
    const auto text = linearLoopIR(blocks, function, "block_" + std::to_string(seed),
                                   "value_" + std::to_string(seed), seed % 2U != 0, seed % 3U != 0);
    auto module = parse(text.c_str(), context);
    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    options.functionName = function;
    const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    expect(result.ok(), "deterministic linear-loop property fixture must lower");
    expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result).ok(),
           "deterministic linear-loop property fixture must verify");
  }

  llvm::LLVMContext pretestContext;
  auto pretest = parse(kLinearPretest, pretestContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions pretestOptions;
  pretestOptions.functionName = "linear_pretest";
  const auto pretestResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*pretest, pretestOptions);
  expect(pretestResult.ok(), "pre-test header termination loop must lower");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*pretest, pretestOptions, pretestResult)
          .ok(),
      "pre-test header termination loop must verify");

  llvm::LLVMContext phiContext;
  auto nonHeaderPhi = parse(kLinearNonHeaderPhi, phiContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions phiOptions;
  phiOptions.functionName = "linear_nonheader_phi";
  const auto phiResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*nonHeaderPhi, phiOptions);
  expect(!phiResult.ok() &&
             phiResult.status ==
                 cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedLoopShape,
         "linear loop with non-header PHI must be rejected");
  expect(!phiResult.diagnostics.empty() &&
             phiResult.diagnostics.front().code ==
                 cgra::frontend::llvm_frontend::LLVMFrontendDiagnosticCode::
                     LLVM_FRONTEND_LINEAR_LOOP_NONHEADER_PHI,
         "non-header PHI rejection must have a stable diagnostic");

  expect(linearAnalysisStatus(kLinearNoPreheader, "linear_no_preheader") ==
             cgra::frontend::llvm_frontend::LinearLoopStatus::NoPreheader,
         "source linear analyzer must identify a missing canonical preheader");
  {
    llvm::LLVMContext context;
    auto module = parse(kLinearNoPreheader, context);
    const auto originalBlocks = module->getFunction("linear_no_preheader")->size();
    cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
    options.functionName = "linear_no_preheader";
    const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
    expect(result.ok(), "selected-loop entry canonicalization must lower a safe natural loop");
    expect(result.normalizedModule != nullptr,
           "entry-canonicalized lowering must retain its normalized analysis module");
    expect(result.metadata && result.metadata->loopEntryCanonicalized,
           "frontend metadata must disclose selected-loop entry canonicalization");
    expect(module->getFunction("linear_no_preheader")->size() == originalBlocks,
           "frontend must not modify the caller's LLVM module in place");
    expect(result.dfg->externalValues().size() == 1,
           "synthetic-preheader PHI must become one selected-loop live-in");
    expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result).ok(),
           "independent verifier must validate the normalized loop-entry environment");
  }
  expect(linearAnalysisStatus(kLinearSwitchTerminator, "linear_switch") !=
             cgra::frontend::llvm_frontend::LinearLoopStatus::Success,
         "linear analyzer must reject switch termination");
  expect(linearAnalysisStatus(kLinearTwoExits, "linear_two_exits") ==
             cgra::frontend::llvm_frontend::LinearLoopStatus::ExitShape,
         "linear analyzer must reject multiple exits");
  expect(linearAnalysisStatus(kLinearInternalDiamond, "linear_internal_diamond") ==
             cgra::frontend::llvm_frontend::LinearLoopStatus::InternalConditionalBranch,
         "linear analyzer must not absorb an internal diamond");
}

std::unique_ptr<llvm::Module> parse(const char* text, llvm::LLVMContext& context) {
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(text, diagnostic, context);
  if (!module)
    throw std::runtime_error("cannot parse LLVM fixture");
  return module;
}

void testScalarAddLoop() {
  llvm::LLVMContext context;
  auto module = parse(kScalarAdd, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "kernel";
  const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(result.ok(), result.message.c_str());
  expect(result.metadata && result.metadata->staticTripCount == 4,
         "static trip count must be recovered");
  expect(result.dfg && result.dfg->nodes().size() == 1, "control-only instructions must be absent");
  expect(result.dfg->node(0).opcode == cgra::ir::Opcode::Add, "x+x must lower to Add");
  expect(result.dfg->externalValues().size() == 1, "x must be one ExternalValue");
  expect(result.dfg->externalBindings().size() == 2, "x must feed both Add operands");
  expect(result.dfg->liveOuts().size() == 1, "LCSSA value must become one LiveOut");
  const auto report = cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result);
  expect(report.ok(), report.format().c_str());
}

void testFrontendInvocationValidation() {
  llvm::LLVMContext context;
  auto module = parse(kScalarAdd, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "kernel";
  const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(result.ok() && result.metadata, "invocation validation fixture must lower");

  cgra::abi::KernelInvocation matching{4, {}, {}};
  const auto match =
      cgra::frontend::llvm_frontend::validateFrontendInvocation(*result.metadata, matching);
  expect(match.ok(), "matching static trip count must pass composition validation");

  cgra::abi::KernelInvocation mismatching{3, {}, {}};
  const auto mismatch =
      cgra::frontend::llvm_frontend::validateFrontendInvocation(*result.metadata, mismatching);
  expect(!mismatch.ok() && mismatch.status ==
                               cgra::frontend::llvm_frontend::FrontendInvocationValidationStatus::
                                   FrontendInvocationMismatch,
         "mismatching static trip count must fail composition validation");

  auto unknown = *result.metadata;
  unknown.staticTripCount.reset();
  const auto unknownReport =
      cgra::frontend::llvm_frontend::validateFrontendInvocation(unknown, mismatching);
  expect(unknownReport.ok(), "unknown static trip count must defer to invocation validation");
}

void testDeterministicSerialization() {
  llvm::LLVMContext context;
  auto module = parse(kScalarAdd, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "kernel";
  const auto first = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  const auto second = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(first.ok() && second.ok(), "deterministic fixture must lower");
  expect(first.toJson() == second.toJson(), "frontend result JSON must be deterministic");
}

void testVerifierRejectsCorruptedProvenance() {
  llvm::LLVMContext context;
  auto module = parse(kScalarAdd, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "kernel";
  auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(result.ok(), "corruption fixture must lower before mutation");
  result.provenance.nodes.front().instruction = nullptr;
  const auto report = cgra::frontend::llvm_frontend::verifyFrontendResult(*module, options, result);
  expect(!report.ok(), "independent verifier must reject missing node provenance");
}

void testSupportedChainAndConstant() {
  llvm::LLVMContext context;
  auto chain = parse(kChain, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "chain";
  const auto chainResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*chain, options);
  expect(chainResult.ok(), "arithmetic chain must lower");
  expect(chainResult.dfg->nodes().size() == 3 && chainResult.dfg->edges().size() == 2,
         "chain must preserve three nodes and two distance-zero edges");
  expect(std::get<cgra::ir::DataEdgeInfo>(chainResult.dfg->edge(0).info).dstOperand == 0,
         "chain first edge operand");
  expect(std::get<cgra::ir::DataEdgeInfo>(chainResult.dfg->edge(1).info).dstOperand == 0,
         "chain second edge operand");

  auto constant = parse(kConstant, context);
  options.functionName = "constant";
  const auto constantResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*constant, options);
  expect(constantResult.ok() && constantResult.dfg->constants().size() == 1,
         "ConstantInt must become one Generic constant");
  expect(constantResult.dfg->constant(0).bits == 11, "constant bit pattern");

  auto allOps = parse(kAllIntegerOps, context);
  options.functionName = "all_integer_ops";
  const auto allOpsResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*allOps, options);
  expect(allOpsResult.ok(), "all supported integer operations must lower");
  expect(allOpsResult.dfg->nodes().size() == 9,
         "all supported integer operations must become Generic nodes");
  const cgra::ir::Opcode expected[] = {
      cgra::ir::Opcode::Add, cgra::ir::Opcode::Sub,  cgra::ir::Opcode::Mul,
      cgra::ir::Opcode::And, cgra::ir::Opcode::Or,   cgra::ir::Opcode::Xor,
      cgra::ir::Opcode::Shl, cgra::ir::Opcode::LShr, cgra::ir::Opcode::AShr};
  for (std::size_t index = 0; index < std::size(expected); ++index)
    expect(allOpsResult.dfg->node(static_cast<cgra::ir::NodeId>(index)).opcode == expected[index],
           "integer opcode lowering order");
}

void expectStatus(const char* text, const char* function,
                  cgra::frontend::llvm_frontend::LLVMFrontendStatus expected) {
  llvm::LLVMContext context;
  auto module = parse(text, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = function;
  const auto result = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  if (result.ok() || result.status != expected) {
    std::cerr << "unexpected status for " << function << ": " << result.message << '\n';
    throw std::runtime_error("unexpected frontend rejection status");
  }
  expect(!result.diagnostics.empty(), "unsupported frontend result needs a structured diagnostic");
}

void testBoundaryRejections() {
  llvm::LLVMContext memoryContext;
  auto memory = parse(kMemory, memoryContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions memoryOptions;
  memoryOptions.functionName = "memory";
  const auto memoryResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*memory, memoryOptions);
  expect(memoryResult.ok(), "T018 must lower an invariant direct Load");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*memory, memoryOptions, memoryResult)
             .ok(),
         "T018 direct Load must pass frontend verification");
  llvm::LLVMContext floatContext;
  auto floatModule = parse(kFloat, floatContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions floatOptions;
  floatOptions.functionName = "float_loop";
  const auto floatResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*floatModule, floatOptions);
  expect(floatResult.ok(), "target-independent frontend must preserve scalar float semantics");
  expect(floatResult.dfg->nodes().size() == 1 &&
             floatResult.dfg->node(0).opcode == cgra::ir::Opcode::Custom &&
             floatResult.dfg->node(0).operationKey == std::optional<std::string>{"FADD"},
         "LLVM fadd must lower to the typed FADD operation key");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*floatModule, floatOptions, floatResult)
          .ok(),
      "independent verifier must accept typed float lowering");

  llvm::LLVMContext fmaContext;
  auto fmaModule = parse(kFusedMultiplyAdd, fmaContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions fmaOptions;
  fmaOptions.functionName = "fma_loop";
  const auto fmaResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*fmaModule, fmaOptions);
  expect(fmaResult.ok() && fmaResult.dfg->nodes().size() == 1,
         "llvm.fmuladd must lower as one semantic operation");
  expect(fmaResult.dfg->node(0).opcode == cgra::ir::Opcode::Custom &&
             fmaResult.dfg->node(0).operationKey == std::optional<std::string>{"FMA"} &&
             fmaResult.dfg->node(0).operandTypes.size() == 3,
         "llvm.fmuladd must lower to one three-input FMA custom operation");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*fmaModule, fmaOptions, fmaResult).ok(),
      "independent verifier must accept FMA intrinsic lowering");

  llvm::LLVMContext helperContext;
  auto helperModule = parse(kPureHelper, helperContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions helperOptions;
  helperOptions.functionName = "pure_helper_loop";
  const auto helperResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*helperModule, helperOptions);
  expect(helperResult.ok() && helperResult.metadata &&
             helperResult.metadata->inlinedPureHelperCalls == 1,
         "one small pure local helper must inline on the analysis clone");
  expect(std::ranges::none_of(helperResult.provenance.nodes,
                              [](const auto& node) { return node.opcode == "call"; }),
         "the Generic graph must contain helper semantics rather than a Call node");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*helperModule, helperOptions,
                                                             helperResult)
             .ok(),
         "inlined helper semantics must pass independent frontend verification");
  expectStatus(kUndefOperand, "undef_operand",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedInstruction);

  llvm::LLVMContext context;
  auto module = parse(kAmbiguous, context);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;
  options.functionName = "two_loops";
  const auto ambiguous = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  expect(ambiguous.status ==
             cgra::frontend::llvm_frontend::LLVMFrontendStatus::AmbiguousLoopSelection,
         "multiple innermost loops must not select nondeterministically");
  options.loopHeader = "b";
  const auto selected = cgra::frontend::llvm_frontend::lowerInnermostLoop(*module, options);
  if (!(selected.ok() && selected.metadata->loopHeader == "b")) {
    std::cerr << "explicit selection status="
              << cgra::frontend::llvm_frontend::toString(selected.status)
              << " message=" << selected.message << '\n';
    throw std::runtime_error("explicit loop header must select the requested loop");
  }
}

void testPredicationLowering() {
  llvm::LLVMContext context;
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;

  auto direct = parse(kDirectSelect, context);
  options.functionName = "direct_select";
  const auto directResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*direct, options);
  expect(directResult.ok(), "direct LLVM Select must lower");
  expect(directResult.dfg->nodes().size() == 2, "direct Select emits ICmp and Select");
  expect(directResult.dfg->edges().size() == 1 &&
             directResult.dfg->edge(0).kind() == cgra::ir::Edge::Kind::Predicate,
         "direct Select uses a PredicateEdge");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*direct, options, directResult).ok(),
         "direct Select verifier");
  auto wrongPredicate = directResult;
  cgra::ir::DFGTestAccess::setEdgeSource(*wrongPredicate.dfg, 0, 1);
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*direct, options, wrongPredicate).ok(),
      "Select verifier must reject a corrupted predicate source");

  auto predicateSSA = parse(kPredicateSSA, context);
  options.functionName = "predicate_ssa";
  const auto predicateResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*predicateSSA, options);
  expect(predicateResult.ok(), "predicate SSA operations must lower");
  const auto predicateXor =
      std::ranges::find_if(predicateResult.dfg->nodes(), [](const auto& node) {
        return node.opcode == cgra::ir::Opcode::Custom && node.operationKey == "PXOR";
      });
  expect(predicateXor != predicateResult.dfg->nodes().end(),
         "i1 xor must retain predicate semantics as PXOR");
  expect(std::ranges::any_of(predicateResult.dfg->nodes(),
                             [](const auto& node) {
                               return node.opcode == cgra::ir::Opcode::Custom &&
                                      node.operationKey == "PZEXT";
                             }),
         "predicate extension must retain its predicate-to-data signature");
  expect(std::ranges::count_if(
             predicateResult.dfg->edges(),
             [](const auto& edge) { return edge.kind() == cgra::ir::Edge::Kind::Predicate; }) == 3,
         "predicate SSA users must be connected by PredicateEdges");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*predicateSSA, options, predicateResult)
          .ok(),
      "predicate SSA verifier");

  auto diamond = parse(kDiamond, context);
  options.functionName = "diamond";
  const auto diamondResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*diamond, options);
  expect(diamondResult.ok(), "diamond branch must lower");
  expect(diamondResult.dfg->nodes().size() == 4, "diamond emits ICmp, two arm adds, and Select");
  expect(diamondResult.dfg->liveOuts().size() == 1, "diamond Select becomes LiveOut");
  expect(diamondResult.provenance.ifConversions.size() == 1, "diamond emits one IfConversion plan");
  expect(!diamondResult.provenance.linearLoop,
         "T017 diamond must not be swallowed by the linear-loop path");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, diamondResult).ok(),
         "diamond verifier");
  auto swappedArms = diamondResult;
  cgra::ir::EdgeId firstArm = 0;
  cgra::ir::EdgeId secondArm = 0;
  for (const auto& edge : swappedArms.dfg->edges()) {
    if (edge.kind() != cgra::ir::Edge::Kind::Data)
      continue;
    const auto info = std::get<cgra::ir::DataEdgeInfo>(edge.info);
    if (info.dstOperand == 1)
      firstArm = edge.id;
    if (info.dstOperand == 2)
      secondArm = edge.id;
  }
  const auto firstSource = swappedArms.dfg->edge(firstArm).src;
  const auto secondSource = swappedArms.dfg->edge(secondArm).src;
  cgra::ir::DFGTestAccess::setEdgeSource(*swappedArms.dfg, firstArm, secondSource);
  cgra::ir::DFGTestAccess::setEdgeSource(*swappedArms.dfg, secondArm, firstSource);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, swappedArms).ok(),
         "Select verifier must reject swapped arm providers");
  auto omittedSelectPlan = diamondResult;
  omittedSelectPlan.provenance.ifConversions.front().selects.clear();
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, omittedSelectPlan)
              .ok(),
         "verifier must reject a Generic Select omitted from the if-conversion plan");
  auto wrongArmOpcode = diamondResult;
  const auto armNode = std::ranges::find_if(wrongArmOpcode.provenance.nodes, [](const auto& item) {
    return item.instruction && item.instruction->hasName() && item.instruction->getName() == "a";
  });
  expect(armNode != wrongArmOpcode.provenance.nodes.end(),
         "diamond fixture must expose a true-arm arithmetic node");
  cgra::ir::DFGTestAccess::setNodeOpcode(*wrongArmOpcode.dfg, armNode->node, cgra::ir::Opcode::Mul);
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, wrongArmOpcode).ok(),
      "if-conversion verifier must reject corrupted ordinary arithmetic semantics");
  auto wrongArmExternal = diamondResult;
  const auto yExternal =
      std::ranges::find_if(wrongArmExternal.provenance.externals, [](const auto& item) {
        return item.value && item.value->hasName() && item.value->getName() == "y";
      });
  expect(yExternal != wrongArmExternal.provenance.externals.end(),
         "diamond fixture must expose y as an ExternalValue");
  cgra::ir::DFGTestAccess::setBindingExternal(*wrongArmExternal.dfg, armNode->node, 0,
                                              yExternal->external);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, wrongArmExternal)
              .ok(),
         "if-conversion verifier must reject a wrong ordinary external provider");

  const auto selectNode = diamondResult.provenance.ifConversions.front().selects.front().node;
  auto missingSelect = diamondResult;
  cgra::ir::DFGTestAccess::eraseNode(*missingSelect.dfg, selectNode);
  std::erase_if(missingSelect.provenance.nodes,
                [&](const auto& item) { return item.node == selectNode; });
  missingSelect.provenance.ifConversions.front().selects.clear();
  missingSelect.provenance.liveOuts.clear();
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, missingSelect).ok(),
      "verifier must reconstruct a missing merge Select from the LLVM CFG");

  auto duplicateSelect = diamondResult;
  const auto duplicateNode =
      cgra::ir::DFGTestAccess::appendDuplicateNode(*duplicateSelect.dfg, selectNode);
  const auto selectNodeProvenance = std::ranges::find_if(
      duplicateSelect.provenance.nodes, [&](const auto& item) { return item.node == selectNode; });
  expect(selectNodeProvenance != duplicateSelect.provenance.nodes.end(),
         "diamond fixture must expose Select node provenance");
  auto duplicateNodeProvenance = *selectNodeProvenance;
  duplicateNodeProvenance.node = duplicateNode;
  duplicateSelect.provenance.nodes.push_back(std::move(duplicateNodeProvenance));
  auto duplicatePlan = duplicateSelect.provenance.ifConversions.front().selects.front();
  duplicatePlan.node = duplicateNode;
  duplicateSelect.provenance.ifConversions.front().selects.push_back(std::move(duplicatePlan));
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, duplicateSelect).ok(),
      "verifier must reject a duplicate Generic Select for one LLVM merge PHI");

  auto missingLiveOut = diamondResult;
  cgra::ir::DFGTestAccess::clearLiveOuts(*missingLiveOut.dfg);
  missingLiveOut.provenance.liveOuts.clear();
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*diamond, options, missingLiveOut).ok(),
      "verifier must reconstruct a missing LiveOut from LLVM outside uses");

  auto triangle = parse(kTriangle, context);
  options.functionName = "triangle";
  const auto triangleResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*triangle, options);
  expect(triangleResult.ok(), "triangle branch must lower");
  expect(triangleResult.dfg->nodes().size() == 3,
         "triangle emits ICmp, one arm operation, and Select");
  expect(!triangleResult.provenance.linearLoop,
         "T017 triangle must not be swallowed by the linear-loop path");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*triangle, options, triangleResult).ok(),
      "triangle verifier");

  auto store = parse(kPredicatedStore, context);
  options.functionName = "predicated_store";
  const auto storeResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*store, options);
  expect(storeResult.ok(), "predicated Store must lower");
  expect(storeResult.metadata->staticTripCount == 4,
         "if-converted loop must preserve ScalarEvolution static trip count");
  cgra::abi::KernelInvocation mismatchedInvocation;
  mismatchedInvocation.tripCount = 3;
  expect(!cgra::frontend::llvm_frontend::validateFrontendInvocation(*storeResult.metadata,
                                                                    mismatchedInvocation)
              .ok(),
         "if-converted loop must reject a mismatched concrete trip count");
  const auto storeNode = storeResult.dfg->nodes().back().id;
  expect(storeResult.dfg->node(storeNode).opcode == cgra::ir::Opcode::Store,
         "predicated Store node exists");
  expect(std::any_of(storeResult.dfg->edges().begin(), storeResult.dfg->edges().end(),
                     [&](const auto& edge) {
                       return edge.kind() == cgra::ir::Edge::Kind::Predicate &&
                              std::get<cgra::ir::PredicateEdgeInfo>(edge.info).dstOperand == 2;
                     }),
         "Store commit uses PredicateEdge");
  expect(std::any_of(storeResult.dfg->edges().begin(), storeResult.dfg->edges().end(),
                     [&](const auto& edge) {
                       return edge.kind() == cgra::ir::Edge::Kind::Memory &&
                              edge.src == storeNode && edge.dst == storeNode && edge.distance == 1;
                     }),
         "predicated Store has self WAW");
  auto badWaw = storeResult;
  for (const auto& edge : badWaw.dfg->edges()) {
    if (edge.kind() == cgra::ir::Edge::Kind::Memory && edge.src == storeNode &&
        edge.dst == storeNode) {
      cgra::ir::DFGTestAccess::setEdgeDistance(*badWaw.dfg, edge.id, 0);
      break;
    }
  }
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*store, options, badWaw).ok(),
         "Store verifier must reject a non-loop-carried self WAW");

  auto missingPredicate = storeResult;
  auto missingPredicateEdge =
      std::find_if(missingPredicate.dfg->edges().begin(), missingPredicate.dfg->edges().end(),
                   [](const auto& edge) { return edge.kind() == cgra::ir::Edge::Kind::Predicate; });
  expect(missingPredicateEdge != missingPredicate.dfg->edges().end(),
         "predicated Store fixture needs a predicate edge");
  cgra::ir::DFGTestAccess::eraseEdge(*missingPredicate.dfg, missingPredicateEdge->id);
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*store, options, missingPredicate).ok(),
      "Store verifier must reject a missing commit predicate edge");

  auto wrongStorePredicate = storeResult;
  auto wrongPredicateEdge =
      std::find_if(wrongStorePredicate.dfg->edges().begin(), wrongStorePredicate.dfg->edges().end(),
                   [](const auto& edge) { return edge.kind() == cgra::ir::Edge::Kind::Predicate; });
  expect(wrongPredicateEdge != wrongStorePredicate.dfg->edges().end(),
         "predicated Store fixture needs a predicate edge for corruption");
  cgra::ir::DFGTestAccess::setEdgeSource(*wrongStorePredicate.dfg, wrongPredicateEdge->id,
                                         storeNode);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*store, options, wrongStorePredicate)
              .ok(),
         "Store verifier must reject the wrong commit predicate source");

  auto missingWaw = storeResult;
  auto wawEdge = std::find_if(missingWaw.dfg->edges().begin(), missingWaw.dfg->edges().end(),
                              [&](const auto& edge) {
                                return edge.kind() == cgra::ir::Edge::Kind::Memory &&
                                       edge.src == storeNode && edge.dst == storeNode;
                              });
  expect(wawEdge != missingWaw.dfg->edges().end(), "predicated Store fixture needs a self WAW");
  cgra::ir::DFGTestAccess::eraseEdge(*missingWaw.dfg, wawEdge->id);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*store, options, missingWaw).ok(),
         "Store verifier must reject a missing self WAW");

  auto badRecurrence = storeResult;
  const auto recurrenceEdge =
      std::ranges::find_if(badRecurrence.dfg->edges(), [](const auto& edge) {
        return edge.kind() == cgra::ir::Edge::Kind::Data && edge.distance == 1;
      });
  expect(recurrenceEdge != badRecurrence.dfg->edges().end(),
         "predicated Store fixture needs a recurrence edge");
  cgra::ir::DFGTestAccess::setEdgeDistance(*badRecurrence.dfg, recurrenceEdge->id, 0);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*store, options, badRecurrence).ok(),
         "if-conversion verifier must reject corrupted recurrence semantics");

  auto falseStore = parse(kFalseArmStore, context);
  options.functionName = "false_arm_store";
  const auto falseStoreResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*falseStore, options);
  expect(falseStoreResult.ok(), "false-arm Store must lower through predicate complement");
  const auto falseStoreVerification =
      cgra::frontend::llvm_frontend::verifyFrontendResult(*falseStore, options, falseStoreResult);
  expect(falseStoreVerification.ok(), "false-arm Store verifier");
  expect(falseStoreResult.provenance.ifConversions.front().predicateComplemented,
         "false-arm Store must record normalized predicate polarity");

  auto twoFalseStores = parse(kTwoFalseArmStores, context);
  options.functionName = "two_false_arm_stores";
  const auto twoFalseStoreResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*twoFalseStores, options);
  expect(twoFalseStoreResult.ok(),
         "all false-arm Stores must share one complemented commit predicate");
  expect(twoFalseStoreResult.provenance.ifConversions.front().predicateComplemented &&
             twoFalseStoreResult.provenance.ifConversions.front().predicatedStores.size() == 2,
         "two false-arm Stores must both use the normalized true predicate");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*twoFalseStores, options,
                                                             twoFalseStoreResult)
             .ok(),
         "two false-arm Store predicate semantics must verify independently");

  auto wrongPolarity = falseStoreResult;
  wrongPolarity.provenance.ifConversions.front().predicateComplemented = false;
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*falseStore, options, wrongPolarity)
              .ok(),
         "verifier must reject corrupted false-arm predicate polarity");

  expectStatus(kPredicatedLoad, "predicated_load",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::PredicatedLoadUnsupported);
  expectStatus(kMultipleBranches, "multiple_branches",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::MultipleInternalBranches);
  expectStatus(kUnsafeSpeculation, "unsafe_speculation",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsafeSpeculation);
  auto multipleStores = parse(kMultipleStores, context);
  options.functionName = "multiple_stores";
  const auto multipleStoreResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*multipleStores, options);
  expect(multipleStoreResult.ok(), "T018 must order multiple same-path predicated Stores");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*multipleStores, options,
                                                             multipleStoreResult)
             .ok(),
         "multiple Store memory/predicate semantics must verify");

  auto gepStore = parse(kGEPStore, context);
  options.functionName = "gep_store";
  const auto gepStoreResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*gepStore, options);
  expect(gepStoreResult.ok(), "T018 must lower a constant-offset predicated Store GEP");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*gepStore, options, gepStoreResult).ok(),
      "predicated GEP Store must pass memory/predicate verification");
  auto conditionalRecurrence = parse(kConditionalRecurrence, context);
  options.functionName = "conditional_recurrence";
  const auto conditionalRecurrenceResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*conditionalRecurrence, options);
  expect(conditionalRecurrenceResult.ok(),
         "canonical control-merge Select must provide a distance-one recurrence");
  expect(std::ranges::any_of(conditionalRecurrenceResult.dfg->edges(),
                             [&](const auto& edge) {
                               if (edge.kind() != cgra::ir::Edge::Kind::Data || edge.distance != 1)
                                 return false;
                               return conditionalRecurrenceResult.dfg->node(edge.src).opcode ==
                                      cgra::ir::Opcode::Select;
                             }),
         "conditional recurrence must use the merge Select as its distance-one producer");
  const auto conditionalRecurrenceVerification =
      cgra::frontend::llvm_frontend::verifyFrontendResult(*conditionalRecurrence, options,
                                                          conditionalRecurrenceResult);
  expect(conditionalRecurrenceVerification.ok(),
         ("independent verifier must validate conditional Select recurrence semantics: " +
          conditionalRecurrenceVerification.format())
             .c_str());

  auto sameAddressStores = parse(kSameAddressArmStores, context);
  options.functionName = "same_address_arm_stores";
  const auto sameAddressStoreResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*sameAddressStores, options);
  expect(sameAddressStoreResult.ok(),
         "same-address mutually exclusive Stores must normalize to Select plus Store");
  expect(sameAddressStoreResult.metadata &&
             sameAddressStoreResult.metadata->coalescedStorePairs == 1,
         "frontend metadata must disclose one coalesced Store pair");
  expect(std::ranges::count_if(
             sameAddressStoreResult.dfg->nodes(),
             [](const auto& node) { return node.opcode == cgra::ir::Opcode::Store; }) == 1 &&
             std::ranges::count_if(
                 sameAddressStoreResult.dfg->nodes(),
                 [](const auto& node) { return node.opcode == cgra::ir::Opcode::Select; }) == 1,
         "coalescing must emit exactly one semantic Store fed by one Select");
  const auto sameAddressStoreVerification = cgra::frontend::llvm_frontend::verifyFrontendResult(
      *sameAddressStores, options, sameAddressStoreResult);
  expect(sameAddressStoreVerification.ok(),
         ("normalized same-address Store semantics must verify independently: " +
          sameAddressStoreVerification.format())
             .c_str());

  auto redundantArmLoad = parse(kRedundantArmLoadStores, context);
  options.functionName = "redundant_arm_load_stores";
  const auto redundantArmLoadResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*redundantArmLoad, options);
  expect(redundantArmLoadResult.ok(),
         "a branch-local exact redundant Load must forward before Store coalescing");
  expect(redundantArmLoadResult.metadata &&
             redundantArmLoadResult.metadata->forwardedBranchLoads == 1 &&
             redundantArmLoadResult.metadata->coalescedStorePairs == 1,
         "frontend metadata must disclose Load forwarding and Store coalescing");
  expect(std::ranges::count_if(
             redundantArmLoadResult.dfg->nodes(),
             [](const auto& node) { return node.opcode == cgra::ir::Opcode::Load; }) == 1,
         "redundant branch Load must not survive in the Generic DFG");
  const auto redundantArmLoadVerification = cgra::frontend::llvm_frontend::verifyFrontendResult(
      *redundantArmLoad, options, redundantArmLoadResult);
  expect(redundantArmLoadVerification.ok(),
         ("independent verifier must validate branch Load forwarding: " +
          redundantArmLoadVerification.format())
             .c_str());
}

void testRecurrenceLowering() {
  llvm::LLVMContext context;
  cgra::frontend::llvm_frontend::LLVMFrontendOptions options;

  auto reduction = parse(kDataPhi, context);
  options.functionName = "reduction";
  const auto reductionResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*reduction, options);
  if (!reductionResult.ok()) {
    std::cerr << "reduction status="
              << cgra::frontend::llvm_frontend::toString(reductionResult.status)
              << " message=" << reductionResult.message << '\n';
    throw std::runtime_error("canonical reduction PHI must lower");
  }
  expect(reductionResult.dfg->nodes().size() == 1, "reduction PHI must not become a node");
  expect(reductionResult.dfg->edges().size() == 1, "reduction must have one recurrence edge");
  const auto& reductionEdge = reductionResult.dfg->edge(0);
  expect(reductionEdge.distance == 1 && reductionEdge.src == reductionEdge.dst,
         "reduction must be a self recurrence with distance one");
  const auto& reductionInfo = std::get<cgra::ir::DataEdgeInfo>(reductionEdge.info);
  expect(reductionInfo.dstOperand == 0 && reductionInfo.boundary.has_value(),
         "reduction recurrence must preserve the seed boundary");
  expect(
      std::holds_alternative<cgra::ir::ConstantRef>(reductionInfo.boundary->values.front().value),
      "constant reduction seed must be a ConstantRef");
  expect(reductionResult.provenance.recurrences.size() == 1,
         "reduction must emit one recurrence descriptor");
  const auto reductionVerification =
      cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, options, reductionResult);
  if (!reductionVerification.ok()) {
    std::cerr << reductionVerification.format() << '\n';
    throw std::runtime_error("reduction recurrence verifier");
  }

  auto induction = parse(kInductionDataUse, context);
  options.functionName = "iv_data";
  const auto inductionResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*induction, options);
  expect(inductionResult.ok(), "induction data recurrence must lower");
  expect(inductionResult.dfg->nodes().size() == 2,
         "induction update and data operation must be Generic nodes");
  expect(inductionResult.dfg->edges().size() == 2, "induction must retain both recurrence uses");
  for (const auto& edge : inductionResult.dfg->edges())
    expect(edge.distance == 1, "induction edges must have iteration distance one");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*induction, options, inductionResult)
             .ok(),
         "induction recurrence verifier");

  auto externalReduction = parse(kExternalDataPhi, context);
  options.functionName = "external_reduction";
  const auto externalResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*externalReduction, options);
  expect(externalResult.ok() && externalResult.dfg->externalValues().size() == 1,
         "external recurrence seed must be interned as one ExternalValue");
  const auto& externalInfo = std::get<cgra::ir::DataEdgeInfo>(externalResult.dfg->edge(0).info);
  expect(externalInfo.boundary && std::holds_alternative<cgra::ir::ExternalValueRef>(
                                      externalInfo.boundary->values.front().value),
         "external recurrence seed must remain an ExternalValueRef before ABI binding");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*externalReduction, options,
                                                             externalResult)
             .ok(),
         "external recurrence verifier");

  auto preheaderExternal = parse(kPreheaderExternal, context);
  options.functionName = "preheader_external";
  const auto preheaderResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*preheaderExternal, options);
  expect(preheaderResult.ok() && preheaderResult.dfg->externalValues().size() == 1,
         "preheader instruction seed must become one ExternalValue");
  expect(cgra::frontend::llvm_frontend::verifyFrontendResult(*preheaderExternal, options,
                                                             preheaderResult)
             .ok(),
         "preheader external recurrence verifier");

  auto repeated = parse(kRepeatedPhiUse, context);
  options.functionName = "repeated_phi";
  const auto repeatedResult = cgra::frontend::llvm_frontend::lowerInnermostLoop(*repeated, options);
  expect(repeatedResult.ok() && repeatedResult.dfg->edges().size() == 3,
         "repeated PHI use must preserve both operand edges and the next recurrence");
  expect(std::get<cgra::ir::DataEdgeInfo>(repeatedResult.dfg->edge(0).info).dstOperand == 0 &&
             std::get<cgra::ir::DataEdgeInfo>(repeatedResult.dfg->edge(1).info).dstOperand == 1,
         "same consumer PHI uses must retain distinct destination operands");

  auto distanceCorrupt = reductionResult;
  cgra::frontend::llvm_frontend::LLVMFrontendOptions reductionOptions;
  reductionOptions.functionName = "reduction";
  cgra::ir::DFGTestAccess::setEdgeDistance(*distanceCorrupt.dfg, 0, 0);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              distanceCorrupt)
              .ok(),
         "verifier must reject a recurrence edge with distance zero");

  auto operandCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::setEdgeOperand(*operandCorrupt.dfg, 0, 1);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              operandCorrupt)
              .ok(),
         "verifier must reject a recurrence edge with the wrong operand");

  auto boundaryCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::clearBoundary(*boundaryCorrupt.dfg, 0);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              boundaryCorrupt)
              .ok(),
         "verifier must reject a recurrence edge without its boundary");

  auto sourceCorrupt = reductionResult;
  sourceCorrupt.provenance.recurrences.front().backedge =
      static_cast<const llvm::Value*>(sourceCorrupt.provenance.recurrences.front().phiValue);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              sourceCorrupt)
              .ok(),
         "verifier must reject a recurrence with the wrong source provenance");

  auto boundaryProviderCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::setBoundaryConstant(*boundaryProviderCorrupt.dfg, 0, 999);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              boundaryProviderCorrupt)
              .ok(),
         "verifier must reject a recurrence with the wrong boundary provider");

  auto offsetCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::setBoundaryOffset(*offsetCorrupt.dfg, 0, 1);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              offsetCorrupt)
              .ok(),
         "verifier must reject a recurrence with the wrong iteration offset");

  auto extraEdge = reductionResult;
  cgra::ir::DFGTestAccess::appendDuplicateEdge(*extraEdge.dfg, 0);
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions, extraEdge)
           .ok(),
      "verifier must reject a spurious duplicate recurrence edge");

  auto wrongDestination = inductionResult;
  cgra::ir::DFGTestAccess::setEdgeDestination(*wrongDestination.dfg, 0, 1);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*induction, options, wrongDestination)
              .ok(),
         "verifier must reject a recurrence edge with the wrong destination");

  auto missingRepeatedUse = repeatedResult;
  cgra::ir::DFGTestAccess::eraseEdge(*missingRepeatedUse.dfg, 1);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions repeatedOptions;
  repeatedOptions.functionName = "repeated_phi";
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*repeated, repeatedOptions,
                                                              missingRepeatedUse)
              .ok(),
         "verifier must reject a missing recurrence edge for a repeated PHI use");

  auto descriptorEdgeCorrupt = reductionResult;
  descriptorEdgeCorrupt.provenance.recurrences.front().uses.front().edge = 999;
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, reductionOptions,
                                                              descriptorEdgeCorrupt)
              .ok(),
         "verifier must reject a descriptor referring to the wrong edge ID");
}

void testRecurrenceNegativeCorpus() {
  expectStatus(kThreeIncomingPhi, "three_incoming",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedRecurrenceShape);
  expectStatus(kPhiToPhi, "phi_to_phi",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedPhiToPhiUse);
  expectStatus(kPointerPhi, "pointer_phi",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedRecurrenceType);
  llvm::LLVMContext floatContext;
  auto floatModule = parse(kFloatPhi, floatContext);
  cgra::frontend::llvm_frontend::LLVMFrontendOptions floatOptions;
  floatOptions.functionName = "float_phi";
  const auto floatResult =
      cgra::frontend::llvm_frontend::lowerInnermostLoop(*floatModule, floatOptions);
  if (!floatResult.ok())
    std::cerr << "float recurrence status=" << static_cast<int>(floatResult.status)
              << " message=" << floatResult.message << '\n';
  expect(floatResult.ok(), "scalar float recurrence must lower target-independently");
  expect(
      cgra::frontend::llvm_frontend::verifyFrontendResult(*floatModule, floatOptions, floatResult)
          .ok(),
      "scalar float recurrence must pass independent verification");
  expectStatus(kVectorPhi, "vector_phi",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedRecurrenceType);
  expectStatus(kRawPhiLiveOut, "raw_phi_liveout",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedPhiLiveOutSemantics);
}

} // namespace

int main() {
  try {
    testScalarAddLoop();
    testFrontendInvocationValidation();
    testDeterministicSerialization();
    testVerifierRejectsCorruptedProvenance();
    testSupportedChainAndConstant();
    testBoundaryRejections();
    testRecurrenceLowering();
    testRecurrenceNegativeCorpus();
    testPredicationLowering();
    testLinearMultiBlockLowering();
    std::cout << "CGRA_LLVM_FRONTEND_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_LLVM_FRONTEND_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

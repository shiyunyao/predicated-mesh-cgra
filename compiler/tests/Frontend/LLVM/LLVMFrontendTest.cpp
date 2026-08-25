// SPDX-License-Identifier: MIT
#include "cgra/Frontend/LLVM/LLVMFrontend.h"
#include "cgra/Frontend/LLVM/LLVMFrontendVerifier.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
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
  static void clearBoundary(DFG& dfg, EdgeId edge) {
    std::get<DataEdgeInfo>(dfg.edges_[edge].info).boundary.reset();
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

void expect(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
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
  expectStatus(kMemory, "memory",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedMemoryOperation);
  expectStatus(kFloat, "float_loop",
               cgra::frontend::llvm_frontend::LLVMFrontendStatus::UnsupportedLLVMType);
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
  cgra::ir::DFGTestAccess::setEdgeDistance(*distanceCorrupt.dfg, 0, 0);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, options, distanceCorrupt)
              .ok(),
         "verifier must reject a recurrence edge with distance zero");

  auto operandCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::setEdgeOperand(*operandCorrupt.dfg, 0, 1);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, options, operandCorrupt)
              .ok(),
         "verifier must reject a recurrence edge with the wrong operand");

  auto boundaryCorrupt = reductionResult;
  cgra::ir::DFGTestAccess::clearBoundary(*boundaryCorrupt.dfg, 0);
  expect(!cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, options, boundaryCorrupt)
              .ok(),
         "verifier must reject a recurrence edge without its boundary");

  auto sourceCorrupt = reductionResult;
  sourceCorrupt.provenance.recurrences.front().backedge =
      static_cast<const llvm::Value*>(sourceCorrupt.provenance.recurrences.front().phiValue);
  expect(
      !cgra::frontend::llvm_frontend::verifyFrontendResult(*reduction, options, sourceCorrupt).ok(),
      "verifier must reject a recurrence with the wrong source provenance");
}

} // namespace

int main() {
  try {
    testScalarAddLoop();
    testDeterministicSerialization();
    testVerifierRejectsCorruptedProvenance();
    testSupportedChainAndConstant();
    testBoundaryRejections();
    testRecurrenceLowering();
    std::cout << "CGRA_LLVM_FRONTEND_TEST_PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "CGRA_LLVM_FRONTEND_TEST_FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

// SPDX-License-Identifier: MIT
#include "cgra/Target/TargetOperation.h"

#include <stdexcept>

namespace cgra {

std::string_view toString(TargetExecutionClass executionClass) {
  switch (executionClass) {
  case TargetExecutionClass::FU:
    return "FU";
  case TargetExecutionClass::LSU:
    return "LSU";
  }
  return "UNKNOWN";
}

TargetExecutionClass targetExecutionClassFromString(std::string_view value) {
  if (value == "FU")
    return TargetExecutionClass::FU;
  if (value == "LSU")
    return TargetExecutionClass::LSU;
  throw std::invalid_argument("unknown target execution class: " + std::string(value));
}

std::string_view toString(TargetOperandRole role) {
  switch (role) {
  case TargetOperandRole::Data:
    return "data";
  case TargetOperandRole::Predicate:
    return "predicate";
  case TargetOperandRole::Address:
    return "address";
  }
  return "unknown";
}

TargetOperandRole targetOperandRoleFromString(std::string_view value) {
  if (value == "data")
    return TargetOperandRole::Data;
  if (value == "predicate")
    return TargetOperandRole::Predicate;
  if (value == "address")
    return TargetOperandRole::Address;
  throw std::invalid_argument("unknown target operand role: " + std::string(value));
}

std::string_view toString(TargetResultRole role) {
  switch (role) {
  case TargetResultRole::Data:
    return "data";
  case TargetResultRole::Predicate:
    return "predicate";
  case TargetResultRole::Void:
    return "void";
  }
  return "unknown";
}

TargetResultRole targetResultRoleFromString(std::string_view value) {
  if (value == "data")
    return TargetResultRole::Data;
  if (value == "predicate")
    return TargetResultRole::Predicate;
  if (value == "void")
    return TargetResultRole::Void;
  throw std::invalid_argument("unknown target result role: " + std::string(value));
}

std::string_view toString(TargetControlSink sink) {
  switch (sink) {
  case TargetControlSink::FuDataA:
    return "FU_SRC_A";
  case TargetControlSink::FuDataB:
    return "FU_SRC_B";
  case TargetControlSink::FuPredicate0:
    return "FU_PRED_0";
  case TargetControlSink::FuPredicate1:
    return "FU_PRED_1";
  case TargetControlSink::LsuAddress:
    return "LSU_ADDR";
  case TargetControlSink::LsuStoreData:
    return "LSU_STORE_DATA";
  case TargetControlSink::LsuCommitPredicate:
    return "LSU_COMMIT_PRED";
  }
  return "UNKNOWN";
}

TargetControlSink targetControlSinkFromString(std::string_view value) {
  if (value == "FU_SRC_A" || value == "fu_data_a")
    return TargetControlSink::FuDataA;
  if (value == "FU_SRC_B" || value == "fu_data_b")
    return TargetControlSink::FuDataB;
  if (value == "FU_PRED_0" || value == "fu_pred_0")
    return TargetControlSink::FuPredicate0;
  if (value == "FU_PRED_1" || value == "fu_pred_1")
    return TargetControlSink::FuPredicate1;
  if (value == "LSU_ADDR" || value == "lsu_address")
    return TargetControlSink::LsuAddress;
  if (value == "LSU_STORE_DATA" || value == "lsu_store_data")
    return TargetControlSink::LsuStoreData;
  if (value == "LSU_COMMIT_PRED" || value == "lsu_commit_predicate")
    return TargetControlSink::LsuCommitPredicate;
  throw std::invalid_argument("unknown target control sink: " + std::string(value));
}

std::string_view toString(TargetResultSource source) {
  switch (source) {
  case TargetResultSource::None:
    return "NONE";
  case TargetResultSource::FuDataResult:
    return "FU_DATA_RESULT";
  case TargetResultSource::FuPredicateResult:
    return "FU_PRED_RESULT";
  case TargetResultSource::LsuLoadData:
    return "LSU_LOAD_DATA";
  }
  return "UNKNOWN";
}

TargetResultSource targetResultSourceFromString(std::string_view value) {
  if (value == "NONE" || value == "none")
    return TargetResultSource::None;
  if (value == "FU_DATA_RESULT" || value == "fu_data_result")
    return TargetResultSource::FuDataResult;
  if (value == "FU_PRED_RESULT" || value == "fu_pred_result")
    return TargetResultSource::FuPredicateResult;
  if (value == "LSU_LOAD_DATA" || value == "lsu_load_data")
    return TargetResultSource::LsuLoadData;
  throw std::invalid_argument("unknown target result source: " + std::string(value));
}

} // namespace cgra

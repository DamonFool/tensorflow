/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantOps.h"  // from @llvm-project  // IWYU pragma: keep
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project  // IWYU pragma: keep
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Support/TypeID.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "stablehlo/dialect/StablehloOps.h"  // from @stablehlo  // IWYU pragma: keep
#include "tensorflow/compiler/mlir/lite/quantization/ir/QuantOps.h"
#include "tensorflow/compiler/mlir/quantization/common/attrs_and_constraints.h"
#include "tensorflow/compiler/mlir/quantization/common/quantization_lib/quantization_config.h"
#include "tensorflow/compiler/mlir/quantization/common/quantization_lib/quantization_utils.h"
#include "tensorflow/compiler/mlir/quantization/stablehlo/passes/passes.h"
#include "tensorflow/compiler/mlir/quantization/stablehlo/passes/quantization_patterns.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"

namespace mlir::quant::stablehlo {

#define GEN_PASS_DEF_QUANTIZEPASS
#include "tensorflow/compiler/mlir/quantization/stablehlo/passes/passes.h.inc"

namespace {

// Base struct for quantization.
template <typename ConcreteT, typename RootOpT = quantfork::DequantizeCastOp>
struct StableHloQuantizationBase
    : public StableHloQuantizationPattern<ConcreteT, quantfork::QuantizeCastOp,
                                          quantfork::DequantizeCastOp,
                                          /*VerifierT=*/void, RootOpT> {
  explicit StableHloQuantizationBase(MLIRContext* ctx,
                                     const QuantPassSpec& quant_params)
      : StableHloQuantizationPattern<ConcreteT, quantfork::QuantizeCastOp,
                                     quantfork::DequantizeCastOp,
                                     /*VerifierT=*/void, RootOpT>(
            ctx, quant_params) {}

  static bool AllowHybridQuantization(Operation& op) { return false; }
};

// Quantization rewrite pattern using DQ as the root op.
struct StableHloQuantization
    : public StableHloQuantizationBase<StableHloQuantization> {
  explicit StableHloQuantization(MLIRContext* ctx,
                                 const QuantPassSpec& quant_params)
      : StableHloQuantizationBase<StableHloQuantization>(ctx, quant_params) {}
};

// Quantization rewrite pattern using Q as the root op. This is for the
// quantizable ops without floating-point operands.
struct StableHloQuantizationReverse
    : public StableHloQuantizationBase<StableHloQuantizationReverse,
                                       quantfork::QuantizeCastOp> {
  explicit StableHloQuantizationReverse(MLIRContext* ctx,
                                        const QuantPassSpec& quant_params)
      : StableHloQuantizationBase<StableHloQuantizationReverse,
                                  quantfork::QuantizeCastOp>(ctx,
                                                             quant_params) {}
};

// Quantization rewrite pattern using DQ as the root op.
struct StableHloQuantizationHybrid
    : public StableHloQuantizationBase<StableHloQuantizationHybrid> {
  explicit StableHloQuantizationHybrid(MLIRContext* ctx,
                                       const QuantPassSpec& quant_params)
      : StableHloQuantizationBase<StableHloQuantizationHybrid>(ctx,
                                                               quant_params) {}

  static bool AllowHybridQuantization(Operation& op) {
    auto call_op = cast<TF::XlaCallModuleOp>(op);
    return call_op && GetEntryFunctionName(call_op).contains("dot_general");
  }
};

class QuantizePass : public impl::QuantizePassBase<QuantizePass> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(QuantizePass)

  using impl::QuantizePassBase<QuantizePass>::QuantizePassBase;

  explicit QuantizePass(const bool enable_per_channel_quantized_weight,
                        const bool enable_weight_only,
                        const QuantizationSpecs& quant_specs) {
    enable_per_channel_quantized_weight_ = enable_per_channel_quantized_weight;
    enable_weight_only_ = enable_weight_only;
    quant_specs_ = quant_specs;
  }

 private:
  void runOnOperation() override;

  QuantizationSpecs quant_specs_;
};

void QuantizePass::runOnOperation() {
  ModuleOp module_op = getOperation();
  MLIRContext& ctx = getContext();

  NumericVerifySpec numeric_verify_spec;
  numeric_verify_spec.verify_numeric = quant_specs_.verify_numeric;
  numeric_verify_spec.whole_model_verify = quant_specs_.whole_model_verify;

  const QuantPassSpec quant_params = {std::move(numeric_verify_spec),
                                      quant_specs_};

  RewritePatternSet patterns(&ctx);
  patterns.add<StableHloQuantization, StableHloQuantizationReverse>(
      &ctx, quant_params);
  if (enable_weight_only_) {
    patterns.add<StableHloQuantizationHybrid>(&ctx, quant_params);
    PopulateQuantizeHybridPatterns(ctx, patterns);
  }

  PopulateQuantizeOpWithRegionPattern(ctx, patterns);
  PopulateFusedGemmStylePatterns(ctx, patterns,
                                 enable_per_channel_quantized_weight_);
  PopulateQuantizeSingularOpPatterns(ctx, patterns);

  if (failed(applyPatternsAndFoldGreedily(module_op, std::move(patterns)))) {
    // There are cases where no rewrites happen even if a pattern matches,
    // causing this to result in a convergence failure. Consider this as a
    // best-effort.
    module_op.emitWarning("Failed to converge pattern at QuantizePass.");
  }
}

}  // namespace

QuantizationSpecs DefaultQuantizationSpecs() {
  QuantizationSpecs quant_specs;
  quant_specs.inference_type = tensorflow::DT_QINT8;
  return quant_specs;
}

}  // namespace mlir::quant::stablehlo

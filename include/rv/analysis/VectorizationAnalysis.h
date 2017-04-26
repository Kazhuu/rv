//===- VectorizationAnalysis.h----------------*- C++ -*-===//
//
//                     The Region Vectorizer
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//

#ifndef RV_VECTORIZATIONANALYSIS_H_
#define RV_VECTORIZATIONANALYSIS_H_

#include <string>
#include <map>
#include <queue>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/raw_ostream.h"

#include "DFG.h"

#include "rv/vectorizationInfo.h"
#include "rv/vectorMapping.h"
#include "rv/VectorizationInfoProxyPass.h"
#include "rv/region/Region.h"
#include "rv/PlatformInfo.h"
#include "rv/analysis/BranchDependenceAnalysis.h"

namespace llvm {
  class LoopInfo;
}

namespace rv {

class VAWrapperPass : public llvm::FunctionPass {
  static char ID;
public:
  VAWrapperPass() : FunctionPass(ID) { }
  VAWrapperPass(const VAWrapperPass&) = delete;
  VAWrapperPass& operator=(VAWrapperPass) = delete;

  void getAnalysisUsage(AnalysisUsage& Info) const override;
  bool runOnFunction(Function& F) override;

  // void print(llvm::raw_ostream& O, const Module* M) const override;
};

class VectorizationAnalysis {
public:
  using ValueMap          = std::map<const Value*, VectorShape>;
  using InstructionSet    = llvm::SmallPtrSet<const Instruction*, 32>;

  VectorizationAnalysis(PlatformInfo & platInfo,
      VectorizationInfo& VecInfo,
      const CDG& cdg,
      const DFG& dfg,
      const LoopInfo& LoopInfo,
      const DominatorTree & domTree, const PostDominatorTree & postDomTree);

  VectorizationAnalysis(const VectorizationAnalysis&) = delete;
  VectorizationAnalysis& operator=(VectorizationAnalysis) = delete;

  void analyze(Function& F);

  //---------------------- Map access -------------------------//
  /// Get the shape for a value
  //  if loop carried, this is the shape observed within the loop that defines @V
  VectorShape getShape(const Value* const V);

  //---------------------- Iterators --------------------------//
  typename ValueMap::iterator begin();
  typename ValueMap::iterator end();
  typename ValueMap::const_iterator begin() const;
  typename ValueMap::const_iterator end() const;

private:
  std::set<const Value*> overrides;
  const DataLayout& layout;

  VectorizationInfo& mVecinfo;  // This will be the output
  const CDG& mCDG;      // Preserves CDG
  const DFG& mDFG;      // Preserves DFG
  BranchDependenceAnalysis BDA;
  const LoopInfo& mLoopInfo; // Preserves LoopInfo
  const VectorFuncMap& mFuncinfo;

  Region* mRegion;

  ValueMap mValue2Shape;    // Computed shapes
  std::queue<const Instruction*> mWorklist;       // Next instructions to handle

  // VectorShape analysis logic

  // Initialize all statically known shapes (constants, arguments via argument mapping,
  // shapes set by the user)
  void init(Function& F);

  // Run Fix-Point-Iteration after initialization
  void compute(Function& F);

  // Returns true if this block is contained in the region we want to analyze
  bool isInRegion(const BasicBlock& BB);
  bool isInRegion(const Instruction& inst);

// specialized transfer functions
  VectorShape computePHIShape(const PHINode & phi);
  // only call these if all operands have defined shape
  VectorShape computeShapeForInst(const Instruction* I);
  VectorShape computeShapeForBinaryInst(const BinaryOperator* I);
  VectorShape computeShapeForCastInst(const CastInst* I);

  // generic (fallback) transfer function for instructions w/o side effects
  VectorShape computeGenericArithmeticTransfer(const Instruction & I);

  // Update a value with its new computed shape, recursing into users if it has changed
  void update(const Value* const V, VectorShape AT);

  void updateShape(const Value* const V, VectorShape AT);
  void analyzeDivergence(const BranchInst* const branch);

  // Calls update on every user of this PHI that is not in its loop
  // void updateOutsideLoopUsesVarying(const PHINode* PHI, const Loop* PHILoop);
  void updateOutsideLoopUsesVarying(const Loop* divLoop);

  // Adds all users of V to the worklist to continue iterating,
  // unless the concept of shape is not defined for the user (e.g. void return calls)
  void addRelevantUsersToWL(const Value* V);

  // Corrects the shapes for any alloca operand to continous/varying
  // and recomputes all shapes dependent on them from scratch
  // IMPORTANT: The result is as if the respective alloca had been
  // initialized continous/varying, the dependent values have
  // their shapes reset to bottom before recomputation
  void updateAllocaOperands(const Instruction* I);

  // Resets the shape of this value and every value in the user graph to bottom
  void eraseUserInfoRecursively(const Value* V);

  bool allExitsUniform(const Loop* loop);

  VectorShape joinOperands(const Instruction& I);

  // Returns true iff all operands currently have a computed shape
  // This is essentially a negated check for bottom
  bool pushMissingOperands(const Instruction* I);

  // Returns true iff the constant is aligned respective to mVectorizationFactor
  unsigned getAlignment(const Constant* c) const;

  // Transfers the computed VectorShapes from mvalues to the VectorizationInfo object
  // TODO just write into mVecinfo immediately?
  void fillVectorizationInfo(Function& F);
};

FunctionPass* createVectorizationAnalysisPass();

}

#endif

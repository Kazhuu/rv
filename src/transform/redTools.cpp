#include "rv/transform/redTools.h"

using namespace llvm;

namespace rv {

static
Instruction&
CreateMinMax(IRBuilder<> & builder, Value & A, Value & B, bool createMin, bool isSigned) {
  auto * aTy = A.getType();
  bool isFloat = aTy->isFPOrFPVectorTy();

  Value * cmpInst;
  if (isFloat) {
    cmpInst = builder.CreateFCmpOGT(&A, &B);
  } else {
    cmpInst = isSigned ? builder.CreateICmpSGT(&A, &B) : builder.CreateICmpUGT(&A, &B);
  }
  return *cast<Instruction>(builder.CreateSelect(cmpInst, createMin ? &B : &A, createMin ? &A : &B));
}

// materialize a single instance of firstArg [[RedKind~OpCode]] secondArg
Instruction&
CreateReductInst(IRBuilder<> & builder, RedKind redKind, Value & firstArg, Value & secondArg) {
  auto * argTy = firstArg.getType();
  bool isFloat = argTy->isFPOrFPVectorTy();

  switch (redKind) {
    case RedKind::Add:
      if (isFloat) {
        return *cast<Instruction>(builder.CreateFAdd(&firstArg, &secondArg, secondArg.getName() + ".r"));
      } else {
        return *cast<Instruction>(builder.CreateAdd(&firstArg, &secondArg, secondArg.getName() + ".r"));
      }

    case RedKind::Or:
        return *cast<Instruction>(builder.CreateOr(&firstArg, &secondArg, secondArg.getName() + ".r"));
    case RedKind::And:
        return *cast<Instruction>(builder.CreateAnd(&firstArg, &secondArg, secondArg.getName() + ".r"));

    case RedKind::Mul:
      if (isFloat) {
        return *cast<Instruction>(builder.CreateFMul(&firstArg, &secondArg, secondArg.getName() + ".r"));
      } else {
        return *cast<Instruction>(builder.CreateMul(&firstArg, &secondArg, secondArg.getName() + ".r"));
      }

    case RedKind::UMax:
    case RedKind::SMax:
      return CreateMinMax(builder, firstArg, secondArg, false, redKind == RedKind::SMax);

    case RedKind::UMin:
    case RedKind::SMin:
      return CreateMinMax(builder, firstArg, secondArg, true, redKind == RedKind::SMin);

    default:
      abort(); // unsupported reduction
  }
}

static
bool
IsPower2(int x) {
  return x && !(x & (x - 1));
}

static Type&
GetScalarType(Value & val) {
  auto * valTy = val.getType();
  if (valTy->isVectorTy()) return *valTy->getVectorElementType();
  else return *valTy;
}

static
Intrinsic::ID
GetIntrinsicID(RedKind kind, Type & elemTy) {
  switch (kind) {
    default:
      return Intrinsic::not_intrinsic;
    case RedKind::Add: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fadd : Intrinsic::experimental_vector_reduce_add;
    case RedKind::And: return Intrinsic::experimental_vector_reduce_and;
    case RedKind::Or: return Intrinsic::experimental_vector_reduce_or;
    case RedKind::Mul: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fmul : Intrinsic::experimental_vector_reduce_mul;
    case RedKind::SMax: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fmax : Intrinsic::experimental_vector_reduce_smax;
    case RedKind::UMax: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fmax : Intrinsic::experimental_vector_reduce_umax;
    case RedKind::SMin: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fmin : Intrinsic::experimental_vector_reduce_smin;
    case RedKind::UMin: return elemTy.isFloatingPointTy() ? Intrinsic::experimental_vector_reduce_fmin : Intrinsic::experimental_vector_reduce_umin;
  }
}

// reduce the vector @vectorVal to a scalar value (using redKind)
Value &
CreateVectorReduce(IRBuilder<> & builder, RedKind redKind, Value & vecVal, Value * initVal) {
  auto & elemTy = *vecVal.getType()->getVectorElementType();

// use LLVM's experimental intrinsics where possible
  Intrinsic::ID ID = GetIntrinsicID(redKind, elemTy);
  if (ID != Intrinsic::not_intrinsic) {
    auto & mod = *builder.GetInsertBlock()->getParent()->getParent();
    auto & vecTy = *vecVal.getType();
    auto & redFunc = *Intrinsic::getDeclaration(&mod, ID, {vecTy.getVectorElementType(), &vecTy});
    auto & redVal = *builder.CreateCall(&redFunc, &vecVal, "red" + to_string(redKind));

    // add init val (if applicable)
    if (initVal && initVal != &GetNeutralElement(redKind, elemTy)) {
      return CreateReductInst(builder, redKind, redVal, *initVal);
    }

    return redVal;
  }

// Otw, use fallback code path
  auto * intTy = Type::getInt32Ty(builder.getContext());
  uint32_t vecWidth = vecVal.getType()->getVectorNumElements();
  if (IsPower2(vecWidth)) {
    auto * accu = &vecVal;
    for (size_t range = vecWidth / 2; range >= 1; range /= 2) {
      // create a permutation vector
      std::vector<Constant*> shuffleVec;
      shuffleVec.reserve(vecWidth);

      // 4 5 6 7 * * * *
      // 2 3 * * * * * *
      // 1 * * * * * * *
      for (size_t i = 0; i < range; ++i) {
        shuffleVec.push_back(ConstantInt::getSigned(intTy, range + i));
      }
      // fill up with undef elements
      while (shuffleVec.size() < vecWidth) shuffleVec.push_back( UndefValue::get(intTy) );

      // fold
      auto * mask = ConstantVector::get(shuffleVec);
      auto * folded = builder.CreateShuffleVector(accu, UndefValue::get(vecVal.getType()), mask, "fold");

      // Create reduction
      accu = &CreateReductInst(builder, redKind, *accu, *folded);
    }

    Value * reducedVec = builder.CreateExtractElement(accu, ConstantInt::getNullValue(intTy), "reduce_last");

    if (initVal && initVal != &GetNeutralElement(redKind, *reducedVec->getType())) {
      return CreateReductInst(builder, redKind, *reducedVec, *initVal);
    } else {
      return *reducedVec;
    }

  } else {
    // create a scalar reduction chain
    Value * accu = initVal ? initVal : &GetNeutralElement(redKind, GetScalarType(vecVal));

    for (size_t i = 0; i < vecWidth; ++i) {
      auto * laneVal = builder.CreateExtractElement(&vecVal, i, "red_ext");
      accu = &CreateReductInst(builder, redKind, *accu, *laneVal);
    }

    return *accu;
  }
}

Value &
CreateExtract(IRBuilder<> & builder, Value & vecVal, int laneOffset) {
  auto * vecTy = dyn_cast<VectorType>(vecVal.getType());
  if (!vecTy) {
    return vecVal; //uniform value
  }

  const int vectorWidth = vecTy->getNumElements();
  int laneIdx = laneOffset >= 0 ? laneOffset : vectorWidth + laneOffset;
  assert(laneIdx >= 0 && laneIdx < vectorWidth);

  return *builder.CreateExtractElement(&vecVal, laneIdx, vecVal.getName().str() + ".ex." + std::to_string(laneIdx));
}


}

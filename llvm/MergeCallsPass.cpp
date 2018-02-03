#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Utils/Local.h"
#include <map>
#include <vector>

using namespace llvm;

namespace {
struct MergeCalls : public FunctionPass {
  static char ID;
  MergeCalls() : FunctionPass(ID) {}

  bool valueEscapes(const Instruction *Inst) const {
    const BasicBlock *BB = Inst->getParent();
    for (const User *U : Inst->users()) {
      const Instruction *UI = cast<Instruction>(U);
      if (UI->getParent() != BB || isa<PHINode>(UI))
        return true;
     }
     return false;
  }

  bool runOnFunction(Function &F) override {
    std::map<Function*, std::vector<CallInst*>> funcToInvokers;

    for(BasicBlock &BB : F) {
        for(Instruction &I : BB) {
           if(I.getOpcode() == Instruction::Call) {
                CallInst& C = cast<CallInst>(I);
                if(C.isInlineAsm()) {
                    // This is inline assembly; this can be deduplicated by a
                    // different pass if necessary. It doesn't call anything.
                    continue;
                }
                if(C.getCalledFunction() == nullptr) {
                    // Indirect invocation (call-by-ptr). Skip for now.
                    continue;
                }
                if(C.getCalledFunction()->isIntrinsic()) {
                    // LLVM intrinsic - don't tamper with this!
                    continue;
                }

                funcToInvokers[C.getCalledFunction()].push_back(&C);
            }
        }
    }

    for(auto &[target, callers] : funcToInvokers) {
        if(callers.size() > 1) {
            std::vector<Value*> callArgs;
            std::map<BasicBlock*, BasicBlock*> callerToRet;
            std::map<Instruction*, BasicBlock*> callerToOrigParent;
            BasicBlock* callBlock = BasicBlock::Create(F.getContext(), "", &F, nullptr);

            // alloca insertion point tracing logic taken verbatim off reg2mem pass.
            BasicBlock* BBEntry = &F.getEntryBlock();
            BasicBlock::iterator I = BBEntry->begin();
            while (isa<AllocaInst>(I)) ++I;

            CastInst *AllocaInsertionPoint = new BitCastInst(Constant::getNullValue(Type::getInt32Ty(F.getContext())), Type::getInt32Ty(F.getContext()), "mergecalls alloca point", &*I);

            for(CallInst* caller : callers) {
                std::vector<Instruction*> toDemote;
                BasicBlock* parentBlock = caller->getParent();
                BasicBlock* returnBlock = parentBlock->splitBasicBlock(caller->getNextNode(), "");
                callerToOrigParent[caller] = parentBlock;
                callerToRet[parentBlock] = returnBlock;

                // We actually need the vector for this:
                // The iterator gets invalidated during demotion.
                for(Instruction &I : *parentBlock) {
                    if (!(isa<AllocaInst>(I) && I.getParent() == BBEntry) && valueEscapes(&I))
                        toDemote.push_back(&I);
                }

                for(Instruction* demotedInstr : toDemote) {
                    DemoteRegToStack(*demotedInstr, false, AllocaInsertionPoint);
                }

                // Move the call instruction to the beginning of the return block (before the first non-PHI instruction).
                caller->moveBefore(returnBlock->getFirstNonPHI());

                // Demote the call instruction as well if it has any users.
                for(User* U : caller->users()) {
                    DemoteRegToStack(cast<Instruction>(*caller), false, AllocaInsertionPoint);
                    break;
                }

                // Generate a branch to our call block and get rid of the branch generated by splitBasicBlock.
                BranchInst* ourBranch = BranchInst::Create(callBlock, parentBlock);
                ourBranch->getPrevNode()->eraseFromParent();
            }

            if(target->arg_size() > 0) {
                int argCtr = 0;
                for(Argument &A : target->args()) {
                    // We have to create a PHI node for each incoming basic block/value pair.
                    PHINode* argNode = PHINode::Create(A.getType(), callers.size(), "", callBlock);
                    for(CallInst* caller : callers) {
                        argNode->addIncoming(caller->getArgOperand(argCtr), callerToOrigParent[caller]);
                    }

                    callArgs.push_back(cast<Value>(argNode));
                    ++argCtr;
                }
            }

            CallInst* callInstr = CallInst::Create(cast<Value>(target), ArrayRef<Value*>(callArgs), "", callBlock);

            for(CallInst* caller : callers) {
                // Get rid of the original call, replace all references to it with the call in our call block.
                caller->replaceAllUsesWith(callInstr);
                caller->eraseFromParent();
            }

            // Emit PHI/switch instructions for branching back to the return blocks:
            PHINode* whereFromNode = PHINode::Create(Type::getInt32Ty(F.getContext()), callers.size(), "", callInstr); 
            SwitchInst* switchBackInstr = SwitchInst::Create(whereFromNode, callerToRet.begin()->second, callerToRet.size(), callBlock);
            int switchCtr = 0;

            for(auto &[parent, ret] : callerToRet) {
                llvm::ConstantInt* branchIdx = llvm::ConstantInt::get(F.getContext(), llvm::APInt(32, switchCtr, true));
                whereFromNode->addIncoming(branchIdx, parent);
                switchBackInstr->addCase(branchIdx, ret);
                ++switchCtr;
            }
        }
    }

    return false;
  }
}; // end of struct MergeCalls
}  // end of anonymous namespace

char MergeCalls::ID = 0;
static RegisterPass<MergeCalls> X("mergecalls", "Merge Calls Pass",
                                  false /* Only looks at CFG */,
                                  false /* Analysis Pass */);
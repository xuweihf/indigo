#include "instruction_schedule.hpp"

namespace backend::instruction_schedule {

const std::string WrongInstExceptionMsg =
    "non-supported arm instrution for instruction schedule";

InstKind getInstKind(arm::Inst* inst) {
  switch (inst->op) {
    case arm::OpCode::B: {
      return InstKind::Branch;
      break;
    }
    case arm::OpCode::Bl: {
      return InstKind::Call;
      break;
    }
    case arm::OpCode::Mov:
    case arm::OpCode::MovT:
    case arm::OpCode::Mvn:
    case arm::OpCode::Lsl:
    case arm::OpCode::Lsr:
    case arm::OpCode::Asr: {
      return InstKind::Integer;
      break;
    }
    case arm::OpCode::Add:
    case arm::OpCode::Sub:
    case arm::OpCode::And:
    case arm::OpCode::Orr:
    case arm::OpCode::Eor:
    case arm::OpCode::Bic: {
      if (shiftByImmed(((arm::Arith3Inst*)inst)->r2)) {
        return InstKind::IntegerM;
      } else {
        return InstKind::Integer;
      }
      break;
    }
    case arm::OpCode::Mul:
    case arm::OpCode::SMMul: {
      return InstKind::IntegerM;
      break;
    }
    case arm::OpCode::Cmp:
    case arm::OpCode::Cmn: {
      if (shiftByImmed(((arm::Arith2Inst*)inst)->r2)) {
        return InstKind::IntegerM;
      } else {
        return InstKind::Integer;
      }
      break;
    }
    case arm::OpCode::LdR: {
      return InstKind::Load;
      break;
    }
    case arm::OpCode::StR: {
      return InstKind::Store;
      break;
    }
    default: {
      throw std::exception(WrongInstExceptionMsg.c_str());
    }
  }
};

uint32_t getInstExeLatency(arm::Inst* inst) {
  switch (inst->op) {
    case arm::OpCode::B: {
      return 1;
      break;
    }
    case arm::OpCode::Bl: {
      return 1;
      break;
    }
    case arm::OpCode::Mov:
    case arm::OpCode::MovT:
    case arm::OpCode::Mvn:
    case arm::OpCode::Lsl:
    case arm::OpCode::Lsr:
    case arm::OpCode::Asr: {
      return 1;
      break;
    }
    case arm::OpCode::Add:
    case arm::OpCode::Sub:
    case arm::OpCode::And:
    case arm::OpCode::Orr:
    case arm::OpCode::Eor:
    case arm::OpCode::Bic: {
      if (shiftByImmed(((arm::Arith3Inst*)inst)->r2)) {
        return 2;
      } else {
        return 1;
      }
      break;
    }
    case arm::OpCode::Mul:
    case arm::OpCode::SMMul: {
      return 3;
      break;
    }
    case arm::OpCode::Cmp:
    case arm::OpCode::Cmn: {
      if (shiftByImmed(((arm::Arith2Inst*)inst)->r2)) {
        return 2;
      } else {
        return 1;
      }
      break;
    }
    case arm::OpCode::LdR: {
      return 4;
      break;
    }
    case arm::OpCode::StR: {
      return 1;
      break;
    }
    default: {
      throw std::exception(WrongInstExceptionMsg.c_str());
    }
  }
};

bool shiftByImmed(arm::Operand2& r2) {
  return (std::holds_alternative<arm::RegisterOperand>(r2) &&
          (std::get<arm::RegisterOperand>(r2).shift !=
               arm::RegisterShiftKind::Lsl ||
           std::get<arm::RegisterOperand>(r2).shift_amount != 0));
};

void InstructionScheduler::scheduleBaseBlock(
    std::vector<arm::Inst*>& blockInsts,
    std::vector<std::unique_ptr<arm::Inst>>& newInsts) {
  buildDependencyDAG(blockInsts);
};

void InstructionScheduler::buildDependencyDAG(
    std::vector<arm::Inst*>& blockInsts) {
  for (size_t i = 0; i < blockInsts.size(); i++) {
    arm::Inst* inst;
    std::shared_ptr<DependencyDAGNode> node;

    inst = blockInsts.at(i);
    node = std::make_shared<DependencyDAGNode>();

    node->originIndex = i;
    node->inst = inst;
    node->instKind = getInstKind(inst);
    node->latency = getInstExeLatency(inst);

    nodes[i] = node;
    inDegrees[i] = 0;

    switch (inst->op) {
      case arm::OpCode::B: {
        for (size_t j = 0; j < i; i++) {
          addSuccessor(j, i);
        }

        break;
      }
      case arm::OpCode::Bl: {
        uint32_t paraNum;

        paraNum = 0;
        for (uint32_t bInstNum = 0; i - 1 - bInstNum >= 0; bInstNum++) {
          if (i - 1 - bInstNum == lastCall) {
            break;
          }
          auto& bInst = blockInsts.at(i - 1 - bInstNum);
          if (paraNum <= 3) {
            if (bInst->op == arm::OpCode::Mov &&
                ((arm::Arith2Inst*)bInst)->r1 == paraNum) {
              addSuccessor(i - 1 - bInstNum, i);
              paraNum++;
              regDefNodes[((arm::Arith2Inst*)bInst)->r1] = i;
            }
            // TODO: deal the movt pass param bug
            else if (bInst->op == arm::OpCode::MovT &&
                     ((arm::Arith2Inst*)bInst)->r1 == paraNum) {
              addSuccessor(i - 1 - bInstNum, i);
            } else {
              break;
            }
          } else {
            if (bInst->op == arm::OpCode::StR) {
              arm::LoadStoreInst* storeInst = (arm::LoadStoreInst*)inst;
              if (std::holds_alternative<arm::MemoryOperand>(storeInst->mem) &&
                  std::get<arm::MemoryOperand>(storeInst->mem).r1 ==
                      arm::REG_SP) {
                arm::MemoryOperand& memOpd =
                    std::get<arm::MemoryOperand>(storeInst->mem);
                if (std::holds_alternative<int16_t>(memOpd.offset) &&
                    std::get<int16_t>(memOpd.offset) == (paraNum - 4) * 4) {
                  addSuccessor(i - 1 - bInstNum, i);
                  paraNum++;
                } else {
                  break;
                }
              } else {
                break;
              }
            } else {
              break;
            }
          }
        }

        if (lastMem != NullIndex) {
          addSuccessor(lastMem, i);
        }
        if (lastCall != NullIndex) {
          addSuccessor(lastCall, i);
        }
        lastMem = i;
        lastCall = i;

        break;
      }
      case arm::OpCode::Mov:
      case arm::OpCode::Mvn: {
        arm::Arith2Inst* movInst = (arm::Arith2Inst*)inst;

        if (movInst->cond != arm::ConditionCode::Always) {
          if (lastCmp != NullIndex) {
            addSuccessor(lastCmp, i);
          }
          lastCmp = i;
        }

        addRegReadDependency(i, movInst->r2);
        regDefNodes[movInst->r1] = i;
        break;
      }
      case arm::OpCode::MovT: {
        arm::Arith2Inst* movtInst = (arm::Arith2Inst*)inst;

        regDefNodes[movtInst->r1] = i;
        break;
      }
      case arm::OpCode::Lsl:
      case arm::OpCode::Lsr:
      case arm::OpCode::Asr:
      case arm::OpCode::Add:
      case arm::OpCode::Sub:
      case arm::OpCode::And:
      case arm::OpCode::Orr:
      case arm::OpCode::Eor:
      case arm::OpCode::Bic:
      case arm::OpCode::Mul:
      case arm::OpCode::SMMul: {
        arm::Arith3Inst* aluInst = (arm::Arith3Inst*)inst;
        if (aluInst->rd == arm::REG_SP && (aluInst->op == arm::OpCode::Add ||
                                           aluInst->op == arm::OpCode::Sub)) {
          if (lastCall != NullIndex) {
            addSuccessor(lastCall, i);
          }
          lastCall = i;
        }

        addRegReadDependency(i, aluInst->r1);
        addRegReadDependency(i, aluInst->r2);
        regDefNodes[aluInst->rd] = i;

        break;
      }
      case arm::OpCode::Cmp:
      case arm::OpCode::Cmn: {
        arm::Arith2Inst* cmpInst = (arm::Arith2Inst*)inst;

        if (lastCmp != NullIndex) {
          addSuccessor(lastCmp, i);
        }
        lastCmp = i;

        addRegReadDependency(i, cmpInst->r1);
        addRegReadDependency(i, cmpInst->r2);

        break;
      }
      case arm::OpCode::LdR: {
        arm::LoadStoreInst* ldInst = (arm::LoadStoreInst*)inst;

        if (lastMem != NullIndex) {
          addSuccessor(lastMem, i);
        }
        lastMem = i;

        if (std::holds_alternative<arm::MemoryOperand>(ldInst->mem)) {
          addRegReadDependency(i, std::get<arm::MemoryOperand>(ldInst->mem));
        }
        regDefNodes[ldInst->rd] = i;

        break;
      }
      case arm::OpCode::StR: {
        arm::LoadStoreInst* stInst = (arm::LoadStoreInst*)inst;

        if (lastMem != NullIndex) {
          addSuccessor(lastMem, i);
        }
        lastMem = i;

        if (std::holds_alternative<arm::MemoryOperand>(stInst->mem)) {
          addRegReadDependency(i, std::get<arm::MemoryOperand>(stInst->mem));
        }
        addRegReadDependency(i, stInst->rd);

        break;
      }
      default: {
        throw std::exception(WrongInstExceptionMsg.c_str());
      }
    }
  }
};

void InstructionScheduler::addSuccessor(uint32_t father, uint32_t successor) {
  nodes[father]->successors.insert(nodes[successor]);
  inDegrees[successor]++;
}

void InstructionScheduler::addRegReadDependency(uint32_t successor,
                                                arm::Operand2& operand2) {
  if (std::holds_alternative<arm::RegisterOperand>(operand2)) {
    arm::Reg& r2 = std::get<arm::RegisterOperand>(operand2).reg;

    addRegReadDependency(successor, r2);
  }
};

void InstructionScheduler::addRegReadDependency(uint32_t successor,
                                                arm::Reg& reg) {
  if (regDefNodes.count(reg) > 0) {
    addSuccessor(regDefNodes[reg], successor);
  }
};

void InstructionScheduler::addRegReadDependency(uint32_t successor,
                                                arm::MemoryOperand& mem) {
  addRegReadDependency(successor, mem.r1);
  if (std::holds_alternative<arm::RegisterOperand>(mem.offset)) {
    arm::Reg& reg = std::get<arm::RegisterOperand>(mem.offset).reg;

    addRegReadDependency(successor, reg);
  }
};

};  // namespace backend::instruction_schedule

namespace backend::codegen {
void InstructionSchedule::optimize_arm(
    arm::ArmCode& armCode, std::map<std::string, std::any>& extraDataRepo){};

void InstructionSchedule::optimize_func(
    arm::Function& f, std::map<std::string, std::any>& extraDataRepo){};
};  // namespace backend::codegen
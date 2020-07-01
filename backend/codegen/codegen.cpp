#include "codegen.hpp"

#include <cassert>
#include <exception>
#include <memory>
#include <optional>
#include <typeinfo>

#include "err.hpp"

namespace backend::codegen {

using namespace arm;

arm::Function Codegen::translate_function() {
  init_reg_map();
  scan_stack();
  scan();
  generate_startup();
  for (auto& bb : func.basic_blks) {
    translate_basic_block(bb.second);
  }
  generate_return_and_cleanup();

  {
    // Put variable to vreg map in extra data
    auto data =
        extra_data.find(optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME);
    if (data == extra_data.end()) {
      extra_data.insert({optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME,
                         optimization::MirVariableToArmVRegType()});
      data = extra_data.find(optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME);
    }
    auto map =
        std::any_cast<optimization::MirVariableToArmVRegType&>(data->second);
    map.insert({func.name, std::move(this->reg_map)});
  }

  return arm::Function{func.name, std::move(this->inst),
                       std::move(this->consts), stack_size};
}

void Codegen::translate_basic_block(mir::inst::BasicBlk& blk) {
  inst.push_back(
      std::make_unique<LabelInst>(format_bb_label(func.name, blk.id)));
  for (auto& inst : blk.inst) {
    auto& i = *inst;
    if (auto x = dynamic_cast<mir::inst::OpInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::CallInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::AssignInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::LoadInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::StoreInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::RefInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::PhiInst*>(&i)) {
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::PtrOffsetInst*>(&i)) {
      translate_inst(*x);
    } else {
      throw new std::bad_cast();
    }
  }
}

void Codegen::init_reg_map() {
  reg_map.insert({mir::inst::VarId(0), Reg(0)});
  for (int i = 0; i < 4 && i < param_size; i++) {
    reg_map.insert({mir::inst::VarId(i + 1), Reg(i)});
  }
}

void Codegen::generate_return_and_cleanup() {
  inst.push_back(std::make_unique<LabelInst>(format_fn_end_label(func.name)));
  // NOTE: Register counting is done AFTER register allocation, so there's not
  // much cleanup here. Insert register counting after end label.
  inst.push_back(std::make_unique<Arith2Inst>(OpCode::Mov, REG_SP, REG_FP));
  inst.push_back(
      std::make_unique<PushPopInst>(OpCode::Pop, std::vector{REG_FP, REG_PC}));
  // ^ this final pop sets PC and returns the function
}

void Codegen::generate_startup() {
  inst.push_back(
      std::make_unique<PushPopInst>(OpCode::Push, std::vector{REG_FP, REG_LR}));
  // TODO: Push all used register to save them, probably after register alloc
  inst.push_back(std::make_unique<Arith2Inst>(OpCode::Mov, REG_FP, REG_SP));
  // TODO: Expand stack here; we haven't allocated the stack for now!
}

arm::Reg Codegen::get_or_alloc_vgp(mir::inst::VarId v_) {
  auto v = get_collapsed_var(v_);
  // If it's param, load before use
  if (v >= 4 && v <= param_size) {
    auto reg = alloc_vgp();
    inst.push_back(std::make_unique<LoadStoreInst>(
        OpCode::LdR, reg, MemoryOperand(REG_FP, (int16_t)(-(v - 4) * 4))));
    return reg;
  } else {
    auto x = stack_space_allocation.find(v);
    if (x != stack_space_allocation.end()) {
      auto reg = alloc_vgp();
      inst.push_back(std::make_unique<LoadStoreInst>(
          OpCode::LdR, reg, MemoryOperand(REG_SP, (int16_t)(-x->second))));
      return reg;
    } else {
      auto found = reg_map.find(v);
      if (found != reg_map.end()) {
        assert(arm::register_type(found->second) ==
               arm::RegisterKind::VirtualGeneralPurpose);
        return found->second;
      } else {
        return alloc_vgp();
      }
    }
  }
}

arm::Reg Codegen::get_or_alloc_vd(mir::inst::VarId v_) {
  auto v = get_collapsed_var(v_);
  auto found = reg_map.find(v);
  if (found != reg_map.end()) {
    assert(arm::register_type(found->second) ==
           arm::RegisterKind::VirtualDoubleVector);
    return found->second;
  } else {
    return alloc_vd();
  }
}

arm::Reg Codegen::get_or_alloc_vq(mir::inst::VarId v_) {
  auto v = get_collapsed_var(v_);
  auto found = reg_map.find(v);
  if (found != reg_map.end()) {
    assert(arm::register_type(found->second) ==
           arm::RegisterKind::VirtualQuadVector);
    return found->second;
  } else {
    return alloc_vq();
  }
}

mir::inst::VarId Codegen::get_collapsed_var(mir::inst::VarId i) {
  auto res = this->var_collapse.find(i);
  if (res != var_collapse.end()) {
    while (true) {
      auto res_ = this->var_collapse.find(res->second);
      if (res_ == var_collapse.end() || res == res_) {
        return res->second;
      }
    }
  } else {
    return i;
  }
}

void Codegen::scan() {
  for (auto& bb : func.basic_blks) {
    for (auto& inst : bb.second.inst) {
      if (auto x = dynamic_cast<mir::inst::PhiInst*>(&*inst)) {
        deal_phi(*x);
      } else if (auto x = dynamic_cast<mir::inst::CallInst*>(&*inst)) {
        deal_call(*x);
      }
    }
  }
}

void deal_call(mir::inst::CallInst& call) {}

void Codegen::deal_phi(mir::inst::PhiInst& phi) {
  auto min = phi.dest.id;
  auto vec = std::vector<mir::inst::VarId>();
  vec.push_back(min);
  for (auto& id : phi.vars) {
    auto x = get_collapsed_var(id);
    if (x < min) min = x;
    vec.push_back(x);
  }
  for (auto& x : vec) {
    var_collapse.insert_or_assign(x, min);
  }
}

void Codegen::scan_stack() {
  for (auto& var_ : func.variables) {
    auto& id = var_.first;
    auto& var = var_.second;
    if (var.is_memory_var) {
      stack_space_allocation.insert({id, stack_size});
      stack_size += var.size();
    }
  }
}

arm::Reg Codegen::alloc_vgp() {
  return arm::make_register(arm::RegisterKind::VirtualGeneralPurpose,
                            vreg_gp_counter++);
}

arm::Reg Codegen::alloc_vd() {
  return arm::make_register(arm::RegisterKind::VirtualDoubleVector,
                            vreg_gp_counter++);
}

arm::Reg Codegen::alloc_vq() {
  return arm::make_register(arm::RegisterKind::VirtualQuadVector,
                            vreg_gp_counter++);
}

arm::Operand2 Codegen::translate_value_to_operand2(mir::inst::Value& v) {
  if (auto x = v.get_if<int32_t>()) {
    auto int_value = *x;
    if (is_valid_immediate(int_value)) {
      return int_value;
    } else {
      auto reg = alloc_vgp();
      uint32_t imm_u = int_value;
      inst.push_back(
          std::make_unique<Arith2Inst>(arm::OpCode::Mov, reg, imm_u & 0xffff));
      if (imm_u > 0xffff) {
        inst.push_back(std::make_unique<Arith2Inst>(arm::OpCode::MovT, reg,
                                                    imm_u & 0xffff0000));
      }
      return reg;
    }
  } else if (auto x = v.get_if<mir::inst::VarId>()) {
    return get_or_alloc_vgp(*x);
  } else {
    throw new prelude::UnreachableException();
  }
}

arm::Reg Codegen::translate_value_to_reg(mir::inst::Value& v) {
  if (auto x = v.get_if<int32_t>()) {
    auto int_value = *x;
    auto reg = alloc_vgp();
    uint32_t imm_u = int_value;
    inst.push_back(
        std::make_unique<Arith2Inst>(arm::OpCode::Mov, reg, imm_u & 0xffff));
    if (imm_u > 0xffff) {
      inst.push_back(std::make_unique<Arith2Inst>(arm::OpCode::MovT, reg,
                                                  imm_u & 0xffff0000));
    }
    return reg;
  } else if (auto x = v.get_if<mir::inst::VarId>()) {
    return get_or_alloc_vgp(*x);
  } else {
    throw new prelude::UnreachableException();
  }
}

arm::Reg Codegen::translate_var_reg(mir::inst::VarId v) {
  return get_or_alloc_vgp(v);
}

void Codegen::translate_inst(mir::inst::AssignInst& i) {
  if (i.src.is_immediate()) {
    auto imm = *i.src.get_if<int32_t>();
    uint32_t imm_u = imm;
    inst.push_back(std::make_unique<Arith2Inst>(
        arm::OpCode::Mov, translate_var_reg(i.dest), imm_u & 0xffff));
    if (imm_u > 0xffff) {
      inst.push_back(std::make_unique<Arith2Inst>(
          arm::OpCode::MovT, translate_var_reg(i.dest), imm_u & 0xffff0000));
    }
  } else {
    inst.push_back(std::make_unique<Arith2Inst>(
        arm::OpCode::Mov, translate_var_reg(i.dest),
        translate_value_to_operand2(i.src)));
  }
}

void Codegen::translate_inst(mir::inst::PhiInst& i) {
  // noop
}

void Codegen::translate_inst(mir::inst::CallInst& i) {
  // TODO: make call stuff
  auto fn_id = i.func.id;
  auto f_ = package.functions.find(fn_id);
  if (f_ == package.functions.end()) {
    throw new FunctionNotFoundError{fn_id};
  }
  auto& f = *f_;
  auto params = f.second.type->params;
  uint32_t param_count = params.size();

  if (params.size() > 0 &&
      params.back().get()->kind() == mir::types::TyKind::RestParam) {
    // Deal with variable length params
    param_count = i.params.size();
  }

  auto stack_size = param_count > 4 ? param_count - 4 : 0;
  auto reg_size = param_count > 4 ? 4 : param_count;
  // Expand stack
  if (stack_size > 0)
    inst.push_back(
        std::make_unique<Arith3Inst>(OpCode::Add, REG_SP, REG_SP, stack_size));

  {
    // Push params
    int param_idx = 0;
    for (auto& param : i.params) {
      if (param_idx < 4) {
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Mov, Reg(param_idx), translate_value_to_operand2(param)));
      } else {
        inst.push_back(std::make_unique<LoadStoreInst>(
            OpCode::StR, translate_value_to_reg(param),
            MemoryOperand(REG_SP, (int16_t)(-param_idx * 4),
                          MemoryAccessKind::None)));
      }
      param_idx++;
    }
  }
  // Call instruction
  inst.push_back(std::make_unique<BrInst>(OpCode::Bl, f.second.name));

  // Shrink stack
  if (stack_size > 0)
    inst.push_back(
        std::make_unique<Arith3Inst>(OpCode::Sub, REG_SP, REG_SP, stack_size));

  // Move return value
  if (!(f.second.type->ret->kind() == mir::types::TyKind::Void))
    inst.push_back(std::make_unique<Arith2Inst>(
        OpCode::Mov, translate_var_reg(i.dest), Reg(0)));

  throw new prelude::NotImplementedException();
}

void Codegen::translate_inst(mir::inst::StoreInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(arm::OpCode::StR,
                                             translate_value_to_reg(i.val),
                                             translate_var_reg(i.dest));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::LoadInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(arm::OpCode::LdR,
                                             translate_value_to_reg(i.src),
                                             translate_var_reg(i.dest));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::RefInst& i) {
  // TODO: Stack variable need no Ref. Global variables are hard to deal with.
  throw new prelude::NotImplementedException();
}

void Codegen::translate_inst(mir::inst::PtrOffsetInst& i) {
  auto ins = std::make_unique<Arith3Inst>(
      arm::OpCode::Add, translate_var_reg(i.dest), translate_var_reg(i.ptr),
      translate_value_to_operand2(i.offset));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::OpInst& i) {
  bool reverse_params = i.lhs.is_immediate() && !i.rhs.is_immediate() &&
                        (i.op != mir::inst::Op::Div);

  mir::inst::Value& lhs = reverse_params ? i.lhs : i.rhs;
  mir::inst::Value& rhs = reverse_params ? i.rhs : i.lhs;

  switch (i.op) {
    case mir::inst::Op::Add:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Add, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Sub:
      if (reverse_params) {
        inst.push_back(std::make_unique<Arith3Inst>(
            arm::OpCode::Rsb, translate_var_reg(i.dest),
            translate_value_to_reg(lhs), translate_value_to_operand2(lhs)));
      } else {
        inst.push_back(std::make_unique<Arith3Inst>(
            arm::OpCode::Sub, translate_var_reg(i.dest),
            translate_value_to_reg(rhs), translate_value_to_operand2(rhs)));
      }
      break;

    case mir::inst::Op::Mul:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Mul, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Div:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::SDiv, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::And:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::And, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Or:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Orr, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Rem:
      // WARN: Mod is a pseudo-instruction!
      // Mod operations needs to be eliminated in later passes.
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::_Mod, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));

    case mir::inst::Op::Gt:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Gt, reverse_params);
      break;
    case mir::inst::Op::Lt:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Lt, reverse_params);
      break;
    case mir::inst::Op::Gte:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Ge, reverse_params);
      break;
    case mir::inst::Op::Lte:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Le, reverse_params);
      break;
    case mir::inst::Op::Eq:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Equal, reverse_params);
      break;
    case mir::inst::Op::Neq:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::NotEqual,
                   reverse_params);
      break;
      break;
  }
}

void Codegen::emit_compare(mir::inst::VarId& dest, mir::inst::Value& lhs,
                           mir::inst::Value& rhs, arm::ConditionCode cond,
                           bool reversed) {
  auto lhsv = translate_value_to_reg(lhs);
  auto rhsv = translate_value_to_operand2(rhs);

  inst.push_back(std::make_unique<Arith2Inst>(
      reversed ? OpCode::Cmn : OpCode::Cmp, translate_value_to_reg(lhs),
      translate_value_to_operand2(rhs)));

  inst.push_back(std::make_unique<Arith2Inst>(
      OpCode::Mov, translate_var_reg(dest), Operand2(0)));
  inst.push_back(std::make_unique<Arith2Inst>(
      OpCode::Mov, translate_var_reg(dest), Operand2(1), cond));
}

void Codegen::translate_branch(mir::inst::JumpInstruction& j) {
  switch (j.kind) {
    case mir::inst::JumpInstructionKind::Br:
      inst.push_back(std::make_unique<BrInst>(
          OpCode::B, format_bb_label(func.name, j.bb_true)));
      break;
    case mir::inst::JumpInstructionKind::BrCond: {
      auto back = inst.end() - 2;

      // Find if a comparison exists
      std::optional<ConditionCode> cond = std::nullopt;
      if (inst.size() >= 2) {
        auto b1 = back->get();
        auto b2 = (back + 1)->get();
        if (b1->op == arm::OpCode::Mov && b2->op == arm::OpCode::Mov) {
          auto b1m = dynamic_cast<Arith2Inst*>(b1);
          auto b2m = dynamic_cast<Arith2Inst*>(b2);
          if (b1m->r1 == b2m->r1 && b1m->r2 == 1 && b2m->r2 == 0 &&
              b1m->cond == arm::ConditionCode::Always &&
              b2m->cond != arm::ConditionCode::Always) {
            cond = {b2m->cond};
          }
        }
      }
      // TODO: Omit the second jump argument if label is right after it
      // TODO: Move this^ to peephole optimization
      if (cond) {
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false),
            inverse_cond(cond.value())));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false)));
      } else {
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Cmp, translate_var_reg(j.cond_or_ret.value()),
            Operand2(0)));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false),
            ConditionCode::NotEqual));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false)));
      }
    } break;
    case mir::inst::JumpInstructionKind::Return:
      if (j.cond_or_ret.has_value()) {
        // Move return value to its register
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Mov, make_register(arm::RegisterKind::GeneralPurpose, 0),
            translate_var_reg(j.cond_or_ret.value())));
      }
      inst.push_back(
          std::make_unique<BrInst>(OpCode::B, format_fn_end_label(func.name)));
      // Jumps to function end, the rest is cleanup generation in function
      // `generate_return_and_cleanup`
      break;
    case mir::inst::JumpInstructionKind::Undefined:
      // WARN: Error about undefined blocks
      throw new std::exception();
      break;
    case mir::inst::JumpInstructionKind::Unreachable:
      // TODO: Discard unreachable blocks beforhand
      break;
  }
}

}  // namespace backend::codegen

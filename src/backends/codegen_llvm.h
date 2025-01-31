// A work-in-progress llvm backend
#pragma once

#include <taichi/common/util.h>
#include <taichi/io/io.h>
#include <set>

#include "../util.h"
#include "../program.h"
#include "../ir.h"

#if defined(TLANG_WITH_LLVM)
#include "llvm_codegen_utils.h"
#endif

TLANG_NAMESPACE_BEGIN

#if defined(TLANG_WITH_LLVM)

using namespace llvm;

class CodeGenLLVM : public IRVisitor, public ModuleBuilder {
 public:
  TaichiLLVMJIT *jit;

  CodeGenBase *codegen;
  Kernel *kernel;
  std::string kernel_name;
  std::vector<Value *> kernel_args;
  llvm::Type *context_ty;
  llvm::Type *physical_coordinate_ty;

  llvm::Value *current_coordinates;
  llvm::BasicBlock *while_after_loop;
  llvm::FunctionType *task_function_type;
  OffloadedStmt *current_offloaded_stmt;
  int task_counter;

  void initialize_context() {
    if (get_current_program().config.arch == Arch::gpu) {
      tlctx = get_current_program().llvm_context_device.get();
    } else {
      tlctx = get_current_program().llvm_context_host.get();
    }
    llvm_context = tlctx->ctx.get();
    jit = tlctx->jit.get();
    builder = new llvm::IRBuilder<>(*llvm_context);
  }

  llvm::Function *func;

  class OffloadedTask {
   public:
    std::string name;
    CodeGenLLVM *codegen;
    using task_fp_type = int32 (*)(void *);
    task_fp_type func;

    int block_dim;
    int grid_dim;
    void *cuda_func;

    OffloadedTask(CodeGenLLVM *codegen) : codegen(codegen) {
      func = nullptr;
    }

    void begin(std::string name) {
      this->name = name;
    }

    void end() {
      codegen->offloaded_tasks.push_back(*this);
    }

    void compile() {
      TC_ASSERT(!func);
      auto kernel_symbol = codegen->jit->lookup(name);
      TC_ASSERT_INFO(kernel_symbol, "Function not found");

      func = (task_fp_type)(void *)(llvm::cantFail(kernel_symbol.getAddress()));
    }

    void operator()(Context *context) {
      TC_ASSERT(func);
      func(context);
    }
  };

  std::unique_ptr<OffloadedTask> current_task;
  std::vector<OffloadedTask> offloaded_tasks;

  CodeGenLLVM(CodeGenBase *codegen, Kernel *kernel)
      // TODO: simplify ModuleBuilder ctor input
      : ModuleBuilder(get_current_program()
                          .get_llvm_context(get_current_program().config.arch)
                          ->clone_struct_module()),
        kernel(kernel),
        task_counter(0) {
    initialize_context();

    context_ty = get_runtime_type("Context");
    physical_coordinate_ty = get_runtime_type("PhysicalCoordinates");
    module->setDataLayout(jit->getDataLayout());

    using namespace llvm;

    for (auto &f : *module) {
      if (!f.isDeclaration())
        f.setLinkage(Function::PrivateLinkage);  // to avoid duplicated symbols
    }

    std::string grad_suffix;
    if (kernel->grad) {
      grad_suffix = "_grad";
    }
    kernel_name = kernel->name + grad_suffix + "_kernel";
  }

  llvm::Value *get_arg(int i) {
    std::vector<llvm::Value *> args;
    for (auto &arg : func->args()) {
      args.push_back(&arg);
    }
    return args[i];
  }

  llvm::Value *get_context() {
    return get_arg(0);
  }

  llvm::Value *get_root() {
    return builder->CreateCall(get_runtime_function("Context_get_buffer"),
                               get_context());
  }

  llvm::Value *get_runtime() {
    auto runtime_ptr = builder->CreateCall(
        get_runtime_function("Context_get_runtime"), get_context());
    return builder->CreateBitCast(
        runtime_ptr, llvm::PointerType::get(get_runtime_type("Runtime"), 0));
  }

  void emit_struct_meta_base(std::string name,
                             llvm::Value *node_meta,
                             SNode *snode) {
    RuntimeObject common("StructMeta", this, builder, node_meta);
    std::size_t element_size;
    if (snode->type != SNodeType::root && snode->type != SNodeType::place) {
      auto element_ty = snode->llvm_type->getArrayElementType();
      element_size = tlctx->get_type_size(element_ty);
    } else {
      auto element_ty = snode->llvm_type;
      element_size = tlctx->get_type_size(element_ty);
    }
    common.set("snode_id", tlctx->get_constant(snode->id));
    common.set("element_size", tlctx->get_constant((uint64)element_size));
    common.set("max_num_elements",
               tlctx->get_constant(1 << snode->total_num_bits));

    /*
    uint8 *(*lookup_element)(uint8 *, int i);
    uint8 *(*from_parent_element)(uint8 *);
    bool (*is_active)(uint8 *, int i);
    int (*get_num_elements)(uint8 *);
    void (*refine_coordinates)(PhysicalCoordinates *inp_coord,
                               PhysicalCoordinates *refined_coord,
                               int index);
                               */

    std::vector<std::string> functions = {"lookup_element", "is_active",
                                          "get_num_elements"};

    for (auto const f : functions)
      common.set(f, get_runtime_function(fmt::format("{}_{}", name, f)));

    // "from_parent_element", "refine_coordinates" are different for different
    // snodes, even if they have the same type.
    if (snode->parent)
      common.set("from_parent_element",
                 get_runtime_function(snode->get_ch_from_parent_func_name()));

    if (snode->type != SNodeType::place)
      common.set("refine_coordinates",
                 get_runtime_function(snode->refine_coordinates_func_name()));
  }

  std::unique_ptr<RuntimeObject> emit_struct_meta_object(SNode *snode) {
    std::unique_ptr<RuntimeObject> meta;
    if (snode->type == SNodeType::dense) {
      meta = std::make_unique<RuntimeObject>("DenseMeta", this, builder);
      emit_struct_meta_base("Dense", meta->ptr, snode);
      meta->call("set_bitmasked", tlctx->get_constant(snode->_bitmasked));
      meta->call("set_morton_dim", tlctx->get_constant((int)snode->_morton));
    } else if (snode->type == SNodeType::root) {
      meta = std::make_unique<RuntimeObject>("RootMeta", this, builder);
      emit_struct_meta_base("Root", meta->ptr, snode);
    } else {
      TC_P(snode_type_name(snode->type));
      TC_NOT_IMPLEMENTED;
    }
    return meta;
  }

  llvm::Value *emit_struct_meta(SNode *snode) {
    auto obj = emit_struct_meta_object(snode);
    TC_ASSERT(obj != nullptr);
    return obj->ptr;
  }

  virtual void emit_to_module() {
    kernel->ir->accept(this);
  }

  virtual FunctionType compile_module_to_executable() {
    jit->addModule(std::move(module));

    for (auto &task : offloaded_tasks) {
      task.compile();
    }
    auto offloaded_tasks_local = offloaded_tasks;
    return [=](Context context) {
      for (auto task : offloaded_tasks_local) {
        task(&context);
      }
    };
  }

  virtual FunctionType gen() {
    emit_to_module();
    return compile_module_to_executable();
  }

  template <typename... Args>
  void emit(std::string f, Args &&... args) {
    TC_NOT_IMPLEMENTED
    codegen->emit(f, std::forward<Args>(args)...);
  }

  void visit(Block *stmt_list) {
    for (auto &stmt : stmt_list->statements) {
      stmt->accept(this);
    }
  }

  void visit(AllocaStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    stmt->value = create_entry_block_alloca(stmt->ret_type.data_type);
    // initialize as zero
    builder->CreateStore(tlctx->get_constant(stmt->ret_type.data_type, 0),
                         stmt->value);
  }

  void visit(RandStmt *stmt) {
    TC_ASSERT(stmt->ret_type.data_type == DataType::f32);
    emit("const auto {} = {}::rand();", stmt->raw_name(),
         stmt->ret_data_type_name());
  }

  virtual void emit_extra_unary(UnaryOpStmt *stmt) {
    auto input = stmt->operand->value;
    auto input_taichi_type = stmt->operand->ret_type.data_type;
    auto input_type = input->getType();
    auto op = stmt->op_type;

#define UNARY_STD(x)                                                   \
  else if (op == UnaryOpType::x) {                                     \
    if (input_taichi_type == DataType::f32) {                          \
      stmt->value =                                                    \
          builder->CreateCall(get_runtime_function(#x "_f32"), input); \
    } else if (input_taichi_type == DataType::f64) {                   \
      stmt->value =                                                    \
          builder->CreateCall(get_runtime_function(#x "_f64"), input); \
    } else if (input_taichi_type == DataType::i32) {                   \
      stmt->value =                                                    \
          builder->CreateCall(get_runtime_function(#x "_i32"), input); \
    } else {                                                           \
      TC_NOT_IMPLEMENTED                                               \
    }                                                                  \
  }
    if (false) {
    }
    UNARY_STD(abs)
    UNARY_STD(exp)
    UNARY_STD(log)
    UNARY_STD(tan)
    UNARY_STD(tanh)
    UNARY_STD(sgn)
    UNARY_STD(logic_not)
    else {
      TC_P(unary_op_type_name(op));
      TC_NOT_IMPLEMENTED
    }
#undef UNARY_STD
  }

  void visit(UnaryOpStmt *stmt) {
    auto input = stmt->operand->value;
    auto input_taichi_type = stmt->operand->ret_type.data_type;
    auto input_type = input->getType();
    auto op = stmt->op_type;

#define UNARY_INTRINSIC(x)                                                   \
  else if (op == UnaryOpType::x) {                                           \
    stmt->value =                                                            \
        builder->CreateIntrinsic(llvm::Intrinsic::x, {input_type}, {input}); \
  }

    if (stmt->op_type != UnaryOpType::cast) {
      if (op == UnaryOpType::sqrt) {
        llvm::Function *sqrt_fn = Intrinsic::getDeclaration(
            module.get(), Intrinsic::sqrt, input->getType());
        stmt->value = builder->CreateCall(sqrt_fn, input, "sqrt");
      } else if (op == UnaryOpType::neg) {
        stmt->value = builder->CreateFNeg(input, "neg");
      }
      UNARY_INTRINSIC(sin)
      UNARY_INTRINSIC(cos)
      UNARY_INTRINSIC(sqrt)
      UNARY_INTRINSIC(floor)
      UNARY_INTRINSIC(ceil)
      else emit_extra_unary(stmt);
#undef UNARY_INTRINSIC
    } else {
      // op = cast
      if (stmt->cast_by_value) {
        llvm::CastInst::CastOps cast_op;
        auto from = stmt->operand->ret_type.data_type;
        auto to = stmt->cast_type;
        TC_ASSERT(from != to);
        if (is_real(from) != is_real(to)) {
          if (from == DataType::f32 && to == DataType::i32) {
            cast_op = llvm::Instruction::CastOps::FPToSI;
          } else if (from == DataType::f64 && to == DataType::i32) {
            cast_op = llvm::Instruction::CastOps::FPToSI;
          } else if (from == DataType::i32 && to == DataType::f32) {
            cast_op = llvm::Instruction::CastOps::SIToFP;
          } else if (from == DataType::i32 && to == DataType::f64) {
            cast_op = llvm::Instruction::CastOps::SIToFP;
          } else {
            TC_P(data_type_name(from));
            TC_P(data_type_name(to));
            TC_NOT_IMPLEMENTED;
          }
          stmt->value =
              builder->CreateCast(cast_op, stmt->operand->value,
                                  tlctx->get_data_type(stmt->cast_type));
        } else if (is_real(from) && is_real(to)) {
          if (data_type_size(from) < data_type_size(to)) {
            stmt->value = builder->CreateFPExt(
                stmt->operand->value, tlctx->get_data_type(stmt->cast_type));
          } else {
            stmt->value = builder->CreateFPTrunc(
                stmt->operand->value, tlctx->get_data_type(stmt->cast_type));
          }
        } else if (!is_real(from) && !is_real(to)) {
          if (data_type_size(from) < data_type_size(to)) {
            stmt->value = builder->CreateSExt(
                stmt->operand->value, tlctx->get_data_type(stmt->cast_type));
          } else {
            stmt->value = builder->CreateTrunc(
                stmt->operand->value, tlctx->get_data_type(stmt->cast_type));
          }
        }
      } else {
        TC_ASSERT(data_type_size(stmt->ret_type.data_type) ==
                  data_type_size(stmt->cast_type));
        stmt->value = builder->CreateBitCast(
            stmt->operand->value, tlctx->get_data_type(stmt->cast_type));
      }
    }
  }

  llvm::Type *llvm_type(DataType dt) {
    if (dt == DataType::i32) {
      return llvm::Type::getInt32Ty(*llvm_context);
    } else if (dt == DataType::i1) {
      return llvm::Type::getInt1Ty(*llvm_context);
    } else if (dt == DataType::f32) {
      return llvm::Type::getFloatTy(*llvm_context);
    } else if (dt == DataType::f64) {
      return llvm::Type::getDoubleTy(*llvm_context);
    } else {
      TC_NOT_IMPLEMENTED;
    }
    return nullptr;
  }

  void visit(BinaryOpStmt *stmt) {
    auto op = stmt->op_type;
    auto ret_type = stmt->ret_type.data_type;
    if (op == BinaryOpType::add) {
      if (is_real(stmt->ret_type.data_type)) {
        stmt->value = builder->CreateFAdd(stmt->lhs->value, stmt->rhs->value);
      } else {
        stmt->value = builder->CreateAdd(stmt->lhs->value, stmt->rhs->value);
      }
    } else if (op == BinaryOpType::sub) {
      if (is_real(stmt->ret_type.data_type)) {
        stmt->value = builder->CreateFSub(stmt->lhs->value, stmt->rhs->value);
      } else {
        stmt->value = builder->CreateSub(stmt->lhs->value, stmt->rhs->value);
      }
    } else if (op == BinaryOpType::mul) {
      if (is_real(stmt->ret_type.data_type)) {
        stmt->value = builder->CreateFMul(stmt->lhs->value, stmt->rhs->value);
      } else {
        stmt->value = builder->CreateMul(stmt->lhs->value, stmt->rhs->value);
      }
    } else if (op == BinaryOpType::div) {
      if (is_real(stmt->ret_type.data_type)) {
        stmt->value = builder->CreateFDiv(stmt->lhs->value, stmt->rhs->value);
      } else {
        stmt->value = builder->CreateSDiv(stmt->lhs->value, stmt->rhs->value);
      }
    } else if (op == BinaryOpType::mod) {
      stmt->value = builder->CreateSRem(stmt->lhs->value, stmt->rhs->value);
    } else if (op == BinaryOpType::bit_and) {
      stmt->value = builder->CreateAnd(stmt->lhs->value, stmt->rhs->value);
    } else if (op == BinaryOpType::bit_or) {
      stmt->value = builder->CreateOr(stmt->lhs->value, stmt->rhs->value);
    } else if (op == BinaryOpType::bit_xor) {
      stmt->value = builder->CreateXor(stmt->lhs->value, stmt->rhs->value);
    } else if (op == BinaryOpType::max) {
      if (is_real(ret_type)) {
        stmt->value = builder->CreateMaxNum(stmt->lhs->value, stmt->rhs->value);
      } else if (ret_type == DataType::i32) {
        stmt->value =
            create_call("max_i32", {stmt->lhs->value, stmt->rhs->value});
      } else {
        TC_P(data_type_name(ret_type));
        TC_NOT_IMPLEMENTED
      }
    } else if (op == BinaryOpType::min) {
      if (is_real(ret_type)) {
        stmt->value = builder->CreateMinNum(stmt->lhs->value, stmt->rhs->value);
      } else if (ret_type == DataType::i32) {
        stmt->value =
            create_call("min_i32", {stmt->lhs->value, stmt->rhs->value});
      } else {
        TC_P(data_type_name(ret_type));
        TC_NOT_IMPLEMENTED
      }
    } else if (is_comparison(op)) {
      llvm::Value *cmp = nullptr;
      auto input_type = stmt->lhs->ret_type.data_type;
      if (op == BinaryOpType::cmp_eq) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpOEQ(stmt->lhs->value, stmt->rhs->value);
        } else {
          cmp = builder->CreateICmpEQ(stmt->lhs->value, stmt->rhs->value);
        }
      } else if (op == BinaryOpType::cmp_le) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpOLE(stmt->lhs->value, stmt->rhs->value);
        } else {
          if (is_signed(input_type)) {
            cmp = builder->CreateICmpSLE(stmt->lhs->value, stmt->rhs->value);
          } else {
            cmp = builder->CreateICmpULE(stmt->lhs->value, stmt->rhs->value);
          }
        }
      } else if (op == BinaryOpType::cmp_ge) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpOGE(stmt->lhs->value, stmt->rhs->value);
        } else {
          if (is_signed(input_type)) {
            cmp = builder->CreateICmpSGE(stmt->lhs->value, stmt->rhs->value);
          } else {
            cmp = builder->CreateICmpUGE(stmt->lhs->value, stmt->rhs->value);
          }
        }
      } else if (op == BinaryOpType::cmp_lt) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpOLT(stmt->lhs->value, stmt->rhs->value);
        } else {
          if (is_signed(input_type)) {
            cmp = builder->CreateICmpSLT(stmt->lhs->value, stmt->rhs->value);
          } else {
            cmp = builder->CreateICmpULT(stmt->lhs->value, stmt->rhs->value);
          }
        }
      } else if (op == BinaryOpType::cmp_gt) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpOGT(stmt->lhs->value, stmt->rhs->value);
        } else {
          if (is_signed(input_type)) {
            cmp = builder->CreateICmpSGT(stmt->lhs->value, stmt->rhs->value);
          } else {
            cmp = builder->CreateICmpUGT(stmt->lhs->value, stmt->rhs->value);
          }
        }
      } else if (op == BinaryOpType::cmp_ne) {
        if (is_real(input_type)) {
          cmp = builder->CreateFCmpONE(stmt->lhs->value, stmt->rhs->value);
        } else {
          cmp = builder->CreateICmpNE(stmt->lhs->value, stmt->rhs->value);
        }
      } else {
        TC_NOT_IMPLEMENTED
      }
      stmt->value = builder->CreateSExt(cmp, llvm_type(DataType::i32));
    } else {
      TC_P(binary_op_type_name(op));
      TC_NOT_IMPLEMENTED
    }
  }

  void visit(TernaryOpStmt *stmt) {
    TC_ASSERT(stmt->op_type == TernaryOpType::select);
    stmt->value = builder->CreateSelect(
        builder->CreateTrunc(stmt->op1->value, llvm_type(DataType::i1)),
        stmt->op2->value, stmt->op3->value);
  }

  void visit(IfStmt *if_stmt) {
    BasicBlock *true_block =
        BasicBlock::Create(*llvm_context, "true_block", func);
    BasicBlock *false_block =
        BasicBlock::Create(*llvm_context, "false_block", func);
    BasicBlock *after_if = BasicBlock::Create(*llvm_context, "after_if", func);
    builder->CreateCondBr(
        builder->CreateICmpNE(if_stmt->cond->value, tlctx->get_constant(0)),
        true_block, false_block);
    builder->SetInsertPoint(true_block);
    if (if_stmt->true_statements) {
      if_stmt->true_statements->accept(this);
    }
    builder->CreateBr(false_block);
    builder->SetInsertPoint(false_block);
    if (if_stmt->false_statements) {
      if_stmt->false_statements->accept(this);
    }
    builder->CreateBr(after_if);
    builder->SetInsertPoint(after_if);
  }

  void visit(PrintStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    std::vector<Value *> args;
    std::string format;
    auto value = stmt->stmt->value;
    if (stmt->stmt->ret_type.data_type == DataType::i32) {
      format = "%d";
    } else if (stmt->stmt->ret_type.data_type == DataType::f32) {
      format = "%f";
      value = builder->CreateFPExt(value, tlctx->get_data_type(DataType::f64));
    } else {
      TC_NOT_IMPLEMENTED
    }
    args.push_back(builder->CreateGlobalStringPtr(
        ("[debug] " + stmt->str + " = " + format + "\n").c_str(),
        "format_string"));
    args.push_back(value);

    stmt->value = builder->CreateCall(get_runtime_function("printf"), args,
                                      "debug_printf");
  }

  void visit(ConstStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    auto val = stmt->val[0];
    if (val.dt == DataType::f32) {
      stmt->value = llvm::ConstantFP::get(*llvm_context,
                                          llvm::APFloat(val.val_float32()));
    } else if (val.dt == DataType::f64) {
      stmt->value = llvm::ConstantFP::get(*llvm_context,
                                          llvm::APFloat(val.val_float64()));
    } else if (val.dt == DataType::i32) {
      stmt->value = llvm::ConstantInt::get(
          *llvm_context, llvm::APInt(32, val.val_int32(), true));
    } else {
      TC_NOT_IMPLEMENTED;
    }
  }

  void visit(WhileControlStmt *stmt) {
    BasicBlock *after_break =
        BasicBlock::Create(*llvm_context, "after_break", func);
    TC_ASSERT(while_after_loop);
    auto cond =
        builder->CreateICmpEQ(stmt->cond->value, tlctx->get_constant(0));
    builder->CreateCondBr(cond, while_after_loop, after_break);
    builder->SetInsertPoint(after_break);
  }

  void visit(WhileStmt *stmt) {
    BasicBlock *body =
        BasicBlock::Create(*llvm_context, "while_loop_body", func);
    builder->CreateBr(body);
    builder->SetInsertPoint(body);

    BasicBlock *after_loop =
        BasicBlock::Create(*llvm_context, "after_while", func);
    auto old_while_after_loop = while_after_loop;
    while_after_loop = after_loop;

    stmt->body->accept(this);

    builder->CreateBr(body);  // jump to head

    builder->SetInsertPoint(after_loop);
    while_after_loop = old_while_after_loop;
  }

  llvm::Value *cast_pointer(llvm::Value *val,
                            std::string dest_ty_name,
                            int addr_space = 0) {
    return builder->CreateBitCast(
        val,
        llvm::PointerType::get(get_runtime_type(dest_ty_name), addr_space));
  }

  void emit_list_gen(OffloadedStmt *listgen) {
    auto snode_child = listgen->snode;
    auto snode_parent = listgen->snode->parent;
    auto meta_child = cast_pointer(emit_struct_meta(snode_child), "StructMeta");
    auto meta_parent =
        cast_pointer(emit_struct_meta(snode_parent), "StructMeta");
    call("element_listgen", get_runtime(), meta_parent, meta_child);
  }

  llvm::Value *create_call(llvm::Value *func, std::vector<Value *> args) {
    check_func_call_signature(func, args);
    return builder->CreateCall(func, args);
  }

  llvm::Value *create_call(std::string func_name, std::vector<Value *> args) {
    auto func = get_runtime_function(func_name);
    return create_call(func, args);
  }

  void create_increment(llvm::Value *ptr, llvm::Value *value) {
    builder->CreateStore(builder->CreateAdd(builder->CreateLoad(ptr), value),
                         ptr);
  }

  // Direct translation
  void create_naive_range_for(RangeForStmt *for_stmt) {
    BasicBlock *body = BasicBlock::Create(*llvm_context, "loop_body", func);
    BasicBlock *after_loop = BasicBlock::Create(*llvm_context, "block", func);
    if (!for_stmt->reversed) {
      builder->CreateStore(for_stmt->begin->value, for_stmt->loop_var->value);
    } else {
      builder->CreateStore(
          builder->CreateSub(for_stmt->end->value, tlctx->get_constant(1)),
          for_stmt->loop_var->value);
    }
    builder->CreateBr(body);

    // body cfg
    builder->SetInsertPoint(body);

    for_stmt->body->accept(this);

    llvm::Value *cond = nullptr;
    if (!for_stmt->reversed) {
      create_increment(for_stmt->loop_var->value, tlctx->get_constant(1));
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                                 builder->CreateLoad(for_stmt->loop_var->value),
                                 for_stmt->end->value);
    } else {
      create_increment(for_stmt->loop_var->value, tlctx->get_constant(-1));
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SGE,
                                 builder->CreateLoad(for_stmt->loop_var->value),
                                 for_stmt->begin->value);
    }

    builder->CreateCondBr(cond, body, after_loop);

    // next cfg
    builder->SetInsertPoint(after_loop);
  }

  virtual void visit(RangeForStmt *for_stmt) {
    create_naive_range_for(for_stmt);
  }

  void visit(ArgLoadStmt *stmt) {
    auto raw_arg =
        builder->CreateCall(get_runtime_function("Context_get_args"),
                            {get_context(), tlctx->get_constant(stmt->arg_id)});
    llvm::Type *dest_ty = nullptr;
    if (stmt->is_ptr) {
      dest_ty = PointerType::get(tlctx->get_data_type(DataType::i32), 0);
      stmt->value = builder->CreateIntToPtr(raw_arg, dest_ty);
    } else {
      dest_ty = tlctx->get_data_type(stmt->ret_type.data_type);
      auto dest_bits = dest_ty->getPrimitiveSizeInBits();
      auto truncated = builder->CreateTrunc(
          raw_arg, Type::getIntNTy(*llvm_context, dest_bits));
      stmt->value = builder->CreateBitCast(truncated, dest_ty);
    }
  }

  void visit(LocalLoadStmt *stmt) {
    TC_ASSERT(stmt->width() == 1);
    stmt->value = builder->CreateLoad(stmt->ptr[0].var->value);
  }

  void visit(LocalStoreStmt *stmt) {
    auto mask = stmt->parent->mask();
    if (mask && stmt->width() != 1) {
      TC_NOT_IMPLEMENTED
    } else {
      builder->CreateStore(stmt->data->value, stmt->ptr->value);
    }
  }

  void visit(SNodeOpStmt *stmt) {
    stmt->ret_type.data_type = DataType::i32;
    if (stmt->op_type == SNodeOpType::probe) {
      emit("{} {};", stmt->ret_data_type_name(), stmt->raw_name());
    }

    for (auto l = 0; l < stmt->width(); l++) {
      auto snode = stmt->snodes[l];
      auto indices = indices_str(snode, l, stmt->indices);

      emit("{{");
      if (stmt->op_type != SNodeOpType::activate &&
          stmt->op_type != SNodeOpType::probe) {
        emit("{} *{}_tmp = access_{}(root, {});", snode->node_type_name,
             snode->node_type_name, snode->node_type_name,
             make_list(indices, ""));
      }
      if (stmt->op_type == SNodeOpType::append) {
        TC_ASSERT(stmt->val->width() == 1);
        emit("{}_tmp->append({}({}[{}]));", snode->node_type_name,
             snode->ch[0]->node_type_name, stmt->val->raw_name(), l);
      } else if (stmt->op_type == SNodeOpType::clear) {
        emit("{}_tmp->clear();", snode->node_type_name);
      } else if (stmt->op_type == SNodeOpType::probe) {
        emit("{}[{}] = query_{}(root, {});", stmt->raw_name(), l,
             snode->node_type_name, make_list(indices, ""));
        if (snode->type == SNodeType::dynamic) {
          emit("if ({}[{}]) {{", stmt->raw_name(), l);
          emit("{} *{}_tmp = access_{}(root, {});", snode->node_type_name,
               snode->node_type_name, snode->node_type_name,
               make_list(indices, ""));
          emit("{}[{}] = {}_tmp->get_n();", stmt->raw_name(), l,
               snode->node_type_name);
          emit("}}");
        }
      } else if (stmt->op_type == SNodeOpType::activate) {
        emit("activate_{}(root, {});", snode->node_type_name,
             make_list(indices, ""));
      } else {
        TC_NOT_IMPLEMENTED
      }
      emit("}}");
    }
  }

  virtual void visit(AtomicOpStmt *stmt) {
    auto mask = stmt->parent->mask();
    for (int l = 0; l < stmt->width(); l++) {
      if (mask) {
        emit("if ({}[{}]) ", mask->raw_name(), l);
      } else {
        TC_ASSERT(stmt->op_type == AtomicOpType::add);
        if (stmt->val->ret_type.data_type == DataType::i32)
          builder->CreateAtomicRMW(
              llvm::AtomicRMWInst::BinOp::Add, stmt->dest->value,
              stmt->val->value, llvm::AtomicOrdering::SequentiallyConsistent);
        else if (stmt->val->ret_type.data_type == DataType::f32) {
          builder->CreateCall(get_runtime_function("atomic_add_cpu_f32"),
                              {stmt->dest->value, stmt->val->value});
        } else if (stmt->val->ret_type.data_type == DataType::f64) {
          builder->CreateCall(get_runtime_function("atomic_add_cpu_f64"),
                              {stmt->dest->value, stmt->val->value});
        } else {
          TC_NOT_IMPLEMENTED
        }
      }
    }
  }

  void visit(GlobalPtrStmt *stmt) {
    TC_ERROR("Global Ptrs should have been lowered.");
  }

  void visit(GlobalStoreStmt *stmt) {
    /*
    if (!current_program->config.force_vectorized_global_store) {
      for (int i = 0; i < stmt->data->ret_type.width; i++) {
        if (stmt->parent->mask()) {
          TC_ASSERT(stmt->width() == 1);
          emit("if ({}[{}])", stmt->parent->mask()->raw_name(), i);
        }
        emit("*({} *){}[{}] = {}[{}];",
             data_type_name(stmt->data->ret_type.data_type),
             stmt->ptr->raw_name(), i, stmt->data->raw_name(), i);
      }
    } else {
      emit("{}.store({}[0]);", stmt->data->raw_name(), stmt->ptr->raw_name());
    }
    */
    TC_ASSERT(!stmt->parent->mask() || stmt->width() == 1);
    /*
    emit("*({} *){}[{}] = {}[{}];",
         data_type_name(stmt->data->ret_type.data_type),
         stmt->ptr->raw_name(), i, stmt->data->raw_name(), i);
    */
    TC_ASSERT(stmt->data->value);
    TC_ASSERT(stmt->ptr->value);
    builder->CreateStore(stmt->data->value, stmt->ptr->value);
  }

  void visit(GlobalLoadStmt *stmt) {
    int width = stmt->width();
    if (get_current_program().config.attempt_vectorized_load_cpu &&
        width >= 4 && stmt->ptr->is<ElementShuffleStmt>()) {
      /*
      TC_ASSERT(stmt->ret_type.data_type == DataType::i32 ||
                stmt->ret_type.data_type == DataType::f32);
      bool loaded[width];
      for (int i = 0; i < width; i++)
        loaded[i] = false;

      auto shuffle = stmt->ptr->as<ElementShuffleStmt>();
      Stmt *statements[width];
      int offsets[width];

      for (int i = 0; i < width; i++) {
        auto src = shuffle->elements[i].stmt;
        if (shuffle->elements[i].stmt->is<IntegerOffsetStmt>()) {
          auto indir = src->as<IntegerOffsetStmt>();
          statements[i] = indir->input;
          offsets[i] = indir->offset;
        } else {
          statements[i] = src;
          offsets[i] = 0;
        }
      }

      emit("{} {};", stmt->ret_data_type_name(), stmt->raw_name());
      for (int i = 0; i < width; i++) {
        if (loaded[i])
          continue;
        bool mask[width];
        std::memset(mask, 0, sizeof(mask));
        mask[i] = true;
        for (int j = i + 1; j < width; j++) {
          if (statements[i] == statements[j]) {
            if ((j - i) * (int)sizeof(int32) == offsets[j] - offsets[i]) {
              mask[j] = true;
            }
          }
        }
        int imm_mask = 0;
        for (int j = width - 1; j >= 0; j--) {
          if (mask[j]) {
            loaded[j] = true;
          }
          imm_mask *= 2;
          imm_mask += (int)mask[j];
        }
        // load and blend in
        if (i == 0) {
          emit("{} = {}::load({}[0]);", stmt->raw_name(),
               stmt->ret_data_type_name(),
               shuffle->elements[i].stmt->raw_name());
        } else {
          emit("{} = blend<{}>({}, {}::load({}[0] - {}));", stmt->raw_name(),
               imm_mask, stmt->raw_name(), stmt->ret_data_type_name(),
               shuffle->elements[i].stmt->raw_name(), i);
        }
      }
       */
    } else {
      /*
      emit("{} {};", stmt->ret_data_type_name(), stmt->raw_name());
      for (int i = 0; i < stmt->ret_type.width; i++) {
        emit("{}[{}] = *{}[{}];", stmt->raw_name(), i, stmt->ptr->raw_name(),
             i);
      }
      */
    }
    TC_ASSERT(stmt->width() == 1);
    stmt->value = builder->CreateLoad(
        tlctx->get_data_type(stmt->ret_type.data_type), stmt->ptr->value);
  }

  void visit(ElementShuffleStmt *stmt) {
    auto init = stmt->elements.serialize(
        [](const VectorElement &elem) {
          return fmt::format("{}[{}]", elem.stmt->raw_name(), elem.index);
        },
        "{");
    if (stmt->pointer) {
      emit("{} * const {} [{}] {};", data_type_name(stmt->ret_type.data_type),
           stmt->raw_name(), stmt->width(), init);
    } else {
      emit("const {} {} ({});", stmt->ret_data_type_name(), stmt->raw_name(),
           init);
    }
  }

  void visit(AssertStmt *stmt) {
    emit("#if defined(TL_DEBUG)");
    emit(R"(TC_ASSERT_INFO({}, "{}");)", stmt->val->raw_name(), stmt->text);
    emit("#endif");
  }

  void visit(OffsetAndExtractBitsStmt *stmt) {
    auto shifted = builder->CreateAdd(stmt->input->value,
                                      tlctx->get_constant((int32)stmt->offset));
    int mask = (1u << (stmt->bit_end - stmt->bit_begin)) - 1;
    stmt->value =
        builder->CreateAnd(builder->CreateLShr(shifted, stmt->bit_begin),
                           tlctx->get_constant(mask));
  }

  void visit(LinearizeStmt *stmt) {
    llvm::Value *val = tlctx->get_constant(0);
    for (int i = 0; i < (int)stmt->inputs.size(); i++) {
      val = builder->CreateAdd(
          builder->CreateMul(val, tlctx->get_constant(stmt->strides[i])),
          stmt->inputs[i]->value);
    }
    stmt->value = val;
  }

  void visit(IntegerOffsetStmt *stmt) {
    TC_NOT_IMPLEMENTED
    if (stmt->input->is<GetChStmt>() &&
        stmt->input->as<GetChStmt>()->output_snode->type == SNodeType::place) {
      auto input = stmt->input->as<GetChStmt>();
      auto dtn = input->output_snode->data_type_name();
      emit(R"({}* {}[1] {{({} *)((char *){}[0] + {})}};)", dtn,
           stmt->raw_name(), dtn, stmt->input->raw_name(), stmt->offset);
    } else {
      emit(R"(auto {} = {} + {};)", stmt->raw_name(), stmt->input->raw_name(),
           stmt->offset);
    }
  }

  void visit(SNodeLookupStmt *stmt) {
    llvm::Value *parent = nullptr;
    if (stmt->input_snode) {
      parent = stmt->input_snode->value;
    } else {
      parent = builder->CreateBitCast(
          get_root(),
          PointerType::get(get_current_program().snode_root->llvm_type, 0));
    }
    TC_ASSERT(parent);
    // This part may need a redesign - why do we need both global indices and
    // linearized indices?
    auto snode = stmt->snode;
    /*
    std::vector<std::string> global_indices(max_num_indices, "0");
    for (int i = 0; i < (int)stmt->global_indices.size(); i++) {
      if (snode->physical_index_position[i] != -1) {
        global_indices[snode->physical_index_position[i]] =
            stmt->global_indices[i]->raw_name() + fmt::format("[{}]", 0);
      }
    }
    */
    if (stmt->activate && stmt->snode->type != SNodeType::place) {
      // emit(R"({}->activate({}, {});)", parent,
      // stmt->input_index->raw_name(),
      //     make_list(global_indices, "{"));
    }
    // emit("auto {}_guarded = {}->look_up({});", stmt->raw_name(), parent,
    //     stmt->input_index->raw_name());
    if (!stmt->activate && snode->has_null()) {
      // safe guard with ambient node
      emit("if({}_guarded == nullptr) {}_guarded = &{}_ambient;",
           stmt->raw_name(), stmt->raw_name(), snode->node_type_name);
    }
    // emit(R"(auto {} = {}_guarded;)", stmt->raw_name(), stmt->raw_name());
    // TC_P(snode_type_name(snode->type));
    if (snode->type == SNodeType::root) {
      stmt->value = builder->CreateGEP(parent, stmt->input_index->value);
    } else {
      TC_ASSERT(snode->type == SNodeType::dense);

      // allocate the struct
      /*
      auto s = builder->CreateAlloca(get_runtime_type("DenseMeta"));
      auto node = builder->CreateBitCast(
          stmt->input_snode->value, llvm::Type::getInt8PtrTy(*llvm_context));
      auto element_ty = stmt->snode->llvm_type->getArrayElementType();
      std::size_t element_size = tlctx->get_type_size(element_ty);
      builder->CreateCall(get_runtime_function("DenseMeta_set_element_size"),
                         {s, tlctx->get_constant((uint64)element_size)});
                         */

      auto s = emit_struct_meta(stmt->snode);
      auto s_ptr =
          builder->CreateBitCast(s, llvm::Type::getInt8PtrTy(*llvm_context));

      // call look up
      auto node_ptr = builder->CreateBitCast(
          stmt->input_snode->value, llvm::Type::getInt8PtrTy(*llvm_context));
      auto elem =
          builder->CreateCall(get_runtime_function("Dense_lookup_element"),
                              {s_ptr, node_ptr, stmt->input_index->value});
      auto element_ty = snode->llvm_type->getArrayElementType();
      stmt->value =
          builder->CreateBitCast(elem, PointerType::get(element_ty, 0));
    }
  }

  void visit(GetChStmt *stmt) {
    // always unvectorized
    // it is OK to directly use GEP here since Components are "dense"
    stmt->value = builder->CreateGEP(
        stmt->input_ptr->value,
        {tlctx->get_constant(0), tlctx->get_constant(stmt->chid)}, "getch");
  }

  void visit(ExternalPtrStmt *stmt) override {
    TC_ASSERT(stmt->width() == 1);
    TC_ASSERT(stmt->indices.size() == 1);
    auto dt = stmt->ret_type.data_type;
    auto base = builder->CreateBitCast(
        stmt->base_ptrs[0]->value,
        llvm::PointerType::get(tlctx->get_data_type(dt), 0));
    stmt->value = builder->CreateGEP(base, stmt->indices[0]->value);
  }

  BasicBlock *func_body_bb;

  void init_task_function(OffloadedStmt *stmt) {
    while_after_loop = nullptr;
    current_offloaded_stmt = stmt;

    task_function_type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*llvm_context),
                                {PointerType::get(context_ty, 0)}, false);

    auto task_kernel_name = fmt::format("{}_{}", kernel_name, task_counter);
    task_counter += 1;
    func = Function::Create(task_function_type, Function::ExternalLinkage,
                            task_kernel_name, module.get());

    current_task = std::make_unique<OffloadedTask>(this);
    current_task->begin(task_kernel_name);

    for (auto &arg : func->args()) {
      kernel_args.push_back(&arg);
    }
    kernel_args[0]->setName("context");

    // entry_block has all the allocas
    this->entry_block = BasicBlock::Create(*llvm_context, "entry", func);

    // The real function body
    func_body_bb = BasicBlock::Create(*llvm_context, "body", func);
    builder->SetInsertPoint(func_body_bb);
  }

  void finalize_task_function() {
    builder->CreateRetVoid();

    // entry_block should jump to the body after all allocas are inserted
    builder->SetInsertPoint(entry_block);
    builder->CreateBr(func_body_bb);

    if (get_current_program().config.print_kernel_llvm_ir) {
      TC_INFO("Kernel Module IR");
      module->print(errs(), nullptr);
      TC_INFO("Kernel Module IR printed.");
    }
    TC_ASSERT(!llvm::verifyFunction(*func, &errs()));
    // TC_INFO("Kernel function verified.");
  }

  void create_offload_range_for(OffloadedStmt *stmt) {
    auto loop_var = create_entry_block_alloca(DataType::i32);
    stmt->loop_vars_llvm.push_back(loop_var);
    BasicBlock *body = BasicBlock::Create(*llvm_context, "loop_body", func);
    BasicBlock *after_loop = BasicBlock::Create(*llvm_context, "block", func);
    if (!stmt->reversed) {
      builder->CreateStore(tlctx->get_constant(stmt->begin), loop_var);
    } else {
      builder->CreateStore(builder->CreateSub(tlctx->get_constant(stmt->end),
                                              tlctx->get_constant(1)),
                           loop_var);
    }
    builder->CreateBr(body);

    // body cfg
    builder->SetInsertPoint(body);

    stmt->body->accept(this);

    llvm::Value *cond = nullptr;
    if (!stmt->reversed) {
      create_increment(loop_var, tlctx->get_constant(1));
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                                 builder->CreateLoad(loop_var),
                                 tlctx->get_constant(stmt->end));
    } else {
      create_increment(loop_var, tlctx->get_constant(-1));
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SGE,
                                 builder->CreateLoad(loop_var),
                                 tlctx->get_constant(stmt->begin));
    }

    builder->CreateCondBr(cond, body, after_loop);

    // next cfg
    builder->SetInsertPoint(after_loop);
  }

  void create_offload_struct_for(OffloadedStmt *stmt, bool spmd = false) {
    llvm::Function *body;
    auto leaf_block = stmt->snode->parent;
    {
      // Create the loop body function
      auto body_function_type = llvm::FunctionType::get(
          llvm::Type::getVoidTy(*llvm_context),
          {
              llvm::PointerType::get(get_runtime_type("Context"), 0),
              llvm::PointerType::get(get_runtime_type("Element"), 0),
              tlctx->get_data_type<int>(),
              tlctx->get_data_type<int>(),
          },
          false);

      body = llvm::Function::Create(body_function_type,
                                    llvm::Function::InternalLinkage,
                                    "loop_body", module.get());
      auto old_func = func;
      // emit into loop body function
      func = body;

      auto allocas = BasicBlock::Create(*llvm_context, "allocs", body);
      auto old_entry = entry_block;
      entry_block = allocas;

      auto entry = BasicBlock::Create(*llvm_context, "entry", func);

      auto ip = builder->saveIP();
      builder->SetInsertPoint(entry);
      // current_struct_for = for_stmt;

      auto body_bb = BasicBlock::Create(*llvm_context, "loop_body", func);
      // per-leaf-block for loop
      auto loop_index =
          create_entry_block_alloca(Type::getInt32Ty(*llvm_context));

      llvm::Value *threadIdx = nullptr, *blockDim = nullptr;

      auto lower_bound = get_arg(2);
      auto upper_bound = get_arg(3);

      if (spmd) {
        threadIdx = builder->CreateIntrinsic(
            Intrinsic::nvvm_read_ptx_sreg_tid_x, {}, {});
        blockDim = builder->CreateIntrinsic(
            Intrinsic::nvvm_read_ptx_sreg_ntid_x, {}, {});
        builder->CreateStore(builder->CreateAdd(threadIdx, lower_bound),
                             loop_index);
      } else {
        builder->CreateStore(lower_bound, loop_index);
      }
      builder->CreateBr(body_bb);

      builder->SetInsertPoint(body_bb);
      // initialize the coordinates

      auto refine =
          get_runtime_function(leaf_block->refine_coordinates_func_name());
      auto new_coordinates = create_entry_block_alloca(physical_coordinate_ty);
      RuntimeObject element("Element", this, builder, get_arg(1));
      create_call(refine, {element.get_ptr("pcoord"), new_coordinates,
                           builder->CreateLoad(loop_index)});

      current_coordinates = new_coordinates;
      stmt->body->accept(this);

      BasicBlock *after_loop = BasicBlock::Create(*llvm_context, "block", func);

      // body cfg

      if (spmd) {
        create_increment(loop_index, blockDim);
      } else {
        create_increment(loop_index, tlctx->get_constant(1));
      }

      auto cond =
          builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                              builder->CreateLoad(loop_index), upper_bound);

      builder->CreateCondBr(cond, body_bb, after_loop);

      // next cfg
      builder->SetInsertPoint(after_loop);

      builder->CreateRetVoid();
      func = old_func;
      builder->restoreIP(ip);

      {
        llvm::IRBuilderBase::InsertPointGuard gurad(*builder);
        builder->SetInsertPoint(allocas);
        builder->CreateBr(entry);
        entry_block = old_entry;
      }
    }

    int num_splits = leaf_block->max_num_elements() / stmt->block_size;
    // traverse leaf node
    create_call("for_each_block",
                {get_context(), tlctx->get_constant(leaf_block->parent->id),
                 tlctx->get_constant(leaf_block->max_num_elements()),
                 tlctx->get_constant(num_splits), body});
  }

  void visit(LoopIndexStmt *stmt) override {
    if (stmt->is_struct_for) {
      stmt->value = builder->CreateLoad(builder->CreateGEP(
          current_coordinates, {tlctx->get_constant(0), tlctx->get_constant(0),
                                tlctx->get_constant(stmt->index)}));
    } else {
      stmt->value = builder->CreateLoad(
          current_offloaded_stmt->loop_vars_llvm[stmt->index]);
    }
  }

  void visit(OffloadedStmt *stmt) override {
    using Type = OffloadedStmt::TaskType;
    init_task_function(stmt);
    if (stmt->task_type == Type::serial) {
      stmt->body->accept(this);
    } else if (stmt->task_type == Type::range_for) {
      create_offload_range_for(stmt);
    } else if (stmt->task_type == Type::struct_for) {
      stmt->block_size =
          std::min(stmt->snode->parent->max_num_elements(), stmt->block_size);
      create_offload_struct_for(stmt);
    } else if (stmt->task_type == Type::listgen) {
      emit_list_gen(stmt);
    } else {
      TC_NOT_IMPLEMENTED
    }
    finalize_task_function();
    current_task->end();
    current_task = nullptr;
  }

  ~CodeGenLLVM() {
    delete builder;
  }
};

#endif

TLANG_NAMESPACE_END

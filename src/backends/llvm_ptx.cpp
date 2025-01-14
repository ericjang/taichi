// A work-in-progress llvm backend

#include <taichi/common/util.h>
#include <taichi/io/io.h>
#include <set>

#include "cuda_context.h"
#include "../util.h"
#include "codegen_cuda.h"
#include "../program.h"
#include "../ir.h"

#if defined(TLANG_WITH_CUDA)
#include "cuda_runtime.h"
#endif

#if defined(TLANG_WITH_LLVM)

#include "codegen_llvm.h"

#endif

TLANG_NAMESPACE_BEGIN

#if defined(TLANG_WITH_LLVM)

using namespace llvm;

// NVVM IR Spec:
// https://docs.nvidia.com/cuda/archive/10.0/pdf/NVVM_IR_Specification.pdf

class CodeGenLLVMGPU : public CodeGenLLVM {
 public:
  int kernel_grid_dim;
  int kernel_block_dim;

  CodeGenLLVMGPU(CodeGenBase *codegen_base, Kernel *kernel)
      : CodeGenLLVM(codegen_base, kernel) {
  }

  void mark_function_as_cuda_kernel(llvm::Function *func) {
    /*******************************************************************
    Example annotation from llvm PTX doc:

    define void @kernel(float addrspace(1)* %A,
                        float addrspace(1)* %B,
                        float addrspace(1)* %C);

    !nvvm.annotations = !{!0}
    !0 = !{void (float addrspace(1)*,
                 float addrspace(1)*,
                 float addrspace(1)*)* @kernel, !"kernel", i32 1}
    *******************************************************************/

    // Mark kernel function as a CUDA __global__ function
    // Add the nvvm annotation that it is considered a kernel function.

    llvm::Metadata *md_args[] = {
        llvm::ValueAsMetadata::get(func),
        MDString::get(*llvm_context, "kernel"),
        llvm::ValueAsMetadata::get(tlctx->get_constant(1))};

    MDNode *md_node = MDNode::get(*llvm_context, md_args);

    module->getOrInsertNamedMetadata("nvvm.annotations")->addOperand(md_node);
  }

  FunctionType compile_module_to_executable() override {
#if defined(TLANG_WITH_CUDA)
    auto offloaded_local = offloaded_tasks;
    for (auto &task : offloaded_local) {
      llvm::Function *func = module->getFunction(task.name);
      TC_ASSERT(func);
      mark_function_as_cuda_kernel(func);
    }

    auto ptx = compile_module_to_ptx(module);
    auto cuda_module = cuda_context.compile(ptx);

    for (auto &task : offloaded_local) {
      task.cuda_func =
          (void *)cuda_context.get_function(cuda_module, task.name);
    }
    return [offloaded_local](Context context) {
      for (auto task : offloaded_local) {
        // TC_INFO("Launching kernel {}<<<{}, {}>>>", task.name, task.grid_dim,
        //    task.block_dim);
        cuda_context.launch((CUfunction)task.cuda_func, &context, task.grid_dim,
                            task.block_dim);
      }
    };
#else
    TC_NOT_IMPLEMENTED;
    return nullptr;
#endif
  }

  void visit(PrintStmt *stmt) override {
    TC_ASSERT(stmt->width() == 1);

    auto value_type = tlctx->get_data_type(stmt->stmt->ret_type.data_type);

    std::string format;

    auto value = stmt->stmt->value;

    if (stmt->stmt->ret_type.data_type == DataType::i32) {
      format = "%d";
    } else if (stmt->stmt->ret_type.data_type == DataType::f32) {
      value_type = llvm::Type::getDoubleTy(*llvm_context);
      value = builder->CreateFPExt(value, value_type);
      format = "%f";
    } else if (stmt->stmt->ret_type.data_type == DataType::f64) {
      format = "%f";
    } else {
      TC_NOT_IMPLEMENTED
    }

    std::vector<llvm::Type *> types{value_type};
    auto stype = llvm::StructType::get(*llvm_context, types, false);
    auto values = builder->CreateAlloca(stype);
    auto value_ptr = builder->CreateGEP(
        values, {tlctx->get_constant(0), tlctx->get_constant(0)});
    builder->CreateStore(value, value_ptr);

    auto format_str = "[debug] " + stmt->str + " = " + format + "\n";

    stmt->value = ModuleBuilder::call(
        builder, "vprintf",
        builder->CreateGlobalStringPtr(format_str, "format_string"),
        builder->CreateBitCast(values,
                               llvm::Type::getInt8PtrTy(*llvm_context)));
  }

  void emit_extra_unary(UnaryOpStmt *stmt) override {
    // functions from libdevice
    auto input = stmt->operand->value;
    auto input_taichi_type = stmt->operand->ret_type.data_type;
    auto input_type = input->getType();
    auto op = stmt->op_type;

#define UNARY_STD(x)                                                        \
  else if (op == UnaryOpType::x) {                                          \
    if (input_taichi_type == DataType::f32) {                               \
      stmt->value =                                                         \
          builder->CreateCall(get_runtime_function("__nv_" #x "f"), input); \
    } else if (input_taichi_type == DataType::f64) {                        \
      stmt->value =                                                         \
          builder->CreateCall(get_runtime_function("__nv_" #x), input);     \
    } else if (input_taichi_type == DataType::i32) {                        \
      stmt->value = builder->CreateCall(get_runtime_function(#x), input);   \
    } else {                                                                \
      TC_NOT_IMPLEMENTED                                                    \
    }                                                                       \
  }
    if (op == UnaryOpType::abs) {
      if (input_taichi_type == DataType::f32) {
        stmt->value =
            builder->CreateCall(get_runtime_function("__nv_fabsf"), input);
      } else if (input_taichi_type == DataType::f64) {
        stmt->value =
            builder->CreateCall(get_runtime_function("__nv_fabs"), input);
      } else if (input_taichi_type == DataType::i32) {
        stmt->value =
            builder->CreateCall(get_runtime_function("__nv_abs"), input);
      } else {
        TC_NOT_IMPLEMENTED
      }
    } else if (op == UnaryOpType::logic_not) {
      if (input_taichi_type == DataType::i32) {
        stmt->value =
            builder->CreateCall(get_runtime_function("logic_not_i32"), input);
      } else {
        TC_NOT_IMPLEMENTED
      }
    }
    UNARY_STD(exp)
    UNARY_STD(log)
    UNARY_STD(tan)
    UNARY_STD(tanh)
    UNARY_STD(sgn)
    else {
      TC_P(unary_op_type_name(op));
      TC_NOT_IMPLEMENTED
    }
#undef UNARY_STD
  }

  void visit(AtomicOpStmt *stmt) override {
    auto mask = stmt->parent->mask();
    for (int l = 0; l < stmt->width(); l++) {
      if (mask) {
        emit("if ({}[{}]) ", mask->raw_name(), l);
      } else {
        TC_ASSERT(stmt->op_type == AtomicOpType::add);
        if (is_integral(stmt->val->ret_type.data_type))
          builder->CreateAtomicRMW(
              llvm::AtomicRMWInst::BinOp::Add, stmt->dest->value,
              stmt->val->value, llvm::AtomicOrdering::SequentiallyConsistent);
        else if (stmt->val->ret_type.data_type == DataType::f32) {
          auto dt = tlctx->get_data_type(DataType::f32);
          builder->CreateIntrinsic(Intrinsic::nvvm_atomic_load_add_f32,
                                   {llvm::PointerType::get(dt, 0)},
                                   {stmt->dest->value, stmt->val->value});
        } else if (stmt->val->ret_type.data_type == DataType::f64) {
          auto dt = tlctx->get_data_type(DataType::f64);
          builder->CreateIntrinsic(Intrinsic::nvvm_atomic_load_add_f64,
                                   {llvm::PointerType::get(dt, 0)},
                                   {stmt->dest->value, stmt->val->value});
        } else {
          TC_NOT_IMPLEMENTED
        }
      }
    }
  }

  void visit(RangeForStmt *for_stmt) override {
    create_naive_range_for(for_stmt);
  }

  void create_offload_range_for(OffloadedStmt *stmt) {
    auto loop_var = create_entry_block_alloca(DataType::i32);
    stmt->loop_vars_llvm.push_back(loop_var);
    auto loop_begin = stmt->begin;
    auto loop_end = stmt->end;
    auto loop_block_dim = stmt->block_size;
    if (loop_block_dim == 0) {
      loop_block_dim = default_gpu_block_size;
    }
    kernel_grid_dim =
        (loop_end - loop_begin + loop_block_dim - 1) / loop_block_dim;
    kernel_block_dim = loop_block_dim;
    BasicBlock *body = BasicBlock::Create(*llvm_context, "loop_body", func);
    BasicBlock *after_loop = BasicBlock::Create(*llvm_context, "block", func);

    auto threadIdx =
        builder->CreateIntrinsic(Intrinsic::nvvm_read_ptx_sreg_tid_x, {}, {});
    auto blockIdx =
        builder->CreateIntrinsic(Intrinsic::nvvm_read_ptx_sreg_ctaid_x, {}, {});
    auto blockDim =
        builder->CreateIntrinsic(Intrinsic::nvvm_read_ptx_sreg_ntid_x, {}, {});

    auto loop_id = builder->CreateAdd(
        tlctx->get_constant(stmt->begin),
        builder->CreateAdd(threadIdx, builder->CreateMul(blockIdx, blockDim)));

    builder->CreateStore(loop_id, loop_var);

    auto cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                                    builder->CreateLoad(loop_var),
                                    tlctx->get_constant(stmt->end));

    builder->CreateCondBr(cond, body, after_loop);
    {
      // body cfg
      builder->SetInsertPoint(body);
      stmt->body->accept(this);
      builder->CreateBr(after_loop);
    }

    builder->SetInsertPoint(after_loop);
  }

  void visit(OffloadedStmt *stmt) override {
#if defined(TLANG_WITH_CUDA)
    using Type = OffloadedStmt::TaskType;
    kernel_grid_dim = 1;
    kernel_block_dim = 1;
    init_task_function(stmt);
    if (stmt->task_type == Type::serial) {
      stmt->body->accept(this);
    } else if (stmt->task_type == Type::range_for) {
      create_offload_range_for(stmt);
    } else if (stmt->task_type == Type::struct_for) {
      int num_SMs;
      cudaDeviceGetAttribute(&num_SMs, cudaDevAttrMultiProcessorCount, 0);
      int max_block_dim;
      cudaDeviceGetAttribute(&max_block_dim, cudaDevAttrMaxBlockDimX, 0);
      kernel_grid_dim = num_SMs * 32;  // each SM can have 16-32 resident blocks
      kernel_block_dim = stmt->block_size;
      if (kernel_block_dim == 0)
        kernel_block_dim = max_block_dim;
      kernel_block_dim =
          std::min(stmt->snode->parent->max_num_elements(), kernel_block_dim);
      stmt->block_size = kernel_block_dim;
      create_offload_struct_for(stmt, true);
    } else if (stmt->task_type == Type::listgen) {
      emit_list_gen(stmt);
    } else {
      TC_NOT_IMPLEMENTED
    }
    finalize_task_function();
    current_task->grid_dim = kernel_grid_dim;
    current_task->block_dim = kernel_block_dim;
    current_task->end();
    current_task = nullptr;
#else
    TC_NOT_IMPLEMENTED
#endif
  }
};

FunctionType GPUCodeGen::codegen_llvm() {
  return CodeGenLLVMGPU(this, kernel).gen();
}

#else

FunctionType GPUCodeGen::codegen_llvm() {
  TC_ERROR("LLVM not found");
}

#endif

TLANG_NAMESPACE_END

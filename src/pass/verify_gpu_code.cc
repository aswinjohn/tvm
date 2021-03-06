/*!
 *  Copyright (c) 2018 by Contributors
 * \file verify_gpu_code.cc
 * \brief Verify the correctness of a GPU IR.
 *        It will check the whether the amount of memory usage or the number of threads
 *        in a block exceeds the limit
 */

#include <tvm/api_registry.h>
#include <tvm/ir.h>
#include <tvm/ir_visitor.h>

namespace tvm {
namespace ir {

class GPUCodeVerifier : public IRVisitor {
 public:
  bool Verify(tvm::Stmt stmt,
              int64_t max_local_memory_per_block,
              int64_t max_shared_memory_per_block,
              int64_t max_thread_per_block,
              int64_t max_thread_x,
              int64_t max_thread_y,
              int64_t max_thread_z) {
    max_local_memory_per_block_ = static_cast<size_t>(max_local_memory_per_block);
    max_shared_memory_per_block_ = static_cast<size_t>(max_shared_memory_per_block);
    max_thread_per_block_ = static_cast<size_t>(max_thread_per_block);
    max_thread_x_ = static_cast<size_t>(max_thread_x);
    max_thread_y_ = static_cast<size_t>(max_thread_y);
    max_thread_z_ = static_cast<size_t>(max_thread_z);

    Reset_();

    this->Visit(stmt);

    return valid_;
  }

  void Visit_(const ProducerConsumer *op) {
    if (nest_level_ == 0) {
      // enter a new kernel, reset statistics
      Reset_();
    }

    if (op->is_producer) {
      nest_level_++;
      IRVisitor::Visit_(op);
      nest_level_--;
    } else {
      IRVisitor::Visit_(op);
    }

    if (nest_level_ == 0) {
      // exit a kernel, check the validity
      valid_ &= thread_per_block_ <= max_thread_per_block_;

      valid_ &= local_memory_per_block_ <= max_local_memory_per_block_;
      valid_ &= shared_memory_per_block_ <= max_shared_memory_per_block_;
    }
  }

  void Visit_(const Allocate *op) {
    IRVisitor::Visit_(op);
    // visit an allocation of a buffer in shared memory, record its size
    if (visited_local_buffers_.count(op->buffer_var.get()) != 0) {
      size_t size = static_cast<size_t>(op->constant_allocation_size());
      local_memory_per_block_ += size * op->type.bytes();
    } else if (visited_shared_buffers_.count(op->buffer_var.get()) != 0) {
      size_t size = static_cast<size_t>(op->constant_allocation_size());
      shared_memory_per_block_ += size * op->type.bytes();
    }
  }

  void Visit_(const AttrStmt *op) {
    if (op->attr_key == attr::storage_scope) {
      if (op->value.as<StringImm>()->value == "local") {
        visited_local_buffers_.insert(op->node.as<tvm::Variable>());
      } else if (op->value.as<StringImm>()->value == "shared") {
        visited_shared_buffers_.insert(op->node.as<tvm::Variable>());
      }
    } else if (op->attr_key == attr::thread_extent) {
      VarExpr var = op->node.as<tvm::IterVarNode>()->var;
      const auto *extent = op->value.as<IntImm>();
      CHECK(extent);

      // record the number of threads in a block
      std::string name = var.get()->name_hint;
      if (name == "threadIdx.x" || name == "threadIdx.y" || name == "threadIdx.z") {
        if (!visited_threads_.count(name)) {
          visited_threads_.insert(name);
          size_t length = static_cast<size_t>(extent->value);
          thread_per_block_ *= length;

          if (name == "threadIdx.x") {
            valid_ &= length <= max_thread_x_;
          } else if (name == "threadIdx.y") {
            valid_ &= length <= max_thread_y_;
          } else if (name == "threadIdx.z") {
            valid_ &= length <= max_thread_z_;
          }
        }
      }
    }
    IRVisitor::Visit_(op);
  }

 private:
  int nest_level_{0};

  std::unordered_set<const tvm::Variable *> visited_local_buffers_;
  std::unordered_set<const tvm::Variable *> visited_shared_buffers_;
  std::unordered_set<std::string> visited_threads_;

  size_t local_memory_per_block_;
  size_t shared_memory_per_block_;
  size_t thread_per_block_;

  size_t max_local_memory_per_block_;
  size_t max_shared_memory_per_block_;
  size_t max_thread_per_block_;
  size_t max_thread_x_, max_thread_y_, max_thread_z_;

  bool valid_{true};

  void Reset_() {
    visited_local_buffers_.clear();
    visited_shared_buffers_.clear();
    local_memory_per_block_ = 0;
    shared_memory_per_block_ = 0;

    visited_threads_.clear();
    thread_per_block_ = 1;
  }
};

bool VerifyGPUCode(Stmt stmt,
                   Map<std::string, Expr> constraints) {
  GPUCodeVerifier verifier;

  auto get_int = [&constraints](std::string key, int64_t def) {
    auto iter = constraints.find(key);
    if (iter != constraints.end()) {
      return ((*iter).second).as<IntImm>()->value;
    } else {
      return def;
    }
  };

  int64_t max_local_memory_per_block = get_int("max_local_memory_per_block", INT64_MAX);
  int64_t max_shared_memory_per_block = get_int("max_shared_memory_per_block", INT64_MAX);
  int64_t max_thread_per_block = get_int("max_thread_per_block", INT64_MAX);
  int64_t max_thread_x = get_int("max_thread_x", INT64_MAX);
  int64_t max_thread_y = get_int("max_thread_y", INT64_MAX);
  int64_t max_thread_z = get_int("max_thread_z", INT64_MAX);

  return verifier.Verify(stmt,
                         max_local_memory_per_block,
                         max_shared_memory_per_block,
                         max_thread_per_block,
                         max_thread_x,
                         max_thread_y,
                         max_thread_z);
}

}  // namespace ir
}  // namespace tvm

/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- host_context.h - CPU thread and memory abstraction -------*- C++ -*-===//
//
// This file declares HostContext.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_HOST_CONTEXT_HOST_CONTEXT_H_
#define TFRT_HOST_CONTEXT_HOST_CONTEXT_H_

#include <type_traits>

#include "llvm/Support/Compiler.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/kernel_registry.h"

namespace tfrt {

class DecodedDiagnostic;

class Chain;
class ConcurrentWorkQueue;
class HostAllocator;
class TypeDescriptor;
class IndirectAsyncValue;
class SharedContext;

// This represents one instance of a CPU device, which can have multiple
// threads, a private heap for tensor data, and a way of reporting errors.  We
// limit the maximum number of HostContext objects that can be created in a
// process to kDummyIndex in order to allow encoding a HostContext
// pointer using only one byte (See HostContextPtr). A HostContext instance is
// expected to be re-used through the life-time of a process, so the limited
// instance numbers are not expected to be a problem in practice.
class HostContext {
  // Extract result type for EnqueueWork and EnqueueBlockingWork.
  template <typename T>
  struct UnwrapExpected {
    using type = T;
  };
  template <typename T>
  struct UnwrapExpected<Expected<T>> {
    using type = T;
  };

  template <typename F>
  using ResultTypeT = typename UnwrapExpected<std::result_of_t<F()>>::type;

 public:
  HostContext(std::function<void(const DecodedDiagnostic&)> diag_handler,
              std::unique_ptr<HostAllocator> allocator,
              std::unique_ptr<ConcurrentWorkQueue> work_queue);
  HostContext(const HostContext&) = delete;
  HostContext& operator=(const HostContext&) = delete;
  ~HostContext();

  KernelRegistry* GetRegistry() { return &registry_; }

  //===--------------------------------------------------------------------===//
  // Error Reporting
  //===--------------------------------------------------------------------===//

  // Emit an error for a specified decoded diagnostic, which gets funneled
  // through a location handler.
  void EmitError(const DecodedDiagnostic& diagnostic);

  // Constructs an AsyncValue that contains an error which can be further
  // propagated.
  RCReference<ErrorAsyncValue> MakeErrorAsyncValueRef(
      DecodedDiagnostic&& diagnostic);

  // Constructs an AsyncValue that contains an error message which can be
  // further propagated.
  RCReference<ErrorAsyncValue> MakeErrorAsyncValueRef(string_view message);

  std::function<void(const DecodedDiagnostic&)> diag_handler() {
    return diag_handler_;
  }

  //===--------------------------------------------------------------------===//
  // Cancel the current execution
  //===--------------------------------------------------------------------===//

  // Cancel the current BEF Execution. This transitions HostContext to the
  // canceled state, which causes all asynchronously executing threads to be
  // canceled when they check the cancellation state (e.g. in BEFExecutor).
  void CancelExecution(string_view msg);

  // Restart() transitions HostContext from the canceled state to the normal
  // execution state.
  void Restart();

  // When HostContext is in the canceled state, GetCancelAsyncValue
  // returns an non-null AsyncValue containing the message for the
  // cancellation. Otherwise, it returns nullptr.
  AsyncValue* GetCancelAsyncValue() const {
    return cancel_value_.load(std::memory_order_acquire);
  }
  AsyncValueRef<Chain> GetReadyChain() const { return ready_chain_.CopyRef(); }

  //===--------------------------------------------------------------------===//
  // Memory Management
  //===--------------------------------------------------------------------===//

  HostAllocator* allocator() { return allocator_.get(); }

  // Allocate the specified number of bytes with the specified alignment.
  void* AllocateBytes(size_t size, size_t alignment);

  // Deallocate the specified pointer that had the specified size.
  void DeallocateBytes(void* ptr, size_t size);

  // Allocate memory for one or more entries of type T.
  template <typename T>
  T* Allocate(size_t num_elements = 1) {
    return static_cast<T*>(AllocateBytes(sizeof(T) * num_elements, alignof(T)));
  }

  // Deallocate the memory for one or more entries of type T.
  template <typename T>
  void Deallocate(T* ptr, size_t num_elements = 1) {
    DeallocateBytes(ptr, sizeof(T) * num_elements);
  }

  // Allocate and initialize an object of type T.
  template <typename T, typename... Args>
  T* Construct(Args&&... args) {
    T* buf = Allocate<T>();
    return new (buf) T(std::forward<Args>(args)...);
  }

  // Destruct and deallocate space for an object of type T.
  template <typename T>
  void Destruct(T* t) {
    t->~T();
    Deallocate(t);
  }

  // Allocate an unconstructed AsyncValueRef. The AsyncValueRef should be made
  // available later by invoking AsyncValueRef::emplace or
  // AsyncValueRef::SetError.
  //
  // TODO(lauj, jingdong) Move MakeUnconstructedAsyncValueRef and
  // MakeConcreteAsyncValueRef to async_value_ref.h. These methods should not
  // depend on HostContext.
  template <typename T>
  AsyncValueRef<T> MakeUnconstructedAsyncValueRef();

  // Allocate and construct an AsyncValueRef without making it available for
  // consumption. The AsyncValueRef should be made available later by invoking
  // AsyncValueRef::SetStateConcrete or AsyncValueRef::SetError.
  template <typename T, typename... Args>
  AsyncValueRef<T> MakeConstructedAsyncValueRef(Args&&... args);

  // Allocate and construct an available AsyncValueRef.
  template <typename T, typename... Args>
  AsyncValueRef<T> MakeConcreteAsyncValueRef(Args&&... args);

  // Construct an empty IndirectAsyncValue, not forwarding to anything.
  RCReference<IndirectAsyncValue> MakeIndirectAsyncValue();

  //===--------------------------------------------------------------------===//
  // Concurrency
  //===--------------------------------------------------------------------===//

  // Block until the specified values are available (either with a value or an
  // error result).
  //
  // This should not be called by a thread managed by the work queue.
  void Await(ArrayRef<RCReference<AsyncValue>> values);

  // Block until the system is quiescent (no pending work and no inflight work).
  //
  // This should not be called by a thread managed by the work queue.
  void Quiesce();

  // Add some non-blocking work to the work_queue managed by this CPU device.
  void EnqueueWork(llvm::unique_function<void()> work);

  // Add some non-blocking work to the work_queue managed by this CPU device.
  // Return AsyncValueRef<R> for work that returns R. R cannot be void.
  //
  // Example:
  // int a = 1, b = 2;
  // AsyncValueRef<int> r = host->EnqueueWork([a, b] { return a + b; });
  template <typename F, typename R = ResultTypeT<F>,
            std::enable_if_t<!std::is_void<R>(), int> = 0>
  LLVM_NODISCARD AsyncValueRef<R> EnqueueWork(F&& work);

  // Add some blocking work to the work_queue managed by this CPU device.
  LLVM_NODISCARD bool EnqueueBlockingWork(llvm::unique_function<void()> work);

  // Add some blocking work to the work_queue managed by this CPU device.
  template <typename F, typename R = ResultTypeT<F>,
            std::enable_if_t<!std::is_void<R>(), int> = 0>
  LLVM_NODISCARD AsyncValueRef<R> EnqueueBlockingWork(F&& work);

  // Returns the number of worker threads in the work_queue managed by this CPU
  // device. This does not include any additional threads that might have been
  // created to handle blocking work (enqueued by EnqueueBlockingWork).
  int GetNumWorkerThreads() const;

  // Run the specified function when the specified set of AsyncValue's are all
  // resolved.  This is a set-version of "AndThen".
  void RunWhenReady(ArrayRef<AsyncValue*> values,
                    llvm::unique_function<void()>&& callee);

  // Calls `compute` in parallel for non-overlapping subranges [start, end) in
  // the [0, n) range. When all subtasks completed, calls `on_done` callback.
  void ParallelFor(size_t n,
                   llvm::unique_function<void(size_t, size_t)> compute,
                   llvm::unique_function<void()> on_done,
                   size_t min_block_size = 1);

  //===--------------------------------------------------------------------===//
  // Shared context
  //===--------------------------------------------------------------------===//
  // Get the shared context instance managed by the host context. Create one, if
  // the shared context instance does not exist yet.
  template <typename SharedContextType>
  SharedContextType& GetOrCreateSharedContext();

  //===--------------------------------------------------------------------===//
  // Kernel Registry
  //===--------------------------------------------------------------------===//
  const KernelRegistry& GetKernelRegistry() { return registry_; }

 private:
  friend class HostContextPtr;

  // Factory function for creating a SharedContext.
  using SharedContextFactory = std::unique_ptr<SharedContext> (*)(HostContext*);

  class SharedContextManager;

  // Dense ID for different shared context types.
  template <typename SharedContextType>
  static int DenseIdForSharedContext();

  static std::atomic<int> num_shared_context_types_;
  // kDummyIndex is reserved as a dummy index in HostContextPtr.
  static HostContext* all_host_contexts_[HostContextPtr::kDummyIndex];

  static HostContext* GetHostContextByIndex(int index) {
    assert(index < HostContextPtr::kDummyIndex);
    assert(all_host_contexts_[index]);
    return all_host_contexts_[index];
  }

  // Index into all_host_contexts_
  int instance_index() const { return instance_ptr_.index(); }
  HostContextPtr instance_ptr() const { return instance_ptr_; }

  SharedContext& GetOrCreateSharedContext(int shared_context_id,
                                          SharedContextFactory factory);

  std::atomic<AsyncValue*> cancel_value_{nullptr};
  // Store a ready chain in HostContext to avoid repeated creations of ready
  // chains on the heap.
  AsyncValueRef<Chain> ready_chain_;
  KernelRegistry registry_;
  std::function<void(const DecodedDiagnostic&)> diag_handler_;
  std::unique_ptr<HostAllocator> allocator_;
  std::unique_ptr<ConcurrentWorkQueue> work_queue_;

  std::unique_ptr<SharedContextManager> shared_context_mgr_;
  const HostContextPtr instance_ptr_;
};

template <typename T, typename... Args>
AsyncValueRef<T> HostContext::MakeConstructedAsyncValueRef(Args&&... args) {
  return AsyncValueRef<T>(TakeRef(Construct<internal::ConcreteAsyncValue<T>>(
      instance_ptr_,
      typename internal::ConcreteAsyncValue<T>::ConstructedPayload{},
      std::forward<Args>(args)...)));
}

template <typename T, typename... Args>
AsyncValueRef<T> HostContext::MakeConcreteAsyncValueRef(Args&&... args) {
  return AsyncValueRef<T>(TakeRef(Construct<internal::ConcreteAsyncValue<T>>(
      instance_ptr_,
      typename internal::ConcreteAsyncValue<T>::ConcretePayload{},
      std::forward<Args>(args)...)));
}

template <typename T>
AsyncValueRef<T> HostContext::MakeUnconstructedAsyncValueRef() {
  return AsyncValueRef<T>(TakeRef(Construct<internal::ConcreteAsyncValue<T>>(
      instance_ptr_,
      typename internal::ConcreteAsyncValue<T>::UnconstructedPayload{})));
}

template <typename SharedContextType>
SharedContextType& HostContext::GetOrCreateSharedContext() {
  int shared_context_id = DenseIdForSharedContext<SharedContextType>();
  auto factory = [](HostContext* host) -> std::unique_ptr<SharedContext> {
    return std::make_unique<SharedContextType>(host);
  };
  return static_cast<SharedContextType&>(
      GetOrCreateSharedContext(shared_context_id, factory));
}

template <typename SharedContextType>
int HostContext::DenseIdForSharedContext() {
  static int id = num_shared_context_types_++;
  return id;
}

template <typename F, typename R, std::enable_if_t<!std::is_void<R>(), int>>
AsyncValueRef<R> HostContext::EnqueueWork(F&& work) {
  auto result = this->MakeUnconstructedAsyncValueRef<R>();
  this->EnqueueWork(
      [result = result.CopyRef(), work = std::forward<F>(work)]() mutable {
        result.emplace(work());
      });
  return result;
}

template <typename F, typename R, std::enable_if_t<!std::is_void<R>(), int>>
AsyncValueRef<R> HostContext::EnqueueBlockingWork(F&& work) {
  auto result = this->MakeUnconstructedAsyncValueRef<R>();
  bool enqueued = this->EnqueueBlockingWork(
      [result = result.CopyRef(), work = std::forward<F>(work)]() mutable {
        result.emplace(work());
      });
  if (!enqueued) {
    result.SetError("Failed to enqueue blocking work.");
  }
  return result;
}
}  // namespace tfrt

#endif  // TFRT_HOST_CONTEXT_HOST_CONTEXT_H_

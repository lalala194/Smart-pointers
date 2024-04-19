#pragma once
#include <iostream>
#include <memory>
#include <type_traits>
#include <typeinfo>

template <typename Alloc>
class AllocatorDestructor {
  using alloc_traits = std::allocator_traits<Alloc>;

 public:
  using pointer = typename alloc_traits::pointer;
  using size_type = typename alloc_traits::size_type;
  AllocatorDestructor(Alloc& alloc, size_type size) noexcept
      : alloc_(alloc), size_(size) {}
  void operator()(pointer ptr) noexcept {
    alloc_traits::deallocate(alloc_, ptr, size_);
  }

 private:
  Alloc& alloc_;
  size_type size_;
};

class SharedCount {
 public:
  explicit SharedCount(size_t count = 0) noexcept : shared_owners_(count) {}
  size_t use_count() const noexcept { return shared_owners_; }
  void add_shared() noexcept { ++shared_owners_; }
  void release_shared() noexcept {
    if (shared_owners_ == 0) {
      zero_shared();
    }
  }
  virtual ~SharedCount() = default;
  virtual void zero_shared() noexcept {};

 protected:
  size_t shared_owners_;
};

class SharedWeakCount : public SharedCount {
 public:
  explicit SharedWeakCount(size_t count = 0) noexcept
      : SharedCount(count), shared_weak_owners_(count) {}

  size_t use_count() const noexcept { return SharedCount::use_count(); }
  void add_shared() noexcept { SharedCount::add_shared(); }
  void add_weak() noexcept { ++shared_weak_owners_; }
  void decrement_weak() noexcept {
    if (shared_weak_owners_ > 0) {
      --shared_weak_owners_;
    }
  }
  void decrement_shared() noexcept {
    if (shared_owners_ > 0) {
      --shared_owners_;
    }
  }
  void release_shared() noexcept { SharedCount::release_shared(); }
  void release_weak() noexcept {
    if (shared_weak_owners_ == 0) {
      zero_shared_and_weak();
    }
  }
  virtual ~SharedWeakCount() = default;
  virtual void zero_shared_and_weak() noexcept = 0;

 private:
  size_t shared_weak_owners_ = 0;
};

template <typename T, typename Deleter, typename Alloc>
class SharedPtrPointer : public SharedWeakCount {
 public:
  explicit SharedPtrPointer(T value, Deleter del, Alloc alloc)
      : value_(std::move(value)),
        deleter_(std::move(del)),
        allocator_(std::move(alloc)) {}

  void zero_shared() noexcept override;
  void zero_shared_and_weak() noexcept override;

 private:
  T value_;
  Deleter deleter_;
  Alloc allocator_;
};

template <typename T, typename Alloc>
struct SharedPtrEmplacer : SharedWeakCount {
  template <typename... Args>
  explicit SharedPtrEmplacer(Alloc alloc, Args&&... args);
  Alloc* get_alloc() noexcept { return storage_.get_alloc(); }
  T* get_elem() noexcept { return storage_.get_elem(); }
  void zero_shared() noexcept override;
  void zero_shared_and_weak() noexcept override;
  ~SharedPtrEmplacer() override = default;

 private:
  using pair = std::pair<Alloc, T>;
  struct alignas(pair) Storage {
    Alloc* get_alloc() noexcept;
    explicit Storage(Alloc&& alloc);
    ~Storage();
    T* get_elem() noexcept;
    char pair_storage[sizeof(pair)];
  };

  static_assert(alignof(Storage) == alignof(pair), "not aligned");
  static_assert(sizeof(Storage) == sizeof(pair), "not equal size");
  Storage storage_;
};

template <typename T, typename Deleter, typename Alloc>
void SharedPtrPointer<T, Deleter, Alloc>::zero_shared() noexcept {
  T* value_ptr = std::addressof(value_);
  deleter_(*value_ptr);
  deleter_.~Deleter();
}

template <typename T, typename Deleter, typename Alloc>
void SharedPtrPointer<T, Deleter, Alloc>::zero_shared_and_weak() noexcept {
  using custom_alloc = typename std::allocator_traits<
      Alloc>::template rebind_alloc<SharedPtrPointer>;
  using custom_traits = std::allocator_traits<custom_alloc>;
  using pointer_traits = std::pointer_traits<typename custom_traits::pointer>;
  custom_alloc alloc(allocator_);
  allocator_.~Alloc();
  alloc.deallocate(pointer_traits::pointer_to(*this), 1);
}

template <typename T, typename Alloc>
Alloc* SharedPtrEmplacer<T, Alloc>::Storage::get_alloc() noexcept {
  pair* pair_ptr = reinterpret_cast<pair*>(pair_storage);
  typename pair::first_type* first = std::addressof(pair_ptr->first);
  Alloc* alloc = reinterpret_cast<Alloc*>(first);
  return alloc;
}

template <typename T, typename Alloc>
T* SharedPtrEmplacer<T, Alloc>::Storage::get_elem() noexcept {
  pair* pair_ptr = reinterpret_cast<pair*>(pair_storage);
  typename pair::second_type* second = std::addressof(pair_ptr->second);
  T* elem = reinterpret_cast<T*>(second);
  return elem;
}

template <typename T, typename Alloc>
SharedPtrEmplacer<T, Alloc>::Storage::Storage(Alloc&& alloc) {
  ::new ((void*)get_alloc()) Alloc(std::move(alloc));
}

template <typename T, typename Alloc>
SharedPtrEmplacer<T, Alloc>::Storage::~Storage() {
  get_alloc()->~Alloc();
}

template <typename T, typename Alloc>
template <typename... Args>
SharedPtrEmplacer<T, Alloc>::SharedPtrEmplacer(Alloc alloc, Args&&... args)
    : storage_(std::move(alloc)) {
  using type_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;
  type_alloc temp(*get_alloc());
  std::allocator_traits<type_alloc>::construct(temp, get_elem(),
                                               std::forward<Args>(args)...);
}

template <typename T, typename Alloc>
void SharedPtrEmplacer<T, Alloc>::zero_shared() noexcept {
  using type_alloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;
  type_alloc temp(*get_alloc());
  std::allocator_traits<type_alloc>::destroy(temp, get_elem());
}

template <typename T, typename Alloc>
void SharedPtrEmplacer<T, Alloc>::zero_shared_and_weak() noexcept {
  using control_block_alloc = typename std::allocator_traits<
      Alloc>::template rebind_alloc<SharedPtrEmplacer>;
  using control_block_pointer =
      typename std::allocator_traits<control_block_alloc>::pointer;
  control_block_alloc temp(*get_alloc());
  storage_.~Storage();
  std::allocator_traits<control_block_alloc>::deallocate(
      temp, std::pointer_traits<control_block_pointer>::pointer_to(*this), 1);
}

template <typename T>
class SharedPtr {
 public:
  SharedPtr() noexcept {}
  SharedPtr(std::nullptr_t) {}
  template <typename Y>
  explicit SharedPtr(Y* ptr);
  SharedPtr(const SharedPtr& other) noexcept;
  SharedPtr(SharedPtr&& other) noexcept;
  template <typename Y>
  SharedPtr(const SharedPtr<Y>& other) noexcept;
  template <typename Y>
  SharedPtr(SharedPtr<Y>&& other) noexcept;
  template <typename Y, typename Deleter>
  SharedPtr(Y* ptr, Deleter del);
  template <typename Y, typename Deleter, typename Alloc>
  SharedPtr(Y* ptr, Deleter del, Alloc alloc);
  SharedPtr& operator=(const SharedPtr& other) noexcept;
  SharedPtr& operator=(SharedPtr&& other) noexcept;
  template <typename Y>
  SharedPtr<T>& operator=(const SharedPtr<Y>& other) noexcept;
  template <typename Y>
  SharedPtr<T>& operator=(SharedPtr<Y>&& other) noexcept;
  ~SharedPtr();
  size_t use_count() const noexcept;
  T* get() const noexcept;
  typename std::add_lvalue_reference<T>::type operator*() const noexcept;
  T* operator->() const noexcept;
  void reset() noexcept;

 private:
  template <typename Y, typename ControlBlock>
  static SharedPtr create_with_control_block(Y* ptr,
                                             ControlBlock* block) noexcept;
  template <typename U>
  struct SharedPtrDefaultAllocator {
    using type = std::allocator<U>;
  };

  template <typename, typename U>
  struct SharedPtrDefaultDeleter : std::default_delete<U> {};

  template <typename U>
  friend class WeakPtr;

  template <typename Y, typename Alloc, typename... Args>
  friend SharedPtr<Y> AllocateShared(const Alloc& alloc, Args&&... args);

  template <typename U>
  friend class SharedPtr;

  void swap(SharedPtr& other) noexcept;
  T* element_ptr_ = nullptr;
  SharedWeakCount* control_ptr_ = nullptr;
};

template <typename T>
template <typename Y>
SharedPtr<T>::SharedPtr(Y* ptr) : element_ptr_(ptr) {
  using alloc_t = typename SharedPtrDefaultAllocator<Y>::type;
  using control_block =
      SharedPtrPointer<Y*, SharedPtrDefaultDeleter<T, Y>, alloc_t>;
  control_ptr_ =
      new control_block(ptr, SharedPtrDefaultDeleter<T, Y>(), alloc_t());
  control_ptr_->add_shared();
}

template <typename T>
template <typename Y, typename Deleter>
SharedPtr<T>::SharedPtr(Y* ptr, Deleter del) : element_ptr_(ptr) {
  try {
    using alloc_t = typename SharedPtrDefaultAllocator<Y>::type;
    using control_block = SharedPtrPointer<Y*, Deleter, alloc_t>;
    control_ptr_ = new control_block(ptr, std::move(del), alloc_t());
    control_ptr_->add_shared();
  } catch (...) {
    del(ptr);
    throw;
  }
}

template <typename T>
template <typename Y, typename Deleter, typename Alloc>
SharedPtr<T>::SharedPtr(Y* ptr, Deleter del, Alloc alloc) : element_ptr_(ptr) {
  using control_block = SharedPtrPointer<Y*, Deleter, Alloc>;
  using rebinded_alloc = typename std::allocator_traits<
      Alloc>::template rebind_alloc<control_block>;
  using destructor = AllocatorDestructor<rebinded_alloc>;
  using rebinded_traits = std::allocator_traits<rebinded_alloc>;
  rebinded_alloc rebinded(alloc);
  control_ptr_ = rebinded_traits::allocate(rebinded, 1);
  try {
    rebinded_traits::construct(rebinded,
                               reinterpret_cast<control_block*>(control_ptr_),
                               control_block(ptr, std::move(del), alloc));
    control_ptr_->add_shared();
  } catch (...) {
    rebinded_traits::deallocate(
        rebinded, reinterpret_cast<control_block*>(control_ptr_), 1);
    del(ptr);
    throw;
  }
}

template <typename T>
SharedPtr<T>::SharedPtr(const SharedPtr& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  if (control_ptr_ != nullptr) {
    control_ptr_->add_shared();
  }
}

template <typename T>
template <typename Y>
SharedPtr<T>::SharedPtr(const SharedPtr<Y>& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  if (control_ptr_ != nullptr) {
    control_ptr_->add_shared();
  }
}

template <typename T>
SharedPtr<T>::SharedPtr(SharedPtr&& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  other.element_ptr_ = nullptr;
  other.control_ptr_ = nullptr;
}

template <typename T>
template <typename Y>
SharedPtr<T>::SharedPtr(SharedPtr<Y>&& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  other.element_ptr_ = nullptr;
  other.control_ptr_ = nullptr;
}

template <typename T>
SharedPtr<T>::~SharedPtr() {
  if (control_ptr_ != nullptr) {
    control_ptr_->decrement_shared();
    control_ptr_->release_shared();
    if (control_ptr_->use_count() == 0) {
      control_ptr_->release_weak();
    }
  }
  control_ptr_ = nullptr;
}

template <typename T>
void SharedPtr<T>::swap(SharedPtr<T>& other) noexcept {
  std::swap(element_ptr_, other.element_ptr_);
  std::swap(control_ptr_, other.control_ptr_);
}

template <typename T>
SharedPtr<T>& SharedPtr<T>::operator=(const SharedPtr& other) noexcept {
  SharedPtr(other).swap(*this);
  return *this;
}

template <typename T>
template <typename Y>
SharedPtr<T>& SharedPtr<T>::operator=(const SharedPtr<Y>& other) noexcept {
  SharedPtr(other).swap(*this);
  return *this;
}

template <typename T>
SharedPtr<T>& SharedPtr<T>::operator=(SharedPtr<T>&& other) noexcept {
  SharedPtr(std::move(other)).swap(*this);
  return *this;
}

template <typename T>
template <typename Y>
SharedPtr<T>& SharedPtr<T>::operator=(SharedPtr<Y>&& other) noexcept {
  SharedPtr(std::move(other)).swap(*this);
  return *this;
}

template <typename T>
void SharedPtr<T>::reset() noexcept {
  SharedPtr().swap(*this);
}

template <typename T>
T* SharedPtr<T>::get() const noexcept {
  return element_ptr_;
}

template <typename T>
typename std::add_lvalue_reference<T>::type SharedPtr<T>::operator*()
    const noexcept {
  return *element_ptr_;
}

template <typename T>
T* SharedPtr<T>::operator->() const noexcept {
  return element_ptr_;
}

template <typename T>
size_t SharedPtr<T>::use_count() const noexcept {
  return control_ptr_ != nullptr ? control_ptr_->use_count() : 0;
}

template <typename T>
template <typename Y, typename ControlBlock>
SharedPtr<T> SharedPtr<T>::create_with_control_block(
    Y* ptr, ControlBlock* block) noexcept {
  SharedPtr<T> smart_ptr;
  smart_ptr.element_ptr_ = ptr;
  smart_ptr.control_ptr_ = block;
  smart_ptr.control_ptr_->add_shared();
  return smart_ptr;
}

template <typename T, typename Alloc, typename... Args>
SharedPtr<T> AllocateShared(const Alloc& alloc, Args&&... args) {
  using control_block = SharedPtrEmplacer<T, Alloc>;
  using cntrl_allocator = typename std::allocator_traits<
      Alloc>::template rebind_alloc<control_block>;
  using cntrl_traits = std::allocator_traits<cntrl_allocator>;
  cntrl_allocator control_alloc(alloc);
  control_block* block = reinterpret_cast<control_block*>(
      cntrl_traits::allocate(control_alloc, 1));
  ::new (reinterpret_cast<control_block*>(block))
      control_block(alloc, std::forward<Args>(args)...);
  return SharedPtr<T>::create_with_control_block((*block).get_elem(),
                                                 std::addressof(*block));
}

template <typename T, typename... Args>
SharedPtr<T> MakeShared(Args&&... args) {
  return AllocateShared<T>(std::allocator<T>(), std::forward<Args>(args)...);
}

template <typename T>
class WeakPtr {
 public:
  WeakPtr() noexcept : element_ptr_(nullptr), control_ptr_(nullptr) {}
  WeakPtr(const WeakPtr& other) noexcept;
  template <typename Y>
  WeakPtr(const WeakPtr<Y>& other) noexcept;
  WeakPtr(WeakPtr&& other) noexcept;
  template <typename Y>
  WeakPtr(WeakPtr<Y>&& other) noexcept;
  ~WeakPtr();
  template <typename Y>
  WeakPtr(const SharedPtr<Y>& other) noexcept;
  WeakPtr& operator=(const WeakPtr& other) noexcept;
  WeakPtr& operator=(WeakPtr&& other) noexcept;
  template <typename Y>
  WeakPtr& operator=(const WeakPtr<Y>& other) noexcept;
  template <typename Y>
  WeakPtr& operator=(WeakPtr<Y>&& other) noexcept;
  bool expired() const noexcept {
    return control_ptr_ == nullptr || control_ptr_->use_count() == 0;
  }
  SharedPtr<T> lock() const noexcept;

 private:
  template <typename Y>
  friend class SharedPtr;
  void swap(WeakPtr& other) noexcept;
  T* element_ptr_ = nullptr;
  SharedWeakCount* control_ptr_ = nullptr;
};

template <typename T>
WeakPtr<T>::WeakPtr(const WeakPtr& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  if (control_ptr_ != nullptr) {
    control_ptr_->add_weak();
  }
}
template <typename T>
template <typename Y>
WeakPtr<T>::WeakPtr(const WeakPtr<Y>& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  if (control_ptr_ != nullptr) {
    control_ptr_->add_weak();
  }
}

template <typename T>
WeakPtr<T>::WeakPtr(WeakPtr&& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  other.element_ptr_ = nullptr;
  other.control_ptr_ = nullptr;
}

template <typename T>
template <typename Y>
WeakPtr<T>::WeakPtr(WeakPtr<Y>&& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  other.element_ptr_ = nullptr;
  other.control_ptr_ = nullptr;
}

template <typename T>
WeakPtr<T>::~WeakPtr() {
  if (control_ptr_ != nullptr) {
    control_ptr_->decrement_weak();
    if (control_ptr_->use_count() == 0) {
      control_ptr_->release_weak();
    }
  }
  control_ptr_ = nullptr;
}

template <typename T>
template <typename Y>
WeakPtr<T>::WeakPtr(const SharedPtr<Y>& other) noexcept
    : element_ptr_(other.element_ptr_), control_ptr_(other.control_ptr_) {
  if (control_ptr_ != nullptr) {
    control_ptr_->add_weak();
  }
}

template <typename T>
void WeakPtr<T>::swap(WeakPtr<T>& other) noexcept {
  std::swap(element_ptr_, other.element_ptr_);
  std::swap(control_ptr_, other.control_ptr_);
}

template <typename T>
WeakPtr<T>& WeakPtr<T>::operator=(const WeakPtr<T>& other) noexcept {
  WeakPtr(other).swap(*this);
  return *this;
}

template <typename T>
WeakPtr<T>& WeakPtr<T>::operator=(WeakPtr<T>&& other) noexcept {
  WeakPtr(std::move(other)).swap(*this);
  return *this;
}

template <typename T>
template <typename Y>
WeakPtr<T>& WeakPtr<T>::operator=(WeakPtr<Y>&& other) noexcept {
  WeakPtr(std::move(other)).swap(*this);
}

template <typename T>
template <typename Y>
WeakPtr<T>& WeakPtr<T>::operator=(const WeakPtr<Y>& other) noexcept {
  WeakPtr(other).swap(*this);
  return *this;
}

template <typename T>
SharedPtr<T> WeakPtr<T>::lock() const noexcept {
  SharedPtr<T> smart_ptr;
  smart_ptr.control_ptr_ = (control_ptr_ != nullptr) ? control_ptr_ : nullptr;
  if (smart_ptr.control_ptr_ != nullptr) {
    smart_ptr.element_ptr_ = element_ptr_;
  }
  return smart_ptr;
}

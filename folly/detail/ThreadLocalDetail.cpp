/*
 * Copyright 2015-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <folly/detail/ThreadLocalDetail.h>
#include <folly/synchronization/CallOnce.h>

#include <list>
#include <mutex>

namespace folly { namespace threadlocal_detail {

StaticMetaBase::StaticMetaBase(ThreadEntry* (*threadEntry)(), bool strict)
    : nextId_(1), threadEntry_(threadEntry), strict_(strict) {
  head_.next = head_.prev = &head_;
  int ret = pthread_key_create(&pthreadKey_, &onThreadExit);
  checkPosixError(ret, "pthread_key_create failed");
  PthreadKeyUnregister::registerKey(pthreadKey_);
}

ThreadEntryList* StaticMetaBase::getThreadEntryList() {
#ifdef FOLLY_TLD_USE_FOLLY_TLS
  static FOLLY_TLS ThreadEntryList threadEntryListSingleton;
  return &threadEntryListSingleton;
#else
  static pthread_key_t pthreadKey;
  static folly::once_flag onceFlag;
  folly::call_once(onceFlag, [&]() {
    int ret = pthread_key_create(&pthreadKey, nullptr);
    checkPosixError(ret, "pthread_key_create failed");
    PthreadKeyUnregister::registerKey(pthreadKey);
  });

  ThreadEntryList* threadEntryList =
      static_cast<ThreadEntryList*>(pthread_getspecific(pthreadKey));

  if (UNLIKELY(!threadEntryList)) {
    threadEntryList = new ThreadEntryList();
    int ret = pthread_setspecific(pthreadKey, threadEntryList);
    checkPosixError(ret, "pthread_setspecific failed");
  }

  return threadEntryList;
#endif
}

void StaticMetaBase::onThreadExit(void* ptr) {
  auto threadEntry = static_cast<ThreadEntry*>(ptr);

  {
    auto& meta = *threadEntry->meta;

    // Make sure this ThreadEntry is available if ThreadLocal A is accessed in
    // ThreadLocal B destructor.
    pthread_setspecific(meta.pthreadKey_, threadEntry);
    SharedMutex::ReadHolder rlock(nullptr);
    if (meta.strict_) {
      rlock = SharedMutex::ReadHolder(meta.accessAllThreadsLock_);
    }
    {
      std::lock_guard<std::mutex> g(meta.lock_);
      meta.erase(&(*threadEntry));
      // No need to hold the lock any longer; the ThreadEntry is private to this
      // thread now that it's been removed from meta.
    }
    // NOTE: User-provided deleter / object dtor itself may be using ThreadLocal
    // with the same Tag, so dispose() calls below may (re)create some of the
    // elements or even increase elementsCapacity, thus multiple cleanup rounds
    // may be required.
    for (bool shouldRun = true; shouldRun;) {
      shouldRun = false;
      FOR_EACH_RANGE (i, 0, threadEntry->elementsCapacity) {
        if (threadEntry->elements[i].dispose(TLPDestructionMode::THIS_THREAD)) {
          shouldRun = true;
        }
      }
    }
    pthread_setspecific(meta.pthreadKey_, nullptr);
  }

  auto threadEntryList = threadEntry->list;
  DCHECK_GT(threadEntryList->count, 0u);

  --threadEntryList->count;

  if (threadEntryList->count) {
    return;
  }

  // dispose all the elements
  for (bool shouldRunOuter = true; shouldRunOuter;) {
    shouldRunOuter = false;
    auto tmp = threadEntryList->head;
    while (tmp) {
      auto& meta = *tmp->meta;
      pthread_setspecific(meta.pthreadKey_, tmp);
      SharedMutex::ReadHolder rlock(nullptr);
      if (meta.strict_) {
        rlock = SharedMutex::ReadHolder(meta.accessAllThreadsLock_);
      }
      for (bool shouldRunInner = true; shouldRunInner;) {
        shouldRunInner = false;
        FOR_EACH_RANGE (i, 0, tmp->elementsCapacity) {
          if (tmp->elements[i].dispose(TLPDestructionMode::THIS_THREAD)) {
            shouldRunInner = true;
            shouldRunOuter = true;
          }
        }
      }
      pthread_setspecific(meta.pthreadKey_, nullptr);
      tmp = tmp->listNext;
    }
  }

  // free the entry list
  auto head = threadEntryList->head;
  threadEntryList->head = nullptr;
  while (head) {
    auto tmp = head;
    head = head->listNext;
    if (tmp->elements) {
      free(tmp->elements);
      tmp->elements = nullptr;
      tmp->elementsCapacity = 0;
    }

#ifndef FOLLY_TLD_USE_FOLLY_TLS
    delete tmp;
#endif
  }

#ifndef FOLLY_TLD_USE_FOLLY_TLS
  delete threadEntryList;
#endif
}

uint32_t StaticMetaBase::allocate(EntryID* ent) {
  uint32_t id;
  auto& meta = *this;
  std::lock_guard<std::mutex> g(meta.lock_);

  id = ent->value.load();
  if (id != kEntryIDInvalid) {
    return id;
  }

  if (!meta.freeIds_.empty()) {
    id = meta.freeIds_.back();
    meta.freeIds_.pop_back();
  } else {
    id = meta.nextId_++;
  }

  uint32_t old_id = ent->value.exchange(id);
  DCHECK_EQ(old_id, kEntryIDInvalid);
  return id;
}

void StaticMetaBase::destroy(EntryID* ent) {
  try {
    auto& meta = *this;

    // Elements in other threads that use this id.
    std::vector<ElementWrapper> elements;

    {
      SharedMutex::WriteHolder wlock(nullptr);
      if (meta.strict_) {
        /*
         * In strict mode, the logic guarantees per-thread instances are
         * destroyed by the moment ThreadLocal<> dtor returns.
         * In order to achieve that, we should wait until concurrent
         * onThreadExit() calls (that might acquire ownership over per-thread
         * instances in order to destroy them) are finished.
         */
        wlock = SharedMutex::WriteHolder(meta.accessAllThreadsLock_);
      }

      {
        std::lock_guard<std::mutex> g(meta.lock_);
        uint32_t id = ent->value.exchange(kEntryIDInvalid);
        if (id == kEntryIDInvalid) {
          return;
        }

        for (ThreadEntry* e = meta.head_.next; e != &meta.head_; e = e->next) {
          if (id < e->elementsCapacity && e->elements[id].ptr) {
            elements.push_back(e->elements[id]);

            /*
             * Writing another thread's ThreadEntry from here is fine;
             * the only other potential reader is the owning thread --
             * from onThreadExit (which grabs the lock, so is properly
             * synchronized with us) or from get(), which also grabs
             * the lock if it needs to resize the elements vector.
             *
             * We can't conflict with reads for a get(id), because
             * it's illegal to call get on a thread local that's
             * destructing.
             */
            e->elements[id].ptr = nullptr;
            e->elements[id].deleter1 = nullptr;
            e->elements[id].ownsDeleter = false;
          }
        }
        meta.freeIds_.push_back(id);
      }
    }
    // Delete elements outside the locks.
    for (ElementWrapper& elem : elements) {
      elem.dispose(TLPDestructionMode::ALL_THREADS);
    }
  } catch (...) { // Just in case we get a lock error or something anyway...
    LOG(WARNING) << "Destructor discarding an exception that was thrown.";
  }
}

/**
 * Reserve enough space in the ThreadEntry::elements for the item
 * @id to fit in.
 */
void StaticMetaBase::reserve(EntryID* id) {
  auto& meta = *this;
  ThreadEntry* threadEntry = (*threadEntry_)();
  size_t prevCapacity = threadEntry->elementsCapacity;

  uint32_t idval = id->getOrAllocate(meta);
  if (prevCapacity > idval) {
    return;
  }
  // Growth factor < 2, see folly/docs/FBVector.md; + 5 to prevent
  // very slow start.
  size_t newCapacity = static_cast<size_t>((idval + 5) * 1.7);
  assert(newCapacity > prevCapacity);
  ElementWrapper* reallocated = nullptr;

  // Need to grow. Note that we can't call realloc, as elements is
  // still linked in meta, so another thread might access invalid memory
  // after realloc succeeds. We'll copy by hand and update our ThreadEntry
  // under the lock.
  if (usingJEMalloc()) {
    bool success = false;
    size_t newByteSize = nallocx(newCapacity * sizeof(ElementWrapper), 0);

    // Try to grow in place.
    //
    // Note that xallocx(MALLOCX_ZERO) will only zero newly allocated memory,
    // even if a previous allocation allocated more than we requested.
    // This is fine; we always use MALLOCX_ZERO with jemalloc and we
    // always expand our allocation to the real size.
    if (prevCapacity * sizeof(ElementWrapper) >= jemallocMinInPlaceExpandable) {
      success =
          (xallocx(threadEntry->elements, newByteSize, 0, MALLOCX_ZERO) ==
           newByteSize);
    }

    // In-place growth failed.
    if (!success) {
      success =
          ((reallocated = static_cast<ElementWrapper*>(
                mallocx(newByteSize, MALLOCX_ZERO))) != nullptr);
    }

    if (success) {
      // Expand to real size
      assert(newByteSize / sizeof(ElementWrapper) >= newCapacity);
      newCapacity = newByteSize / sizeof(ElementWrapper);
    } else {
      throw std::bad_alloc();
    }
  } else { // no jemalloc
    // calloc() is simpler than malloc() followed by memset(), and
    // potentially faster when dealing with a lot of memory, as it can get
    // already-zeroed pages from the kernel.
    reallocated = static_cast<ElementWrapper*>(
        calloc(newCapacity, sizeof(ElementWrapper)));
    if (!reallocated) {
      throw std::bad_alloc();
    }
  }

  // Success, update the entry
  {
    std::lock_guard<std::mutex> g(meta.lock_);

    if (prevCapacity == 0) {
      meta.push_back(threadEntry);
    }

    if (reallocated) {
      /*
       * Note: we need to hold the meta lock when copying data out of
       * the old vector, because some other thread might be
       * destructing a ThreadLocal and writing to the elements vector
       * of this thread.
       */
      if (prevCapacity != 0) {
        memcpy(
            reallocated,
            threadEntry->elements,
            sizeof(*reallocated) * prevCapacity);
      }
      std::swap(reallocated, threadEntry->elements);
    }
    threadEntry->elementsCapacity = newCapacity;
  }

  free(reallocated);
}


FOLLY_STATIC_CTOR_PRIORITY_MAX
PthreadKeyUnregister PthreadKeyUnregister::instance_;
} // namespace threadlocal_detail
} // namespace folly

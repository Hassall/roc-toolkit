/*
 * Copyright (c) 2020 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

//! @file roc_core/free_list.h
//! @brief Lock free Intrusive singly linked list (stack).

#ifndef ROC_CORE_FREE_LIST_H_
#define ROC_CORE_FREE_LIST_H_

#include "roc_core/list_node.h"
#include "roc_core/noncopyable.h"
#include "roc_core/ownership_policy.h"
#include "roc_core/panic.h"
#include "roc_core/stddefs.h"

namespace roc {
namespace core {

//! Intrusive singly linked list 
//!
//! Does not perform allocations.
//! Provides O(1) insertion, and removal.
//!
//! @tparam T defines object type, it should inherit ListNode.
//!
//! @tparam OwnershipPolicy defines ownership policy which is used to acquire an
//! element ownership when it's added to the list and release ownership when it's
//! removed from the list.
template <class T, template <class TT> class OwnershipPolicy = RefCountedOwnership>
class FreeList : public NonCopyable<> {
public:
    //! Pointer type.
    //! @remarks
    //!  either raw or smart pointer depending on the ownership policy.
    typedef typename OwnershipPolicy<T>::Pointer Pointer;

    //! Initialize empty list.
    FreeList() {
        head_.next = &head_;
        head_.list = this;
    }

    //! Release ownership of containing objects.
    ~FreeList() {
    }

private:
    static T* container_of_(ListNode::ListNodeData* data) {
        return static_cast<T*>(data->container_of());
    }

    static void check_is_member_(const ListNode::ListNodeData* data, const FreeList* list) {
        if (data->list != list) {
            roc_panic("list: element is member of wrong list: expected %p, got %p",
                      (const void*)list, (const void*)data->list);
        }
    }

    ListNode::ListNodeData head_;
};

} // namespace core
} // namespace roc

#endif // ROC_CORE_FREE_LIST_H_

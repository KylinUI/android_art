/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_GC_MARK_SWEEP_INL_H_
#define ART_SRC_GC_MARK_SWEEP_INL_H_

#include "heap.h"
#include "mirror/class.h"
#include "mirror/field.h"
#include "mirror/object_array.h"

namespace art {

template <typename MarkVisitor>
inline void MarkSweep::ScanObjectVisit(const mirror::Object* obj, const MarkVisitor& visitor) {
  DCHECK(obj != NULL);
  if (kIsDebugBuild && !IsMarked(obj)) {
    heap_->DumpSpaces();
    LOG(FATAL) << "Scanning unmarked object " << obj;
  }
  mirror::Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  if (klass == java_lang_Class_) {
    DCHECK_EQ(klass->GetClass(), java_lang_Class_);
    if (kCountScannedTypes) {
      ++class_count_;
    }
    VisitClassReferences(klass, obj, visitor);
  } else if (klass->IsArrayClass()) {
    if (kCountScannedTypes) {
      ++array_count_;
    }
    visitor(obj, klass, mirror::Object::ClassOffset(), false);
    if (klass->IsObjectArrayClass()) {
      VisitObjectArrayReferences(obj->AsObjectArray<mirror::Object>(), visitor);
    }
  } else {
    if (kCountScannedTypes) {
      ++other_count_;
    }
    VisitOtherReferences(klass, obj, visitor);
    if (UNLIKELY(klass->IsReferenceClass())) {
      DelayReferenceReferent(const_cast<mirror::Object*>(obj));
    }
  }
}

template <typename Visitor>
inline void MarkSweep::VisitObjectReferences(const mirror::Object* obj, const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_,
                          Locks::mutator_lock_) {
  DCHECK(obj != NULL);
  DCHECK(obj->GetClass() != NULL);

  mirror::Class* klass = obj->GetClass();
  DCHECK(klass != NULL);
  if (klass == mirror::Class::GetJavaLangClass()) {
    DCHECK_EQ(klass->GetClass(), mirror::Class::GetJavaLangClass());
    VisitClassReferences(klass, obj, visitor);
  } else {
    if (klass->IsArrayClass()) {
      visitor(obj, klass, mirror::Object::ClassOffset(), false);
      if (klass->IsObjectArrayClass()) {
        VisitObjectArrayReferences(obj->AsObjectArray<mirror::Object>(), visitor);
      }
    } else {
      VisitOtherReferences(klass, obj, visitor);
    }
  }
}

template <typename Visitor>
inline void MarkSweep::VisitInstanceFieldsReferences(const mirror::Class* klass,
                                                     const mirror::Object* obj,
                                                     const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  DCHECK(obj != NULL);
  DCHECK(klass != NULL);
  VisitFieldsReferences(obj, klass->GetReferenceInstanceOffsets(), false, visitor);
}

template <typename Visitor>
inline void MarkSweep::VisitClassReferences(const mirror::Class* klass, const mirror::Object* obj,
                                            const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  VisitInstanceFieldsReferences(klass, obj, visitor);
  VisitStaticFieldsReferences(obj->AsClass(), visitor);
}

template <typename Visitor>
inline void MarkSweep::VisitStaticFieldsReferences(const mirror::Class* klass,
                                                   const Visitor& visitor)
    SHARED_LOCKS_REQUIRED(Locks::heap_bitmap_lock_, Locks::mutator_lock_) {
  DCHECK(klass != NULL);
  VisitFieldsReferences(klass, klass->GetReferenceStaticOffsets(), true, visitor);
}

template <typename Visitor>
inline void MarkSweep::VisitFieldsReferences(const mirror::Object* obj, uint32_t ref_offsets,
                                             bool is_static, const Visitor& visitor) {
  if (LIKELY(ref_offsets != CLASS_WALK_SUPER)) {
    // Found a reference offset bitmap.  Mark the specified offsets.
    while (ref_offsets != 0) {
      size_t right_shift = CLZ(ref_offsets);
      MemberOffset field_offset = CLASS_OFFSET_FROM_CLZ(right_shift);
      const mirror::Object* ref = obj->GetFieldObject<const mirror::Object*>(field_offset, false);
      visitor(obj, ref, field_offset, is_static);
      ref_offsets &= ~(CLASS_HIGH_BIT >> right_shift);
    }
  } else {
    // There is no reference offset bitmap.  In the non-static case,
    // walk up the class inheritance hierarchy and find reference
    // offsets the hard way. In the static case, just consider this
    // class.
    for (const mirror::Class* klass = is_static ? obj->AsClass() : obj->GetClass();
         klass != NULL;
         klass = is_static ? NULL : klass->GetSuperClass()) {
      size_t num_reference_fields = (is_static
                                     ? klass->NumReferenceStaticFields()
                                     : klass->NumReferenceInstanceFields());
      for (size_t i = 0; i < num_reference_fields; ++i) {
        mirror::Field* field = (is_static ? klass->GetStaticField(i)
                                          : klass->GetInstanceField(i));
        MemberOffset field_offset = field->GetOffset();
        const mirror::Object* ref = obj->GetFieldObject<const mirror::Object*>(field_offset, false);
        visitor(obj, ref, field_offset, is_static);
      }
    }
  }
}

template <typename Visitor>
inline void MarkSweep::VisitObjectArrayReferences(const mirror::ObjectArray<mirror::Object>* array,
                                                  const Visitor& visitor) {
  const int32_t length = array->GetLength();
  for (int32_t i = 0; i < length; ++i) {
    const mirror::Object* element = array->GetWithoutChecks(i);
    const size_t width = sizeof(mirror::Object*);
    MemberOffset offset = MemberOffset(i * width + mirror::Array::DataOffset(width).Int32Value());
    visitor(array, element, offset, false);
  }
}

}  // namespace art

#endif  // ART_SRC_GC_MARK_SWEEP_INL_H_
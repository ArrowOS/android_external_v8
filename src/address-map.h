// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_ADDRESS_MAP_H_
#define V8_ADDRESS_MAP_H_

#include "src/assert-scope.h"
#include "src/base/hashmap.h"
#include "src/objects.h"

namespace v8 {
namespace internal {

class AddressMapBase {
 protected:
  static void SetValue(base::HashMap::Entry* entry, uint32_t v) {
    entry->value = reinterpret_cast<void*>(v);
  }

  static uint32_t GetValue(base::HashMap::Entry* entry) {
    return static_cast<uint32_t>(reinterpret_cast<intptr_t>(entry->value));
  }

  inline static base::HashMap::Entry* LookupEntry(base::HashMap* map,
                                                  HeapObject* obj,
                                                  bool insert) {
    if (insert) {
      map->LookupOrInsert(Key(obj), Hash(obj));
    }
    return map->Lookup(Key(obj), Hash(obj));
  }

 private:
  static uint32_t Hash(HeapObject* obj) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(obj->address()));
  }

  static void* Key(HeapObject* obj) {
    return reinterpret_cast<void*>(obj->address());
  }
};

class RootIndexMap : public AddressMapBase {
 public:
  explicit RootIndexMap(Isolate* isolate);

  static const int kInvalidRootIndex = -1;

  int Lookup(HeapObject* obj) {
    base::HashMap::Entry* entry = LookupEntry(map_, obj, false);
    if (entry) return GetValue(entry);
    return kInvalidRootIndex;
  }

 private:
  base::HashMap* map_;

  DISALLOW_COPY_AND_ASSIGN(RootIndexMap);
};

class SerializerReference {
 public:
  SerializerReference() : bitfield_(Special(kInvalidValue)) {}

  static SerializerReference FromBitfield(uint32_t bitfield) {
    return SerializerReference(bitfield);
  }

  static SerializerReference BackReference(AllocationSpace space,
                                           uint32_t chunk_index,
                                           uint32_t chunk_offset) {
    DCHECK(IsAligned(chunk_offset, kObjectAlignment));
    DCHECK_NE(LO_SPACE, space);
    return SerializerReference(
        SpaceBits::encode(space) | ChunkIndexBits::encode(chunk_index) |
        ChunkOffsetBits::encode(chunk_offset >> kObjectAlignmentBits));
  }

  static SerializerReference MapReference(uint32_t index) {
    return SerializerReference(SpaceBits::encode(MAP_SPACE) |
                               ValueIndexBits::encode(index));
  }

  static SerializerReference LargeObjectReference(uint32_t index) {
    return SerializerReference(SpaceBits::encode(LO_SPACE) |
                               ValueIndexBits::encode(index));
  }

  static SerializerReference AttachedReference(uint32_t index) {
    return SerializerReference(SpaceBits::encode(kAttachedReferenceSpace) |
                               ValueIndexBits::encode(index));
  }

  static SerializerReference DummyReference() {
    return SerializerReference(Special(kDummyValue));
  }

  bool is_valid() const { return bitfield_ != Special(kInvalidValue); }

  bool is_back_reference() const {
    return SpaceBits::decode(bitfield_) <= LAST_SPACE;
  }

  AllocationSpace space() const {
    DCHECK(is_back_reference());
    return static_cast<AllocationSpace>(SpaceBits::decode(bitfield_));
  }

  uint32_t chunk_offset() const {
    DCHECK(is_back_reference());
    return ChunkOffsetBits::decode(bitfield_) << kObjectAlignmentBits;
  }

  uint32_t map_index() const {
    DCHECK(is_back_reference());
    return ValueIndexBits::decode(bitfield_);
  }

  uint32_t large_object_index() const {
    DCHECK(is_back_reference());
    return ValueIndexBits::decode(bitfield_);
  }

  uint32_t chunk_index() const {
    DCHECK(is_back_reference());
    return ChunkIndexBits::decode(bitfield_);
  }

  uint32_t back_reference() const {
    DCHECK(is_back_reference());
    return bitfield_ & (ChunkOffsetBits::kMask | ChunkIndexBits::kMask);
  }

  bool is_attached_reference() const {
    return SpaceBits::decode(bitfield_) == kAttachedReferenceSpace;
  }

  int attached_reference_index() const {
    DCHECK(is_attached_reference());
    return ValueIndexBits::decode(bitfield_);
  }

 private:
  explicit SerializerReference(uint32_t bitfield) : bitfield_(bitfield) {}

  inline static uint32_t Special(int value) {
    return SpaceBits::encode(kSpecialValueSpace) |
           ValueIndexBits::encode(value);
  }

  // We use the 32-bit bitfield to encode either a back reference, a special
  // value, or an attached reference index.
  // Back reference:
  //   [ Space index             ] [ Chunk index ] [ Chunk offset ]
  //   [ LO_SPACE                ] [ large object index           ]
  // Special value
  //   [ kSpecialValueSpace      ] [ Special value index          ]
  // Attached reference
  //   [ kAttachedReferenceSpace ] [ Attached reference index     ]

  static const int kChunkOffsetSize = kPageSizeBits - kObjectAlignmentBits;
  static const int kChunkIndexSize = 32 - kChunkOffsetSize - kSpaceTagSize;
  static const int kValueIndexSize = kChunkOffsetSize + kChunkIndexSize;

  static const int kSpecialValueSpace = LAST_SPACE + 1;
  static const int kAttachedReferenceSpace = kSpecialValueSpace + 1;
  STATIC_ASSERT(kAttachedReferenceSpace < (1 << kSpaceTagSize));

  static const int kInvalidValue = 0;
  static const int kDummyValue = 1;

  // The chunk offset can also be used to encode the index of special values.
  class ChunkOffsetBits : public BitField<uint32_t, 0, kChunkOffsetSize> {};
  class ChunkIndexBits
      : public BitField<uint32_t, ChunkOffsetBits::kNext, kChunkIndexSize> {};
  class ValueIndexBits : public BitField<uint32_t, 0, kValueIndexSize> {};
  STATIC_ASSERT(ChunkIndexBits::kNext == ValueIndexBits::kNext);
  class SpaceBits : public BitField<int, kValueIndexSize, kSpaceTagSize> {};
  STATIC_ASSERT(SpaceBits::kNext == 32);

  uint32_t bitfield_;

  friend class SerializerReferenceMap;
};

// Mapping objects to their location after deserialization.
// This is used during building, but not at runtime by V8.
class SerializerReferenceMap : public AddressMapBase {
 public:
  SerializerReferenceMap()
      : no_allocation_(), map_(), attached_reference_index_(0) {}

  SerializerReference Lookup(HeapObject* obj) {
    base::HashMap::Entry* entry = LookupEntry(&map_, obj, false);
    return entry ? SerializerReference(GetValue(entry)) : SerializerReference();
  }

  void Add(HeapObject* obj, SerializerReference b) {
    DCHECK(b.is_valid());
    DCHECK_NULL(LookupEntry(&map_, obj, false));
    base::HashMap::Entry* entry = LookupEntry(&map_, obj, true);
    SetValue(entry, b.bitfield_);
  }

  SerializerReference AddAttachedReference(HeapObject* attached_reference) {
    SerializerReference reference =
        SerializerReference::AttachedReference(attached_reference_index_++);
    Add(attached_reference, reference);
    return reference;
  }

 private:
  DisallowHeapAllocation no_allocation_;
  base::HashMap map_;
  int attached_reference_index_;
  DISALLOW_COPY_AND_ASSIGN(SerializerReferenceMap);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_ADDRESS_MAP_H_

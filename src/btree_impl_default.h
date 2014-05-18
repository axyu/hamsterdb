/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Btree node layout for variable length keys/records and duplicates
 * =================================================================
 *
 * This is the default hamsterdb layout. It is chosen for
 * 1. variable length keys (with or without duplicates)
 * 2. fixed length keys with duplicates
 *
 * Unlike the PAX layout implemented in btree_impl_pax.h, the layout implemented
 * here stores key data and records next to each other. However, since keys
 * (and duplicate records) have variable length, each node has a small
 * index area upfront. This index area stores metadata about the key like
 * the key's size, the number of records (=duplicates), flags and the
 * offset of the actual data.
 *
 * The actual data starting at this offset contains the key's data (which
 * can be a 64bit blob ID if the key is too big), and the record's data.
 * If duplicate keys exist, then all records are stored next to each other.
 * If there are too many duplicates, then all of them are offloaded to
 * a blob - a "duplicate table".
 *
 * To avoid expensive memcpy-operations, erasing a key only affects this
 * upfront index: the relevant slot is moved to a "freelist". This freelist
 * contains the same meta information as the index table.
 *
 * The flat memory layout looks like this:
 *
 * |Idx1|Idx2|...|Idxn|F1|F2|...|Fn|...(space)...|Key1Rec1|Key2Rec2|...|
 *
 * ... where Idx<n> are the indices (of slot <n>)
 *     where F<n> are freelist entries
 *     where Key<n> is the key data of slot <n>
 *        ... directly followed by one or more Records.
 *
 * In addition, the first few bytes in the node store the following
 * information:
 *   0  (4 bytes): total capacity of index keys (used keys + freelist)
 *   4  (4 bytes): number of used freelist entries
 *   8  (4 bytes): offset for the next key at the end of the page
 *
 * In total, |capacity| contains the number of maximum keys (and index
 * entries) that can be stored in the node. The number of used index keys
 * is in |m_node->get_count()|. The number of used freelist entries is
 * returned by |get_freelist_count()|. The freelist indices start directly
 * after the key indices. The key space (with key data and records) starts at
 * N * capacity, where |N| is the size of an index entry (the size depends
 * on the actual btree configuration, i.e. whether key size is fixed,
 * duplicates are used etc).
 *
 * If keys exceed a certain Threshold (get_extended_threshold()), they're moved
 * to a blob and the flag |kExtendedKey| is set for this key. These extended
 * keys are cached in a std::map to improve performance.
 *
 * This layout supports duplicate keys. If the number of duplicate keys
 * exceeds a certain threshold (get_duplicate_threshold()), they are all moved
 * to a table which is stored as a blob, and the |kExtendedDuplicates| flag
 * is set.
 * The record counter is 1 byte. It counts the total number of inline records
 * assigned to the current key (a.k.a the number of duplicate keys). It is
 * not used if the records were moved to a duplicate table.
 *
 * If records have fixed length then all records of a key (with duplicates)
 * are stored next to each other. If they have variable length then each of
 * these records is stored with 1 byte for flags:
 *   Rec1|F1|Rec2|F2|...
 * where Recn is an 8 bytes record-ID (offset in the file) OR inline record,
 * and F1 is 1 byte for flags (kBlobSizeSmall etc).
 */

#ifndef HAM_BTREE_IMPL_DEFAULT_H__
#define HAM_BTREE_IMPL_DEFAULT_H__

#include <algorithm>
#include <vector>
#include <map>

#include "globals.h"
#include "util.h"
#include "page.h"
#include "btree_node.h"
#include "blob_manager.h"
#include "env_local.h"
#include "btree_index.h"

#ifdef WIN32
// MSVC: disable warning about use of 'this' in base member initializer list
#  pragma warning(disable:4355)
#  undef min  // avoid MSVC conflicts with std::min
#endif

namespace hamsterdb {

namespace DefLayout {

//
// A small index which manages variable length buffers for keys and records
//
class UpfrontIndex
{
  public:
    // Constructor
    UpfrontIndex()
      : m_data(0), m_stride(0) {
    }

    // Initialization routine; sets data pointer and the initial capacity
    // If |capacity| is 0 then use the value that is already stored in the page
    void initialize(ham_u8_t *data, size_t capacity,
                    bool is_fixed_size, bool has_duplicates,
                    size_t sizeof_offset) {
      m_data = data;
      if (capacity)
        set_capacity(capacity);
      m_is_fixed_size = is_fixed_size;
      m_has_duplicates = has_duplicates;
      m_sizeof_offset = sizeof_offset;
      // 1 byte flags + 2 byte key size (optional) + 2 (or 4) byte offset
      //   + 1 byte record counter (optional)
      m_stride = 1 + (m_is_fixed_size ? 0 : 2)
                    + m_sizeof_offset
                    + (m_has_duplicates ? 1 : 0);
    }

    // Returns the actual size of a single index
    size_t get_full_index_size() const {
      return (m_stride);
    }

    // Sets the start offset of the key data
    void set_key_data_offset(ham_u32_t slot, ham_u32_t offset) {
      ham_u8_t *p;
      if (m_has_duplicates)
        p = &m_data[m_stride * slot + 2];
      else
        p = &m_data[m_stride * slot + 1];
      if (m_sizeof_offset == 4)
        *(ham_u32_t *)p = ham_h2db32(offset);
      else
        *(ham_u16_t *)p = ham_h2db16((ham_u16_t)offset);
    }

    // Returns the start offset of a key's data
    ham_u32_t get_key_data_offset(ham_u32_t slot) const {
      ham_u8_t *p;
      if (m_has_duplicates)
        p = &m_data[m_stride * slot + 2];
      else
        p = &m_data[m_stride * slot + 1];
      if (m_sizeof_offset == 4)
        return (ham_db2h32(*(ham_u32_t *)p));
      else
        return (ham_db2h16(*(ham_u16_t *)p));
    }

    // Returns the capacity
    size_t get_capacity() const {
      return (ham_db2h32(*(ham_u32_t *)m_data));
    }

    // Stores the capacity
    void set_capacity(size_t capacity) {
      *(ham_u32_t *)m_data = ham_h2db32(capacity);
    }

    // Returns the number of freelist entries
    size_t get_freelist_count() const {
      return (ham_db2h32(*(ham_u32_t *)(m_data + 4)));
    }

    // Sets the number of freelist entries
    void set_freelist_count(size_t freelist_count) {
      *(ham_u32_t *)(m_data + 4) = ham_h2db32(freelist_count);
    }

    // Returns the offset of the unused space at the end of the page
    ham_u32_t get_next_offset() const {
      return (ham_db2h32(*(ham_u32_t *)(m_data + 8)));
    }

    // Sets the offset of the unused space at the end of the page
    void set_next_offset(ham_u32_t next_offset) {
      *(ham_u32_t *)(m_data + 8) = ham_h2db32(next_offset);
    }

  private:
    // The physical data in the node
    ham_u8_t *m_data;

    bool m_is_fixed_size;
    bool m_has_duplicates;
    size_t m_sizeof_offset;
    size_t m_stride;
};

//
// Variable length keyslist
//
class BinaryKeyList
{
  public:
    BinaryKeyList(LocalDatabase *db)
      : m_index(0), m_data(0) {
    }

    // Sets the data pointer; required for initialization
    void initialize(ham_u8_t *ptr, size_t capacity, UpfrontIndex *index) {
      m_index = index;
      m_data = ptr;
    }

    // Returns the actual key size including overhead; this is just a guess
    // since we don't know how large the keys will become
    size_t get_full_key_size() const {
      return (32 - 8); // + 5 - 8
    }

    // Returns the size of a single key
    ham_u32_t get_key_size(ham_u32_t slot) const {
      ham_u32_t offset = m_index->get_key_data_offset(slot);
      return (ham_db2h16(*(ham_u16_t *)(m_data + offset)));
    }

  private:
    UpfrontIndex *m_index;
    ham_u8_t *m_data;
};

class DuplicateInlineRecordList
{
  public:
    DuplicateInlineRecordList(LocalDatabase *db)
      : m_record_size(db->get_record_size()) {
    }

    // Sets the data pointer; required for initialization
    void initialize(ham_u8_t *ptr, size_t capacity) {
      m_data = ptr;
    }

    // Returns the actual key record including overhead
    size_t get_full_record_size() const {
      return (2 + m_record_size);
    }

  private:
    ham_u8_t *m_data;
    size_t m_record_size;
};

class DuplicateDefaultRecordList
{
  public:
    DuplicateDefaultRecordList(LocalDatabase *db) {
    }

    // Sets the data pointer; required for initialization
    void initialize(ham_u8_t *ptr, size_t capacity) {
      m_data = ptr;
    }

    // Returns the actual key record including overhead
    size_t get_full_record_size() const {
      return (2 + 8);
    }

  private:
    ham_u8_t *m_data;
};

} // namespace DefLayout

//
// A BtreeNodeProxy layout which can handle...
//
//   1. fixed length keys w/ duplicates
//   2. variable length keys w/ duplicates
//   3. variable length keys w/o duplicates
//
// Fixed length keys are stored sequentially and reuse the layout from pax.
// Same for the distinct RecordList (if duplicates are disabled).
//
template<typename Offset, typename KeyList, typename RecordList>
class DefaultNodeImpl
{
    // the type of |this| object
    typedef DefaultNodeImpl<Offset, KeyList, RecordList> NodeType;

    enum {
      // for capacity, freelist_count, next_offset
      kPayloadOffset = 12
    };

  public:
    // Constructor
    DefaultNodeImpl(Page *page)
      : m_page(page), m_node(PBtreeNode::from_page(m_page)),
        m_keys(page->get_db()), m_records(page->get_db()),
        m_recalc_capacity(false) {
      initialize();
    }

    // Destructor
    ~DefaultNodeImpl() {
    }

    // Checks the integrity of this node. Throws an exception if there is a
    // violation.
    void check_integrity() const {
    }

    // Compares two keys
    template<typename Cmp>
    int compare(const ham_key_t *lhs, ham_u32_t rhs, Cmp &cmp) {
      return (0);
    }

    // Searches the node for the key and returns the slot of this key
    template<typename Cmp>
    int find_child(ham_key_t *key, Cmp &comparator, ham_u64_t *precord_id,
                    int *pcmp) {
      return (0);
    }

    // Searches the node for the key and returns the slot of this key
    // - only for exact matches!
    template<typename Cmp>
    int find_exact(ham_key_t *key, Cmp &comparator) {
      int cmp;
      int r = find_child(key, comparator, 0, &cmp);
      if (cmp)
        return (-1);
      return (r);
    }

    // Iterates all keys, calls the |visitor| on each
    void scan(ScanVisitor *visitor, ham_u32_t start, bool distinct) {
    }

    // Returns a deep copy of the key
    void get_key(ham_u32_t slot, ByteArray *arena, ham_key_t *dest) {
    }

    // Returns the number of records of a key
    ham_u32_t get_record_count(ham_u32_t slot) {
      return (0);
    }

    // Returns the full record and stores it in |dest|
    void get_record(ham_u32_t slot, ByteArray *arena, ham_record_t *record,
                    ham_u32_t flags, ham_u32_t duplicate_index) {
    }

    // Sets the record of a key, or adds a duplicate
    void set_record(ham_u32_t slot, ham_record_t *record,
                    ham_u32_t duplicate_index, ham_u32_t flags,
                    ham_u32_t *new_duplicate_index) {
    }

    // Returns the record size of a key or one of its duplicates
    ham_u64_t get_record_size(ham_u32_t slot, int duplicate_index) {
      return (0);
    }

    // Erases an extended key
    void erase_key(ham_u32_t slot) {
    }

    // Erases one (or all) records of a key
    void erase_record(ham_u32_t slot, ham_u32_t duplicate_index,
                    bool all_duplicates) {
    }

    // Erases a key from the index. Does NOT erase the records!
    void erase(ham_u32_t slot) {
    }

    // Inserts a new key at |slot|. 
    // Also inserts an empty record which has to be overwritten in
    // the next call of set_record().
    void insert(ham_u32_t slot, const ham_key_t *key) {
    }

    // Returns true if |key| cannot be inserted because a split is required.
    // Unlike implied by the name, this function will try to re-arrange the
    // node in order for the key to fit in.
    bool requires_split() {
      return (false);
    }

    // Returns true if the node requires a merge or a shift
    bool requires_merge() const {
      return (m_node->get_count() <= 3);
    }

    // Splits this node and moves some/half of the keys to |other|
    void split(DefaultNodeImpl *other, int pivot) {
    }

    // Merges keys from |other| to this node
    void merge_from(DefaultNodeImpl *other) {
    }

    // Returns a record id
    ham_u64_t get_record_id(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) const {
      return (0);
    }

    // Sets a record id
    void set_record_id(ham_u32_t slot, ham_u64_t ptr,
                    ham_u32_t duplicate_index = 0) {
    }

    // Returns the key's flags
    ham_u32_t get_key_flags(ham_u32_t slot) const {
      return (0);
    }

    // Sets the flags of a key
    void set_key_flags(ham_u32_t slot, ham_u32_t flags) {
    }

    // Returns the key size as specified by the user
    ham_u32_t get_key_size(ham_u32_t slot) const {
      return (m_keys.get_key_size(slot));
    }

    // Sets the size of a key
    void set_key_size(ham_u32_t slot, ham_u32_t size) {
    }

    // Returns a pointer to the (inline) key data
    ham_u8_t *get_key_data(ham_u32_t slot) {
      return (0);
    }

    // Returns a pointer to the (inline) key data (const flavour)
    ham_u8_t *get_key_data(ham_u32_t slot) const {
      return (0);
    }

    // Sets the inline key data
    void set_key_data(ham_u32_t slot, const void *ptr, ham_u32_t len) {
    }

    // Returns the flags of a record; defined in btree_flags.h
    ham_u8_t get_record_flags(ham_u32_t slot, ham_u32_t duplicate_index = 0)
                    const {
      return (0);
    }

    // Returns the capacity
    size_t get_capacity() const {
      return (0);
    }

    // Clears the page with zeroes and reinitializes it; only for testing
    void test_clear_page() {
      memset(m_page->get_payload(), 0,
                    m_page->get_db()->get_local_env()->get_usable_page_size());
      initialize();
    }

    // Sets a key; only for testing
    void test_set_key(ham_u32_t slot, const char *data,
                    size_t data_size, ham_u32_t flags, ham_u64_t record_id) {
      set_record_id(slot, record_id);
      set_key_flags(slot, flags);
      set_key_size(slot, (ham_u16_t)data_size);
      set_key_data(slot, data, (ham_u32_t)data_size);
    }

  private:
    // Initializes the node
    void initialize() {
      LocalDatabase *db = m_page->get_db();
      size_t key_size = db->get_btree_index()->get_key_size();

      // is this a fresh page which has not yet been initialized?
      if (m_node->get_count() == 0 && !(db->get_rt_flags() & HAM_READ_ONLY)) {
        // if yes then ask the btree for the default capacity (it keeps
        // track of the average capacity of older pages).
        size_t capacity = db->get_btree_index()->get_statistics()->get_default_page_capacity();
        // no data so far? then come up with a good default
        if (capacity == 0) {
          size_t page_size = db->get_local_env()->get_page_size();
          size_t usable_page_size = get_usable_page_size();

          if (key_size == HAM_KEY_SIZE_UNLIMITED)
            key_size = m_keys.get_full_key_size();
          // TODO only binary keys!
          else if (key_size > calculate_extended_threshold(page_size))
            key_size = sizeof(ham_u64_t);

          capacity = usable_page_size
                            / (m_index.get_full_index_size()
                                + key_size
                                + m_records.get_full_record_size());

          // continue initialization with the original value
          key_size = db->get_btree_index()->get_key_size();

          // the default might not be precise and might be recalculated later
          m_recalc_capacity = true;
        }

        m_index.initialize(m_node->get_data(), capacity,
                key_size != HAM_KEY_SIZE_UNLIMITED,
                (db->get_local_env()->get_flags() & HAM_ENABLE_DUPLICATES) != 0,
                sizeof(Offset));
        m_index.set_freelist_count(0);
        m_index.set_next_offset(0);
      }
      else
        m_index.initialize(m_node->get_data(), 0,
                key_size != HAM_KEY_SIZE_UNLIMITED,
                (db->get_local_env()->get_flags() & HAM_ENABLE_DUPLICATES) != 0,
                sizeof(Offset));
    }

    // Calculates the extended threshold based on the page size
    static size_t calculate_extended_threshold(size_t page_size) {
      if (page_size == 1024)
        return (64);
      if (page_size <= 1024 * 8)
        return (128);
      return (256);
    }

    // Returns the usable page size that can be used for actually
    // storing the data
    size_t get_usable_page_size() const {
      return (m_page->get_db()->get_local_env()->get_usable_page_size()
                    - kPayloadOffset
                    - PBtreeNode::get_entry_offset());
    }


    // The page that we're operating on
    Page *m_page;

    // The node that we're operating on
    PBtreeNode *m_node;

    // Provides access to the upfront index
    DefLayout::UpfrontIndex m_index;

    // The KeyList provides access to the stored keys
    KeyList m_keys;

    // The RecordList provides access to the stored records
    RecordList m_records;

    // A memory arena for various tasks
    ByteArray m_arena;

    // Allow the capacity to be recalculated later on
    bool m_recalc_capacity;
};

#if 0
template<typename KeyList, typename RecordList>
class DefaultNodeImpl;

//
// A (static) helper class for dealing with extended duplicate tables
//
struct DuplicateTable
{
  // Returns the number of used elements in a duplicate table
  static ham_u32_t get_count(ByteArray *table) {
    ham_assert(table->get_size() > 4);
    ham_u32_t count = *(ham_u32_t *)table->get_ptr();
    return (ham_db2h32(count));
  }

  // Sets the number of used elements in a duplicate table
  static void set_count(ByteArray *table, ham_u32_t count) {
    *(ham_u32_t *)table->get_ptr() = ham_h2db32(count);
  }

  // Returns the maximum capacity of elements in a duplicate table
  static ham_u32_t get_capacity(ByteArray *table) {
    ham_assert(table->get_size() >= 8);
    ham_u32_t count = *(ham_u32_t *)((ham_u8_t *)table->get_ptr() + 4);
    return (ham_db2h32(count));
  }

  // Sets the maximum capacity of elements in a duplicate table
  static void set_capacity(ByteArray *table, ham_u32_t capacity) {
    ham_assert(table->get_size() >= 8);
    *(ham_u32_t *)((ham_u8_t *)table->get_ptr() + 4) = ham_h2db32(capacity);
  }
};

//
// A LayoutImplementation for fixed size keys WITH duplicates.
// This class has two template parameters:
//   |Offset| is the type that is supposed to be used for offset pointers
//     into the data area of the node. If the page size is small enough, two
//     bytes (ham_u16_t) are used. Otherwise four bytes (ham_u32_t) are
//     required.
//   |HasDuplicates| is a boolean whether this layout should support duplicate
//     keys. If yes, each index contains a duplicate counter.
//
template<typename Offset, bool HasDuplicates>
class FixedKeyList
{
    enum {
      // 1 byte flags + 2 (or 4) byte offset
      //   + 1 byte record counter (optional)
      kSpan = 1 + sizeof(Offset) + (HasDuplicates ? 1 : 0)
    };

  public:
    // Performs initialization
    void initialize(ham_u8_t *data, size_t key_size) {
      m_data = data;
      m_key_size = key_size;
      // this layout only works with fixed sizes!
      ham_assert(m_key_size != HAM_KEY_SIZE_UNLIMITED);
    }

    // Returns a pointer to this key's index
    ham_u8_t *get_key_index_ptr(ham_u32_t slot) {
      return (&m_data[kSpan * slot]);
    }

    // Returns the memory span from one key to the next
    ham_u32_t get_key_index_span() const {
      return (kSpan);
    }

    // Returns the (persisted) flags of a key; defined in btree_flags.h
    ham_u8_t get_key_flags(ham_u32_t slot) const {
      return (m_data[kSpan * slot]);
    }

    // Sets the flags of a key; defined in btree_flags.h
    void set_key_flags(ham_u32_t slot, ham_u8_t flags) {
      m_data[kSpan * slot] = flags;
    }

    // Returns the size of a key
    ham_u16_t get_key_size(ham_u32_t slot) const {
      return (m_key_size);
    }

    // Sets the size of a key
    void set_key_size(ham_u32_t slot, ham_u16_t size) {
      ham_assert(size == m_key_size);
    }

    // Sets the start offset of the key data
    void set_key_data_offset(ham_u32_t slot, ham_u32_t offset) {
      ham_u8_t *p;
      if (HasDuplicates)
        p = &m_data[kSpan * slot + 2];
      else
        p = &m_data[kSpan * slot + 1];
      if (sizeof(Offset) == 4)
        *(ham_u32_t *)p = ham_h2db32(offset);
      else
        *(ham_u16_t *)p = ham_h2db16((ham_u16_t)offset);
    }

    // Returns the start offset of a key's data
    ham_u32_t get_key_data_offset(ham_u32_t slot) const {
      ham_u8_t *p;
      if (HasDuplicates)
        p = &m_data[kSpan * slot + 2];
      else
        p = &m_data[kSpan * slot + 1];
      if (sizeof(Offset) == 4)
        return (ham_db2h32(*(ham_u32_t *)p));
      else
        return (ham_db2h16(*(ham_u16_t *)p));
    }

    // Sets the record count of a key.
    // If this layout does not support duplicates, then an internal flag
    // is set.
    void set_inline_record_count(ham_u32_t slot, ham_u8_t count) {
      if (HasDuplicates == true)
        m_data[kSpan * slot + 1] = count;
      else {
        if (count == 0)
          set_key_flags(slot, get_key_flags(slot) | BtreeKey::kHasNoRecords);
        else
          set_key_flags(slot, get_key_flags(slot) & (~BtreeKey::kHasNoRecords));
      }
    }

    // Returns the record counter
    ham_u8_t get_inline_record_count(ham_u32_t slot) const {
      if (HasDuplicates)
        return (m_data[kSpan * slot + 1]);
      return (get_key_flags(slot) & BtreeKey::kHasNoRecords
                      ? 0
                      : 1);
    }

  private:
    // Pointer to the data
    ham_u8_t *m_data;

    // The constant key size
    size_t m_key_size;
};

//
// A LayoutImplementation for variable size keys. Uses the same template
// parameters as |FixedKeyList|.
//
template<typename Offset, bool HasDuplicates>
class DefaultKeyList
{
    enum {
      // 1 byte flags + 2 byte key size + 2 (or 4) byte offset
      //   + 1 byte record counter (optional)
      kSpan = 3 + sizeof(Offset) + (HasDuplicates ? 1 : 0)
    };

  public:
    // Initialization
    void initialize(ham_u8_t *data, size_t key_size) {
      m_data = data;
      // this layout only works with unlimited/variable sizes!
      ham_assert(key_size == HAM_KEY_SIZE_UNLIMITED);
    }

    // Returns a pointer to the index of a key
    ham_u8_t *get_key_index_ptr(ham_u32_t slot) {
      return (&m_data[kSpan * slot]);
    }

    // Returns the memory span from one key to the next
    ham_u32_t get_key_index_span() const {
      return (kSpan);
    }

    // Returns the (persisted) flags of a key; see btree_flags.h
    ham_u8_t get_key_flags(ham_u32_t slot) const {
      return (m_data[kSpan * slot]);
    }

    // Sets the flags of a key; defined in btree_flags.h
    void set_key_flags(ham_u32_t slot, ham_u8_t flags) {
      m_data[kSpan * slot] = flags;
    }

    // Returns the size of a key
    ham_u16_t get_key_size(ham_u32_t slot) const {
      ham_u8_t *p = &m_data[kSpan * slot + 1];
      return (ham_db2h16(*(ham_u16_t *)p));
    }

    // Sets the size of a key
    void set_key_size(ham_u32_t slot, ham_u16_t size) {
      ham_u8_t *p = &m_data[kSpan * slot + 1];
      *(ham_u16_t *)p = ham_h2db16(size);
    }

    // Sets the start offset of the key data
    void set_key_data_offset(ham_u32_t slot, ham_u32_t offset) {
      ham_u8_t *p;
      if (HasDuplicates)
        p = &m_data[kSpan * slot + 4];
      else
        p = &m_data[kSpan * slot + 3];
      if (sizeof(Offset) == 4)
        *(ham_u32_t *)p = ham_h2db32(offset);
      else
        *(ham_u16_t *)p = ham_h2db16((ham_u16_t)offset);
    }

    // Returns the start offset of the key data
    ham_u32_t get_key_data_offset(ham_u32_t slot) const {
      ham_u8_t *p;
      if (HasDuplicates)
        p = &m_data[kSpan * slot + 4];
      else
        p = &m_data[kSpan * slot + 3];
      if (sizeof(Offset) == 4)
        return (ham_db2h32(*(ham_u32_t *)p));
      else
        return (ham_db2h16(*(ham_u16_t *)p));
    }

    // Sets the record counter
    void set_inline_record_count(ham_u32_t slot, ham_u8_t count) {
      if (HasDuplicates == true)
        m_data[kSpan * slot + 3] = count;
      else {
        if (count == 0)
          set_key_flags(slot, get_key_flags(slot) | BtreeKey::kHasNoRecords);
        else
          set_key_flags(slot, get_key_flags(slot) & (~BtreeKey::kHasNoRecords));
      }
    }

    // Returns the record counter
    ham_u8_t get_inline_record_count(ham_u32_t slot) const {
      if (HasDuplicates)
        return (m_data[kSpan * slot + 3]);
      return (get_key_flags(slot) & BtreeKey::kHasNoRecords
                      ? 0
                      : 1);
    }

  private:
    // the serialized data
    ham_u8_t *m_data;
};

//
// A RecordList for the default inline records, storing 8 byte record IDs
// or inline records with size <= 8 bytes. If duplicates are supported then
// the record flags are stored in an additional byte, otherwise they're
// stored in the key flags.
//
template<typename KeyList, bool HasDuplicates>
class DefaultInlineRecordImpl
{
    typedef DefaultNodeImpl<KeyList, DefaultInlineRecordImpl> NodeType;

  public:
    // Constructor
    DefaultInlineRecordImpl(NodeType *layout, ham_u32_t record_size)
      : m_keys(layout) {
    }

    // Constructor
    DefaultInlineRecordImpl(const NodeType *layout, ham_u32_t record_size)
      : m_keys((NodeType *)layout) {
    }

    // Returns true if the record is inline
    bool is_record_inline(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) const {
      ham_u32_t flags = get_record_flags(slot, duplicate_index);
      return ((flags & BtreeRecord::kBlobSizeTiny)
              || (flags & BtreeRecord::kBlobSizeSmall)
              || (flags & BtreeRecord::kBlobSizeEmpty) != 0);
    }

    // Sets the inline record data
    void set_inline_record_data(ham_u32_t slot, const void *data,
                    ham_u32_t size, ham_u32_t duplicate_index) {
      if (size == 0) {
        set_record_flags(slot, BtreeRecord::kBlobSizeEmpty, duplicate_index);
        return;
      }
      if (size < 8) {
        /* the highest byte of the record id is the size of the blob */
        char *p = (char *)m_keys->get_inline_record_data(slot,
                        duplicate_index);
        p[sizeof(ham_u64_t) - 1] = size;
        memcpy(p, data, size);
        set_record_flags(slot, BtreeRecord::kBlobSizeTiny, duplicate_index);
        return;
      }
      else if (size == 8) {
        char *p = (char *)m_keys->get_inline_record_data(slot,
                        duplicate_index);
        memcpy(p, data, size);
        set_record_flags(slot, BtreeRecord::kBlobSizeSmall, duplicate_index);
        return;
      }

      ham_verify(!"shouldn't be here");
    }

    // Returns the size of the record, if inline
    ham_u32_t get_inline_record_size(ham_u32_t slot,
                    ham_u32_t duplicate_index)const {
      ham_u32_t flags = get_record_flags(slot, duplicate_index);

      ham_assert(m_keys->is_record_inline(slot, duplicate_index) == true);

      if (flags & BtreeRecord::kBlobSizeTiny) {
        /* the highest byte of the record id is the size of the blob */
        char *p = (char *)m_keys->get_inline_record_data(slot,
                        duplicate_index);
        return (p[sizeof(ham_u64_t) - 1]);
      }
      if (flags & BtreeRecord::kBlobSizeSmall)
        return (sizeof(ham_u64_t));
      if (flags & BtreeRecord::kBlobSizeEmpty)
        return (0);
      ham_verify(!"shouldn't be here");
      return (0);
    }

    // Returns the maximum size of inline records PAYLOAD ONLY!
    ham_u32_t get_max_inline_record_size() const {
      return (sizeof(ham_u64_t));
    }

    // Returns the maximum size of inline records INCL overhead!
    ham_u32_t get_total_inline_record_size() const {
      return (sizeof(ham_u64_t) + 1);
    }

    // Returns the flags of a record
    ham_u8_t get_record_flags(ham_u32_t slot, ham_u32_t duplicate_index) const {
      if (!HasDuplicates) {
        ham_u8_t flags = m_keys->get_key_flags(slot);
        return (flags & (BtreeRecord::kBlobSizeTiny
                        | BtreeRecord::kBlobSizeSmall
                        | BtreeRecord::kBlobSizeEmpty));
      }
      ham_u8_t *p = (ham_u8_t *)m_keys->get_inline_record_data(slot,
                      duplicate_index);
      return (*(p + get_max_inline_record_size()));
    }

    // Sets the flags of a record
    void set_record_flags(ham_u32_t slot, ham_u8_t flags,
                    ham_u32_t duplicate_index) {
      if (!HasDuplicates) {
        ham_u8_t oldflags = m_keys->get_key_flags(slot);
        oldflags &= ~(BtreeRecord::kBlobSizeTiny
                        | BtreeRecord::kBlobSizeSmall
                        | BtreeRecord::kBlobSizeEmpty);
        m_keys->set_key_flags(slot, oldflags | flags);
        return;
      }
      ham_u8_t *p = (ham_u8_t *)m_keys->get_inline_record_data(slot,
                      duplicate_index);
      *(p + get_max_inline_record_size()) = flags;
    }

    // Returns true if a record in a duplicate table is inline
    bool table_is_record_inline(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_count(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      ham_u32_t flags = *(ptr + get_max_inline_record_size());
      return ((flags & BtreeRecord::kBlobSizeTiny)
              || (flags & BtreeRecord::kBlobSizeSmall)
              || (flags & BtreeRecord::kBlobSizeEmpty) != 0);
    }

    // Returns the size of a record from the duplicate table
    ham_u32_t table_get_record_size(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_count(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      ham_u32_t flags = *(ptr + get_max_inline_record_size());
      if (flags & BtreeRecord::kBlobSizeTiny) {
        /* the highest byte of the record id is the size of the blob */
        return (ptr[sizeof(ham_u64_t) - 1]);
      }
      if (flags & BtreeRecord::kBlobSizeSmall)
        return (sizeof(ham_u64_t));
      if (flags & BtreeRecord::kBlobSizeEmpty)
        return (0);
      ham_assert(!"shouldn't be here");
      return (0);
    }

    // Returns a record id from the duplicate table
    ham_u64_t table_get_record_id(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_count(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      return (ham_db2h_offset(*(ham_u64_t *)ptr));
    }

    // Sets a record id in the duplicate table
    void table_set_record_id(ByteArray *table,
                    ham_u32_t duplicate_index, ham_u64_t id) {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_capacity(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      *(ham_u64_t *)ptr = ham_h2db_offset(id);
      // initialize flags
      *(ptr + get_max_inline_record_size()) = 0;
    }

    // Returns a pointer to the inline record data from the duplicate table
    ham_u8_t *table_get_record_data(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_capacity(table));
      ham_u8_t *ptr = (ham_u8_t *)table->get_ptr();
      ptr += 8; // skip count, capacity and other records
      ptr += get_total_inline_record_size() * duplicate_index;
      return (ptr);
    }

    // Sets the inline record data in the duplicate table
    void table_set_record_data(ByteArray *table, ham_u32_t duplicate_index,
                    void *data, ham_u32_t size) {
      ham_assert(HasDuplicates == true);
      ham_assert(duplicate_index < DuplicateTable::get_capacity(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      ham_assert(size <= 8);

      if (size == 0) {
        *(ptr + get_max_inline_record_size()) = BtreeRecord::kBlobSizeEmpty;
        return;
      }
      if (size < 8) {
        /* the highest byte of the record id is the size of the blob */
        ptr[sizeof(ham_u64_t) - 1] = size;
        memcpy(ptr, data, size);
        *(ptr + get_max_inline_record_size()) = BtreeRecord::kBlobSizeTiny;
        return;
      }
      if (size == 8) {
        *(ptr + get_max_inline_record_size()) = BtreeRecord::kBlobSizeSmall;
        memcpy(ptr, data, size);
        return;
      }
      ham_assert(!"shouldn't be here");
    }

    // Deletes a record from the table; also adjusts the count
    void table_erase_record(ByteArray *table, ham_u32_t duplicate_index) {
      ham_u32_t count = DuplicateTable::get_count(table);
      ham_assert(duplicate_index < count);
      ham_assert(count > 0);
      if (duplicate_index < count - 1) {
        ham_u8_t *lhs = table_get_record_data(table, duplicate_index);
        ham_u8_t *rhs = lhs + get_total_inline_record_size();
        memmove(lhs, rhs, get_total_inline_record_size()
                                * (count - duplicate_index - 1));
      }
      // adjust the counter
      DuplicateTable::set_count(table, count - 1);
    }

  private:
    NodeType *m_keys;
};

//
// A RecordList for fixed length inline records
//
template<typename KeyList>
class FixedInlineRecordImpl
{
    typedef DefaultNodeImpl<KeyList, FixedInlineRecordImpl> NodeType;

  public:
    FixedInlineRecordImpl(NodeType *layout, ham_u32_t record_size)
      : m_record_size(record_size), m_keys(layout) {
    }

    FixedInlineRecordImpl(const NodeType *layout, ham_u32_t record_size)
      : m_record_size(record_size), m_keys((NodeType *)layout) {
    }

    // Returns true if the record is inline
    bool is_record_inline(ham_u32_t slot, ham_u32_t duplicate_index = 0) const {
      return (true);
    }

    // Sets the inline record data
    void set_inline_record_data(ham_u32_t slot, const void *data,
                    ham_u32_t size, ham_u32_t duplicate_index) {
      ham_assert(size == m_record_size);
      char *p = (char *)m_keys->get_inline_record_data(slot, duplicate_index);
      memcpy(p, data, size);
    }

    // Returns the size of the record, if inline
    ham_u32_t get_inline_record_size(ham_u32_t slot,
                    ham_u32_t duplicate_index) const {
      return (m_record_size);
    }

    // Returns the maximum size of inline records PAYLOAD only!
    ham_u32_t get_max_inline_record_size() const {
      return (m_record_size);
    }

    // Returns the maximum size of inline records INCL overhead!
    ham_u32_t get_total_inline_record_size() const {
      return (get_max_inline_record_size());
    }

    // Returns the flags of a record
    ham_u8_t get_record_flags(ham_u32_t slot, ham_u32_t duplicate_index) const {
      ham_u8_t flags = m_keys->get_key_flags(slot);
      return (flags & (BtreeRecord::kBlobSizeTiny
                        | BtreeRecord::kBlobSizeSmall
                        | BtreeRecord::kBlobSizeEmpty));
    }

    // Sets the flags of a record
    void set_record_flags(ham_u32_t slot, ham_u8_t flags,
                    ham_u32_t duplicate_index) {
      ham_u8_t oldflags = m_keys->get_key_flags(slot);
      oldflags &= ~(BtreeRecord::kBlobSizeTiny
                      | BtreeRecord::kBlobSizeSmall
                      | BtreeRecord::kBlobSizeEmpty);
      m_keys->set_key_flags(slot, oldflags | flags);
    }

    // Returns true if a record in a duplicate table is inline
    bool table_is_record_inline(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      return (true);
    }

    // Returns the size of a record from the duplicate table
    ham_u32_t table_get_record_size(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      return (m_record_size);
    }

    // Returns a record id from the duplicate table
    ham_u64_t table_get_record_id(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(duplicate_index < DuplicateTable::get_count(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      return (ham_db2h_offset(*(ham_u64_t *)ptr));
    }

    // Sets a record id in the duplicate table
    void table_set_record_id(ByteArray *table,
                    ham_u32_t duplicate_index, ham_u64_t id) {
      ham_assert(duplicate_index < DuplicateTable::get_count(table));
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      *(ham_u64_t *)ptr = ham_h2db_offset(*(ham_u64_t *)ptr);
    }

    // Returns a pointer to the inline record data from the duplicate table
    ham_u8_t *table_get_record_data(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(duplicate_index < DuplicateTable::get_capacity(table));
      ham_u8_t *ptr = (ham_u8_t *)table->get_ptr();
      ptr += 8; // skip count, capacity and other records
      ptr += get_max_inline_record_size() * duplicate_index;
      return (ptr);
    }

    // Sets the inline record data in the duplicate table
    void table_set_record_data(ByteArray *table, ham_u32_t duplicate_index,
                    void *data, ham_u32_t size) {
      ham_assert(duplicate_index < DuplicateTable::get_capacity(table));
      ham_assert(size == m_record_size);
      ham_u8_t *ptr = table_get_record_data(table, duplicate_index);
      memcpy(ptr, data, size);
    }

    // Deletes a record from the table; also adjusts the count
    void table_erase_record(ByteArray *table, ham_u32_t duplicate_index) {
      ham_u32_t count = DuplicateTable::get_count(table);
      ham_assert(duplicate_index < count);
      ham_assert(count > 0);
      if (duplicate_index < count - 1) {
        ham_u8_t *lhs = table_get_record_data(table, duplicate_index);
        ham_u8_t *rhs = table_get_record_data(table, duplicate_index + 1);
        memmove(lhs, rhs, get_max_inline_record_size()
                                * (count - duplicate_index - 1));
      }
      // adjust the counter
      DuplicateTable::set_count(table, count - 1);
    }

  private:
    // the record size, as specified when the database was created
    ham_u32_t m_record_size;

    // pointer to the parent layout
    NodeType *m_keys;
};

//
// A RecordList for variable length inline records of size 8 (for internal
// nodes, no duplicates). Internal nodes only store Page IDs as records. This
// class is optimized for record IDs.
//
template<typename KeyList>
class InternalInlineRecordImpl
{
    typedef DefaultNodeImpl<KeyList, InternalInlineRecordImpl> NodeType;

  public:
    // Constructor
    InternalInlineRecordImpl(NodeType *layout, ham_u32_t record_size)
      : m_keys(layout) {
    }

    // Constructor
    InternalInlineRecordImpl(const NodeType *layout, ham_u32_t record_size)
      : m_keys((NodeType *)layout) {
    }

    // Returns true if the record is inline
    bool is_record_inline(ham_u32_t slot, ham_u32_t duplicate_index = 0) const {
      ham_assert(duplicate_index == 0);
      return (true);
    }

    // Sets the inline record data
    void set_inline_record_data(ham_u32_t slot, const void *data,
                    ham_u32_t size, ham_u32_t duplicate_index) {
      ham_assert(size == sizeof(ham_u64_t));
      ham_assert(duplicate_index == 0);
      char *p = (char *)m_keys->get_inline_record_data(slot);
      memcpy(p, data, size);
    }

    // Returns the size of the record, if inline
    ham_u32_t get_inline_record_size(ham_u32_t slot,
                    ham_u32_t duplicate_index) const {
      ham_assert(duplicate_index == 0);
      return (sizeof(ham_u64_t));
    }

    // Returns the maximum size of inline records PAYLOAD only!
    ham_u32_t get_max_inline_record_size() const {
      return (sizeof(ham_u64_t));
    }

    // Returns the maximum size of inline records INCL overhead!
    ham_u32_t get_total_inline_record_size() const {
      return (get_max_inline_record_size());
    }

    // Returns the flags of a record
    ham_u8_t get_record_flags(ham_u32_t slot, ham_u32_t duplicate_index) const {
      ham_u8_t flags = m_keys->get_key_flags(slot);
      return (flags & (BtreeRecord::kBlobSizeTiny
                        | BtreeRecord::kBlobSizeSmall
                        | BtreeRecord::kBlobSizeEmpty));
    }

    // Sets the flags of a record
    void set_record_flags(ham_u32_t slot, ham_u8_t flags,
                    ham_u32_t duplicate_index) {
      ham_u8_t oldflags = m_keys->get_key_flags(slot);
      oldflags &= ~(BtreeRecord::kBlobSizeTiny
                      | BtreeRecord::kBlobSizeSmall
                      | BtreeRecord::kBlobSizeEmpty);
      m_keys->set_key_flags(slot, oldflags | flags);
    }

    // Returns true if a record in a duplicate table is inline
    bool table_is_record_inline(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(!"shouldn't be here");
      return (false);
    }

    // Returns the size of a record from the duplicate table
    ham_u32_t table_get_record_size(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(!"shouldn't be here");
      return (0);
    }

    // Returns a record id from the duplicate table
    ham_u64_t table_get_record_id(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(!"shouldn't be here");
      return (0);
    }

    // Sets a record id in the duplicate table
    void table_set_record_id(ByteArray *table,
                    ham_u32_t duplicate_index, ham_u64_t id) {
      ham_assert(!"shouldn't be here");
    }

    // Returns a pointer to the inline record data from the duplicate table
    ham_u8_t *table_get_record_data(ByteArray *table,
                    ham_u32_t duplicate_index) const {
      ham_assert(!"shouldn't be here");
      return (0);
    }

    // Sets the inline record data in the duplicate table
    void table_set_record_data(ByteArray *table, ham_u32_t duplicate_index,
                    void *data, ham_u32_t size) {
      ham_assert(!"shouldn't be here");
    }

    // Deletes a record from the table; also adjusts the count
    void table_erase_record(ByteArray *table, ham_u32_t duplicate_index) {
      ham_assert(!"shouldn't be here");
    }

  private:
    NodeType *m_keys;
};

//
// A helper class to sort ranges; used during validation of the up-front
// index in check_index_integrity()
//
struct SortHelper {
  ham_u32_t offset;
  ham_u32_t slot;

  bool operator<(const SortHelper &rhs) const {
    return (offset < rhs.offset);
  }
};

static bool
sort_by_offset(const SortHelper &lhs, const SortHelper &rhs) {
  return (lhs.offset < rhs.offset);
}

//
// A BtreeNodeProxy layout which stores key flags, key size, key data
// and the record pointer next to each other.
// This is the format used since the initial hamsterdb version.
//
template<typename KeyList, typename RecordList>
class DefaultNodeImpl
{
    // for caching external keys
    typedef std::map<ham_u64_t, ByteArray> ExtKeyCache;

    // for caching external duplicate tables
    typedef std::map<ham_u64_t, ByteArray> DupTableCache;

    // the type of |this| object
    typedef DefaultNodeImpl<KeyList, RecordList> NodeType;

    enum {
      // for capacity, freelist_count, next_offset
      kPayloadOffset = 12,

      // only rearrange if freelist_count is high enough
      kRearrangeThreshold = 5,

      // sizeof(ham_u64_t) + 1 (for flags)
      kExtendedDuplicatesSize = 9
    };

  public:
    // Constructor
    DefaultNodeImpl(Page *page)
      : m_page(page), m_node(PBtreeNode::from_page(m_page)),
        m_index(this), m_records(this, m_page->get_db()->get_record_size()),
        m_extkey_cache(0), m_duptable_cache(0), m_recalc_capacity(false) {
      initialize();
    }

    // Destructor
    ~DefaultNodeImpl() {
      clear_caches();
    }

    // Checks the integrity of this node. Throws an exception if there is a
    // violation.
    void check_integrity() const {
      if (m_node->get_count() == 0)
        return;

      ByteArray arena;
      ham_u32_t count = m_node->get_count();
      for (ham_u32_t i = 0; i < count; i++) {
        // internal nodes: only allowed flag is kExtendedKey
        if ((get_key_flags(i) != 0
            && get_key_flags(i) != BtreeKey::kExtendedKey)
            && !m_node->is_leaf()) {
          ham_log(("integrity check failed in page 0x%llx: item #%u "
                  "has flags but it's not a leaf page",
                  m_page->get_address(), i));
          throw Exception(HAM_INTEGRITY_VIOLATED);
        }

        if (get_key_size(i) > get_extended_threshold()
            && !(get_key_flags(i) & BtreeKey::kExtendedKey)) {
          ham_log(("key size %d, but is not extended", get_key_size(i)));
          throw Exception(HAM_INTEGRITY_VIOLATED);
        }

        if (get_key_flags(i) & BtreeKey::kInitialized) {
          ham_log(("integrity check failed in page 0x%llx: item #%u"
                  "is initialized (w/o record)", m_page->get_address(), i));
          throw Exception(HAM_INTEGRITY_VIOLATED);
        }

        if (get_key_flags(i) & BtreeKey::kExtendedKey) {
          ham_u64_t blobid = get_extended_blob_id(i);
          if (!blobid) {
            ham_log(("integrity check failed in page 0x%llx: item "
                    "is extended, but has no blob", m_page->get_address()));
            throw Exception(HAM_INTEGRITY_VIOLATED);
          }

          // make sure that the extended blob can be loaded
          ham_record_t record = {0};
          m_page->get_db()->get_local_env()->get_blob_manager()->read(
                          m_page->get_db(), blobid, &record, 0, &arena);

          // compare it to the cached key (if there is one)
          if (m_extkey_cache) {
            ExtKeyCache::iterator it = m_extkey_cache->find(blobid);
            if (it != m_extkey_cache->end()) {
              if (record.size != it->second.get_size()) {
                ham_log(("Cached extended key differs from real key"));
                throw Exception(HAM_INTEGRITY_VIOLATED);
              }
              if (memcmp(record.data, it->second.get_ptr(), record.size)) {
                ham_log(("Cached extended key differs from real key"));
                throw Exception(HAM_INTEGRITY_VIOLATED);
              }
            }
          }
        }
      }

      check_index_integrity(m_node->get_count());
    }

    // Compares two keys
    template<typename Cmp>
    int compare(const ham_key_t *lhs, ham_u32_t rhs, Cmp &cmp) {
      if (get_key_flags(rhs) & BtreeKey::kExtendedKey) {
        ham_key_t tmp = {0};
        get_extended_key(get_extended_blob_id(rhs), &tmp);
        return (cmp(lhs->data, lhs->size, tmp.data, tmp.size));
      }
      return (cmp(lhs->data, lhs->size, get_key_data(rhs), get_key_size(rhs)));
    }

    // Searches the node for the key and returns the slot of this key
    template<typename Cmp>
    int find_child(ham_key_t *key, Cmp &comparator, ham_u64_t *precord_id,
                    int *pcmp) {
      ham_u32_t count = m_node->get_count();
      int i, l = 1, r = count - 1;
      int ret = 0, last = count + 1;
      int cmp = -1;

#ifdef HAM_DEBUG
      check_index_integrity(count);
#endif

      ham_assert(count > 0);

      for (;;) {
        /* get the median item; if it's identical with the "last" item,
         * we've found the slot */
        i = (l + r) / 2;

        if (i == last) {
          ham_assert(i >= 0);
          ham_assert(i < (int)count);
          cmp = 1;
          ret = i;
          break;
        }

        /* compare it against the key */
        cmp = compare(key, i, comparator);

        /* found it? */
        if (cmp == 0) {
          ret = i;
          break;
        }
        /* if the key is bigger than the current item: search "to the left" */
        else if (cmp < 0) {
          if (r == 0) {
            ham_assert(i == 0);
            ret = -1;
            break;
          }
          r = i - 1;
        }
        /* if the key is smaller than the current item: search "to the right" */
        else {
          last = i;
          l = i + 1;
        }
      }

      *pcmp = cmp;
      if (precord_id) {
        if (ret == -1)
          *precord_id = m_node->get_ptr_down();
        else
          *precord_id = get_record_id(ret);
      }
      return (ret);
    }

    // Searches the node for the key and returns the slot of this key
    // - only for exact matches!
    template<typename Cmp>
    int find_exact(ham_key_t *key, Cmp &comparator) {
      int cmp;
      int r = find_child(key, comparator, 0, &cmp);
      if (cmp)
        return (-1);
      return (r);
    }

    // Iterates all keys, calls the |visitor| on each
    void scan(ScanVisitor *visitor, ham_u32_t start, bool distinct) {
      ham_u32_t count = m_node->get_count() - start;
      ham_key_t key = {0};

      for (ham_u32_t i = start; i < count; i++) {
        void *key_data;
        ham_u16_t key_size;
        if (get_key_flags(i) & BtreeKey::kExtendedKey) {
          get_key(i, &m_arena, &key);
          key_data = key.data;
          key_size = key.size;
        }
        else {
          key_data = get_key_data(i);
          key_size = get_key_size(i);
        }
        (*visitor)(key_data, key_size, distinct ? 1 : get_record_count(i));
      }
    }

    // Returns a deep copy of the key
    void get_key(ham_u32_t slot, ByteArray *arena, ham_key_t *dest) {
      LocalDatabase *db = m_page->get_db();

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif

      if (!(dest->flags & HAM_KEY_USER_ALLOC)) {
        arena->resize(get_key_size(slot));
        dest->data = arena->get_ptr();
        dest->size = get_key_size(slot);
      }

      if (get_key_flags(slot) & BtreeKey::kExtendedKey) {
        ham_key_t tmp = {0};
        get_extended_key(get_extended_blob_id(slot), &tmp);
        memcpy(dest->data, tmp.data, tmp.size);
      }
      else
        memcpy(dest->data, get_key_data(slot), get_key_size(slot));

      /* recno databases: recno is stored in db-endian! */
      if (db->get_rt_flags() & HAM_RECORD_NUMBER) {
        ham_assert(dest->data != 0);
        ham_assert(dest->size == sizeof(ham_u64_t));
        ham_u64_t recno = *(ham_u64_t *)dest->data;
        recno = ham_db2h64(recno);
        memcpy(dest->data, &recno, sizeof(ham_u64_t));
      }
    }

    // Returns the number of records of a key
    ham_u32_t get_record_count(ham_u32_t slot) {
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates) {
        ByteArray table = get_duplicate_table(get_record_id(slot));
        return (DuplicateTable::get_count(&table));
      }
      return (m_keys.get_inline_record_count(slot));
    }

    // Returns the full record and stores it in |dest|
    void get_record(ham_u32_t slot, ByteArray *arena, ham_record_t *record,
                    ham_u32_t flags, ham_u32_t duplicate_index) {
      LocalDatabase *db = m_page->get_db();
      LocalEnvironment *env = db->get_local_env();

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif

      // extended duplicate table
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates) {
        ByteArray table = get_duplicate_table(get_record_id(slot));
        if (m_records.table_is_record_inline(&table, duplicate_index)) {
          ham_u32_t size = m_records.table_get_record_size(&table,
                                    duplicate_index);
          if (size == 0) {
            record->data = 0;
            record->size = 0;
            return;
          }
          if (flags & HAM_PARTIAL) {
            ham_trace(("flag HAM_PARTIAL is not allowed if record->size <= 8"));
            throw Exception(HAM_INV_PARAMETER);
          }
          if (!(record->flags & HAM_RECORD_USER_ALLOC)
              && (flags & HAM_DIRECT_ACCESS)) {
            record->data = m_records.table_get_record_data(&table,
                                duplicate_index);
          }
          else {
            if (!(record->flags & HAM_RECORD_USER_ALLOC)) {
              arena->resize(size);
              record->data = arena->get_ptr();
            }
            void *p = m_records.table_get_record_data(&table,
                                duplicate_index);
            memcpy(record->data, p, size);
          }
          record->size = size;
          return;
        }

        // non-inline duplicate (record data is a record ID)
        ham_u64_t rid = m_records.table_get_record_id(&table,
                        duplicate_index);
        env->get_blob_manager()->read(db, rid, record, flags, arena);
        return;
      }

      // inline records (with or without duplicates)
      if (is_record_inline(slot, duplicate_index)) {
        ham_u32_t size = get_inline_record_size(slot, duplicate_index);
        if (size == 0) {
          record->data = 0;
          record->size = 0;
          return;
        }
        if (flags & HAM_PARTIAL) {
          ham_trace(("flag HAM_PARTIAL is not allowed if record->size <= 8"));
          throw Exception(HAM_INV_PARAMETER);
        }
        if (!(record->flags & HAM_RECORD_USER_ALLOC)
            && (flags & HAM_DIRECT_ACCESS)) {
          record->data = get_inline_record_data(slot, duplicate_index);
        }
        else {
          if (!(record->flags & HAM_RECORD_USER_ALLOC)) {
            arena->resize(size);
            record->data = arena->get_ptr();
          }
          void *p = get_inline_record_data(slot, duplicate_index);
          memcpy(record->data, p, size);
        }
        record->size = size;
        return;
      }

      // non-inline duplicate (record data is a record ID)
      env->get_blob_manager()->read(db, get_record_id(slot, duplicate_index),
                                  record, flags, arena);
    }

    // Sets the record of a key, or adds a duplicate
    void set_record(ham_u32_t slot, ham_record_t *record,
                    ham_u32_t duplicate_index, ham_u32_t flags,
                    ham_u32_t *new_duplicate_index) {
      LocalDatabase *db = m_page->get_db();
      LocalEnvironment *env = db->get_local_env();

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif

      // automatically overwrite an existing key unless this is a
      // duplicate operation
      if ((flags & (HAM_DUPLICATE
                    | HAM_DUPLICATE
                    | HAM_DUPLICATE_INSERT_BEFORE
                    | HAM_DUPLICATE_INSERT_AFTER
                    | HAM_DUPLICATE_INSERT_FIRST
                    | HAM_DUPLICATE_INSERT_LAST)) == 0)
        flags |= HAM_OVERWRITE;

      // record does not yet exist - simply overwrite the first record
      // of this key
      if (get_key_flags(slot) & BtreeKey::kInitialized) {
        flags |= HAM_OVERWRITE;
        duplicate_index = 0;
        // also remove the kInitialized flag
        set_key_flags(slot, get_key_flags(slot) & (~BtreeKey::kInitialized));
        // fall through into the next branch
      }

      // a key is overwritten (NOT in a duplicate table)
      if ((flags & HAM_OVERWRITE)
          && !(get_key_flags(slot) & BtreeKey::kExtendedDuplicates)) {
        // existing record is stored non-inline (in a blob)
        if (!is_record_inline(slot, duplicate_index)) {
          ham_u64_t ptr = get_record_id(slot, duplicate_index);
          // non-inline record is overwritten with another non-inline record?
          if (record->size > get_max_inline_record_size()) {
            ptr = env->get_blob_manager()->overwrite(db, ptr, record, flags);
            set_record_id(slot, ptr, duplicate_index);
            return;
          }
          // free the existing record (if there is one)
          env->get_blob_manager()->erase(db, ptr);
          // fall through
        }
        // write the new inline record
        if (record->size <= get_max_inline_record_size())
          set_inline_record_data(slot, record->data, record->size,
                          duplicate_index);
        // ... or the non-inline record
        else {
          ham_u64_t ptr = env->get_blob_manager()->allocate(db, record, flags);
          m_records.set_record_flags(slot, 0, duplicate_index);
          set_record_id(slot, ptr, duplicate_index);
        }

#ifdef HAM_DEBUG
        check_index_integrity(m_node->get_count());
#endif
        return;
      }

      // a key is overwritten in a duplicate table
      if ((flags & HAM_OVERWRITE)
          && (get_key_flags(slot) & BtreeKey::kExtendedDuplicates)) {
        ByteArray table = get_duplicate_table(get_record_id(slot));
        // old record is non-inline? free or overwrite it
        if (!m_records.table_is_record_inline(&table, duplicate_index)) {
          ham_u64_t ptr = m_records.table_get_record_id(&table,
                                duplicate_index);
          if (record->size > get_max_inline_record_size()) {
            ptr = env->get_blob_manager()->overwrite(db, ptr, record, flags);
            m_records.table_set_record_id(&table, duplicate_index, ptr);
            return;
          }
          db->get_local_env()->get_blob_manager()->erase(db, ptr, 0);
        }

        // now overwrite with an inline key
        if (record->size <= get_max_inline_record_size())
          m_records.table_set_record_data(&table, duplicate_index,
                          record->data, record->size);
        // ... or with a (non-inline) key
        else {
          m_records.table_set_record_id(&table, duplicate_index,
                    env->get_blob_manager()->allocate(db, record, flags));
        }
#ifdef HAM_DEBUG
        check_index_integrity(m_node->get_count());
#endif
        return;
      }

      // from here on we insert duplicate keys!

      // it's possible that the page is full and new space cannot be allocated.
      // even if there's no duplicate overflow table (yet): check if a new
      // record would fit into the page. If not then force the use of a
      // duplicate table.
      bool force_duptable = false;

      if ((get_key_flags(slot) & BtreeKey::kExtendedDuplicates) == 0) {
        ham_u32_t threshold = get_duplicate_threshold();

        if (m_keys.get_inline_record_count(slot) >= threshold)
          force_duptable = true;
        else {
          ham_u32_t tmpsize = get_total_key_data_size(slot)
                        + get_total_inline_record_size();

          if (!has_enough_space(tmpsize, false))
            force_duptable = true;
        }

        // already too many duplicates, or the record does not fit? then
        // allocate an overflow duplicate list and move all duplicates to
        // this list
        if (force_duptable) {
          ham_u32_t size = 8 + get_total_inline_record_size() * (threshold * 2);
          ByteArray table(size);
          DuplicateTable::set_count(&table,
                          m_keys.get_inline_record_count(slot));
          DuplicateTable::set_capacity(&table, threshold * 2);
          ham_u8_t *ptr = (ham_u8_t *)table.get_ptr() + 8;
          memcpy(ptr, get_inline_record_data(slot, 0),
                          m_keys.get_inline_record_count(slot)
                                * get_total_inline_record_size());

          // add to cache
          ham_u64_t tableid = add_duplicate_table(&table);

          // allocate new space, copy the key data and write the new record id
          m_keys.set_key_data_offset(slot,
                          append_key(m_node->get_count(), get_key_data(slot),
                                  get_key_data_size(slot)
                                        + kExtendedDuplicatesSize,
                                  true));

          // finally write the new record id
          set_key_flags(slot, get_key_flags(slot)
                          | BtreeKey::kExtendedDuplicates);
          set_record_id(slot, tableid);
          set_inline_record_count(slot, 0);

          // adjust next offset
          m_index.set_next_offset(m_index.calc_next_offset(m_node->get_count()));

          // ran out of space? rearrange, otherwise the space which just was
          // freed would be lost
          if (force_duptable)
            rearrange(m_node->get_count(), true);

          // fall through
        }
      }

      // a duplicate table exists
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates) {
        ByteArray table = get_duplicate_table(get_record_id(slot));
        ham_u32_t count = DuplicateTable::get_count(&table);

        // adjust flags
        if (flags & HAM_DUPLICATE_INSERT_BEFORE && duplicate_index == 0)
          flags |= HAM_DUPLICATE_INSERT_FIRST;
        else if (flags & HAM_DUPLICATE_INSERT_AFTER) {
          if (duplicate_index == count)
            flags |= HAM_DUPLICATE_INSERT_LAST;
          else {
            flags |= HAM_DUPLICATE_INSERT_BEFORE;
            duplicate_index++;
          }
        }

        // need to resize the table? (also update the cache)
        if (count == DuplicateTable::get_capacity(&table))
          table = grow_duplicate_table(get_record_id(slot));

        ham_u32_t position;

        // handle overwrites or inserts/appends
        if (flags & HAM_DUPLICATE_INSERT_FIRST) {
          if (count) {
            ham_u8_t *ptr = m_records.table_get_record_data(&table, 0);
            memmove(m_records.table_get_record_data(&table, 1),
                        ptr, count * get_total_inline_record_size());
          }
          position = 0;

        }
        else if (flags & HAM_DUPLICATE_INSERT_BEFORE) {
          memmove(m_records.table_get_record_data(&table, duplicate_index),
                      m_records.table_get_record_data(&table,
                              duplicate_index + 1),
                      (count - duplicate_index)
                            * get_total_inline_record_size());
          position = duplicate_index;
        }
        else // HAM_DUPLICATE_INSERT_LAST
          position = count;

        // now write the record
        if (record->size <= get_max_inline_record_size())
          m_records.table_set_record_data(&table, position, record->data,
                    record->size);
        else
          m_records.table_set_record_id(&table, position,
                    env->get_blob_manager()->allocate(db, record, flags));

        DuplicateTable::set_count(&table, count + 1);

        // store the modified duplicate table
        set_record_id(slot, flush_duplicate_table(get_record_id(slot), &table));

        if (new_duplicate_index)
          *new_duplicate_index = position;

#ifdef HAM_DEBUG
        check_index_integrity(m_node->get_count());
#endif
        return;
      }

      // still here? then we have to insert/append a duplicate to an existing
      // inline duplicate list. This means we have to allocte new space
      // for the list, and add the previously used space to the freelist (if
      // there is enough capacity).
      ham_u32_t required_size = get_total_key_data_size(slot)
                                    + get_total_inline_record_size();
      ham_u32_t offset = allocate(m_node->get_count(), required_size, true);

      // first copy the key data
      ham_u8_t *orig = get_key_data(slot);
      ham_u8_t *dest = m_node->get_data() + kPayloadOffset + offset
                      + m_keys.get_key_index_span() * m_index.get_capacity();
      ham_u32_t size = get_key_data_size(slot);
      memcpy(dest, orig, size);
      orig += size;
      dest += size;

      // adjust flags
      if (flags & HAM_DUPLICATE_INSERT_BEFORE && duplicate_index == 0)
        flags |= HAM_DUPLICATE_INSERT_FIRST;
      else if (flags & HAM_DUPLICATE_INSERT_AFTER) {
        if (duplicate_index == m_keys.get_inline_record_count(slot))
          flags |= HAM_DUPLICATE_INSERT_LAST;
        else {
          flags |= HAM_DUPLICATE_INSERT_BEFORE;
          duplicate_index++;
        }
      }

      // store the new offset, otherwise set_inline_record_data() et al
      // will not work
      bool set_offset = (m_index.get_next_offset()
                        == m_keys.get_key_data_offset(slot)
                            + get_total_key_data_size(slot));
      m_keys.set_key_data_offset(slot, offset);

      ham_u32_t position;

      // we have enough space - copy all duplicates, and insert the new
      // duplicate wherever required
      if (flags & HAM_DUPLICATE_INSERT_FIRST) {
        memcpy(dest + get_total_inline_record_size(), orig,
                m_keys.get_inline_record_count(slot)
                      * get_total_inline_record_size());
        position = 0;

      }
      else if (flags & HAM_DUPLICATE_INSERT_BEFORE) {
        memcpy(dest, orig, duplicate_index * get_total_inline_record_size());
        orig += duplicate_index * get_total_inline_record_size();
        dest += (duplicate_index + 1) * get_total_inline_record_size();
        memcpy(dest, orig,
                  (m_keys.get_inline_record_count(slot) - duplicate_index)
                        * get_total_inline_record_size());
        position = duplicate_index;
      }
      else { // HAM_DUPLICATE_INSERT_LAST
        memcpy(dest, orig, m_keys.get_inline_record_count(slot)
                                * get_total_inline_record_size());
        position = m_keys.get_inline_record_count(slot);
      }

      // now insert the record
      if (record->size <= get_max_inline_record_size())
        set_inline_record_data(slot, record->data, record->size, position);
      else
        set_record_id(slot, env->get_blob_manager()->allocate(db,
                                record, flags), position);

      if (new_duplicate_index)
        *new_duplicate_index = position;

      // increase the record counter
      set_inline_record_count(slot, m_keys.get_inline_record_count(slot) + 1);

      // adjust next offset, if necessary
      if (set_offset)
        m_index.set_next_offset(m_index.calc_next_offset(m_node->get_count()));

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif
    }

    // Returns the record size of a key or one of its duplicates
    ham_u64_t get_record_size(ham_u32_t slot, int duplicate_index) {
      ham_u64_t ptr = 0;

      // extended duplicate table
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates) {
        ByteArray table = get_duplicate_table(get_record_id(slot));
        if (m_records.table_is_record_inline(&table, duplicate_index))
          return (m_records.table_get_record_size(&table,
                                  duplicate_index));

        ptr = m_records.table_get_record_id(&table, duplicate_index);
      }
      // inline record
      else {
        if (is_record_inline(slot, duplicate_index))
          return (get_inline_record_size(slot, duplicate_index));
        ptr = get_record_id(slot, duplicate_index);
      }

      LocalDatabase *db = m_page->get_db();
      LocalEnvironment *env = db->get_local_env();
      return (env->get_blob_manager()->get_blob_size(db, ptr));
    }

    // Erases an extended key
    void erase_key(ham_u32_t slot) {
      if (get_key_flags(slot) & BtreeKey::kExtendedKey) {
        // delete the extended key from the cache
        erase_extended_key(get_extended_blob_id(slot));
        // and transform into a key which is non-extended and occupies
        // the same space as before, when it was extended
        set_key_flags(slot, get_key_flags(slot) & (~BtreeKey::kExtendedKey));
        set_key_size(slot, sizeof(ham_u64_t));
      }
    }

    // Erases one (or all) records of a key
    void erase_record(ham_u32_t slot, ham_u32_t duplicate_index,
                    bool all_duplicates) {
      LocalDatabase *db = m_page->get_db();

      // extended duplicate list?
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates) {
        ham_u64_t ptr;
        ByteArray table = get_duplicate_table(get_record_id(slot));
        // delete ALL duplicates?
        if (all_duplicates) {
          ham_u32_t count = DuplicateTable::get_count(&table);
          for (ham_u32_t i = 0; i < count; i++) {
            // non-inline record? free the blob
            if (!m_records.table_is_record_inline(&table, i)) {
              ptr = m_records.table_get_record_id(&table, i);
              db->get_local_env()->get_blob_manager()->erase(db, ptr, 0);
            }
          }

          DuplicateTable::set_count(&table, 0);
          // fall through
        }
        else {
          // non-inline record? free the blob
          if (!m_records.table_is_record_inline(&table, duplicate_index)) {
            ptr = m_records.table_get_record_id(&table, duplicate_index);
            db->get_local_env()->get_blob_manager()->erase(db, ptr, 0);
          }

          // remove the record from the duplicate table
          m_records.table_erase_record(&table, duplicate_index);
        }

        // if the table is now empty: delete it
        if (DuplicateTable::get_count(&table) == 0) {
          ham_u32_t flags = get_key_flags(slot);
          set_key_flags(slot, flags & (~BtreeKey::kExtendedDuplicates));

          // delete duplicate table blob and cached table
          erase_duplicate_table(get_record_id(slot));

          // adjust next offset
          if (m_index.get_next_offset() == m_keys.get_key_data_offset(slot)
                                        + get_key_data_size(slot)
                                        + kExtendedDuplicatesSize)
            m_index.set_next_offset(m_index.get_next_offset() - kExtendedDuplicatesSize);
        }
        // otherwise store the modified table
        else {
          set_record_id(slot,
                      flush_duplicate_table(get_record_id(slot), &table));
        }
#ifdef HAM_DEBUG
        check_index_integrity(m_node->get_count());
#endif
        return;
      }

      // handle duplicate keys
      ham_u32_t record_count = m_keys.get_inline_record_count(slot);
      ham_assert(record_count > 0);

      // ALL duplicates?
      if (all_duplicates) {
        // if records are not inline: delete the blobs
        for (ham_u32_t i = 0; i < record_count; i++) {
          if (!is_record_inline(slot, i))
            db->get_local_env()->get_blob_manager()->erase(db,
                            get_record_id(slot, i), 0);
            remove_inline_record(slot, i);
        }
        set_inline_record_count(slot, 0);

        // adjust next offset?
        if (m_index.get_next_offset() == m_keys.get_key_data_offset(slot)
                        + get_key_data_size(slot)
                        + get_total_inline_record_size() * record_count)
          m_index.set_next_offset(m_keys.get_key_data_offset(slot)
                        + get_key_data_size(slot));
      }
      else {
        // if record is not inline: delete the blob
        if (!is_record_inline(slot, duplicate_index))
          db->get_local_env()->get_blob_manager()->erase(db,
                          get_record_id(slot, duplicate_index), 0);

        // shift duplicate records "to the left"
        if (duplicate_index + 1 < m_keys.get_inline_record_count(slot))
          memmove(get_inline_record_data(slot, duplicate_index),
                        get_inline_record_data(slot, duplicate_index + 1),
                        get_total_inline_record_size() * (record_count - 1));
        else
          remove_inline_record(slot, duplicate_index);

        // decrease record counter and adjust next offset
        set_inline_record_count(slot, record_count - 1);
        if (m_index.get_next_offset() == m_keys.get_key_data_offset(slot)
                        + get_key_data_size(slot)
                        + get_total_inline_record_size() * record_count)
          m_index.set_next_offset(m_index.get_next_offset()
                          - get_total_inline_record_size());
      }
#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif
    }

    // Erases a key from the index. Does NOT erase the records!
    void erase(ham_u32_t slot) {
#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif

      // if this is the last key in this page: just re-initialize
      if (m_node->get_count() == 1) {
        m_index.set_freelist_count(0);
        m_index.set_next_offset(0);
        return;
      }

      // adjust next offset?
      bool recalc_offset = false;
      if (m_index.get_next_offset() == m_keys.get_key_data_offset(slot)
                                    + get_key_data_size(slot)
                                    + get_total_inline_record_size())
        recalc_offset = true;

      // get rid of the extended key (if there is one)
      erase_key(slot);

      // now add this key to the freelist
      if (m_index.get_freelist_count() + 1 + m_node->get_count()
                      <= m_index.get_capacity())
        m_index.copy_to_freelist(slot);

      // then remove index key by shifting all remaining indices/freelist
      // items "to the left"
      memmove(m_keys.get_key_index_ptr(slot),
                      m_keys.get_key_index_ptr(slot + 1),
                      m_keys.get_key_index_span()
                          * (m_index.get_freelist_count() + m_node->get_count()
                                    - slot - 1));

      if (recalc_offset)
        m_index.set_next_offset(m_index.calc_next_offset(m_node->get_count() - 1));

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count() - 1);
#endif
    }

    // Inserts a new key at |slot|. 
    // Also inserts an empty record which has to be overwritten in
    // the next call of set_record().
    void insert(ham_u32_t slot, const ham_key_t *key) {
      ham_u32_t count = m_node->get_count();

#ifdef HAM_DEBUG
      check_index_integrity(count);
#endif

      ham_u32_t offset = (ham_u32_t)-1;
      bool extended_key = key->size > get_extended_threshold();

      // search the freelist for free key space
      int idx = m_index.find_in_freelist(count,
                      (extended_key ? sizeof(ham_u64_t) : key->size)
                            + get_total_inline_record_size());
      // found: remove this freelist entry
      if (idx != -1) {
        offset = m_keys.get_key_data_offset(idx);
        ham_u32_t size = get_total_key_data_size(idx);
        m_index.remove_from_freelist(idx);
        // adjust the next key offset, if required
        if (m_index.get_next_offset() == offset + size)
          m_index.set_next_offset(offset
                      + (extended_key ? sizeof(ham_u64_t) : key->size)
                      + get_total_inline_record_size());
      }
      // not found: append at the end
      else {
        if (count == 0)
          offset = 0;
        else
          offset = m_index.get_next_offset();

        // make sure that the key really fits! if not then use an extended key.
        // this can happen if a page is split, but the new key still doesn't
        // fit into the splitted page.
        if (!extended_key) {
          if (offset + m_keys.get_key_index_span() * m_index.get_capacity()
              + key->size + get_total_inline_record_size()
                  >= get_usable_page_size()) {
            extended_key = true;
            // check once more if the key fits
            ham_assert(offset
                  + m_keys.get_key_index_span() * m_index.get_capacity()
                  + sizeof(ham_u64_t) + get_total_inline_record_size()
                      < get_usable_page_size());
          }
        }

        m_index.set_next_offset(offset
                        + (extended_key ? sizeof(ham_u64_t) : key->size)
                        + get_total_inline_record_size());
      }

      // once more assert that the new key fits
      ham_assert(offset
              + m_keys.get_key_index_span() * m_index.get_capacity()
              + (extended_key ? sizeof(ham_u64_t) : key->size)
              + get_total_inline_record_size()
                  <= get_usable_page_size());

      // make space for the new index
      if (slot < count || m_index.get_freelist_count() > 0) {
        memmove(m_keys.get_key_index_ptr(slot + 1),
                      m_keys.get_key_index_ptr(slot),
                      m_keys.get_key_index_span()
                            * (count + m_index.get_freelist_count() - slot));
      }

      // store the key index
      m_keys.set_key_data_offset(slot, offset);

      // now finally copy the key data
      if (extended_key) {
        ham_u64_t blobid = add_extended_key(key);

        set_extended_blob_id(slot, blobid);
        // remove all flags, set Extended flag
        set_key_flags(slot, BtreeKey::kExtendedKey | BtreeKey::kInitialized);
        set_key_size(slot, key->size);
      }
      else {
        set_key_flags(slot, BtreeKey::kInitialized);
        set_key_size(slot, key->size);
        set_key_data(slot, key->data, key->size);
      }

      set_inline_record_count(slot, 1);
      m_records.set_record_flags(slot, BtreeRecord::kBlobSizeEmpty, 0);

#ifdef HAM_DEBUG
      check_index_integrity(count + 1);
#endif
    }

    // Returns true if |key| cannot be inserted because a split is required.
    // Unlike implied by the name, this function will try to re-arrange the
    // node in order for the key to fit in.
    bool requires_split() {
      ham_u32_t size = m_page->get_db()->get_btree_index()->get_key_size();
      if (size == HAM_KEY_SIZE_UNLIMITED)
        size = get_extended_threshold();
      if (has_enough_space(size, true))
        return (false);

      // get rid of freelist entries and gaps
      if (rearrange(m_node->get_count()) && has_enough_space(size, true))
        return (false);

      // shift data area to left or right to make more space
      if (m_recalc_capacity)
        return (resize(m_node->get_count() + 1, size));

      return (true);
    }

    // Returns true if the node requires a merge or a shift
    bool requires_merge() const {
      return (m_node->get_count() <= 3);
    }

    // Splits this node and moves some/half of the keys to |other|
    void split(DefaultNodeImpl *other, int pivot) {
      int start = pivot;
      int count = m_node->get_count() - pivot;

#ifdef HAM_DEBUG
      check_index_integrity(m_node->get_count());
#endif
      ham_assert(0 == other->m_node->get_count());
      ham_assert(0 == other->m_index.get_freelist_count());

      // if we split a leaf then the pivot element is inserted in the leaf
      // page. in internal nodes it is propagated to the parent instead.
      // (this propagation is handled by the caller.)
      if (!m_node->is_leaf()) {
        start++;
        count--;
      }

      clear_caches();

      // make sure that the other node (which is currently empty) can fit
      // all the keys
      if (other->m_index.get_capacity() <= (ham_u32_t)count)
        other->m_index.set_capacity(count + 1); // + 1 for the pivot key

      // move |count| keys to the other node
      memcpy(other->m_keys.get_key_index_ptr(0),
                      m_keys.get_key_index_ptr(start),
                      m_keys.get_key_index_span() * count);
      for (int i = 0; i < count; i++) {
        ham_u32_t key_size = get_key_data_size(start + i);
        ham_u32_t rec_size = get_record_data_size(start + i);
        ham_u8_t *data = get_key_data(start + i);
        ham_u32_t offset = other->append_key(i, data, key_size + rec_size,
                                false);
        other->m_keys.set_key_data_offset(i, offset);
      }

      // now move all shifted keys to the freelist. those shifted keys are
      // always at the "right end" of the node, therefore we just decrease
      // m_node->get_count() and increase freelist_count simultaneously
      // (m_node->get_count() is decreased by the caller).
      m_index.set_freelist_count(m_index.get_freelist_count() + count);
      if (m_index.get_freelist_count() > kRearrangeThreshold)
        rearrange(pivot);
      else
        m_index.set_next_offset(m_index.calc_next_offset(pivot));

#ifdef HAM_DEBUG
      check_index_integrity(pivot);
      other->check_index_integrity(count);
#endif
    }

    // Merges keys from |other| to this node
    void merge_from(DefaultNodeImpl *other) {
      ham_u32_t count = m_node->get_count();
      ham_u32_t other_count = other->m_node->get_count();

#ifdef HAM_DEBUG
      check_index_integrity(count);
      other->check_index_integrity(other_count);
#endif

      other->clear_caches();

      // re-arrange the node: moves all keys sequentially to the beginning
      // of the key space, removes the whole freelist
      rearrange(m_node->get_count(), true);

      ham_assert(m_node->get_count() + other_count <= m_index.get_capacity());

      // now append all indices from the sibling
      memcpy(m_keys.get_key_index_ptr(count),
                      other->m_keys.get_key_index_ptr(0),
                      m_keys.get_key_index_span() * other_count);

      // for each new key: copy the key data
      for (ham_u32_t i = 0; i < other_count; i++) {
        ham_u32_t key_size = other->get_key_data_size(i);
        ham_u32_t rec_size = other->get_record_data_size(i);
        ham_u8_t *data = other->get_key_data(i);
        ham_u32_t offset = append_key(count + i, data,
                                key_size + rec_size, false);
        m_keys.set_key_data_offset(count + i, offset);
        m_keys.set_key_size(count + i, other->get_key_size(i));
      }

      other->m_index.set_next_offset(0);
      other->m_index.set_freelist_count(0);
#ifdef HAM_DEBUG
      check_index_integrity(count + other_count);
#endif
    }

    // Returns a record id
    ham_u64_t get_record_id(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) const {
      ham_u64_t ptr = *(ham_u64_t *)get_inline_record_data(slot,
                      duplicate_index);
      return (ham_db2h_offset(ptr));
    }

    // Sets a record id
    void set_record_id(ham_u32_t slot, ham_u64_t ptr,
                    ham_u32_t duplicate_index = 0) {
      set_key_flags(slot, get_key_flags(slot) & (~BtreeKey::kInitialized));
      ham_u8_t *p = (ham_u8_t *)get_inline_record_data(slot, duplicate_index);
      *(ham_u64_t *)p = ham_h2db_offset(ptr);
      // initialize flags
      m_records.set_record_flags(slot, 0, duplicate_index);
    }

    // Returns the key's flags
    ham_u32_t get_key_flags(ham_u32_t slot) const {
      return (m_keys.get_key_flags(slot));
    }

    // Sets the flags of a key
    void set_key_flags(ham_u32_t slot, ham_u32_t flags) {
      m_keys.set_key_flags(slot, flags);
    }

    // Returns the key size as specified by the user
    ham_u32_t get_key_size(ham_u32_t slot) const {
      return (m_keys.get_key_size(slot));
    }

    // Sets the size of a key
    void set_key_size(ham_u32_t slot, ham_u32_t size) {
      m_keys.set_key_size(slot, size);
    }

    // Returns a pointer to the (inline) key data
    ham_u8_t *get_key_data(ham_u32_t slot) {
      ham_u32_t offset = m_keys.get_key_data_offset(slot)
              + m_keys.get_key_index_span() * m_index.get_capacity();
      return (m_node->get_data() + kPayloadOffset + offset);
    }

    // Returns a pointer to the (inline) key data (const flavour)
    ham_u8_t *get_key_data(ham_u32_t slot) const {
      ham_u32_t offset = m_keys.get_key_data_offset(slot)
              + m_keys.get_key_index_span() * m_index.get_capacity();
      return (m_node->get_data() + kPayloadOffset + offset);
    }

    // Sets the inline key data
    void set_key_data(ham_u32_t slot, const void *ptr, ham_u32_t len) {
      ham_u32_t offset = m_keys.get_key_data_offset(slot)
              + m_keys.get_key_index_span() * m_index.get_capacity();
      memcpy(m_node->get_data() + kPayloadOffset + offset, ptr, len);
    }

    // Returns the flags of a record; defined in btree_flags.h
    ham_u8_t get_record_flags(ham_u32_t slot, ham_u32_t duplicate_index = 0)
                    const {
      return (m_records.get_record_flags(slot, duplicate_index));
    }

    // Removes an inline record; basically this overwrites the record's
    // data with nulls and resets the flags. Does NOT "shift" the remaining
    // records "to the left"!
    void remove_inline_record(ham_u32_t slot, ham_u32_t duplicate_index = 0) {
      m_records.set_record_flags(slot, 0, duplicate_index);
      memset(get_inline_record_data(slot, duplicate_index),
                      0, get_total_inline_record_size());
    }
    
    // Returns the capacity
    size_t get_capacity() const {
      return (m_index.get_capacity());
    }

    // Clears the page with zeroes and reinitializes it; only for testing
    void test_clear_page() {
      memset(m_page->get_payload(), 0,
                    m_page->get_db()->get_local_env()->get_usable_page_size());
      initialize();
    }

    // Sets a key; only for testing
    void test_set_key(ham_u32_t slot, const char *data,
                    size_t data_size, ham_u32_t flags, ham_u64_t record_id) {
      set_record_id(slot, record_id);
      set_key_flags(slot, flags);
      set_key_size(slot, (ham_u16_t)data_size);
      set_key_data(slot, data, (ham_u32_t)data_size);
    }

  private:
    friend class FixedInlineRecordImpl<KeyList>;
    friend class InternalInlineRecordImpl<KeyList>;
    friend class DefaultInlineRecordImpl<KeyList, true>;
    friend class DefaultInlineRecordImpl<KeyList, false>;

    // Initializes the node
    void initialize() {
      LocalDatabase *db = m_page->get_db();
      ham_u32_t key_size = db->get_btree_index()->get_key_size();

      m_keys.initialize(m_node->get_data() + kPayloadOffset, key_size);

      if (m_node->get_count() == 0 && !(db->get_rt_flags() & HAM_READ_ONLY)) {
        // ask the btree for the default capacity (it keeps track of the
        // average capacity of older pages).
        ham_u32_t capacity = db->get_btree_index()->get_statistics()->get_default_page_capacity();
        // no data so far? then come up with a good default
        if (capacity == 0) {
          ham_u32_t page_size = get_usable_page_size();

          ham_u32_t rec_size = db->get_btree_index()->get_record_size();
          if (rec_size == HAM_RECORD_SIZE_UNLIMITED || rec_size > 32)
            rec_size = 9;

          bool has_duplicates = db->get_local_env()->get_flags()
                                & HAM_ENABLE_DUPLICATES;
          capacity = page_size
                            / (m_keys.get_key_index_span()
                              + get_actual_key_size(page_size, key_size,
                                      has_duplicates)
                              + rec_size);

          // the default might not be precise and might be recalculated later
          m_recalc_capacity = true;
        }

        m_index.initialize(m_node->get_data(), capacity);
        m_index.set_freelist_count(0);
        m_index.set_next_offset(0);
      }
      else
        m_index.initialize(m_node->get_data(), 0);
    }

    // Returns the actual key size (including overhead, without record).
    // This is a rough guess and only for calculating the initial node
    // capacity.
    ham_u16_t get_actual_key_size(ham_u32_t page_size, ham_u32_t key_size,
                    bool enable_duplicates = false) {
      // unlimited/variable keys require 5 bytes for flags + key size + offset;
      // assume an average key size of 32 bytes (this is a random guess, but
      // will be good enough)
      if (key_size == HAM_KEY_SIZE_UNLIMITED)
        return ((ham_u16_t)32 - 8);// + 5 - 8
      if (key_size > calculate_extended_threshold(page_size))
        key_size = 8;

      // otherwise 1 byte for flags and 1 byte for record counter
      return ((ham_u16_t)(key_size + (enable_duplicates ? 2 : 0)));
    }

    // Clears the caches for extended keys and duplicate keys
    void clear_caches() {
      if (m_extkey_cache) {
        delete m_extkey_cache;
        m_extkey_cache = 0;
      }
      if (m_duptable_cache) {
        delete m_duptable_cache;
        m_duptable_cache = 0;
      }
    }

    // Retrieves the extended key at |blobid| and stores it in |key|; will
    // use the cache.
    void get_extended_key(ham_u64_t blobid, ham_key_t *key) {
      if (!m_extkey_cache)
        m_extkey_cache = new ExtKeyCache();
      else {
        ExtKeyCache::iterator it = m_extkey_cache->find(blobid);
        if (it != m_extkey_cache->end()) {
          key->size = it->second.get_size();
          key->data = it->second.get_ptr();
          return;
        }
      }

      ByteArray arena;
      ham_record_t record = {0};
      LocalDatabase *db = m_page->get_db();
      db->get_local_env()->get_blob_manager()->read(db, blobid, &record,
                      0, &arena);
      (*m_extkey_cache)[blobid] = arena;
      arena.disown();
      key->data = record.data;
      key->size = record.size;
    }

    // Retrieves the extended duplicate table at |tableid| and stores it the
    // cache; returns the ByteArray with the cached data
    ByteArray get_duplicate_table(ham_u64_t tableid) {
      if (!m_duptable_cache)
        m_duptable_cache = new DupTableCache();
      else {
        DupTableCache::iterator it = m_duptable_cache->find(tableid);
        if (it != m_duptable_cache->end()) {
          ByteArray arena = it->second;
          arena.disown();
          return (arena);
        }
      }

      ByteArray arena;
      ham_record_t record = {0};
      LocalDatabase *db = m_page->get_db();
      db->get_local_env()->get_blob_manager()->read(db, tableid, &record,
                      0, &arena);
      (*m_duptable_cache)[tableid] = arena;
      arena.disown();
      return (arena);
    }

    // Adds a new duplicate table to the cache
    ham_u64_t add_duplicate_table(ByteArray *table) {
      LocalDatabase *db = m_page->get_db();

      if (!m_duptable_cache)
        m_duptable_cache = new DupTableCache();

      ham_record_t rec = {0};
      rec.data = table->get_ptr();
      rec.size = table->get_size();
      ham_u64_t tableid = db->get_local_env()->get_blob_manager()->allocate(db,
                                &rec, 0);

      (*m_duptable_cache)[tableid] = *table;
      table->disown();

      // increment counter (for statistics)
      Globals::ms_extended_duptables++;

      return (tableid);
    }

    // Doubles the size of a duplicate table; only works on the cached table;
    // does not update the underlying record id!
    ByteArray grow_duplicate_table(ham_u64_t tableid) {
      DupTableCache::iterator it = m_duptable_cache->find(tableid);
      ham_assert(it != m_duptable_cache->end());

      ByteArray &table = it->second;
      ham_u32_t count = DuplicateTable::get_count(&table);
      table.resize(8 + (count * 2) * get_total_inline_record_size());
      DuplicateTable::set_capacity(&table, count * 2);

      // return a "disowned" copy
      ByteArray copy = table;
      copy.disown();
      return (copy);
    }

    // Writes the modified duplicate table to disk; returns the new
    // table-id
    ham_u64_t flush_duplicate_table(ham_u64_t tableid, ByteArray *table) {
      LocalDatabase *db = m_page->get_db();
      ham_record_t record = {0};
      record.data = table->get_ptr();
      record.size = table->get_size();
      ham_u64_t newid = db->get_local_env()->get_blob_manager()->overwrite(db,
                      tableid, &record, 0);
      if (tableid != newid) {
        DupTableCache::iterator it = m_duptable_cache->find(tableid);
        ham_assert(it != m_duptable_cache->end());
        ByteArray copy = it->second;
        // do not free memory in |*table|
        it->second.disown();
        (*m_duptable_cache).erase(it);
        // now re-insert the table in the cache
        (*m_duptable_cache)[newid] = copy;
        // again make sure that it won't be freed when |copy| goes out of scope
        copy.disown();
      }
      return (newid);
    }

    // Deletes the duplicate table from disk (and from the cache)
    void erase_duplicate_table(ham_u64_t tableid) {
      DupTableCache::iterator it = m_duptable_cache->find(tableid);
      if (it != m_duptable_cache->end())
        m_duptable_cache->erase(it);

      LocalDatabase *db = m_page->get_db();
      db->get_local_env()->get_blob_manager()->erase(db, tableid, 0);
    }

    // Erases an extended key from disk and from the cache
    void erase_extended_key(ham_u64_t blobid) {
      LocalDatabase *db = m_page->get_db();
      db->get_local_env()->get_blob_manager()->erase(db, blobid);
      if (m_extkey_cache) {
        ExtKeyCache::iterator it = m_extkey_cache->find(blobid);
        if (it != m_extkey_cache->end())
          m_extkey_cache->erase(it);
      }
    }

    // Copies an extended key; adds the copy to the extkey-cache
    ham_u64_t copy_extended_key(ham_u64_t oldblobid) {
      ham_key_t oldkey = {0};

      // do NOT use the cache when retrieving the existing blob - this
      // blob belongs to a different page and we do not have access to
      // its layout
      ham_record_t record = {0};
      LocalDatabase *db = m_page->get_db();
      db->get_local_env()->get_blob_manager()->read(db, oldblobid, &record,
                      0, &m_arena);
      oldkey.data = record.data;
      oldkey.size = record.size;

      return (add_extended_key(&oldkey));
    }

    // Allocates an extended key and stores it in the extkey-Cache
    ham_u64_t add_extended_key(const ham_key_t *key) {
      if (!m_extkey_cache)
        m_extkey_cache = new ExtKeyCache();

      ByteArray arena;
      arena.resize(key->size);
      memcpy(arena.get_ptr(), key->data, key->size);

      ham_record_t rec = {0};
      rec.data = key->data;
      rec.size = key->size;

      LocalDatabase *db = m_page->get_db();
      ham_u64_t blobid = db->get_local_env()->get_blob_manager()->allocate(db,
                            &rec, 0);
      ham_assert(blobid != 0);
      ham_assert(m_extkey_cache->find(blobid) == m_extkey_cache->end());

      (*m_extkey_cache)[blobid] = arena;
      arena.disown();

      // increment counter (for statistics)
      Globals::ms_extended_keys++;

      return (blobid);
    }

    // Returns the size of the memory occupied by the key
    ham_u32_t get_key_data_size(ham_u32_t slot) const {
      if (m_keys.get_key_flags(slot) & BtreeKey::kExtendedKey)
        return (sizeof(ham_u64_t));
      return (m_keys.get_key_size(slot));
    }

    // Returns the total size of the key  - key data + record(s)
    ham_u32_t get_total_key_data_size(ham_u32_t slot) const {
      ham_u32_t size;
      if (m_keys.get_key_flags(slot) & BtreeKey::kExtendedKey)
        size = sizeof(ham_u64_t);
      else
        size = m_keys.get_key_size(slot);
      if (m_keys.get_key_flags(slot) & BtreeKey::kExtendedDuplicates)
        return (size + kExtendedDuplicatesSize);
      else
        return (size + get_total_inline_record_size()
                        * m_keys.get_inline_record_count(slot));
    }

    // Returns the size of the memory occupied by the record
    ham_u32_t get_record_data_size(ham_u32_t slot) const {
      if (get_key_flags(slot) & BtreeKey::kExtendedDuplicates)
        return (kExtendedDuplicatesSize);
      else
        return (m_keys.get_inline_record_count(slot)
                        * get_total_inline_record_size());
    }

    // Returns the inline record data
    void *get_inline_record_data(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) {
      return (get_key_data(slot)
                      + get_key_data_size(slot)
                      + get_total_inline_record_size() * (duplicate_index));
    }

    // Returns the inline record data (const flavour)
    void *get_inline_record_data(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) const {
      return (get_key_data(slot)
                      + get_key_data_size(slot)
                      + get_total_inline_record_size() * duplicate_index);
    }

    // Appends a key to the key space; if |use_freelist| is true, it will
    // first search for a sufficiently large freelist entry. Returns the
    // offset of the new key.
    ham_u32_t append_key(ham_u32_t count, const void *key_data,
                    ham_u32_t total_size, bool use_freelist) {
      ham_u32_t offset = allocate(count, total_size, use_freelist);
     
      // copy the key data AND the record data
      ham_u8_t *p = m_node->get_data() + kPayloadOffset + offset
                      + m_keys.get_key_index_span() * m_index.get_capacity();
      memmove(p, key_data, total_size);

      return (offset);
    }

    // Allocates storage for a key. If |use_freelist| is true, it will
    // first search for a sufficiently large freelist entry. Returns the
    // offset.
    ham_u32_t allocate(ham_u32_t count, ham_u32_t total_size,
                    bool use_freelist) {
      // search the freelist for free key space
      int idx = -1;
      ham_u32_t offset;

      if (use_freelist) {
        idx = m_index.find_in_freelist(count, total_size);
        // found: remove this freelist entry
        if (idx != -1) {
          offset = m_keys.get_key_data_offset(idx);
          ham_u32_t size = get_total_key_data_size(idx);
          m_index.remove_from_freelist(idx);
          // adjust the next key offset, if required
          if (m_index.get_next_offset() == offset + size)
            m_index.set_next_offset(offset + total_size);
        }
      }

      if (idx == -1) {
        if (count == 0) {
          offset = 0;
          m_index.set_next_offset(total_size);
        }
        else {
          offset = m_index.get_next_offset();
          m_index.set_next_offset(offset + total_size);
        }
      }

      return (offset);
    }

    // Create a map with all occupied ranges in freelist and indices;
    // then make sure that there are no overlaps
    void check_index_integrity(ham_u32_t count) const {
      typedef std::pair<ham_u32_t, ham_u32_t> Range;
      typedef std::vector<Range> RangeVec;
      ham_u32_t total = count + m_index.get_freelist_count();
      RangeVec ranges;
      ranges.reserve(total);
      ham_u32_t next_offset = 0;
      for (ham_u32_t i = 0; i < total; i++) {
        ham_u32_t next = m_keys.get_key_data_offset(i)
                    + get_total_key_data_size(i);
        if (next >= next_offset)
          next_offset = next;
        ranges.push_back(std::make_pair(m_keys.get_key_data_offset(i),
                             get_total_key_data_size(i)));
      }
      std::sort(ranges.begin(), ranges.end());
      if (!ranges.empty()) {
        for (ham_u32_t i = 0; i < ranges.size() - 1; i++) {
          if (ranges[i].first + ranges[i].second > ranges[i + 1].first) {
            ham_trace(("integrity violated: slot %u/%u overlaps with %lu",
                        ranges[i].first, ranges[i].second,
                        ranges[i + 1].first));
            throw Exception(HAM_INTEGRITY_VIOLATED);
          }
        }
      }
      if (next_offset != m_index.get_next_offset()) {
        ham_trace(("integrity violated: next offset %d, cached offset %d",
                    next_offset, m_index.get_next_offset()));
        throw Exception(HAM_INTEGRITY_VIOLATED);
      }
      if (next_offset != m_index.calc_next_offset(count)) {
        ham_trace(("integrity violated: next offset %d, cached offset %d",
                    next_offset, m_index.get_next_offset()));
        throw Exception(HAM_INTEGRITY_VIOLATED);
      }
    }

    // Re-arranges the node: moves all keys sequentially to the beginning
    // of the key space, removes the whole freelist
    bool rearrange(ham_u32_t count, bool force = false) {
      // only continue if it's very likely that we can make free space;
      // otherwise this call would be too expensive
      if (!force && m_index.get_freelist_count() == 0 && count > 10)
        return false;

      // get rid of the freelist - this node is now completely rewritten,
      // and the freelist would just complicate things
      m_index.set_freelist_count(0);

      // make a copy of all indices (excluding the freelist)
      SortHelper *s = (SortHelper *)m_arena.resize(count * sizeof(SortHelper));
      for (ham_u32_t i = 0; i < count; i++) {
        s[i].slot = i;
        s[i].offset = m_keys.get_key_data_offset(i);
      }

      // sort them by offset
      std::sort(&s[0], &s[count], sort_by_offset);

      // shift all keys to the left, get rid of all gaps at the front of the
      // key data or between the keys
      ham_u32_t next_offset = 0;
      ham_u32_t start = kPayloadOffset
                       + m_keys.get_key_index_span() * m_index.get_capacity();
      for (ham_u32_t i = 0; i < count; i++) {
        ham_u32_t offset = s[i].offset;
        ham_u32_t slot = s[i].slot;
        ham_u32_t size = get_total_key_data_size(slot);
        if (offset != next_offset) {
          // shift key to the left
          memmove(m_node->get_data() + start + next_offset,
                          get_key_data(slot), size);
          // store the new offset
          m_keys.set_key_data_offset(slot, next_offset);
        }
        next_offset += size;
      }

      m_index.set_next_offset(next_offset);

#ifdef HAM_DEBUG
      check_index_integrity(count);
#endif
      return (true);
    }

    // Tries to resize the node's capacity to fit |new_count| keys and at
    // least |size| additional bytes
    //
    // Returns true if the resize operation was not successful and a split
    // is required
    bool resize(ham_u32_t new_count, ham_u32_t size) {
      ham_u32_t page_size = get_usable_page_size();

      // increase capacity of the indices by shifting keys "to the right"
      if (new_count + m_index.get_freelist_count() >= m_index.get_capacity() - 1) {
        // the absolute offset of the new key (including length and record)
        ham_u32_t capacity = m_index.get_capacity();
        ham_u32_t offset = m_index.get_next_offset();
        offset += (size > get_extended_threshold()
                        ? sizeof(ham_u64_t)
                        : size)
                + get_total_inline_record_size();
        offset += m_keys.get_key_index_span() * (capacity + 1);

        if (offset >= page_size)
          return (true);

        ham_u8_t *src = m_node->get_data() + kPayloadOffset
                        + capacity * m_keys.get_key_index_span();
        capacity++;
        ham_u8_t *dst = m_node->get_data() + kPayloadOffset
                        + capacity * m_keys.get_key_index_span();
        memmove(dst, src, m_index.get_next_offset());

        // store the new capacity
        m_index.set_capacity(capacity);

        // check if the new space is sufficient
        return (!has_enough_space(size, true));
      }

      // increase key data capacity by reducing capacity and shifting
      // keys "to the left"
      else {
        // number of slots that we would have to shift left to get enough
        // room for the new key
        ham_u32_t gap = (size + get_total_inline_record_size())
                            / m_keys.get_key_index_span();
        gap++;

        // if the space is not available then return, and the caller can
        // perform a split
        if (gap + new_count + m_index.get_freelist_count() >= m_index.get_capacity() - 1)
          return (true);

        ham_u32_t capacity = m_index.get_capacity();

        // if possible then shift a bit more, hopefully this can avoid another
        // shift when the next key is inserted
        gap = std::min((size_t)gap,
                    (capacity - new_count - m_index.get_freelist_count()) / 2);

        // now shift the keys and adjust the capacity
        ham_u8_t *src = m_node->get_data() + kPayloadOffset
                + capacity * m_keys.get_key_index_span();
        capacity -= gap;
        ham_u8_t *dst = m_node->get_data() + kPayloadOffset
                + capacity * m_keys.get_key_index_span();
        memmove(dst, src, m_index.get_next_offset());

        // store the new capacity
        m_index.set_capacity(capacity);

        return (!has_enough_space(size, true));
      }
    }

    // Returns true if a key of |size| bytes can be inserted without
    // splitting the page
    bool has_enough_space(ham_u32_t size, bool use_extended_keys) {
      ham_u32_t count = m_node->get_count();

      if (count == 0) {
        m_index.set_freelist_count(0);
        m_index.set_next_offset(0);
        return (true);
      }

      // leave some headroom - a few operations create new indices; make sure
      // that they have index capacity left
      if (count + m_index.get_freelist_count() >= m_index.get_capacity() - 2)
        return (false);

      ham_u32_t offset = m_index.get_next_offset();
      if (use_extended_keys)
        offset += size > get_extended_threshold()
                    ? sizeof(ham_u64_t)
                    : size;
      else
        offset += size;

      // need at least 8 byte for the record, in case we need to store a
      // reference to a duplicate table
      if (get_total_inline_record_size() < sizeof(ham_u64_t))
        offset += sizeof(ham_u64_t);
      else
        offset += get_total_inline_record_size();
      offset += m_keys.get_key_index_span() * m_index.get_capacity();
      if (offset < get_usable_page_size())
        return (true);

      // if there's a freelist entry which can store the new key then
      // a split won't be required
      return (-1 != m_index.find_in_freelist(count,
                        size + get_total_inline_record_size()));
    }

    // Returns true if the record is inline
    bool is_record_inline(ham_u32_t slot, ham_u32_t duplicate_index = 0) const {
      return (m_records.is_record_inline(slot, duplicate_index));
    }

    // Sets the inline record data
    void set_inline_record_data(ham_u32_t slot, const void *data,
                    ham_u32_t size, ham_u32_t duplicate_index) {
      m_records.set_inline_record_data(slot, data, size, duplicate_index);
    }

    // Returns the size of the record, if inline
    ham_u32_t get_inline_record_size(ham_u32_t slot,
                    ham_u32_t duplicate_index = 0) const {
      return (m_records.get_inline_record_size(slot, duplicate_index));
    }

    // Returns the maximum size of inline records (payload only!)
    ham_u32_t get_max_inline_record_size() const {
      return (m_records.get_max_inline_record_size());
    }

    // Returns the maximum size of inline records (incl. overhead!)
    ham_u32_t get_total_inline_record_size() const {
      return (m_records.get_total_inline_record_size());
    }

    // Sets the record counter
    void set_inline_record_count(ham_u32_t slot, ham_u8_t count) {
      m_keys.set_inline_record_count(slot, count);
    }

    // Returns the threshold for extended keys
    ham_u32_t get_extended_threshold() const {
      if (Globals::ms_extended_threshold)
        return (Globals::ms_extended_threshold);
      ham_u32_t page_size = m_page->get_db()->get_local_env()->get_page_size();
      return (Globals::ms_extended_threshold
                      = calculate_extended_threshold(page_size));
    }

    // Calculates the extended threshold based on the page size
    static ham_u32_t calculate_extended_threshold(ham_u32_t page_size) {
      if (page_size == 1024)
        return (64);
      if (page_size <= 1024 * 8)
        return (128);
      return (256);
    }

    // Returns the threshold for duplicate tables
    ham_u32_t get_duplicate_threshold() const {
      if (Globals::ms_duplicate_threshold)
        return (Globals::ms_duplicate_threshold);
      ham_u32_t page_size = m_page->get_db()->get_local_env()->get_page_size();
      if (page_size == 1024)
        return (Globals::ms_duplicate_threshold = 32);
      if (page_size <= 1024 * 8)
        return (Globals::ms_duplicate_threshold = 64);
      if (page_size <= 1024 * 16)
        return (Globals::ms_duplicate_threshold = 128);
      // 0xff/255 is the maximum that we can store in the record
      // counter (1 byte!)
      return (Globals::ms_duplicate_threshold = 255);
    }

    // Returns the usable page size that can be used for actually
    // storing the data
    ham_u32_t get_usable_page_size() const {
      return (m_page->get_db()->get_local_env()->get_usable_page_size()
                    - kPayloadOffset
                    - PBtreeNode::get_entry_offset());
    }

    // Returns the record address of an extended key overflow area
    ham_u64_t get_extended_blob_id(ham_u32_t slot) const {
      ham_u64_t rid = *(ham_u64_t *)get_key_data(slot);
      return (ham_db2h_offset(rid));
    }

    // Sets the record address of an extended key overflow area
    void set_extended_blob_id(ham_u32_t slot, ham_u64_t blobid) {
      *(ham_u64_t *)get_key_data(slot) = ham_h2db_offset(blobid);
    }

    // The page that we're operating on
    Page *m_page;

    // The node that we're operating on
    PBtreeNode *m_node;

    // Provides access to the upfront index
    UpfrontIndex<NodeType> m_index;

    // The KeyList provides access to the stored keys
    KeyList m_keys;

    // The RecordList provides access to the stored records
    RecordList m_records;

    // A memory arena for various tasks
    ByteArray m_arena;

    // Cache for extended keys
    ExtKeyCache *m_extkey_cache;

    // Cache for external duplicate tables
    DupTableCache *m_duptable_cache;

    // Allow the capacity to be recalculated later on
    bool m_recalc_capacity;
};
#endif

} // namespace hamsterdb

#endif /* HAM_BTREE_IMPL_DEFAULT_H__ */

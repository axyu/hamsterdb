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

#include "0root/root.h"

#include <string.h>

// Always verify that a file of level N does not include headers > N!
#include "1base/error.h"
#include "2page/page.h"
#include "3page_manager/page_manager.h"
#include "3btree/btree_index.h"
#include "3btree/btree_index_factory.h"
#include "3btree/btree_node_proxy.h"
#include "4db/db.h"
#include "4env/env.h"
#include "4cursor/cursor.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

uint64_t BtreeIndex::ms_btree_smo_split = 0;
uint64_t BtreeIndex::ms_btree_smo_merge = 0;
uint64_t BtreeIndex::ms_btree_smo_shift = 0;

BtreeIndex::BtreeIndex(LocalDatabase *db, uint32_t descriptor, uint32_t flags,
                uint32_t key_type, uint32_t key_size)
  : m_db(db), m_key_size(0), m_key_type(key_type), m_rec_size(0),
    m_descriptor_index(descriptor), m_flags(flags), m_root_address(0)
{
  m_leaf_traits = BtreeIndexFactory::create(db, flags, key_type,
                  key_size, true);
  m_internal_traits = BtreeIndexFactory::create(db, flags, key_type,
                  key_size, false);
}

void
BtreeIndex::create(uint16_t key_type, uint32_t key_size, uint32_t rec_size)
{
  ham_assert(key_size != 0);

  /* allocate a new root page */
  Page *root = m_db->get_local_env()->get_page_manager()->alloc_page(m_db,
                    Page::kTypeBroot, PageManager::kClearWithZero);

  // initialize the new page
  PBtreeNode *node = PBtreeNode::from_page(root);
  node->set_flags(PBtreeNode::kLeafNode);

  m_key_size = key_size;
  m_key_type = key_type;
  m_rec_size = rec_size;
  m_root_address = root->get_address();

  flush_descriptor();
}

void
BtreeIndex::open()
{
  uint64_t rootadd;
  uint16_t key_size;
  uint16_t key_type;
  uint32_t flags;
  uint32_t rec_size;
  PBtreeHeader *desc = m_db->get_local_env()->get_btree_descriptor(m_descriptor_index);

  key_size = desc->get_key_size();
  key_type = desc->get_key_type();
  rec_size = desc->get_record_size();
  rootadd = desc->get_root_address();
  flags = desc->get_flags();

  ham_assert(key_size > 0);
  ham_assert(rootadd > 0);

  m_root_address = rootadd;
  m_key_size = key_size;
  m_key_type = key_type;
  m_flags = flags;
  m_rec_size = rec_size;
}

void
BtreeIndex::flush_descriptor()
{
  if (m_db->get_rt_flags() & HAM_READ_ONLY)
    return;

  LocalEnvironment *env = m_db->get_local_env();

  PBtreeHeader *desc = env->get_btree_descriptor(m_descriptor_index);

  desc->set_dbname(m_db->get_name());
  desc->set_key_size(get_key_size());
  desc->set_rec_size(get_record_size());
  desc->set_key_type(get_key_type());
  desc->set_root_address(get_root_address());
  desc->set_flags(get_flags());

  env->mark_header_page_dirty();
}

Page *
BtreeIndex::find_child(Page *page, const ham_key_t *key,
                uint32_t page_manager_flags, int *idxptr)
{
  BtreeNodeProxy *node = get_node_from_page(page);

  // make sure that we're not in a leaf page, and that the
  // page is not empty
  ham_assert(node->get_ptr_down() != 0);

  uint64_t record_id;
  int slot = node->find_child((ham_key_t *)key, &record_id);

  if (idxptr)
    *idxptr = slot;

  return (m_db->get_local_env()->get_page_manager()->fetch_page(m_db,
                    record_id, page_manager_flags));
}

int
BtreeIndex::find_leaf(Page *page, ham_key_t *key, uint32_t flags,
                uint32_t *approx_match)
{
  *approx_match = 0;

  /* ensure the approx flag is NOT set by anyone yet */
  BtreeNodeProxy *node = get_node_from_page(page);
  if (node->get_count() == 0)
    return (-1);

  int cmp;
  int slot = node->find_child(key, 0, &cmp);

  /* successfull match */
  if (cmp == 0 && (flags == 0 || flags & HAM_FIND_EXACT_MATCH))
    return (slot);

  /* approx. matching: smaller key is required */
  if (flags & HAM_FIND_LT_MATCH) {
    if (cmp == 0 && (flags & HAM_FIND_GT_MATCH)) {
      *approx_match = BtreeKey::kLower;
      return (slot + 1);
    }

    if (slot < 0 && (flags & HAM_FIND_GT_MATCH)) {
      *approx_match = BtreeKey::kGreater;
      return (0);
    }
    *approx_match = BtreeKey::kLower;
    if (cmp <= 0)
      return (slot - 1);
    return (slot);
  }

  /* approx. matching: greater key is required */
  if (flags & HAM_FIND_GT_MATCH) {
    *approx_match = BtreeKey::kGreater;
    return (slot + 1);
  }

  return (cmp ? -1 : slot);
}

//
// visitor object for estimating / counting the number of keys
///
class CalcKeysVisitor : public BtreeVisitor {
  public:
    CalcKeysVisitor(LocalDatabase *db, bool distinct)
      : m_db(db), m_distinct(distinct), m_count(0) {
    }

    virtual bool is_read_only() const {
      return (true);
    }

    virtual void operator()(BtreeNodeProxy *node) {
      size_t node_count = node->get_count();

      if (m_distinct
          || (m_db->get_rt_flags() & HAM_ENABLE_DUPLICATE_KEYS) == 0) {
        m_count += node_count;
        return;
      }

      for (size_t i = 0; i < node_count; i++)
        m_count += node->get_record_count(i);
    }

    uint64_t get_result() const {
      return (m_count);
    }

  private:
    LocalDatabase *m_db;
    bool m_distinct;
    uint64_t m_count;
};

uint64_t
BtreeIndex::count(bool distinct)
{
  CalcKeysVisitor visitor(m_db, distinct);
  visit_nodes(visitor, false);
  return (visitor.get_result());
}

//
// visitor object to free all allocated blobs
///
class FreeBlobsVisitor : public BtreeVisitor {
  public:
    virtual void operator()(BtreeNodeProxy *node) {
      node->remove_all_entries();
    }

    virtual bool is_read_only() const {
      return (false);
    }
};

void
BtreeIndex::release()
{
  FreeBlobsVisitor visitor;
  visit_nodes(visitor, true);
}

} // namespace hamsterdb

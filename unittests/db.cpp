/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
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

#include "3rdparty/catch/catch.hpp"

#include "utils.h"

#include "ham/hamsterdb.h"

#include "2page/page.h"
#include "3btree/btree_index.h"
#include "3btree/btree_index_factory.h"
#include "3blob_manager/blob_manager.h"
#include "3page_manager/page_manager.h"
#include "3page_manager/page_manager_test.h"
#include "3btree/btree_node.h"
#include "4context/context.h"
#include "4txn/txn.h"
#include "4db/db.h"
#include "4env/env.h"
#include "4env/env_header.h"

namespace hamsterdb {

struct DbFixture {
  ham_db_t *m_db;
  LocalDatabase *m_dbp;
  ham_env_t *m_env;
  bool m_inmemory;
  ScopedPtr<Context> m_context;

  DbFixture(bool inmemory = false)
    : m_db(0), m_dbp(0), m_env(0), m_inmemory(inmemory) {
    REQUIRE(0 ==
        ham_env_create(&m_env, Utils::opath(".test"),
            (m_inmemory ? HAM_IN_MEMORY : 0), 0644, 0));
    REQUIRE(0 ==
        ham_env_create_db(m_env, &m_db, 13,
            HAM_ENABLE_DUPLICATE_KEYS, 0));
    m_dbp = (LocalDatabase *)m_db;
    m_context.reset(new Context((LocalEnvironment *)m_env, 0, m_dbp));
  }

  ~DbFixture() {
    m_context->changeset.clear();
    REQUIRE(0 == ham_env_close(m_env, HAM_AUTO_CLEANUP));
  }

  void headerTest() {
    LocalEnvironment *lenv = (LocalEnvironment *)m_env;
    lenv->header()->set_magic('1', '2', '3', '4');
    REQUIRE(true == lenv->header()->verify_magic('1', '2', '3', '4'));

    lenv->header()->set_version(1, 2, 3, 4);
    REQUIRE((uint8_t)1 == lenv->header()->version(0));
    REQUIRE((uint8_t)2 == lenv->header()->version(1));
    REQUIRE((uint8_t)3 == lenv->header()->version(2));
    REQUIRE((uint8_t)4 == lenv->header()->version(3));
  }

  void defaultCompareTest() {
    ham_key_t key1 = {0}, key2 = {0};
    key1.data = (void *)"abc";
    key1.size = 3;
    key2.data = (void *)"abc";
    key2.size = 3;
    REQUIRE( 0 == m_dbp->btree_index()->compare_keys(&key1, &key2));
    key1.data = (void *)"ab";
    key1.size = 2;
    key2.data = (void *)"abc";
    key2.size = 3;
    REQUIRE(-1 == m_dbp->btree_index()->compare_keys(&key1, &key2));
    key1.data = (void *)"abc";
    key1.size = 3;
    key2.data = (void *)"bcd";
    key2.size = 3;
    REQUIRE(-1 == m_dbp->btree_index()->compare_keys(&key1, &key2));
    key1.data = (void *)"abc";
    key1.size = 3;
    key2.data = (void *)0;
    key2.size = 0;
    REQUIRE(+1 == m_dbp->btree_index()->compare_keys(&key1, &key2));
    key1.data = (void *)0;
    key1.size = 0;
    key2.data = (void *)"abc";
    key2.size = 3;
    REQUIRE(-1 == m_dbp->btree_index()->compare_keys(&key1, &key2));
  }

  void flushPageTest() {
    Page *page;
    uint64_t address;
    uint8_t *p;

    PageManager *pm = ((LocalEnvironment *)m_env)->page_manager();
    PageManagerTest test = pm->test();

    REQUIRE((page = pm->alloc(m_context.get(), 0)));
    m_context->changeset.clear(); // unlock pages

    REQUIRE(m_dbp == page->get_db());
    p = page->get_payload();
    for (int i = 0; i < 16; i++)
      p[i] = (uint8_t)i;
    page->set_dirty(true);
    address = page->get_address();
    page->flush();
    test.remove_page(page);
    delete page;

    REQUIRE((page = pm->fetch(m_context.get(), address)));
    m_context->changeset.clear(); // unlock pages
    REQUIRE(page != 0);
    REQUIRE(address == page->get_address());
    test.remove_page(page);
    delete page;
  }

  void checkStructurePackingTest() {
    // checks to make sure structure packing by the compiler is still okay
    // HAM_PACK_0 HAM_PACK_1 HAM_PACK_2 OFFSETOF
    REQUIRE(sizeof(PBlobHeader) == 28);
    REQUIRE(sizeof(PBtreeNode) == 33);
    REQUIRE(sizeof(PEnvironmentHeader) == 32);
    REQUIRE(sizeof(PBtreeHeader) == 24);
    REQUIRE(sizeof(PPageData) == 17);
    PPageData p;
    REQUIRE(sizeof(p.header) == 17);
    REQUIRE(Page::kSizeofPersistentHeader == 16);

    REQUIRE(PBtreeNode::get_entry_offset() == 32);
    Page page(0);
    DatabaseConfiguration config;
    config.db_name = 1;
    LocalDatabase db((LocalEnvironment *)m_env, config);

    page.set_address(1000);
    page.set_db(&db);
    db.m_btree_index.reset(new BtreeIndex(&db, 0, 0, 0,
                HAM_KEY_SIZE_UNLIMITED));
    db.m_btree_index->m_key_size = 666;
    REQUIRE(Page::kSizeofPersistentHeader == 16);
    // make sure the 'header page' is at least as large as your usual
    // header page, then hack it...
    struct {
      PPageData drit;
      PEnvironmentHeader drat;
    } hdrpage_pers = {{{0}}};
    Page hdrpage(0);
    hdrpage.set_data((PPageData *)&hdrpage_pers);
    Page *hp = &hdrpage;
    uint8_t *pl1 = hp->get_payload();
    REQUIRE(pl1);
    REQUIRE((pl1 - (uint8_t *)hdrpage.get_data()) == 16);
    PEnvironmentHeader *hdrptr = (PEnvironmentHeader *)(hdrpage.get_payload());
    REQUIRE(((uint8_t *)hdrptr - (uint8_t *)hdrpage.get_data()) == 16);
    hdrpage.set_data(0);
  }

};

TEST_CASE("Db/checkStructurePackingTest", "")
{
  DbFixture f;
  f.checkStructurePackingTest();
}

TEST_CASE("Db/headerTest", "")
{
  DbFixture f;
  f.headerTest();
}

TEST_CASE("Db/defaultCompareTest", "")
{
  DbFixture f;
  f.defaultCompareTest();
}

TEST_CASE("Db/flushPageTest", "")
{
  DbFixture f;
  f.flushPageTest();
}


TEST_CASE("Db-inmem/checkStructurePackingTest", "")
{
  DbFixture f(true);
  f.checkStructurePackingTest();
}

TEST_CASE("Db-inmem/headerTest", "")
{
  DbFixture f(true);
  f.headerTest();
}

TEST_CASE("Db-inmem/defaultCompareTest", "")
{
  DbFixture f(true);
  f.defaultCompareTest();
}

} // namespace hamsterdb

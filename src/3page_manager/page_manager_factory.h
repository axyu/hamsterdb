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

/*
 * Factory for creating PageManager objects
 *
 * @exception_safe: unknown
 * @thread_safe: unknown
 */

#ifndef HAM_PAGE_MANAGER_FACTORY_H
#define HAM_PAGE_MANAGER_FACTORY_H

#include "0root/root.h"

#include <map>

// Always verify that a file of level N does not include headers > N!
#include "3page_manager/page_manager.h"

#ifndef HAM_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

class LocalEnvironment;

struct PageManagerFactory
{
  static PageManager *create(LocalEnvironment *env, size_t cache_size) {
    return (new PageManager(PageManagerState(env, cache_size)));
  }
};

} // namespace hamsterdb

#endif /* HAM_PAGE_MANAGER_FACTORY_H */
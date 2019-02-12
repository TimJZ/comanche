/*
   Copyright [2019] [IBM Corporation]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

/*
 * Authors:
 *
 * Daniel G. Waddington (daniel.waddington@ibm.com)
 *
 */

#ifndef __NUPM_PMEM_LOW_LEVEL_H__
#define __NUPM_PMEM_LOW_LEVEL_H__

#include "x86_64/flush.h"
//#include "x86_64/fast_memcpy_avx.h"

namespace nupm
{
inline static void mem_flush(const void *addr, size_t len)
{
  flush_clflushopt_nolog(addr, len);
}
}  // namespace nupm

#endif
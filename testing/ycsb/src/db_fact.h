#ifndef __YCSB_DB_FACT_H__
#define __YCSB_DB_FACT_H__

#include "aerospikedb.h"
#include "dawndb.h"
#include "db.h"
#include "memcached.h"
#include "properties.h"

namespace ycsb
{
class DBFactory {
 public:
  static DB* create(Properties& props, unsigned core = 0)
  {
    if (props.getProperty("db") == "dawn") {
      return new DawnDB(props, core);
    }
    else if (props.getProperty("db") == "memcached") {
      return new Memcached(props, core);
    }
    else if (props.getProperty("db") == "aerospike") {
      return new AerospikeDB(props, core);
    }
    else
      return nullptr;
  }
};

}  // namespace ycsb

#endif

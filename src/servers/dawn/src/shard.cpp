//#define PROFILE

#include <api/components.h>
#include <common/dump_utils.h>
#ifdef PROFILE
#include <gperftools/profiler.h>
#endif


#include "shard.h"

using namespace Dawn;

namespace Dawn {

/* global parameters */
namespace Global {
unsigned debug_level = 0;
}

void Shard::initialize_components(const Program_options& po)
{
  using namespace Component;
  auto& backend = po.backend;
  
  /* STORE */
  {
    IBase * comp;
    
    if(backend == "pmstore")
      comp = load_component("libcomanche-pmstore.so", pmstore_factory);      
    else if(backend == "mapstore")
      comp = load_component("libcomanche-storemap.so", mapstore_factory);
    else if(backend == "hstore")
      comp = load_component("libcomanche-hstore.so", hstore_factory);
    else if(backend == "nvmestore")
      comp = load_component("libcomanche-nvmestore.so", nvmestore_factory);
    else if(backend == "filestore")
      comp = load_component("libcomanche-storefile.so", filestore_factory);
    else throw General_exception("invalid backend (%s)", backend.c_str());

    if(option_DEBUG)
      PLOG("Shard: using store backend (%s)", backend.c_str());

    if(!comp)
      throw General_exception("unable to initialize PMSTORE comanche component");

    IKVStore_factory * fact = (IKVStore_factory *) comp->query_interface(IKVStore_factory::iid());
    assert(fact);

    if(backend == "nvmestore") {
      if(po.pci_addr.empty())
        throw General_exception("nvmestore backend needs --pci-addr option");
      
      _i_kvstore = fact->create("owner","name",po.pci_addr);
    }
    else {
      _i_kvstore = fact->create("owner","name");
    }
    fact->release_ref();
  }
}

void Shard::main_loop()
{
  using namespace Dawn::Protocol;

  assert(_i_kvstore);

#ifdef PROFILE
  ProfilerStart("shard_loop");
#endif
  
  uint64_t tick = 0;
  static constexpr uint64_t CHECK_CONNECTION_INTERVAL = 10000;

  Connection_handler::action_t action;
  std::vector<std::vector<Connection_handler*>::iterator> pending_close;
    
  while(_thread_exit == false) {
 
    /* check for new connections - but not too often */
    if(tick % CHECK_CONNECTION_INTERVAL == 0)
      check_for_new_connections();

    /* iterate connection handlers (each connection is a client session) */
    for(std::vector<Connection_handler*>::iterator handler_iter=_handlers.begin();
        handler_iter != _handlers.end(); handler_iter++) {

      const auto handler = *handler_iter;

      /* issue tick */
      auto tick_response = handler->tick();
      
      if(tick_response == Dawn::Connection_handler::TICK_RESPONSE_CLOSE) {

        if(_forced_exit)
          _thread_exit = true;
        
        if(option_DEBUG)
          PLOG("Shard: closing connection %p", handler);
        
        for(auto& pool : handler->open_pool_set())
          _i_kvstore->close_pool(pool);

        pending_close.push_back(handler_iter);
      }

      /* process ALL deferred actions */
      while(handler->get_pending_action(action)) {
        switch (action.op) {
        case Connection_handler::ACTION_RELEASE_VALUE_LOCK:
          if(option_DEBUG)
            PLOG("releasing value lock (%p)", action.parm);
          release_locked_value(action.parm);
          break;
        default:
          throw Logic_exception("unknown action type");
        }
      }

      /* collect ALL available messages */
      buffer_t *iob;
      Protocol::Message* p_msg = nullptr;
      while((iob = handler->get_pending_msg(p_msg))!=nullptr) {
        assert(p_msg);
        switch(p_msg->type_id) {
        case MSG_TYPE_POOL_REQUEST:
          process_message_pool_request(handler,
                                       static_cast<Protocol::Message_pool_request*>(p_msg));
          break;
        case MSG_TYPE_IO_REQUEST:
          process_message_IO_request(handler,
                                     static_cast<Protocol::Message_IO_request*>(p_msg));
          break;
        default:
          throw General_exception("unrecognizable message type");
        }
        handler->free_recv_buffer();
      }
    }

    /* handle pending close sessions */
    if(unlikely(!pending_close.empty())) {
      for(auto& h : pending_close) {
        delete *h;
        _handlers.erase(h);
      }
      
      pending_close.clear();
    }

    tick++;
  }

#ifdef PROFILE
  ProfilerStop();
  ProfilerFlush();
#endif
  
}

void Shard::process_message_pool_request(Connection_handler* handler,
                                         Protocol::Message_pool_request* msg)
{
  // validate auth id
  assert(msg->op);

  /* allocate response buffer */
  auto response_iob = handler->allocate();
  assert(response_iob);
  assert(response_iob->base());
  memset(response_iob->iov->iov_base,0,response_iob->iov->iov_len);

  Protocol::Message_pool_response * response =
    new (response_iob->base()) Protocol::Message_pool_response(handler->auth_id());
  assert(response);

  /* handle operation */
  if(msg->op == Dawn::Protocol::OP_CREATE) {

    if(option_DEBUG)
      PINF("# Message_pool_request: op=%u path=%s pool_name=%s",
           msg->op, msg->path(), msg->pool_name());

    try {
      auto pool = _i_kvstore->create_pool(msg->path(), msg->pool_name(), msg->pool_size);

      if(option_DEBUG)
        PLOG("OP_CREATE: new pool id: %lx", pool);
      
      handler->add_as_open_pool(pool);
      response->pool_id = pool;
      response->status = S_OK;
    }
    catch(...) {
      PERR("OP_CREATE: error");
      response->pool_id = 0;
      response->status = E_FAIL; // TODO: improve error code flow through
    }
  }
  else if(msg->op == Dawn::Protocol::OP_OPEN) {

    if(option_DEBUG)
      PINF("# Message_pool_request: op=%u path=%s pool_name=%s",
           msg->op, msg->path(), msg->pool_name());

    try {
      auto pool = _i_kvstore->open_pool(_data_dir, msg->pool_name());

      if(option_DEBUG)
        PLOG("OP_OPEN: pool id: %lx", pool);
      
      handler->add_as_open_pool(pool);
      response->pool_id = pool;
    }
    catch(...) {
      PERR("OP_OPEN: error");
      response->pool_id = 0;
      response->status = E_FAIL;
    }
  }
  else if(msg->op == Dawn::Protocol::OP_CLOSE) {

    if(option_DEBUG)
      PINF("# Message_pool_request: op=%u", msg->op);
        
    try {
      auto pool = msg->pool_id;
      if(option_DEBUG)
        PLOG("OP_CLOSE: pool id: %lx", pool);
      _i_kvstore->close_pool(pool);
      handler->remove_as_open_pool(pool);
      response->pool_id = pool;
    }
    catch(...) {
      PERR("OP_CLOSE: error");
      response->pool_id = 0;
      response->status = E_FAIL;
    }
  }
  else if(msg->op == Dawn::Protocol::OP_DELETE) {

    if(option_DEBUG)
      PINF("# Message_pool_request: op=%u", msg->op);
        
    try {
      auto pool = msg->pool_id;
      if(option_DEBUG)
        PLOG("OP_DELETE: pool id: %lx", pool);
      _i_kvstore->delete_pool(pool);
      response->pool_id = pool;
      handler->remove_as_open_pool(pool);
    }
    catch(...) {
      PERR("OP_DELETE: error");
      response->pool_id = 0;
      response->status = E_FAIL;
    }
  }
  else throw Protocol_exception("process_message_pool_request - bad operation (msg->op = %d)", msg->op);

  if(option_DEBUG)
     PLOG("response len: %d", response->msg_len);

  /* trim response length */
  response_iob->set_length(response->msg_len);
  
  /* finally, send response */
  handler->post_response(response_iob);

}


void Shard::process_message_IO_request(Connection_handler* handler,
                                       Protocol::Message_IO_request* msg)
{
  using namespace Component;
  
  if(!handler->validate_pool(msg->pool_id))
    throw Protocol_exception("invalid pool identifier");

  /* State that does not post response (yet) */
  if(msg->op == Protocol::OP_PUT_ADVANCE) {

    if(option_DEBUG)
      PLOG("PUT_ADVANCE: (%p) key=(%.*s) value_len=%lu request_id=%lu",
           this, (int) msg->key_len, msg->key(), msg->val_len, msg->request_id);

    const auto val_len = msg->val_len;

    /* open memory */
    void * target = nullptr;
    size_t target_len = msg->val_len;
    assert(target_len > 0);

    /* create (if needed) and lock value */
    auto key_handle = _i_kvstore->lock(msg->pool_id,
                                       msg->key(),
                                       IKVStore::STORE_LOCK_WRITE,
                                       target,
                                       target_len);

    if(key_handle == Component::IKVStore::KEY_NONE)
      throw Program_exception("PUT_ADVANCE failed to lock value (lock() returned KEY_NONE) ");

    if(target_len != msg->val_len)
      throw Logic_exception("locked value length mismatch");

    auto pool_id = msg->pool_id;
    add_locked_value(pool_id, key_handle, target);
    
    /* register memory */
    auto region = ondemand_register(handler, target, target_len);
      
    handler->set_pending_value(target, target_len, region);

    return;
  }

  /* States that require a response */
  const auto iob = handler->allocate();

  Protocol::Message_IO_response * response =
    new (iob->base()) Protocol::Message_IO_response(iob->length(), handler->auth_id());

  int status;
  if(msg->op == Protocol::OP_PUT) {

    /* for basic 'puts' we have to do a memcpy - to support "in-place"
       puts for larger data, we use a two-stage operation 
    */
    if(option_DEBUG)
      PLOG("PUT: (%p) key=(%.*s) value=(%.*s)",
           this, (int) msg->key_len, msg->key(), (int) msg->val_len, msg->value());

    if(msg->resvd & Dawn::Protocol::MSG_RESVD_SCBE) {
      status = S_OK; // short-circuit backend
      if(option_DEBUG)
        PLOG("PUT: short-circuited backend");
    }
    else {
      std::string k;
      k.assign(msg->key(), msg->key_len);
      status = _i_kvstore->put(msg->pool_id,
                               k,
                               msg->value(),
                               msg->val_len);
    }

    if(option_DEBUG) {
      if(status == Component::IKVStore::E_ALREADY_EXISTS)
        PLOG("kvstore->put returned E_ALREADY_EXISTS");
      else
        PLOG("kvstore->put returned %d", status);
    }
  }
  else if(msg->op == Protocol::OP_GET) {

    if(option_DEBUG)
      PMAJOR("GET: (%p) (request=%lu) key=(%.*s) ",
             this, msg->request_id, (int) msg->key_len, msg->key());

    void * value_out = nullptr;
    size_t value_out_len = 0;
    
    std::string k;
    k.assign(msg->key(), msg->key_len);
    
    auto key_handle = _i_kvstore->lock(msg->pool_id,
                                       k,
                                       IKVStore::STORE_LOCK_READ,
                                       value_out,
                                       value_out_len);

    
    if(option_DEBUG)
      PLOG("shard: locked OK: value_out=%p (%.*s) value_out_len=%lu",
           value_out, (int) value_out_len, (char*) value_out, value_out_len);

    iob->set_length(response->base_message_size());
        
    if(key_handle == Component::IKVStore::KEY_NONE) { /* key not found */
      response->status = E_NOT_FOUND;
      handler->post_response(iob, nullptr);
      return;
    }
    
    assert(value_out_len);
    assert(value_out);

    /* register region if needed */
    auto region = ondemand_register(handler, value_out, value_out_len);
    assert(region);

    response->data_len = value_out_len;
    response->request_id = msg->request_id;
    response->status = S_OK;
    
    /* create value iob */
    buffer_t * value_buffer;

    if(response->status == S_OK) {
      value_buffer = new buffer_t(value_out_len);
      value_buffer->iov = new iovec{(void*) value_out, value_out_len};
      value_buffer->region = region;
      value_buffer->desc = handler->get_memory_descriptor(region);

      /* register clean up task for value */
      add_locked_value(msg->pool_id, key_handle, value_out);
    }
    
    if(value_out_len <= (handler->IO_buffer_size() - response->base_message_size())) {
      if(option_DEBUG)
        PLOG("posting response header and value together");
      
      /* post both buffers together in same response packet */
      handler->post_response(iob, value_buffer);
    }
    else {
      /* for large gets we use a two-stage protocol sending
         response message and value separately
       */
      response->set_twostage_bit();

      /* send two separate packets */
      handler->post_response(iob);

      /* the client is allocating the recv buffer only after
         receiving the response advance. this could timeout if
         this side issues before the remote buffer is ready */
      handler->post_send_value_buffer(value_buffer);
    }
    
    return;
  }
  else throw Protocol_exception("operation not implemented");

  response->request_id = msg->request_id;  
  response->status = status;

  iob->set_length(response->msg_len);
  handler->post_response(iob); // issue IO request response
}

void Shard::check_for_new_connections()
{
  /* new connections are transferred from the connection handler
     to the shard thread */
  Connection_handler * handler;

  while((handler = get_new_connection()) != nullptr) {      
    if(option_DEBUG)
      PLOG("Shard: processing new connection (%p)", handler);
    _handlers.push_back(handler);
  }
}


} // namespace Dawn
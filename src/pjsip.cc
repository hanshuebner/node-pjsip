// -*- C++ -*-

// Copyright Hans Huebner and contributors. All rights reserved.
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include <iostream>
#include <map>
#include <vector>
#include <typeinfo>

#include <stdarg.h>

#include <v8.h>
#include <node.h>
#include <node_events.h>                                    // needed?
#include <node/ev.h>                                        // needed?

// prevent name clash between pjsua.h and node.h
#define pjsip_module pjsip_module_
#include <pjsua-lib/pjsua.h>
#undef pjsip_module

#include "mutex.h"                                          // needed?

using namespace std;
using namespace v8;
using namespace node;

// //////////////////////////////////////////////////////////////////
// Throwable error class that can be converted to a JavaScript
// exception
// //////////////////////////////////////////////////////////////////
class JSException
{
public:
  JSException(const string& text) : _message(text) {};
  virtual const string message() const { return _message; }
  virtual Handle<Value> asV8Exception() const { return ThrowException(String::New(message().c_str())); }

protected:
  string _message;
};

// //////////////////////////////////////////////////////////////////
// Throwable convertible error class that carries PJ error
// information
// //////////////////////////////////////////////////////////////////
class PJJSException
  : public JSException
{
public:
  PJJSException(const string& text, pj_status_t pjStatus)
    : JSException(text),
      _pjStatus(pjStatus)
  {}

  virtual const string message() const;

private:
  pj_status_t _pjStatus;
};

const string
PJJSException::message() const
{
  char buf[1024];
  pj_strerror(_pjStatus, buf, sizeof buf);
  return _message + ": " + string(buf);
}

class UnknownEnumerationKey
  : public JSException
{
public:
  UnknownEnumerationKey(const char* name, const char* typenamestring)
    : JSException("Unknown enumeration key \"" + string(name) + "\" in table " + string(typenamestring))
  {}
};

template <class EnumType>
class EnumMap
{
public:
  EnumMap(const char* symbols[])
  {
    for (int i = 0; symbols[i]; i++) {
      _nameToIdMap[symbols[i]] = (EnumType) i;
      _idToNameMap.push_back(symbols[i]);
    }
  }

  const char* idToName(EnumType id) const
  {
    unsigned i = (unsigned) id;
    if (i > _idToNameMap.size()) {
      return "UNKNOWN-ID-OUT-OF-RANGE";
    } else {
      return _idToNameMap[i];
    }
  }

  const EnumType nameToId(const char* name) const
  {
    typename map<const string, EnumType>::const_iterator i = _nameToIdMap.find(name);
    if (i == _nameToIdMap.end()) {
      throw UnknownEnumerationKey(name, typeid(*this).name());
    } else {
      return i->second;
    }
  }

  const EnumType nameToId(Handle<String> name) const
  {
    return nameToId(*String::Utf8Value(name));
  }

  const EnumType nameToId(Handle<Value> name) const
  {
    return nameToId(*String::Utf8Value(name->ToString()));
  }

private:
  map<const string, EnumType> _nameToIdMap;
  vector<const char*> _idToNameMap;
};

// //////////////////////////////////////////////////////////////////////

static inline void
setKey(Handle<Object> object, const char* key, const char* value, int length = -1)
{
  object->Set(String::NewSymbol(key), String::New(value, length));
}

static inline void
setKey(Handle<Object> object, const char* key, int value)
{
  object->Set(String::NewSymbol(key), Integer::New(value));
}

static inline void
setKey(Handle<Object> object, const char* key, unsigned value)
{
  object->Set(String::NewSymbol(key), Integer::New(value));
}

static inline void
setKey(Handle<Object> object, const char* key, bool value)
{
  object->Set(String::NewSymbol(key), Boolean::New(value));
}

static inline void
setKey(Handle<Object> object, const char* key, Handle<Object> value)
{
  object->Set(String::NewSymbol(key), value);
}

static inline void
setKey(Handle<Object> object, const char* key, double value)
{
  object->Set(String::NewSymbol(key), Number::New(value));
}

static inline void
setKey(Handle<Object> object, const char* key, const pj_str_t* value)
{
  object->Set(String::NewSymbol(key), String::New(value->ptr, value->slen));
}

static inline void
setKey(Handle<Object> object, const char* key, const pj_str_t& value)
{
  object->Set(String::NewSymbol(key), String::New(value.ptr, value.slen));
}

#define PJ_TIME_VAL_TO_DOUBLE(pjtv) ((double) pjtv.sec + ((double) pjtv.msec * 0.001d))

// //////////////////////////////////////////////////////////////////////

// The callback functions invoked by PJSIP from a separate thread need
// to access V8 in order to invoke the JavaScript callback functions.
// V8 itself is not thread safe, i.e. only one thread may access it at
// any time.  Thus, Node's thread needs to be suspended while the
// PJSIP callbacks are processed.  This is facilitated by a ev_async
// event to signal Node's thread that a PJSIP callback wants to access
// V8, a condition variable to signal the callback thread that Node's
// thread has suspended, and another condition variable to signal
// Node's that that the callback has finished processing.
  
class NodeMutex
{
public:
  class Lock
  {
  public:
    Lock(NodeMutex& mutex);
    ~Lock();

  private:
    NodeMutex& _mutex;
    Locker* _locker;
    Context::Scope* _scope;
  };

  friend class Lock;

  // Note:  NodeMutex instances must be created from within Node's thread.
  NodeMutex();
  ~NodeMutex();

  void setCallback(Local<Function> callback);
  Local<Value> invokeCallback(const char* eventName, int argc, ...);

private:
  // Methods to suspend and resume Node's thread
  void suspendNodeThread();
  void resumeNodeThread();
  
  static void eventCallback(EV_P_ ev_async* w, int revents);

  void suspend();               // suspend the current (Node) thread, giving way to the other thread

  pthread_t _nodeThreadId;      // thread ID of node's thread

  Persistent<Function> _callback;
  Persistent<Context> _callbackContext;

  ev_async _watcher;            // signalled by the other thread to suspend Node's thread
  condition_variable _proceed;  // signaled by the ev_async callback when the other thread may access V8
  mutex _proceedMutex;          // protects the _proceed condition
  condition_variable _complete; // signaled by the other thread when it has completed processing
  mutex _completeMutex;         // protects the _complete condition
};

// //////////////////////////////////////////////////////////////////////

// Class PJSUA encapsulates the connection between Node and PJ

class PJSUA
{
  static Persistent<Function> _callback;
  static NodeMutex _nodeMutex;

  // //////////////////////////////////////////////////////////////////////
  //
  // Callback functions to be called by PJ

  static Handle<Object>
  getCallInfo(pjsua_call_id call_id)
  {
    pjsua_call_info callInfoBinary;
    pjsua_call_get_info(call_id, &callInfoBinary);

    Local<Object> callInfo = Object::New();
    setKey(callInfo, "id", callInfoBinary.id);
    setKey(callInfo, "role", (callInfoBinary.role == PJSIP_ROLE_UAC) ? "UAC" : "UAS");
    setKey(callInfo, "acc_id", callInfoBinary.acc_id);
    setKey(callInfo, "local_info", callInfoBinary.local_info);
    setKey(callInfo, "local_contact", callInfoBinary.local_contact);
    setKey(callInfo, "remote_info", callInfoBinary.remote_info);
    setKey(callInfo, "remote_contact", callInfoBinary.remote_contact);
    setKey(callInfo, "call_id", callInfoBinary.call_id);
    setKey(callInfo, "state", (int) callInfoBinary.state);
    setKey(callInfo, "state_text", callInfoBinary.state_text);
    setKey(callInfo, "last_status", (int) callInfoBinary.last_status);
    setKey(callInfo, "last_status_text", callInfoBinary.last_status_text);
    setKey(callInfo, "media_status", (int) callInfoBinary.media_status);
    setKey(callInfo, "media_dir", (int) callInfoBinary.media_dir);
    setKey(callInfo, "conf_slot", (int) callInfoBinary.conf_slot);
    setKey(callInfo, "connect_duration", PJ_TIME_VAL_TO_DOUBLE(callInfoBinary.connect_duration));
    setKey(callInfo, "total_duration", PJ_TIME_VAL_TO_DOUBLE(callInfoBinary.total_duration));

    return callInfo;
  }

  static Handle<Object>
  getAccInfo(pjsua_acc_id accId)
  {
    pjsua_acc_info accInfoBinary;
    pjsua_acc_get_info(accId, &accInfoBinary);

    Local<Object> accInfo = Object::New();
    setKey(accInfo, "id", accInfoBinary.id);
    setKey(accInfo, "is_default", (bool) accInfoBinary.is_default);
    setKey(accInfo, "acc_uri", accInfoBinary.acc_uri);
    setKey(accInfo, "has_registration", (bool) accInfoBinary.has_registration);
    setKey(accInfo, "expires", accInfoBinary.expires);
    setKey(accInfo, "status", accInfoBinary.status);
    setKey(accInfo, "reg_last_err", accInfoBinary.reg_last_err);
    setKey(accInfo, "status_text", accInfoBinary.status_text);
    setKey(accInfo, "online_status", (bool) accInfoBinary.online_status);
    setKey(accInfo, "online_status_text", accInfoBinary.online_status_text);

    Local<Object> rpid = Object::New();
    setKey(rpid, "type", accInfoBinary.rpid.type);
    setKey(rpid, "id", accInfoBinary.rpid.id);
    setKey(rpid, "activity", accInfoBinary.rpid.activity);
    setKey(rpid, "note", accInfoBinary.rpid.note);
    setKey(accInfo, "rpid", rpid);

    return accInfo;
  }

  static void
  on_call_state(pjsua_call_id call_id,
                pjsip_event *e)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("call_state", 1, getCallInfo(call_id));
  }

  static void
  on_incoming_call(pjsua_acc_id acc_id,
                   pjsua_call_id call_id,
                   pjsip_rx_data *rdata)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("incoming_call", 3, getAccInfo(acc_id), getCallInfo(call_id), Undefined());                   
  }

  static void
  on_call_tsx_state(pjsua_call_id call_id,
                    pjsip_transaction *tsx,
                    pjsip_event *e)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("tsx_state", 3, getCallInfo(call_id), Undefined(), Undefined());
  }

  static void
  on_call_media_state(pjsua_call_id call_id)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("media_state", 1, getCallInfo(call_id));
  }

  static void
  on_stream_created(pjsua_call_id call_id,
                    pjmedia_session *sess,
                    unsigned stream_idx,
                    pjmedia_port **p_port)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("stream_created", 4, getCallInfo(call_id), Undefined(), Integer::New(stream_idx), Undefined());
  }

  static void
  on_stream_destroyed(pjsua_call_id call_id,
                      pjmedia_session *sess,
                      unsigned stream_idx)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("stream_destroyed", 3, getCallInfo(call_id), Undefined(), Integer::New(stream_idx));
  }

  static void
  on_dtmf_digit(pjsua_call_id call_id,
                int digit)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("dtmf_digit", 2, getCallInfo(call_id), Integer::New(digit));
  }

  static void
  on_call_transfer_request(pjsua_call_id call_id,
                           const pj_str_t *dst,
                           pjsip_status_code *code)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    Local<Value> result = _nodeMutex.invokeCallback("transfer_request", 3, getCallInfo(call_id), Undefined(), Undefined());
    *code = (pjsip_status_code) result->ToInteger()->Value();
  }

  static void
  on_call_transfer_status(pjsua_call_id call_id,
                          int st_code,
                          const pj_str_t *st_text,
                          pj_bool_t final,
                          pj_bool_t *p_cont)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    Local<Value> result = _nodeMutex.invokeCallback("transfer_status", 4, getCallInfo(call_id), Integer::New(st_code),
                                                    String::New(st_text->ptr, st_text->slen), Boolean::New(final));
    *p_cont = result->ToBoolean()->Value();
  }

  static void
  on_call_replace_request(pjsua_call_id call_id,
                          pjsip_rx_data *rdata,
                          int *st_code,
                          pj_str_t *st_text)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    Local<Value> result = _nodeMutex.invokeCallback("call_replace_request", 2, getCallInfo(call_id), Undefined());
    *st_code = result->ToInteger()->Value();
    // FIXME: st_text not supported
  }

  static void
  on_call_replaced(pjsua_call_id old_call_id,
                   pjsua_call_id new_call_id)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("call_replaced", 2, getCallInfo(old_call_id), getCallInfo(new_call_id));
  }

  static void
  on_reg_state(pjsua_acc_id acc_id)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("reg_state", 1, getAccInfo(acc_id));
  }

  static void
  on_reg_state2(pjsua_acc_id acc_id,
                pjsua_reg_info *info)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("reg_state2", 2, getAccInfo(acc_id), Undefined());
  }

  static void
  on_incoming_subscribe(pjsua_acc_id acc_id,
                        pjsua_srv_pres *srv_pres,
                        pjsua_buddy_id buddy_id,
                        const pj_str_t *from,
                        pjsip_rx_data *rdata,
                        pjsip_status_code *code,
                        pj_str_t *reason,
                        pjsua_msg_data *msg_data)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    Local<Value> result = _nodeMutex.invokeCallback("incoming_subscribe", 5, getAccInfo(acc_id), Undefined(), Undefined(),
                                                    String::New(from->ptr, from->slen), Undefined());
    *code = (pjsip_status_code) result->ToInteger()->Value();
    // FIXME: reason, msg_data not supported
  }

  static void
  on_srv_subscribe_state(pjsua_acc_id acc_id,
                         pjsua_srv_pres *srv_pres,
                         const pj_str_t *remote_uri,
                         pjsip_evsub_state state,
                         pjsip_event *event)
  {
    NodeMutex::Lock lock(_nodeMutex);
    HandleScope handleScope;

    _nodeMutex.invokeCallback("srv_subscribe_state", 5, getAccInfo(acc_id), Undefined(),
                              String::New(remote_uri->ptr, remote_uri->slen), Undefined(), Undefined());
  }

  static void
  on_buddy_state(pjsua_buddy_id buddy_id)
  {
    // FIXME: NYI
  }

  static void
  on_buddy_evsub_state(pjsua_buddy_id buddy_id,
                       pjsip_evsub *sub,
                       pjsip_event *event)
  {
    // FIXME: NYI
  }

  static void
  on_pager(pjsua_call_id call_id,
           const pj_str_t *from,
           const pj_str_t *to,
           const pj_str_t *contact,
           const pj_str_t *mime_type,
           const pj_str_t *body)
  {
    // FIXME: NYI
  }

  static void
  on_pager2(pjsua_call_id call_id,
            const pj_str_t *from,
            const pj_str_t *to,
            const pj_str_t *contact,
            const pj_str_t *mime_type,
            const pj_str_t *body,
            pjsip_rx_data *rdata,
            pjsua_acc_id acc_id)
  {
    // FIXME: NYI
  }

  static void
  on_pager_status(pjsua_call_id call_id,
                  const pj_str_t *to,
                  const pj_str_t *body,
                  void *user_data,
                  pjsip_status_code status,
                  const pj_str_t *reason)
  {
    // FIXME: NYI
  }

  static void
  on_pager_status2(pjsua_call_id call_id,
                   const pj_str_t *to,
                   const pj_str_t *body,
                   void *user_data,
                   pjsip_status_code status,
                   const pj_str_t *reason,
                   pjsip_tx_data *tdata,
                   pjsip_rx_data *rdata,
                   pjsua_acc_id acc_id)
  {
    // FIXME: NYI
  }

  static void
  on_typing(pjsua_call_id call_id,
            const pj_str_t *from,
            const pj_str_t *to,
            const pj_str_t *contact,
            pj_bool_t is_typing)
  {
    // FIXME: NYI
  }

  static void
  on_typing2(pjsua_call_id call_id,
             const pj_str_t *from,
             const pj_str_t *to,
             const pj_str_t *contact,
             pj_bool_t is_typing,
             pjsip_rx_data *rdata,
             pjsua_acc_id acc_id)
  {
    // FIXME: NYI
  }

  static void
  on_nat_detect(const pj_stun_nat_detect_result *res)
  {
    // FIXME: NYI
  }

  static void
  on_mwi_info(pjsua_acc_id acc_id,
              pjsua_mwi_info *mwi_info)
  {
    // FIXME: NYI
  }

  static void
  on_transport_state(pjsip_transport *tp,
                     pjsip_transport_state state,
                     const pjsip_transport_state_info *info)
  {
    // FIXME: NYI
  }

  static void
  on_ice_transport_error(int index,
                         pj_ice_strans_op op,
                         pj_status_t status,
                         void *param)
  {
    // FIXME: NYI
  }

public:
  static void Initialize(Handle<Object> target);

private:
  static Handle<Value> start(const Arguments& args);
  static Handle<Value> addAccount(const Arguments& args);
  static Handle<Value> callAnswer(const Arguments& args);
  static Handle<Value> callMakeCall(const Arguments& args);
  static Handle<Value> callHangup(const Arguments& args);
  static Handle<Value> stop(const Arguments& args);
};

// //////////////////////////////////////////////////////////////////////

NodeMutex PJSUA::_nodeMutex;

// //////////////////////////////////////////////////////////////////////

// PJSIP-Node-Thread synchronization

// //////////////////////////////////////////////////////////////////////

// NodeMutex::Lock is a scoped lock that locks out Node's thread,
// i.e. when the instance has been constructed: Node's thread is
// suspended during the life time of a NodeMutex::Lock instances.  In
// addition to suspending Node's thread by the way of libev and
// condition variables, the Lock instance also locks V8 using
// v8::Locker and enters the context of the callback.

NodeMutex::Lock::Lock(NodeMutex& mutex)
  : _mutex(mutex)
{
  _mutex.suspendNodeThread();

  _locker = new Locker();
  _scope = new Context::Scope(mutex._callbackContext);
}

NodeMutex::Lock::~Lock()
{
  delete _scope;
  delete _locker;

  _mutex.resumeNodeThread();
}

NodeMutex::NodeMutex()
  : _nodeThreadId(pthread_self())
{
  ev_init(&_watcher, eventCallback);
  _watcher.data = this;
  ev_async_start(EV_DEFAULT_UC_ &_watcher);
}

NodeMutex::~NodeMutex()
{
  ev_async_stop(EV_DEFAULT_UC_ &_watcher);
}

void
NodeMutex::setCallback(Local<Function> callback)
{
  _callback = Persistent<Function>::New(callback);
}

Local<Value>
NodeMutex::invokeCallback(const char* eventName, int argc, ...)
{
  Local<Value> args[argc + 1];
  args[0] = String::New(eventName);

  va_list vl;
  va_start(vl, argc);
  for (int i = 0; i < argc; i++) {
    args[i + 1] = *va_arg(vl, Handle<Value>);
  }

  TryCatch tryCatch;
  Local<Value> retval = _callback->Call(_callbackContext->Global(), argc + 1, args);

  if (tryCatch.HasCaught()) {
    FatalException(tryCatch);
  }

  return retval;
}

void
NodeMutex::eventCallback(EV_P_ ev_async* w, int revents)
{
  NodeMutex* nodeMutex = reinterpret_cast<NodeMutex*>(w->data);
  nodeMutex->suspend();
}

void
NodeMutex::suspend()
{
  // set context in mutex so that other thread can access it (see invokeCallback / Lock)
  _callbackContext = Persistent<Context>::New(Context::GetCurrent());

  Unlocker unlocker;            // relinquish control over v8

  unique_lock<mutex> completeLock(_completeMutex);

  {
    unique_lock<mutex> proceedLock(_proceedMutex);
    
    _proceed.notify_one();      // tell the other thread to go ahead
  }

  // FIXME: _callbackContext needs to be released
  _complete.wait(completeLock); // wait for the other thread to complete processing
}

void
NodeMutex::suspendNodeThread()
{
  if (!pthread_equal(_nodeThreadId, pthread_self())) {
    unique_lock<mutex> lock(_proceedMutex);

    ev_async_send(EV_DEFAULT_ &_watcher);

    _proceed.wait(lock);        // wait until Node's thread is suspended
  }
}

void
NodeMutex::resumeNodeThread()
{
  if (!pthread_equal(_nodeThreadId, pthread_self())) {
    unique_lock<mutex> lock(_completeMutex);

    _complete.notify_one();     // tell Node's that that we're done
  }
}

// //////////////////////////////////////////////////////////////////////

void
PJSUA::Initialize(Handle<Object> target)
{
  HandleScope scope;

  target->Set(String::NewSymbol("start"), FunctionTemplate::New(start)->GetFunction());
  target->Set(String::NewSymbol("addAccount"), FunctionTemplate::New(addAccount)->GetFunction());
  target->Set(String::NewSymbol("callAnswer"), FunctionTemplate::New(callAnswer)->GetFunction());
  target->Set(String::NewSymbol("callMakeCall"), FunctionTemplate::New(callMakeCall)->GetFunction());
  target->Set(String::NewSymbol("callHangup"), FunctionTemplate::New(callHangup)->GetFunction());
  target->Set(String::NewSymbol("stop"), FunctionTemplate::New(stop)->GetFunction());
}

Handle<Value>
PJSUA::start(const Arguments& args)
{
  HandleScope scope;
  try {

    if (args.Length() > 0) {
      if (!args[0]->IsFunction()) {
        throw JSException("need callback function as argument");
      }
    }

    _nodeMutex.setCallback(Local<Function>::Cast(args[0]));

    /* Init pjsua */
    {
      pjsua_config cfg;
      pjsua_logging_config log_cfg;

      pjsua_config_default(&cfg);
      cfg.cb.on_call_state = on_call_state;
      cfg.cb.on_incoming_call = on_incoming_call;
      cfg.cb.on_call_tsx_state = on_call_tsx_state;
      cfg.cb.on_call_media_state = on_call_media_state;
      cfg.cb.on_stream_created = on_stream_created;
      cfg.cb.on_stream_destroyed = on_stream_destroyed;
      cfg.cb.on_dtmf_digit = on_dtmf_digit;
      cfg.cb.on_call_transfer_request = on_call_transfer_request;
      cfg.cb.on_call_transfer_status = on_call_transfer_status;
      cfg.cb.on_call_replace_request = on_call_replace_request;
      cfg.cb.on_call_replaced = on_call_replaced;
      cfg.cb.on_reg_state = on_reg_state;
      cfg.cb.on_reg_state2 = on_reg_state2;
      cfg.cb.on_incoming_subscribe = on_incoming_subscribe;
      cfg.cb.on_srv_subscribe_state = on_srv_subscribe_state;
      cfg.cb.on_buddy_state = on_buddy_state;
      cfg.cb.on_buddy_evsub_state = on_buddy_evsub_state;
      cfg.cb.on_pager = on_pager;
      cfg.cb.on_pager2 = on_pager2;
      cfg.cb.on_pager_status = on_pager_status;
      cfg.cb.on_pager_status2 = on_pager_status2;
      cfg.cb.on_typing = on_typing;
      cfg.cb.on_typing2 = on_typing2;
      cfg.cb.on_nat_detect = on_nat_detect;
      cfg.cb.on_mwi_info = on_mwi_info;
      cfg.cb.on_transport_state = on_transport_state;
      cfg.cb.on_ice_transport_error = on_ice_transport_error;

      pjsua_logging_config_default(&log_cfg);
      log_cfg.console_level = 1;

      pj_status_t status = pjsua_init(&cfg, &log_cfg, NULL);
      if (status != PJ_SUCCESS) {
        throw PJJSException("Error creating transport", status);
      }
    }

    /* Add UDP transport. */
    {
      pjsua_transport_config cfg;

      pjsua_transport_config_default(&cfg);
      cfg.port = 5060;
      pj_status_t status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
      if (status != PJ_SUCCESS) {
        throw PJJSException("Error creating transport", status);
      }
    }

    /* Initialization is done, now start pjsua */
    {
      pj_status_t status = pjsua_start();
      if (status != PJ_SUCCESS) {
        throw PJJSException("Error starting pjsua", status);
      }
    }

    return Undefined();
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::addAccount(const Arguments& args)
{
  try {
    if (args.Length() != 3) {
      throw JSException("Invalid number of arguments to addAccount, need sipUser, sipDomain and sipPassword");
    }

    const string sipUser = *String::Utf8Value(args[0]);
    const string sipDomain = *String::Utf8Value(args[1]);
    const string sipPassword = *String::Utf8Value(args[2]);

    const string id = "sip:" + sipUser + "@" + sipDomain;
    const string regUri = "sip:" + sipDomain;
  
    /* Register to SIP server by creating SIP account. */
    {
      pjsua_acc_config cfg;
      pjsua_acc_id acc_id;

      pjsua_acc_config_default(&cfg);
      cfg.id = pj_str((char*) id.c_str());
      cfg.reg_uri = pj_str((char*) regUri.c_str());
      cfg.cred_count = 1;
      cfg.cred_info[0].realm = pj_str((char*) sipDomain.c_str());
      cfg.cred_info[0].scheme = pj_str((char*) "digest");
      cfg.cred_info[0].username = pj_str((char*) sipUser.c_str());
      cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
      cfg.cred_info[0].data = pj_str((char*) sipPassword.c_str());

      pj_status_t status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
      if (status != PJ_SUCCESS) {
        throw PJJSException("Error adding account", status);
      }
      return Integer::New(acc_id);
    }
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::callAnswer(const Arguments& args)
{
  try {
    if (args.Length() < 1 || args.Length() > 4) {
      throw JSException("Invalid number of arguments to callAnswer (callId[, status[, reason[, msg_data]]])");
    }

    pjsua_call_id call_id;
    unsigned code = 200;
    pj_str_t* reason = 0;
    pjsua_msg_data* msg_data = 0;

    switch (args.Length()) {
      // FIXME: implement reason/msg_data
    case 4:
    case 3:
      throw JSException("reason and msg_data arguments not implemented");
    case 2:
      code = args[1]->Int32Value();
    case 1:
      call_id = args[0]->Int32Value();
    }

    pj_status_t status = pjsua_call_answer(call_id, code, reason, msg_data);
    if (status != PJ_SUCCESS) {
      throw PJJSException("Error answering call", status);
    }

    return Undefined();
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::callHangup(const Arguments& args)
{
  try {
    if (args.Length() < 1 || args.Length() > 4) {
      throw JSException("Invalid number of arguments to callHangup (callId[, status[, reason[, msg_data]]])");
    }

    pjsua_call_id call_id = args[0]->Int32Value();
    unsigned code = 0;
    pj_str_t* reason = 0;
    pjsua_msg_data* msg_data = 0;

    switch (args.Length()) {
      // FIXME: implement reason/msg_data
    case 4:
    case 3:
      throw JSException("reason and msg_data arguments not implemented");
    case 2:
      code = args[1]->Int32Value();
    }

    pj_status_t status = pjsua_call_hangup(call_id, code, reason, msg_data);
    if (status != PJ_SUCCESS) {
      throw PJJSException("Error hanging up", status);
    }

    return Undefined();
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::callMakeCall(const Arguments& args)
{
  try {
    if (args.Length() < 2 || args.Length() > 5) {
      throw JSException("Invalid number of arguments to callMakeCall (accId, destUri[, options[, user_data[, msg_data]]])");
    }

    pjsua_acc_id acc_id = args[0]->Int32Value();
    String::Utf8Value dest_uri(args[1]);
    unsigned options = 0;
    void* user_data = 0;
    pjsua_msg_data* msg_data = 0;
    pjsua_call_id call_id = 0;

    switch (args.Length()) {
      // FIXME: implement options/user_data/msg_data
    case 5:
    case 4:
    case 3:
      throw JSException("options, user_data and msg_data arguments not implemented");
    }

    pj_str_t pj_dest_uri;
    pj_dest_uri.ptr = (char*) *dest_uri;
    pj_dest_uri.slen = dest_uri.length();

    pj_status_t status = pjsua_call_make_call(acc_id, &pj_dest_uri, options, user_data, msg_data, &call_id);
    if (status != PJ_SUCCESS) {
      throw PJJSException("Error making call", status);
    }

    return Integer::New(call_id);
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::stop(const Arguments& args)
{
  return Undefined();
}

extern "C" {

  static void uninit()
  {
    pjsua_destroy();
  }

  static void init(Handle<Object> target)
  {
    if (pjsua_create() != PJ_SUCCESS) {
      cerr << "error in pjsua_create()" << endl;
      abort();
    }
    PJSUA::Initialize(target);
    atexit(uninit);
  }

  NODE_MODULE(pjsip, init);
}

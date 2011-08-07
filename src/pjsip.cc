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
#include <queue>

#include <v8.h>
#include <node.h>
#include <node_events.h>

// prevent name clash between pjsua.h and node.h
#define pjsip_module pjsip_module_
#include <pjsua-lib/pjsua.h>
#undef pjsip_module

#include "json/json.h"

#include "mutex.h"

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
;

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

// //////////////////////////////////////////////////////////////////////
//
// Event classes - Callbacks invoked by PJSUA instantiate subclass
// instances and queue them to the main Node thread for processing.
// Every event class carries a representation of the event.
// 
// //////////////////////////////////////////////////////////////////////

class PJSUAEvent
{
public:
  virtual const string eventName() const = 0;
  virtual ~PJSUAEvent() {};

  const Json::Value& eventData() const { return _eventData; }

protected:
  Json::Value _eventData;
};

class PJSUACallEvent
  : public virtual PJSUAEvent
{
protected:
  PJSUACallEvent(pjsua_call_id callId);
};


class PJSUAAccStateEvent
  : public virtual PJSUAEvent
{
protected:
  PJSUAAccStateEvent(pjsua_acc_id acc_id);
};

class PJSUACallStateEvent
  : public PJSUACallEvent
{
public:
  PJSUACallStateEvent(pjsua_call_id call_id,
                      pjsip_event *e)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_state"; }
};

class PJSUAIncomingCallEvent
  : public PJSUACallEvent,
    public PJSUAAccStateEvent
{
public:
  PJSUAIncomingCallEvent(pjsua_acc_id acc_id,
                         pjsua_call_id call_id,
                         pjsip_rx_data *rdata)
    : PJSUACallEvent(call_id),
      PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "incoming_call"; }
};

class PJSUACallTsxStateEvent
  : public PJSUACallEvent
{
public:
  PJSUACallTsxStateEvent(pjsua_call_id call_id,
                         pjsip_transaction *tsx,
                         pjsip_event *e)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_tsx_state"; }
};

class PJSUACallMediaStateEvent
  : public PJSUACallEvent
{
public:
  PJSUACallMediaStateEvent(pjsua_call_id call_id)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_media_state"; }
};

class PJSUAStreamCreatedEvent
  : public PJSUACallEvent
{
public:
  PJSUAStreamCreatedEvent(pjsua_call_id call_id,
                          pjmedia_session *sess,
                          unsigned stream_idx,
                          pjmedia_port **p_port)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "stream_created"; }
};

class PJSUAStreamDestroyedEvent
  : public PJSUACallEvent
{
public:
  PJSUAStreamDestroyedEvent(pjsua_call_id call_id,
                            pjmedia_session *sess,
                            unsigned stream_idx)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "stream_destroyed"; }
};

class PJSUADtmfDigitEvent
  : public PJSUACallEvent
{
public:
  PJSUADtmfDigitEvent(pjsua_call_id call_id,
                      int digit)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "dtmf_digit"; }
};

class PJSUACallTransferRequestEvent
  : public PJSUACallEvent
{
public:
  PJSUACallTransferRequestEvent(pjsua_call_id call_id,
                                const pj_str_t *dst,
                                pjsip_status_code *code)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_transfer_request"; }
};

class PJSUACallTransferStatusEvent
  : public PJSUACallEvent
{
public:
  PJSUACallTransferStatusEvent(pjsua_call_id call_id,
                               int st_code,
                               const pj_str_t *st_text,
                               pj_bool_t final,
                               pj_bool_t *p_cont)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_transfer_status"; }
};

class PJSUACallReplaceRequestEvent
  : public PJSUACallEvent
{
public:
  PJSUACallReplaceRequestEvent(pjsua_call_id call_id,
                               pjsip_rx_data *rdata,
                               int *st_code,
                               pj_str_t *st_text)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "call_replace_request"; }
};

class PJSUACallReplacedEvent
  : public PJSUACallEvent
{
public:
  PJSUACallReplacedEvent(pjsua_call_id old_call_id,
                         pjsua_call_id new_call_id)
    : PJSUACallEvent(old_call_id)
  {}
  virtual const string eventName() const { return "call_replaced"; }
};

class PJSUARegStateEvent
  : public PJSUAAccStateEvent
{
public:
  PJSUARegStateEvent(pjsua_acc_id acc_id)
    : PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "reg_state"; }
};

class PJSUARegState2Event
  : public PJSUAAccStateEvent
{
public:
  PJSUARegState2Event(pjsua_acc_id acc_id,
                      pjsua_reg_info *info)
    : PJSUAAccStateEvent(acc_id)
  {
  }
  virtual const string eventName() const { return "reg_state2"; }
};

class PJSUAIncomingSubscribeEvent
  : public PJSUAAccStateEvent
{
public:
  PJSUAIncomingSubscribeEvent(pjsua_acc_id acc_id,
                              pjsua_srv_pres *srv_pres,
                              pjsua_buddy_id buddy_id,
                              const pj_str_t *from,
                              pjsip_rx_data *rdata,
                              pjsip_status_code *code,
                              pj_str_t *reason,
                              pjsua_msg_data *msg_data)
    : PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "incoming_subscribe"; }
};

class PJSUASrvSubscribeStateEvent
  : public PJSUAAccStateEvent
{
public:
  PJSUASrvSubscribeStateEvent(pjsua_acc_id acc_id,
                              pjsua_srv_pres *srv_pres,
                              const pj_str_t *remote_uri,
                              pjsip_evsub_state state,
                              pjsip_event *event)
    : PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "srv_subscribe_state"; }
};

class PJSUABuddyStateEvent
  : public PJSUAEvent
{
public:
  PJSUABuddyStateEvent(pjsua_buddy_id buddy_id)
  {}
  virtual const string eventName() const { return "buddy_state"; }
};

class PJSUABuddyEvsubStateEvent
  : public PJSUAEvent
{
public:
  PJSUABuddyEvsubStateEvent(pjsua_buddy_id buddy_id,
                            pjsip_evsub *sub,
                            pjsip_event *event)
  {}
  virtual const string eventName() const { return "buddy_evsub_state"; }
};

class PJSUAPagerEvent
  : public PJSUACallEvent
{
public:
  PJSUAPagerEvent(pjsua_call_id call_id,
                  const pj_str_t *from,
                  const pj_str_t *to,
                  const pj_str_t *contact,
                  const pj_str_t *mime_type,
                  const pj_str_t *body)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "pager"; }
};

class PJSUAPager2Event
  : public PJSUACallEvent,
    public PJSUAAccStateEvent
{
public:
  PJSUAPager2Event(pjsua_call_id call_id,
                   const pj_str_t *from,
                   const pj_str_t *to,
                   const pj_str_t *contact,
                   const pj_str_t *mime_type,
                   const pj_str_t *body,
                   pjsip_rx_data *rdata,
                   pjsua_acc_id acc_id)
    : PJSUACallEvent(call_id),
      PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "pager2"; }
};

class PJSUAPagerStatusEvent
  : public PJSUACallEvent
{
public:
  PJSUAPagerStatusEvent(pjsua_call_id call_id,
                        const pj_str_t *to,
                        const pj_str_t *body,
                        void *user_data,
                        pjsip_status_code status,
                        const pj_str_t *reason)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "pager_status"; }
};

class PJSUAPagerStatus2Event
  : public PJSUACallEvent,
    public PJSUAAccStateEvent
{
public:
  PJSUAPagerStatus2Event(pjsua_call_id call_id,
                         const pj_str_t *to,
                         const pj_str_t *body,
                         void *user_data,
                         pjsip_status_code status,
                         const pj_str_t *reason,
                         pjsip_tx_data *tdata,
                         pjsip_rx_data *rdata,
                         pjsua_acc_id acc_id)
    : PJSUACallEvent(call_id),
      PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "pager_status2"; }
};

class PJSUATypingEvent
  : public PJSUACallEvent
{
public:
  PJSUATypingEvent(pjsua_call_id call_id,
                   const pj_str_t *from,
                   const pj_str_t *to,
                   const pj_str_t *contact,
                   pj_bool_t is_typing)
    : PJSUACallEvent(call_id)
  {}
  virtual const string eventName() const { return "typing"; }
};

class PJSUATyping2Event
  : public PJSUACallEvent,
    public PJSUAAccStateEvent
{
public:
  PJSUATyping2Event(pjsua_call_id call_id,
                    const pj_str_t *from,
                    const pj_str_t *to,
                    const pj_str_t *contact,
                    pj_bool_t is_typing,
                    pjsip_rx_data *rdata,
                    pjsua_acc_id acc_id)
    : PJSUACallEvent(call_id),
      PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "typing2"; }
};

class PJSUANatDetectEvent
  : public PJSUAEvent
{
public:
  PJSUANatDetectEvent(const pj_stun_nat_detect_result *res)
  {}
  virtual const string eventName() const { return "nat_detect"; }
};

class PJSUAMwiInfoEvent
  : public PJSUAAccStateEvent
{
public:
  PJSUAMwiInfoEvent(pjsua_acc_id acc_id,
                    pjsua_mwi_info *mwi_info)
    : PJSUAAccStateEvent(acc_id)
  {}
  virtual const string eventName() const { return "mwi_info"; }
};

class PJSUATransportStateEvent
  : public PJSUAEvent
{
public:
  PJSUATransportStateEvent(pjsip_transport *tp,
                           pjsip_transport_state state,
                           const pjsip_transport_state_info *info)
  {}
  virtual const string eventName() const { return "transport_state"; }
};

class PJSUAIceTransportErrorEvent
  : public PJSUAEvent
{
public:
  PJSUAIceTransportErrorEvent(int index,
                              pj_ice_strans_op op,
                              pj_status_t status,
                              void *param)
  {}
  virtual const string eventName() const { return "ice_transport_error"; }
};

// //////////////////////////////////////////////////////////////////////

// Class PJSUA encapsulates the connection between Node and PJ

class PJSUA
{
  static queue<PJSUAEvent*> _eventQueue;
  static mutex _eventQueueLock;
  static condition_variable _eventQueueFull;
  static Persistent<Function> _callback;

  // //////////////////////////////////////////////////////////////////////
  //
  // libev interface
  
  static int EIO_waitForEvent(eio_req* req);
  static int EIO_waitForEventDone(eio_req* req);

  static void startEio()
  {
    eio_custom(EIO_waitForEvent,
               EIO_PRI_DEFAULT,
               EIO_waitForEventDone,
               0);
  }

  // //////////////////////////////////////////////////////////////////////
  //
  // Callback functions to be called by PJ

  static void on_call_state(pjsua_call_id call_id,
                            pjsip_event *e)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallStateEvent(call_id, e));
    _eventQueueFull.notify_one();
  }

  static void on_incoming_call(pjsua_acc_id acc_id,
                               pjsua_call_id call_id,
                               pjsip_rx_data *rdata)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAIncomingCallEvent(acc_id, call_id, rdata));
    _eventQueueFull.notify_one();
  }

  static void on_call_tsx_state(pjsua_call_id call_id,
                                pjsip_transaction *tsx,
                                pjsip_event *e)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallTsxStateEvent(call_id, tsx, e));
    _eventQueueFull.notify_one();
  }

  static void on_call_media_state(pjsua_call_id call_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallMediaStateEvent(call_id));
    _eventQueueFull.notify_one();
  }

  static void on_stream_created(pjsua_call_id call_id,
                                pjmedia_session *sess,
                                unsigned stream_idx,
                                pjmedia_port **p_port)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAStreamCreatedEvent(call_id, sess, stream_idx, p_port));
    _eventQueueFull.notify_one();
  }

  static void on_stream_destroyed(pjsua_call_id call_id,
                                  pjmedia_session *sess,
                                  unsigned stream_idx)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAStreamDestroyedEvent(call_id, sess, stream_idx));
    _eventQueueFull.notify_one();
  }

  static void on_dtmf_digit(pjsua_call_id call_id,
                            int digit)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUADtmfDigitEvent(call_id, digit));
    _eventQueueFull.notify_one();
  }

  static void on_call_transfer_request(pjsua_call_id call_id,
                                       const pj_str_t *dst,
                                       pjsip_status_code *code)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallTransferRequestEvent(call_id, dst, code));
    _eventQueueFull.notify_one();
  }

  static void on_call_transfer_status(pjsua_call_id call_id,
                                      int st_code,
                                      const pj_str_t *st_text,
                                      pj_bool_t final,
                                      pj_bool_t *p_cont)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallTransferStatusEvent(call_id, st_code, st_text, final, p_cont));
    _eventQueueFull.notify_one();
  }

  static void on_call_replace_request(pjsua_call_id call_id,
                                      pjsip_rx_data *rdata,
                                      int *st_code,
                                      pj_str_t *st_text)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallReplaceRequestEvent(call_id, rdata, st_code, st_text));
    _eventQueueFull.notify_one();
  }

  static void on_call_replaced(pjsua_call_id old_call_id,
                               pjsua_call_id new_call_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUACallReplacedEvent(old_call_id, new_call_id));
    _eventQueueFull.notify_one();
  }

  static void on_reg_state(pjsua_acc_id acc_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUARegStateEvent(acc_id));
    _eventQueueFull.notify_one();
  }

  static void on_reg_state2(pjsua_acc_id acc_id,
                            pjsua_reg_info *info)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUARegState2Event(acc_id, info));
    _eventQueueFull.notify_one();
  }

  static void on_incoming_subscribe(pjsua_acc_id acc_id,
                                    pjsua_srv_pres *srv_pres,
                                    pjsua_buddy_id buddy_id,
                                    const pj_str_t *from,
                                    pjsip_rx_data *rdata,
                                    pjsip_status_code *code,
                                    pj_str_t *reason,
                                    pjsua_msg_data *msg_data)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAIncomingSubscribeEvent(acc_id, srv_pres, buddy_id, from, rdata, code, reason, msg_data));
    _eventQueueFull.notify_one();
  }

  static void on_srv_subscribe_state(pjsua_acc_id acc_id,
                                     pjsua_srv_pres *srv_pres,
                                     const pj_str_t *remote_uri,
                                     pjsip_evsub_state state,
                                     pjsip_event *event)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUASrvSubscribeStateEvent(acc_id, srv_pres, remote_uri, state, event));
    _eventQueueFull.notify_one();
  }

  static void on_buddy_state(pjsua_buddy_id buddy_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUABuddyStateEvent(buddy_id));
    _eventQueueFull.notify_one();
  }

  static void on_buddy_evsub_state(pjsua_buddy_id buddy_id,
                                   pjsip_evsub *sub,
                                   pjsip_event *event)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUABuddyEvsubStateEvent(buddy_id, sub, event));
    _eventQueueFull.notify_one();
  }

  static void on_pager(pjsua_call_id call_id,
                       const pj_str_t *from,
                       const pj_str_t *to,
                       const pj_str_t *contact,
                       const pj_str_t *mime_type,
                       const pj_str_t *body)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAPagerEvent(call_id, from, to, contact, mime_type, body));
    _eventQueueFull.notify_one();
  }

  static void on_pager2(pjsua_call_id call_id,
                        const pj_str_t *from,
                        const pj_str_t *to,
                        const pj_str_t *contact,
                        const pj_str_t *mime_type,
                        const pj_str_t *body,
                        pjsip_rx_data *rdata,
                        pjsua_acc_id acc_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAPager2Event(call_id, from, to, contact, mime_type, body, rdata, acc_id));
    _eventQueueFull.notify_one();
  }

  static void on_pager_status(pjsua_call_id call_id,
                              const pj_str_t *to,
                              const pj_str_t *body,
                              void *user_data,
                              pjsip_status_code status,
                              const pj_str_t *reason)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAPagerStatusEvent(call_id, to, body, user_data, status, reason));
    _eventQueueFull.notify_one();
  }

  static void on_pager_status2(pjsua_call_id call_id,
                               const pj_str_t *to,
                               const pj_str_t *body,
                               void *user_data,
                               pjsip_status_code status,
                               const pj_str_t *reason,
                               pjsip_tx_data *tdata,
                               pjsip_rx_data *rdata,
                               pjsua_acc_id acc_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAPagerStatus2Event(call_id, to, body, user_data, status, reason, tdata, rdata, acc_id));
    _eventQueueFull.notify_one();
  }

  static void on_typing(pjsua_call_id call_id,
                        const pj_str_t *from,
                        const pj_str_t *to,
                        const pj_str_t *contact,
                        pj_bool_t is_typing)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUATypingEvent(call_id, from, to, contact, is_typing));
    _eventQueueFull.notify_one();
  }

  static void on_typing2(pjsua_call_id call_id,
                         const pj_str_t *from,
                         const pj_str_t *to,
                         const pj_str_t *contact,
                         pj_bool_t is_typing,
                         pjsip_rx_data *rdata,
                         pjsua_acc_id acc_id)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUATyping2Event(call_id, from, to, contact, is_typing, rdata, acc_id));
    _eventQueueFull.notify_one();
  }

  static void on_nat_detect(const pj_stun_nat_detect_result *res)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUANatDetectEvent(res));
    _eventQueueFull.notify_one();
  }

  static void on_mwi_info(pjsua_acc_id acc_id,
                          pjsua_mwi_info *mwi_info)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAMwiInfoEvent(acc_id, mwi_info));
    _eventQueueFull.notify_one();
  }

  static void on_transport_state(pjsip_transport *tp,
                                 pjsip_transport_state state,
                                 const pjsip_transport_state_info *info)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUATransportStateEvent(tp, state, info));
    _eventQueueFull.notify_one();
  }

  static void on_ice_transport_error(int index,
                                     pj_ice_strans_op op,
                                     pj_status_t status,
                                     void *param)
  {
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUAIceTransportErrorEvent(index, op, status, param));
    _eventQueueFull.notify_one();
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

// Event instantiation

// //////////////////////////////////////////////////////////////////////

#define PJSTR_TO_JSON(pjstr) Json::Value(pjstr.ptr, pjstr.ptr + pjstr.slen)
#define PJ_TIME_VAL_TO_DOUBLE(pjtv) ((double) pjtv.sec + ((double) pjtv.msec * 0.001d))

PJSUACallEvent::PJSUACallEvent(pjsua_call_id callId)
{
  pjsua_call_info ci;
  pjsua_call_get_info(callId, &ci);

  Json::Value json_ci;
  json_ci["id"] = ci.id;
  json_ci["role"] = (ci.role == PJSIP_ROLE_UAC) ? "UAC" : "UAS";
  json_ci["acc_id"] = ci.acc_id;
  json_ci["local_info"] = PJSTR_TO_JSON(ci.local_info);
  json_ci["local_contact"] = PJSTR_TO_JSON(ci.local_contact);
  json_ci["remote_info"] = PJSTR_TO_JSON(ci.remote_info);
  json_ci["remote_contact"] = PJSTR_TO_JSON(ci.remote_contact);
  json_ci["call_id"] = PJSTR_TO_JSON(ci.call_id);
  json_ci["state"] = (int) ci.state;
  json_ci["state_text"] = PJSTR_TO_JSON(ci.state_text);
  json_ci["last_status"] = (int) ci.last_status;
  json_ci["last_status_text"] = PJSTR_TO_JSON(ci.last_status_text);
  json_ci["media_status"] = (int) ci.media_status;
  json_ci["media_dir"] = (int) ci.media_dir;
  json_ci["conf_slot"] = (int) ci.conf_slot;
  json_ci["connect_duration"] = PJ_TIME_VAL_TO_DOUBLE(ci.connect_duration);
  json_ci["total_duration"] = PJ_TIME_VAL_TO_DOUBLE(ci.total_duration);
  _eventData["ci"] = json_ci;
}

PJSUAAccStateEvent::PJSUAAccStateEvent(pjsua_acc_id accId)
{
  pjsua_acc_info ai;
  pjsua_acc_get_info(accId, &ai);

  Json::Value json_ai;
  json_ai["id"] = ai.id;
  json_ai["is_default"] = (bool) ai.is_default;
  json_ai["acc_uri"] = PJSTR_TO_JSON(ai.acc_uri);
  json_ai["has_registration"] = (bool) ai.has_registration;
  json_ai["expires"] = ai.expires;
  json_ai["status"] = ai.status;
  json_ai["reg_last_err"] = ai.reg_last_err;
  json_ai["status_text"] = PJSTR_TO_JSON(ai.status_text);
  json_ai["online_status"] = (bool) ai.online_status;
  json_ai["online_status_text"] = PJSTR_TO_JSON(ai.online_status_text);
  json_ai["rpid"]["type"] = ai.rpid.type;
  json_ai["rpid"]["id"] = PJSTR_TO_JSON(ai.rpid.id);
  json_ai["rpid"]["activity"] = ai.rpid.activity;
  json_ai["rpid"]["note"] = PJSTR_TO_JSON(ai.rpid.note);
  _eventData["ai"] = json_ai;
}

// //////////////////////////////////////////////////////////////////////

queue<PJSUAEvent*> PJSUA::_eventQueue;
mutex PJSUA::_eventQueueLock;
condition_variable PJSUA::_eventQueueFull;
Persistent<Function> PJSUA::_callback;

int
PJSUA::EIO_waitForEvent(eio_req* req)
{
  unique_lock<mutex> lock(_eventQueueLock);
  while (_eventQueue.empty()) {
    _eventQueueFull.wait(lock);
  }
  req->data = _eventQueue.front();
  _eventQueue.pop();

  return 0;
}

Handle<Value>
JsonToV8(Json::Value value)
{
  switch (value.type()) {
  case Json::nullValue:
    return Undefined();
  case Json::intValue:
    return Integer::New(value.asInt());
  case Json::uintValue:
    return Integer::New(value.asUInt());
  case Json::realValue:
    return Number::New(value.asDouble());
  case Json::stringValue:
    return String::New(value.asCString());
  case Json::booleanValue:
    return value.asBool() ? True() : False();
  case Json::arrayValue:
    {
      Handle<Array> v8array = Array::New(value.size());
      for (Json::ArrayIndex i = 0; i < value.size(); i++) {
        v8array->Set(i, JsonToV8(value[i]));
      }
      return v8array;
    }
  case Json::objectValue:
    {
      Handle<Object> v8object = Object::New();
      const Json::Value::Members& members = value.getMemberNames();
      for (vector<string>::const_iterator i = members.begin(); i != members.end(); i++) {
        v8object->Set(JsonToV8((*i).c_str()), JsonToV8(value[*i]));
      }
      return v8object;
    }
  default:
    // FIXME: better error reporting
    cerr << "unrecognized type " << value.type() << " in JSON data" << endl;
    abort();
  }
}

int
PJSUA::EIO_waitForEventDone(eio_req* req)
{
  PJSUAEvent* event = (PJSUAEvent*) req->data;

  HandleScope scope;

  Local<Value> argv[2];
  argv[0] = String::New(event->eventName().c_str());
  argv[1] = *JsonToV8(event->eventData());

  TryCatch tryCatch;
  _callback->Call(Context::GetCurrent()->Global(), 2, argv);

  if (tryCatch.HasCaught()) {
    FatalException(tryCatch);
  }

  delete event;
  startEio();

  return 0;
}

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

    _callback = Persistent<Function>::New(Local<Function>::Cast(args[0]));

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

    {
      startEio();

      ev_ref(EV_DEFAULT_UC);
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
    if (args.Length() < 2 || args.Length() > 6) {
      throw JSException("Invalid number of arguments to callMakeCall (accId, destUri[, options[, user_data[, msg_data[, p_call_id]]]])");
    }

    pjsua_acc_id acc_id = args[0]->Int32Value();
    String::Utf8Value dest_uri(args[1]);
    unsigned options = 0;
    void* user_data = 0;
    pjsua_msg_data* msg_data = 0;
    pjsua_call_id* p_call_id = 0;

    switch (args.Length()) {
      // FIXME: implement options/user_data/msg_data/p_call_id
    case 6:
    case 5:
    case 4:
    case 3:
      throw JSException("options, user_data, msg_data and p_call_id arguments not implemented");
    }

    pj_str_t pj_dest_uri;
    pj_dest_uri.ptr = (char*) *dest_uri;
    pj_dest_uri.slen = dest_uri.length();

    pj_status_t status = pjsua_call_make_call(acc_id, &pj_dest_uri, options, user_data, msg_data, p_call_id);
    if (status != PJ_SUCCESS) {
      throw PJJSException("Error making call", status);
    }

    return Undefined();
  }
  catch (const JSException& e) {
    return e.asV8Exception();
  }
}

Handle<Value>
PJSUA::stop(const Arguments& args)
{
  ev_unref(EV_DEFAULT_UC);

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

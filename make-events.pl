#!/bin/perl

@defs = split(/\n/, 'call_state pjsua_call_id call_id, pjsip_event *e
incoming_call pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata
call_tsx_state pjsua_call_id call_id, pjsip_transaction *tsx, pjsip_event *e
call_media_state pjsua_call_id call_id
stream_created pjsua_call_id call_id, pjmedia_session *sess, unsigned stream_idx, pjmedia_port **p_port
stream_destroyed pjsua_call_id call_id, pjmedia_session *sess, unsigned stream_idx
dtmf_digit pjsua_call_id call_id, int digit
call_transfer_request pjsua_call_id call_id, const pj_str_t *dst, pjsip_status_code *code
call_transfer_status pjsua_call_id call_id, int st_code, const pj_str_t *st_text, pj_bool_t final, pj_bool_t *p_cont
call_replace_request pjsua_call_id call_id, pjsip_rx_data *rdata, int *st_code, pj_str_t *st_text
call_replaced pjsua_call_id old_call_id, pjsua_call_id new_call_id
reg_state pjsua_acc_id acc_id
reg_state2 pjsua_acc_id acc_id, pjsua_reg_info *info
incoming_subscribe pjsua_acc_id acc_id, pjsua_srv_pres *srv_pres, pjsua_buddy_id buddy_id, const pj_str_t *from, pjsip_rx_data *rdata, pjsip_status_code *code, pj_str_t *reason, pjsua_msg_data *msg_data
srv_subscribe_state pjsua_acc_id acc_id, pjsua_srv_pres *srv_pres, const pj_str_t *remote_uri, pjsip_evsub_state state, pjsip_event *event
buddy_state pjsua_buddy_id buddy_id
buddy_evsub_state pjsua_buddy_id buddy_id, pjsip_evsub *sub, pjsip_event *event
pager pjsua_call_id call_id, const pj_str_t *from, const pj_str_t *to, const pj_str_t *contact, const pj_str_t *mime_type, const pj_str_t *body
pager2 pjsua_call_id call_id, const pj_str_t *from, const pj_str_t *to, const pj_str_t *contact, const pj_str_t *mime_type, const pj_str_t *body, pjsip_rx_data *rdata, pjsua_acc_id acc_id
pager_status pjsua_call_id call_id, const pj_str_t *to, const pj_str_t *body, void *user_data, pjsip_status_code status, const pj_str_t *reason
pager_status2 pjsua_call_id call_id, const pj_str_t *to, const pj_str_t *body, void *user_data, pjsip_status_code status, const pj_str_t *reason, pjsip_tx_data *tdata, pjsip_rx_data *rdata, pjsua_acc_id acc_id
typing pjsua_call_id call_id, const pj_str_t *from, const pj_str_t *to, const pj_str_t *contact, pj_bool_t is_typing
typing2 pjsua_call_id call_id, const pj_str_t *from, const pj_str_t *to, const pj_str_t *contact, pj_bool_t is_typing, pjsip_rx_data *rdata, pjsua_acc_id acc_id
nat_detect const pj_stun_nat_detect_result *res
mwi_info pjsua_acc_id acc_id, pjsua_mwi_info *mwi_info
transport_state pjsip_transport *tp, pjsip_transport_state state, const pjsip_transport_state_info *info
ice_transport_error int index, pj_ice_strans_op op, pj_status_t status, void *param');

foreach (@defs) {
    my ($event, $args) = split(/ +/, $_, 2);
    print "static void on_$event(", join(",\n", split(', ', $args)), ")
{
    cout << \"$event\" << endl;
    unique_lock<mutex> lock(_eventQueueLock);
    _eventQueue.push(new PJSUA", join('', map(ucfirst, split(/_/, $event))), "Event(", join(', ', (map { s/.* \*?//; $_ } split(/, */, $args))), "));
    _eventQueueFull.notify_one();
}

";
}

foreach (@defs) {
    my ($event, $args) = split(/ +/, $_, 2);
print "      cfg.cb.on_$event = on_$event;
";
}

foreach (@defs) {
    my ($event, $args) = split(/ +/, $_, 2);
    my $className = "PJSUA" . join('', map(ucfirst, split(/_/, $event))) . "Event";
    print "
class $className
  : public PJSUAEvent
{
public:
  $className(", join(",\n", split(', ', $args)), ")
  {}
  virtual const string eventName() const { return \"$event\"; }
};

";
}


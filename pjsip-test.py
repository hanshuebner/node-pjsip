# GPL applies

import sys
import pjsua as pj
import threading

current_call = None

def log_cb(level, str, len):
    print str,
    sys.stdout.flush()

class MyAccountCallback(pj.AccountCallback):
    register_sem = None

    def __init__(self, account):
        pj.AccountCallback.__init__(self, account)

    def wait(self):
        self.register_sem = threading.Semaphore(0)
        self.register_sem.acquire()

    def on_reg_state(self):
        if self.register_sem:
            if self.account.info().reg_status >= 200:
                self.register_sem.release()

    # Notification on incoming call
    def on_incoming_call(self, call):
        global current_call 
        if current_call:
            call.answer(486, "Busy")
            return
            
        print "INCOMING", call.info().remote_uri

        current_call = call

        call_cb = MyCallCallback(current_call)
        current_call.set_callback(call_cb)

        current_call.answer(180)

# Callback to receive events from Call
class MyCallCallback(pj.CallCallback):

    def __init__(self, call=None):
        pj.CallCallback.__init__(self, call)
        self.media_active = False

    # Notification when call state has changed
    def on_state(self):
        global current_call
        print "STATE", self.call.info().state_text,
        print "CODE", self.call.info().last_code, 
        print self.call.info().last_reason
        
        if self.call.info().state == pj.CallState.DISCONNECTED:
            current_call = None

    # Notification when call's media state has changed.
    def on_media_state(self):
        if self.call.info().media_state == pj.MediaState.ACTIVE:
            # Connect the call to sound device
            call_slot = self.call.info().conf_slot
            lib = pj.Lib.instance()
            lib.conf_connect(call_slot, 0)
            lib.conf_connect(0, call_slot)
            if self.call.info().state != pj.CallState.CONFIRMED:
                print "MEDIA active, but not confirmed"
            elif not self.media_active:
                lib.set_snd_dev(0, 0)
                print "MEDIA active"
                self.media_active = True
            else:
                print "MEDIA already active"
        else:
            self.media_active = False
            print "MEDIA inactive"

# Function to make call
def make_call(uri):
    try:
        print "CALLING", uri
        return acc.make_call(uri, cb=MyCallCallback())
    except pj.Error, e:
        print "Exception: " + str(e)
        return None
        
lib = pj.Lib()

try:
    log_cfg = pj.LogConfig(console_level=1, callback=log_cb)
    media_cfg = pj.MediaConfig()
#    media_cfg.snd_auto_close_time = 0
    media_cfg.no_vad = 0
    lib.init(log_cfg=log_cfg, media_cfg=media_cfg)
    transport = lib.create_transport(pj.TransportType.UDP, pj.TransportConfig(5080))
    lib.start()

    for index, sound_device_info in enumerate(lib.enum_snd_dev()):
        direction = ""
        if sound_device_info.input_channels:
            direction = direction + "IN"
        if sound_device_info.output_channels:
            direction = direction + "OUT"
        print "SOUND_DEVICE", index, direction, sound_device_info.name

    # fixme - make asynchronous?
    acc = lib.create_account(pj.AccountConfig("192.168.2.2", "2002", "1234"))

    lib.set_null_snd_dev()

    acc_cb = MyAccountCallback(acc)
    acc.set_callback(acc_cb)
    acc_cb.wait()

    print "REGISTRATION_COMPLETE", acc.info().reg_status, acc.info().reg_reason

    my_sip_uri = "sip:" + transport.info().host + ":" + str(transport.info().port)

    # Menu loop
    while True:
        print "SIP_URI", my_sip_uri

        input = sys.stdin.readline().rstrip("\r\n").split();
        if input == []:
            continue
        command = input[0].upper();
        if command == "CALL":
            if current_call:
                print "ERROR Already have another call"
                continue
            lck = lib.auto_lock()
            current_call = make_call(input[1])
            del lck

        elif command == "HANGUP":
            if not current_call:
                print "ERROR There is no call"
                continue
            current_call.hangup()

        elif command == "ANSWER":
            if not current_call:
                print "ERROR There is no call"
                continue
            current_call.answer(200)

        elif command == "QUIT":
            break

    # Shutdown the library
    transport = None
    acc.delete()
    acc = None
    lib.destroy()
    lib = None

except pj.Error, e:
    print "Exception: " + str(e)
    lib.destroy()


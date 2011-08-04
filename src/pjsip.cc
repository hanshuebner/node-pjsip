// -*- C++ -*-

// MIDI.cc - MIDI interface for node.js, based on the portmidi
// cross-platform MIDI library

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

#include <v8.h>
#include <node.h>
#include <node_events.h>

// prevent name clash between pjsua.h and node.h
#define pjsip_module pjsip_module_
#include <pjsua-lib/pjsua.h>
#undef pjsip_module

using namespace std;
using namespace v8;
using namespace node;

extern "C" {

  static void init (Handle<Object> target)
  {
    if (pjsua_create() != PJ_SUCCESS) {
      cerr << "error in pjsua_create()" << endl;
      abort();
    }
  }

  NODE_MODULE(pjsip, init);
}

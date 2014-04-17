/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/ext/ext_xenon.h"
#include "hphp/runtime/ext/ext_function.h"
#include "hphp/runtime/ext/asio/waitable_wait_handle.h"
#include "hphp/runtime/ext/ext_asio.h"
#include "hphp/runtime/base/request-injection-data.h"
#include "hphp/runtime/base/thread-info.h"
#include "hphp/runtime/base/runtime-option.h"
#include <signal.h>
#include <vector>
#include <time.h>

#include <iostream>

namespace HPHP {

TRACE_SET_MOD(xenon);

void *s_waitThread(void *arg) {
  TRACE(1, "s_waitThread Starting\n");
  sem_t* sem = static_cast<sem_t*>(arg);
  while (sem_wait(sem) == 0) {
    TRACE(1, "s_waitThread Fired\n");
    if (Xenon::getInstance().m_stopping) {
      TRACE(1, "s_waitThread is exiting\n");
      return nullptr;
    }
    Xenon::getInstance().surpriseAll();
  }
  TRACE(1, "s_waitThread Ending\n");
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

// Data that is kept per request and is only valid per request.
// This structure gathers a php and async stack trace when log is called.
// These logged stacks can be then gathered via php a call, xenon_get_data.
// It needs to allocate and free its Array per request, because Array lifetime
// is per-request.  So the flow for these objects are:
// allocated when a web request begins (if Xenon is enabled)
// grab snapshots of the php and async stack when log is called
// detach itself from its snapshots when the request is ending.
struct XenonRequestLocalData : public RequestEventHandler  {
  XenonRequestLocalData();
  virtual ~XenonRequestLocalData();
  void log(bool skipFirst);
  Array logAsyncStack();
  Array createResponse();

  // virtual from RequestEventHandler
  virtual void requestInit();
  virtual void requestShutdown();

  // an array of (php, async) stacks
  Array m_stackSnapshots;
  // number of times we invoked, but the async stack was invalid
  int m_asyncInvalidCount;
};
IMPLEMENT_STATIC_REQUEST_LOCAL(XenonRequestLocalData, s_xenonData);

///////////////////////////////////////////////////////////////////////////////
// statics used by the Xenon classes

const StaticString
  s_class("class"),
  s_function("function"),
  s_file("file"),
  s_type("type"),
  s_line("line"),
  s_time("time"),
  s_asyncInvalidCount("asyncInvalidCount"),
  s_phpStack("phpStack"),
  s_asyncStack("asyncStack");

static Array parsePhpStack(const Array& bt) {
  Array stack;
  for (ArrayIter it(bt); it; ++it) {
    const Array& frame = it.second().toArray();
    if (frame.exists(s_function)) {
      if (frame.exists(s_class)) {
        std::ostringstream ss;
        ss << frame[s_class].toString().c_str()
           << frame[s_type].toString().c_str()
           << frame[s_function].toString().c_str();
        Array element;
        element.set(s_function, ss.str(), true);
        element.set(s_file, frame[s_file], true);
        element.set(s_line, frame[s_line], true);
        stack.append(element);
      } else {
        Array element;
        element.set(s_function, frame[s_function].toString().c_str(), true);
        if (frame.exists(s_file) && frame.exists(s_line)) {
          element.set(s_file, frame[s_file], true);
          element.set(s_line, frame[s_line], true);
        }
        stack.append(element);
      }
    }
  }
  return stack;
}

static c_WaitableWaitHandle *objToWaitableWaitHandle(Object o) {
  assert(o->instanceof(c_WaitableWaitHandle::classof()));
  return static_cast<c_WaitableWaitHandle*>(o.get());
}

///////////////////////////////////////////////////////////////////////////
// A singleton object that handles the two Xenon modes (always or timer).
// If in always on mode, the Xenon Surprise flags have to be on for each thread
// and are never cleared.
// For timer mode, when start is invoked, it adds a new timer to the existing
// handler for SIGVTALRM.

Xenon& Xenon::getInstance() {
  static Xenon instance;
  return instance;
}

Xenon::Xenon() : m_stopping(false), m_sec(10*60), m_nsec(0) {
#ifndef __APPLE__
  m_timerid = 0;
#endif
}

Xenon::~Xenon() {
}

// XenonForceAlwaysOn is active - it doesn't need a timer, it is always on.
// Xenon needs to be started once per process.
// The number of seconds has to be greater than zero.
// We need to create a semaphore and a thread.
// If all of those happen, then we need a timer attached to a signal handler.
void Xenon::start(double seconds) {
#ifndef __APPLE__
  TRACE(1, "XenonForceAlwaysOn %d\n", RuntimeOption::XenonForceAlwaysOn);
  if (!RuntimeOption::XenonForceAlwaysOn
      && m_timerid == 0
      && seconds > 0
      && sem_init(&m_timerTriggered, 0, 0) == 0
      && pthread_create(&m_triggerThread, nullptr, s_waitThread,
          static_cast<void*>(&m_timerTriggered)) == 0) {

    m_sec = (int)seconds;
    m_nsec = (int)((seconds - m_sec) * 1000000000);
    TRACE(1, "Xenon::start %ld seconds, %ld nanoseconds\n", m_sec, m_nsec);

    sigevent sev={};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGVTALRM;
    sev.sigev_value.sival_ptr = nullptr; // null for Xenon signals
    timer_create(CLOCK_REALTIME, &sev, &m_timerid);

    itimerspec ts={};
    ts.it_value.tv_sec = m_sec;
    ts.it_value.tv_nsec = m_nsec;
    ts.it_interval.tv_sec = m_sec;
    ts.it_interval.tv_nsec = m_nsec;
    timer_settime(m_timerid, 0, &ts, nullptr);
  }
#endif
}

// If Xenon owns a pthread, tell it to stop, also clean up anything from start.
void Xenon::stop() {
#ifndef __APPLE__
  if (m_timerid) {
    m_stopping = true;
    sem_post(&m_timerTriggered);
    pthread_join(m_triggerThread, nullptr);
    TRACE(1, "Xenon::stop has stopped the waiting thread\n");
    timer_delete(m_timerid);
    sem_destroy(&m_timerTriggered);
  }
#endif
}

// Xenon data is gathered for logging per request, "if we should"
// meaning that if Xenon's Surprise flag has been turned on by someone, we
// should log the stacks.  If we are in XenonForceAlwaysOn, do not clear
// the Surprise flag.  The data is gathered in thread local storage.
void Xenon::log(bool skipFirst) {
  RequestInjectionData *rid = &ThreadInfo::s_threadInfo->m_reqInjectionData;
  if (rid->checkXenonSignalFlag()) {
    if (!RuntimeOption::XenonForceAlwaysOn) {
      rid->clearXenonSignalFlag();
    }
    TRACE(1, "Xenon::log\n");
    s_xenonData->log(skipFirst);
  }
}

// Called from timer handler, Lets non-signal code know the timer was fired.
void Xenon::onTimer() {
  sem_post(&m_timerTriggered);
}

// Turns on the Xenon Surprise flag for every thread via a lambda function
// passed to ExecutePerThread.
void Xenon::surpriseAll() {
  TRACE(1, "Xenon::surpriseAll\n");
  ThreadInfo::ExecutePerThread(
    [](ThreadInfo *t) {t->m_reqInjectionData.setXenonSignalFlag();} );
}

///////////////////////////////////////////////////////////////////////////////
// There is one XenonRequestLocalData per thread, stored in thread local area

XenonRequestLocalData::XenonRequestLocalData() {
  TRACE(1, "XenonRequestLocalData\n");
}

XenonRequestLocalData::~XenonRequestLocalData() {
  TRACE(1, "~XenonRequestLocalData\n");
}

Array XenonRequestLocalData::logAsyncStack() {
  Array bt;
  auto session = AsioSession::Get();
  // we need this check here to see if the asio is in a valid state for queries
  // if it is not, then return
  // calling getCurrentWaitHandle directly while asio is not in a valid state
  // will assert, so we need to check this ourselves before invoking it
  if (session->isInContext() && !session->getCurrentContext()->isRunning()) {
    ++m_asyncInvalidCount;
    return bt;
  }

  auto currentWaitHandle = session->getCurrentWaitHandle();
  if (currentWaitHandle == nullptr) {
    // if we have a nullptr, then we have no async stack to store for this log
    return bt;
  }
  Array depStack = currentWaitHandle->t_getdependencystack();

  for (ArrayIter iter(depStack); iter; ++iter) {
    Array frameData;
    if (iter.secondRef().isNull()) {
      frameData.set(s_function, "<prep>", true);
    } else {
      auto wh = objToWaitableWaitHandle(iter.secondRef().toObject());
      frameData.set(s_function, wh->t_getname(), true);
      // Continuation wait handles may have a source location to add.
      auto contWh = dynamic_cast<c_AsyncFunctionWaitHandle*>(wh);
      if (contWh != nullptr  && !contWh->isRunning()) {
        frameData.set(s_file, contWh->getFileName(), true);
        frameData.set(s_line, contWh->getLineNumber(), true);
      }
    }
    bt.append(frameData);
  }
  return bt;
}

// Creates an array to respond to the Xenon PHP extension;
// builds the data into the format neeeded.
Array XenonRequestLocalData::createResponse() {
  Array stacks;
  for (ArrayIter it(m_stackSnapshots); it; ++it) {
    Array frame = it.second().toArray();
    Array element;
    element.set(s_time, frame[s_time], true);
    element.set(s_phpStack, parsePhpStack(frame[s_phpStack].toArray()), true);
    element.set(s_asyncStack, frame[s_asyncStack], true);
    stacks.append(element);
  }
  stacks.set(s_asyncInvalidCount, m_asyncInvalidCount, true);
  return stacks;
}

void XenonRequestLocalData::log(bool skipFirst) {
  TRACE(1, "XenonRequestLocalData::log\n");
  time_t now = time(nullptr);
  Array snapshot;
  snapshot.set(s_time, now, true);
  snapshot.set(s_phpStack, g_context->debugBacktrace(skipFirst, true, false,
    nullptr, true), true);
  snapshot.set(s_asyncStack, logAsyncStack(), true);
  m_stackSnapshots.append(snapshot);
}

void XenonRequestLocalData::requestInit() {
  TRACE(1, "XenonRequestLocalData::requestInit\n");
  m_asyncInvalidCount = 0;
  m_stackSnapshots = Array::Create();
  if (RuntimeOption::XenonForceAlwaysOn) {
    ThreadInfo::s_threadInfo->m_reqInjectionData.setXenonSignalFlag();
  } else {
    // clear any Xenon flags that might still be on in this thread so
    // that we do not have a bias towards the first function
    ThreadInfo::s_threadInfo->m_reqInjectionData.clearXenonSignalFlag();
  }
}

void XenonRequestLocalData::requestShutdown() {
  TRACE(1, "XenonRequestLocalData::requestShutdown\n");
  ThreadInfo::s_threadInfo->m_reqInjectionData.clearXenonSignalFlag();
  m_stackSnapshots.detach();
}

///////////////////////////////////////////////////////////////////////////////
// Function that allows php code to access request local data that has been
// gathered via surprise flags.

static Array HHVM_FUNCTION(xenon_get_data, void) {
  if (RuntimeOption::XenonForceAlwaysOn ||
      RuntimeOption::XenonPeriodSeconds > 0) {
    TRACE(1, "xenon_get_data\n");
    return s_xenonData->createResponse();
  }
  return Array::Create();
}

class xenonExtension : public Extension {
 public:
  xenonExtension() : Extension("xenon", "1.0") { }

  void moduleInit() override {
    HHVM_FALIAS(HH\\xenon_get_data, xenon_get_data);
    loadSystemlib();
  }
} s_xenon_extension;

///////////////////////////////////////////////////////////////////////////////
}

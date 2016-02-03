/* Copyright 2016 Stanford University, NVIDIA Corporation
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

#include "event_impl.h"

#include "proc_impl.h"
#include "runtime_impl.h"
#include "logging.h"
#include "threads.h"

namespace Realm {

  Logger log_event("event");
  Logger log_barrier("barrier");
  Logger log_poison("poison");

  // used in places that don't currently propagate poison but should
  static const bool POISON_FIXME = false;

  ////////////////////////////////////////////////////////////////////////
  //
  // class DeferredEventTrigger
  //

  class DeferredEventTrigger : public EventWaiter {
  public:
    DeferredEventTrigger(Event _after_event);

    virtual ~DeferredEventTrigger(void);
    
    virtual bool event_triggered(Event e, bool poisoned);

    virtual void print(std::ostream& os) const;

  protected:
    Event after_event;
  };
  
  DeferredEventTrigger::DeferredEventTrigger(Event _after_event)
    : after_event(_after_event)
  {}

  DeferredEventTrigger::~DeferredEventTrigger(void) { }

  bool DeferredEventTrigger::event_triggered(Event e, bool poisoned)
  {
    if(poisoned) {
      log_poison.info() << "poisoned deferred event: event=" << after_event;
      GenEventImpl::trigger(after_event, true /*poisoned*/);
      return true;
    }
    
    log_event.info() << "deferred trigger occuring: " << after_event;
    GenEventImpl::trigger(after_event, false /*!poisoned*/);
    return true;
  }

  void DeferredEventTrigger::print(std::ostream& os) const
  {
    os << "deferred trigger: after=" << after_event;
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class Event
  //

  /*static*/ const Event Event::NO_EVENT = { 0, 0 };
  // Take this you POS c++ type system
  /* static */ const UserEvent UserEvent::NO_USER_EVENT = 
    *(static_cast<UserEvent*>(const_cast<Event*>(&Event::NO_EVENT)));

  bool Event::has_triggered(void) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    if(!id) return true; // special case: NO_EVENT has always triggered
    EventImpl *e = get_runtime()->get_event_impl(*this);
    bool poisoned = false;
    if(e->has_triggered(gen, poisoned)) {
      // a poisoned event causes an exception because the caller isn't prepared for it
      assert(poisoned == POISON_FIXME);
      return true;
    } else
      return false;
  }

  bool Event::has_triggered_faultaware(bool& poisoned) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    if(!id) return true; // special case: NO_EVENT has always triggered
    EventImpl *e = get_runtime()->get_event_impl(*this);
    return e->has_triggered(gen, poisoned);
  }

  // creates an event that won't trigger until all input events have
  /*static*/ Event Event::merge_events(const std::set<Event>& wait_for)
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    return GenEventImpl::merge_events(wait_for, false /*!ignore faults*/);
  }

  /*static*/ Event Event::merge_events(Event ev1, Event ev2,
				       Event ev3 /*= NO_EVENT*/, Event ev4 /*= NO_EVENT*/,
				       Event ev5 /*= NO_EVENT*/, Event ev6 /*= NO_EVENT*/)
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    return GenEventImpl::merge_events(ev1, ev2, ev3, ev4, ev5, ev6);
  }

  /*static*/ Event Event::merge_events_ignorefaults(const std::set<Event>& wait_for)
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    return GenEventImpl::merge_events(wait_for, true /*ignore faults*/);
  }

  class EventTriggeredCondition {
  public:
    EventTriggeredCondition(EventImpl* _event, Event::gen_t _gen);

    class Callback : public EventWaiter {
    public:
      virtual ~Callback(void);
      virtual bool event_triggered(Event e, bool poisoned);
      virtual void print(std::ostream& os) const;
      virtual void operator()(bool poisoned) = 0;
    };

    void add_callback(Callback& cb) const;

  protected:
    EventImpl *event;
    Event::gen_t gen;
  };

  EventTriggeredCondition::EventTriggeredCondition(EventImpl* _event, Event::gen_t _gen)
    : event(_event), gen(_gen)
  {}

  void EventTriggeredCondition::add_callback(Callback& cb) const
  {
    event->add_waiter(gen, &cb);
  }

  EventTriggeredCondition::Callback::~Callback(void)
  {
  }
  
  bool EventTriggeredCondition::Callback::event_triggered(Event e, bool poisoned)
  {
    (*this)(poisoned);
    // we don't manage the memory any more
    return false;
  }

  void EventTriggeredCondition::Callback::print(std::ostream& os) const
  {
    os << "EventTriggeredCondition (thread unknown)";
  }  

  void Event::wait(void) const
  {
    bool poisoned = false;
    wait_faultaware(poisoned);
    // a poisoned event causes an exception because the caller isn't prepared for it
    assert(poisoned == POISON_FIXME);
  }

  void Event::wait_faultaware(bool& poisoned) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    if(!id) return;  // special case: never wait for NO_EVENT
    EventImpl *e = get_runtime()->get_event_impl(*this);

    // early out case too
    if(e->has_triggered(gen, poisoned)) return;

    // waiting on an event does not count against the low level's time
    DetailedTimer::ScopedPush sp2(TIME_NONE);

    Thread *thread = Thread::self();
    if(thread) {
      log_event.info() << "thread blocked: thread=" << thread << " event=" << *this;
      // describe the condition we want the thread to wait on
      thread->wait_for_condition(EventTriggeredCondition(e, gen), poisoned);
      log_event.info() << "thread resumed: thread=" << thread << " event=" << *this << " poisoned=" << poisoned;
      return;
    }

    assert(0); // if we're not a Thread, we have a problem
    return;
    //assert(ptr != 0);
  }

  void Event::external_wait(void) const
  {
    bool poisoned = false;
    external_wait_faultaware(poisoned);
    assert(poisoned == POISON_FIXME);
  }

  void Event::external_wait_faultaware(bool& poisoned) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    if(!id) return;  // special case: never wait for NO_EVENT
    EventImpl *e = get_runtime()->get_event_impl(*this);

    // early out case too
    if(e->has_triggered(gen, poisoned)) return;
    
    // waiting on an event does not count against the low level's time
    DetailedTimer::ScopedPush sp2(TIME_NONE);
    
    log_event.info() << "external thread blocked: event=" << *this;
    e->external_wait(gen, poisoned);
    log_event.info() << "external thread resumed: event=" << *this;
  }

  void Event::cancel_operation(const void *reason_data, size_t reason_len) const
  {
    assert(0 && "TODO: faults");
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class UserEvent
  //

  /*static*/ UserEvent UserEvent::create_user_event(void)
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
    Event e = GenEventImpl::create_genevent()->current_event();
    assert(e.id != 0);
    UserEvent u;
    u.id = e.id;
    u.gen = e.gen;
    log_event.info() << "user event created: event=" << e;
    return u;
  }

  void UserEvent::trigger(Event wait_on) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);

#ifdef EVENT_GRAPH_TRACE
    Event enclosing = find_enclosing_termination_event();
    log_event_graph.info("Event Trigger: (" IDFMT ",%d) (" IDFMT 
			 ",%d) (" IDFMT ",%d)",
			 id, gen, wait_on.id, wait_on.gen,
			 enclosing.id, enclosing.gen);
#endif

    if(!wait_on.has_triggered()) {
      // deferred trigger
      log_event.info() << "deferring user event trigger: event=" << *this << " wait_on=" << wait_on;
      EventImpl::add_waiter(wait_on, new DeferredEventTrigger(*this));
      return;
    }

    log_event.info() << "user event trigger: event=" << *this << " wait_on=" << wait_on;
    GenEventImpl::trigger(*this, false /*!poisoned*/);
  }

  void UserEvent::cancel(void) const
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);

#ifdef EVENT_GRAPH_TRACE
    // TODO: record cancellation?
    Event enclosing = find_enclosing_termination_event();
    log_event_graph.info("Event Trigger: (" IDFMT ",%d) (" IDFMT 
			 ",%d) (" IDFMT ",%d)",
			 id, gen, wait_on.id, wait_on.gen,
			 enclosing.id, enclosing.gen);
#endif

    log_event.info() << "user event cancelled: event=" << *this;
    GenEventImpl::trigger(*this, true /*poisoned*/);
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class Barrier
  //

  /*static*/ Barrier Barrier::create_barrier(unsigned expected_arrivals,
					     ReductionOpID redop_id /*= 0*/,
					     const void *initial_value /*= 0*/,
					     size_t initial_value_size /*= 0*/)
  {
    DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);

    BarrierImpl *impl = BarrierImpl::create_barrier(expected_arrivals, redop_id, initial_value, initial_value_size);
    Barrier b = impl->current_barrier();

#ifdef EVENT_GRAPH_TRACE
    log_event_graph.info("Barrier Creation: " IDFMT " %d", b.id, expected_arrivals);
#endif

    return b;
  }

  void Barrier::destroy_barrier(void)
  {
    log_barrier.info("barrier destruction request: " IDFMT "/%d", id, gen);
  }

  Barrier Barrier::advance_barrier(void) const
  {
    Barrier nextgen;
    nextgen.id = id;
    nextgen.gen = gen + 1;
    nextgen.timestamp = 0;

    return nextgen;
  }

  Barrier Barrier::alter_arrival_count(int delta) const
  {
    timestamp_t timestamp = __sync_fetch_and_add(&BarrierImpl::barrier_adjustment_timestamp, 1);
#ifdef EVENT_GRAPH_TRACE
    Event enclosing = find_enclosing_termination_event();
    log_event_graph.info("Barrier Alter: (" IDFMT ",%d) (" IDFMT
			 ",%d) %d", id, gen, enclosing.id, enclosing.gen, delta);
#endif
    BarrierImpl *impl = get_runtime()->get_barrier_impl(*this);
    impl->adjust_arrival(gen, delta, timestamp, Event::NO_EVENT, 0, 0);

    Barrier with_ts;
    with_ts.id = id;
    with_ts.gen = gen;
    with_ts.timestamp = timestamp;

    return with_ts;
  }

  Barrier Barrier::get_previous_phase(void) const
  {
    Barrier result = *this;
    result.gen--;
    return result;
  }

  void Barrier::arrive(unsigned count /*= 1*/, Event wait_on /*= Event::NO_EVENT*/,
		       const void *reduce_value /*= 0*/, size_t reduce_value_size /*= 0*/) const
  {
#ifdef EVENT_GRAPH_TRACE
    Event enclosing = find_enclosing_termination_event();
    log_event_graph.info("Barrier Arrive: (" IDFMT ",%d) (" IDFMT
			 ",%d) (" IDFMT ",%d) %d",
			 id, gen, wait_on.id, wait_on.gen,
			 enclosing.id, enclosing.gen, count);
#endif
    // arrival uses the timestamp stored in this barrier object
    BarrierImpl *impl = get_runtime()->get_barrier_impl(*this);
    impl->adjust_arrival(gen, -count, timestamp, wait_on,
			 reduce_value, reduce_value_size);
  }

  bool Barrier::get_result(void *value, size_t value_size) const
  {
    BarrierImpl *impl = get_runtime()->get_barrier_impl(*this);
    return impl->get_result(gen, value, value_size);
  }


  ////////////////////////////////////////////////////////////////////////
  //
  // class GenEventImpl
  //

  GenEventImpl::GenEventImpl(void)
    : me((ID::IDType)-1), owner(-1)
  {
    generation = 0;
    gen_subscribed = 0;
    next_free = 0;
    num_poisoned_generations = 0;
    poisoned_generations = 0;
    has_local_triggers = false;
  }

  void GenEventImpl::init(ID _me, unsigned _init_owner)
  {
    me = _me;
    owner = _init_owner;
    generation = 0;
    gen_subscribed = 0;
    next_free = 0;
    num_poisoned_generations = 0;
    poisoned_generations = 0;
    has_local_triggers = false;
  }


    // Perform our merging events in a lock free way
    class EventMerger : public EventWaiter {
    public:
      EventMerger(Event _finish_event, bool _ignore_faults)
	: finish_event(_finish_event)
	, ignore_faults(_ignore_faults)
	, count_needed(1)
	, faults_observed(0)
      {
      }

      virtual ~EventMerger(void)
      {
      }

      void add_event(Event wait_for)
      {
	bool poisoned = false;
	if(wait_for.has_triggered_faultaware(poisoned)) {
	  // always count faults, but don't necessarily propagage
	  bool first_fault = (__sync_fetch_and_add(&faults_observed, 1) == 0);
	  if(first_fault && !ignore_faults) {
	    log_poison.info() << "event merger early poison: after=" << finish_event;
	    GenEventImpl::trigger(finish_event, true /*poisoned*/);
	  }
	  // either way we return to the caller without updating the count_needed
	  return;
	}

        // Increment the count and then add ourselves
        __sync_fetch_and_add(&count_needed, 1);
	// step 2: enqueue ourselves on the input event
	EventImpl::add_waiter(wait_for, this);
      }

      // arms the merged event once you're done adding input events - just
      //  decrements the count for the implicit 'init done' event
      // return a boolean saying whether it triggered upon arming (which
      //  means the caller should delete this EventMerger)
      bool arm(void)
      {
	bool nuke = event_triggered(Event::NO_EVENT, false /*!poisoned*/);
        return nuke;
      }

      virtual bool event_triggered(Event triggered, bool poisoned)
      {
	// if the input is poisoned, we propagate that poison eagerly
	if(poisoned) {
	  bool first_fault = (__sync_fetch_and_add(&faults_observed, 1) == 0);
	  if(first_fault && !ignore_faults) {
	    log_poison.info() << "event merger poisoned: after=" << finish_event;
	    GenEventImpl::trigger(finish_event, true /*poisoned*/);
	  }
	}

	int count_left = __sync_fetch_and_add(&count_needed, -1);

        // Put the logging first to avoid segfaults
	log_event.debug() << "received trigger merged event=" << finish_event << " left=" << count_left << " poisoned=" << poisoned;

	// count is the value before the decrement, so it was 1, it's now 0
	bool last_trigger = (count_left == 1);

	// trigger on the last input event, unless we did an early poison propagation
	if(last_trigger && (ignore_faults || (faults_observed == 0))) {
	  GenEventImpl::trigger(finish_event, false /*!poisoned*/);
	}

        // caller can delete us if this was the last trigger
        return last_trigger;
      }

      virtual void print(std::ostream& os) const
      {
	os << "event merger: " << finish_event << " left=" << count_needed;
      }

    protected:
      Event finish_event;
      bool ignore_faults;
      int count_needed;
      int faults_observed;
    };

    // creates an event that won't trigger until all input events have
    /*static*/ Event GenEventImpl::merge_events(const std::set<Event>& wait_for,
						bool ignore_faults)
    {
      if (wait_for.empty())
        return Event::NO_EVENT;
      // scan through events to see how many exist/haven't fired - we're
      //  interested in counts of 0, 1, or 2+ - also remember the first
      //  event we saw for the count==1 case
      int wait_count = 0;
      Event first_wait;
      for(std::set<Event>::const_iterator it = wait_for.begin();
	  (it != wait_for.end()) && (wait_count < 2);
	  it++) {
	bool poisoned = false;
	if((*it).has_triggered_faultaware(poisoned)) {
	  // if we're not ignoring faults, we need to propagate this fault, and can do
	  //  so by just returning this poisoned event
	  if(!ignore_faults) {
	    log_poison.info() << "merging events - " << (*it) << " already poisoned";
	    return *it;
	  }
	} else {
	  if(!wait_count) first_wait = *it;
	  wait_count++;
	}
      }
      log_event.debug() << "merging events - at least " << wait_count << " not triggered";

      // Avoid these optimizations if we are doing event graph tracing
      // we also cannot return an input event directly in the (wait_count == 1) case
      //  if we're ignoring faults
#ifndef EVENT_GRAPH_TRACE
      // counts of 0 or 1 don't require any merging
      if(wait_count == 0) return Event::NO_EVENT;
      if((wait_count == 1) && !ignore_faults) return first_wait;
#else
      if((wait_for.size() == 1) && !ignore_faults)
        return *(wait_for.begin());
#endif
      // counts of 2+ require building a new event and a merger to trigger it
      Event finish_event = GenEventImpl::create_genevent()->current_event();
      EventMerger *m = new EventMerger(finish_event, ignore_faults);

#ifdef EVENT_GRAPH_TRACE
      log_event_graph.info("Event Merge: (" IDFMT ",%d) %ld", 
			   finish_event.id, finish_event.gen, wait_for.size());
#endif

      for(std::set<Event>::const_iterator it = wait_for.begin();
	  it != wait_for.end();
	  it++) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << *it;
	m->add_event(*it);
#ifdef EVENT_GRAPH_TRACE
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ",%d)",
                             finish_event.id, finish_event.gen,
                             it->id, it->gen);
#endif
      }

      // once they're all added - arm the thing (it might go off immediately)
      if(m->arm())
        delete m;

      return finish_event;
    }

    /*static*/ Event GenEventImpl::merge_events(Event ev1, Event ev2,
						Event ev3 /*= NO_EVENT*/, Event ev4 /*= NO_EVENT*/,
						Event ev5 /*= NO_EVENT*/, Event ev6 /*= NO_EVENT*/)
    {
      // scan through events to see how many exist/haven't fired - we're
      //  interested in counts of 0, 1, or 2+ - also remember the first
      //  event we saw for the count==1 case
      // any poison on input events is immediately propagated (by simply returning
      //  the poisoned input event)
      int wait_count = 0;
      Event first_wait;
#define CHECK_EVENT(ev) \
      do {		       \
	bool poisoned = false; \
	if((ev).has_triggered_faultaware(poisoned)) { \
	  if(poisoned) \
	    return(ev); \
	} else { \
	  first_wait = (ev); \
	  wait_count++; \
	} \
      } while(0)
      CHECK_EVENT(ev6);
      CHECK_EVENT(ev5);
      CHECK_EVENT(ev4);
      CHECK_EVENT(ev3);
      CHECK_EVENT(ev2);
      CHECK_EVENT(ev1);

      log_event.debug() << "merging events - at least " << wait_count << " not triggered";

      // Avoid these optimizations if we are doing event graph tracing
#ifndef EVENT_GRAPH_TRACE
      // counts of 0 or 1 don't require any merging
      if(wait_count == 0) return Event::NO_EVENT;
      if(wait_count == 1) return first_wait;
#else
      int existential_count = 0;
      if (ev1.exists()) existential_count++;
      if (ev2.exists()) existential_count++;
      if (ev3.exists()) existential_count++;
      if (ev4.exists()) existential_count++;
      if (ev5.exists()) existential_count++;
      if (ev6.exists()) existential_count++;
      if (existential_count == 0)
        return Event::NO_EVENT;
      if (existential_count == 1)
      {
        if (ev1.exists()) return ev1;
        if (ev2.exists()) return ev2;
        if (ev3.exists()) return ev3;
        if (ev4.exists()) return ev4;
        if (ev5.exists()) return ev5;
        if (ev6.exists()) return ev6;
      }
#endif

      // counts of 2+ require building a new event and a merger to trigger it
      Event finish_event = GenEventImpl::create_genevent()->current_event();
      EventMerger *m = new EventMerger(finish_event, false /*!ignore faults*/);

      if(ev1.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev1;
	m->add_event(ev1);
      }
      if(ev2.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev2;
	m->add_event(ev2);
      }
      if(ev3.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev3;
	m->add_event(ev3);
      }
      if(ev4.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev4;
	m->add_event(ev4);
      }
      if(ev5.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev5;
	m->add_event(ev5);
      }
      if(ev6.exists()) {
	log_event.info() << "event merging: event=" << finish_event << " wait_on=" << ev6;
	m->add_event(ev6);
      }

#ifdef EVENT_GRAPH_TRACE
      log_event_graph.info("Event Merge: (" IDFMT ",%d) %d",
               finish_event->me.id(), finish_event->generation, existential_count);
      if (ev1.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev1.id, ev1.gen);
      if (ev2.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev2.id, ev2.gen);
      if (ev3.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev3.id, ev3.gen);
      if (ev4.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev4.id, ev4.gen);
      if (ev5.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev5.id, ev5.gen);
      if (ev6.exists())
        log_event_graph.info("Event Precondition: (" IDFMT ",%d) (" IDFMT ", %d)",
            finish_event->me.id(), finish_event->generation, ev6.id, ev6.gen);
#endif

      // once they're all added - arm the thing (it might go off immediately)
      if(m->arm())
        delete m;

      return finish_event;
    }

    /*static*/ GenEventImpl *GenEventImpl::create_genevent(void)
    {
      GenEventImpl *impl = get_runtime()->local_event_free_list->alloc_entry();
      assert(impl);
      assert(ID(impl->me).type() == ID::ID_EVENT);

      log_event.spew() << "event created: event=" << impl->current_event();

#ifdef EVENT_TRACING
      {
	EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
	item.event_id = impl->me.id();
	item.event_gen = impl->me.gen;
	item.action = EventTraceItem::ACT_CREATE;
      }
#endif
      return impl;
    }

    bool GenEventImpl::add_waiter(Event::gen_t needed_gen, EventWaiter *waiter)
    {
#ifdef EVENT_TRACING
      {
        EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
        item.event_id = me->id;
        item.event_gen = needed_gen;
        item.action = EventTraceItem::ACT_WAIT;
      }
#endif
      // no early check here as the caller will generally have tried has_triggered()
      //  before allocating its EventWaiter object

      bool trigger_now = false;
      bool trigger_poisoned = false;

      int subscribe_owner = -1;
      Event::gen_t previous_subscribe_gen = 0;
      {
	AutoHSLLock a(mutex);

	// three cases below

	if(needed_gen <= generation) {
	  // 1) the event has triggered and any poison information is in the poisoned generation list
	  trigger_now = true; // actually do trigger outside of mutex
	  trigger_poisoned = is_generation_poisoned(needed_gen);
	} else {
	  std::map<Event::gen_t, bool>::const_iterator it = local_triggers.find(needed_gen);
	  if(it != local_triggers.end()) {
	    // 2) we're not the owner node, but we've locally triggered this and have correct poison info
	    assert(owner != gasnet_mynode());
	    trigger_now = true;
	    trigger_poisoned = it->second;
	  } else {
	    // 3) we don't know of a trigger of this event, so record the waiter and subscribe if needed

	    log_event.debug("event not ready: event=" IDFMT "/%d owner=%d gen=%d subscr=%d",
			    me.id(), needed_gen, owner, generation, gen_subscribed);

	    // is this for the "current" next generation?
	    if(needed_gen == (generation + 1)) {
	      // yes, put in the current waiter list
	      current_local_waiters.push_back(waiter);
	    } else {
	      // no, put it in an appropriate future waiter list - only allowed for non-owners
	      assert(owner != gasnet_mynode());
	      future_local_waiters[needed_gen].push_back(waiter);
	    }

	    // do we need to subscribe to this event?
	    if((owner != gasnet_mynode()) && (gen_subscribed < needed_gen)) {
	      previous_subscribe_gen = gen_subscribed;
	      gen_subscribed = needed_gen;
	      subscribe_owner = owner;
	    }
	  }
	}
      }

      if((subscribe_owner != -1))
	EventSubscribeMessage::send_request(owner,
					    make_event(needed_gen),
					    previous_subscribe_gen);

      if(trigger_now) {
	bool nuke = waiter->event_triggered(make_event(needed_gen),
					    trigger_poisoned);
        if(nuke)
          delete waiter;
      }

      return true;  // waiter is always either enqueued or triggered right now
    }

    inline bool GenEventImpl::is_generation_poisoned(Event::gen_t gen) const
    {
      // common case: no poisoned generations
      if(__builtin_expect((num_poisoned_generations == 0), 1))
	return false;
      
      for(int i = 0; i < num_poisoned_generations; i++)
	if(poisoned_generations[i] == gen)
	  return true;
      return false;
    }


    ///////////////////////////////////////////////////
    // Events


  template <typename T>
  struct MediumBroadcastHelper : public T::RequestArgs {
    inline void apply(gasnet_node_t target)
    {
      T::Message::request(target, *this, payload, payload_size, payload_mode);
    }

    void broadcast(const NodeSet& targets,
		   const void *_payload, size_t _payload_size,
		   int _payload_mode)
    {
      payload = _payload;
      payload_size = _payload_size;
      payload_mode = _payload_mode;
      assert((payload_mode != PAYLOAD_FREE) && "cannot use PAYLOAD_FREE with broadcast!");
      targets.map(*this);
    }

    const void *payload;
    size_t payload_size;
    int payload_mode;
  };
  

  /*static*/ void EventTriggerMessage::send_request(gasnet_node_t target, Event event,
						    bool poisoned)
  {
    RequestArgs args;

    args.node = gasnet_mynode();
    args.event = event;
    args.poisoned = poisoned;

    Message::request(target, args);
  }

  /*static*/ void EventUpdateMessage::send_request(gasnet_node_t target, Event event,
						   int num_poisoned,
						   const Event::gen_t *poisoned_generations)
  {
    RequestArgs args;

    args.event = event;

    Message::request(target, args,
		     poisoned_generations, num_poisoned * sizeof(Event::gen_t),
		     PAYLOAD_KEEP);
  }

  /*static*/ void EventUpdateMessage::broadcast_request(const NodeSet& targets, Event event,
							int num_poisoned,
							const Event::gen_t *poisoned_generations)
  {
    MediumBroadcastHelper<EventUpdateMessage> args;

    args.event = event;

    args.broadcast(targets,
		   poisoned_generations, num_poisoned * sizeof(Event::gen_t),
		   PAYLOAD_KEEP);
  }

  /*static*/ void EventSubscribeMessage::send_request(gasnet_node_t target, Event event, Event::gen_t previous_gen)
  {
    RequestArgs args;

    args.node = gasnet_mynode();
    args.event = event;
    args.previous_subscribe_gen = previous_gen;
    Message::request(target, args);
  }

    // only called for generational events
    /*static*/ void EventSubscribeMessage::handle_request(EventSubscribeMessage::RequestArgs args)
    {
      log_event.debug("event subscription: node=%d event=" IDFMT "/%d",
		args.node, args.event.id, args.event.gen);

      GenEventImpl *impl = get_runtime()->get_genevent_impl(args.event);

#ifdef EVENT_TRACING
      {
        EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
        item.event_id = args.event.id; 
        item.event_gen = args.event.gen;
        item.action = EventTraceItem::ACT_WAIT;
      }
#endif

      // we may send a trigger message in response to the subscription
      Event::gen_t trigger_gen = 0;
      bool subscription_recorded = false;

      // early-out case: if we can see the generation needed has already
      //  triggered, signal without taking the mutex
      Event::gen_t stale_gen = impl->generation;
      if(stale_gen >= args.event.gen) {
	trigger_gen = stale_gen;
      } else {
	AutoHSLLock a(impl->mutex);

	// look at the previously-subscribed generation from the requestor - we'll send
	//  a trigger message if anything newer has triggered
        if(impl->generation > args.previous_subscribe_gen)
	  trigger_gen = impl->generation;

	// are they subscribing to the current generation?
	if(args.event.gen == (impl->generation + 1)) {
	  impl->remote_waiters.add(args.node);
	  subscription_recorded = true;
	} else {
	  // should never get subscriptions newer than our current
	  assert(args.event.gen <= impl->generation);
	}
      }

      if(subscription_recorded)
	log_event.debug() << "event subscription recorded: node=" << args.node
			  << " event=" << args.event << " (> " << impl->generation << ")";

      if(trigger_gen > 0) {
	log_event.debug() << "event subscription immediate trigger: node=" << args.node
			  << " event=" << args.event << " (<= " << trigger_gen << ")";
	Event triggered = args.event;
	triggered.gen = trigger_gen;
	// it is legal to use poisoned generation info like this because it is always
	// updated before the generation - the barrier makes sure we read in the correct
	// order
	__sync_synchronize();
	EventUpdateMessage::send_request(args.node,
					 triggered,
					 impl->num_poisoned_generations,
					 impl->poisoned_generations);
      }
    } 

    /*static*/ void EventTriggerMessage::handle_request(EventTriggerMessage::RequestArgs args)
    {
      DetailedTimer::ScopedPush sp(TIME_LOW_LEVEL);
      log_event.debug("Remote trigger of event " IDFMT "/%d from node %d!",
		args.event.id, args.event.gen, args.node);
      GenEventImpl *impl = get_runtime()->get_genevent_impl(args.event);
      impl->trigger(args.event.gen, args.node, args.poisoned);
    }

  template <typename T>
  struct ArrayOstreamHelper {
    ArrayOstreamHelper(const T *_base, size_t _count)
      : base(_base), count(_count)
    {}

    const T *base;
    size_t count;
  };

  template <typename T>
  std::ostream& operator<<(std::ostream& os, const ArrayOstreamHelper<T>& h)
  {
    switch(h.count) {
    case 0: return os << "0:{}";
    case 1: return os << "1:{ " << h.base[0] << " }";
    default:
      os << h.count << ":{ " << h.base[0];
      for(size_t i = 1; i < h.count; i++)
	os << ", " << h.base[i];
      os << " }";
      return os;
    }
  }


  void GenEventImpl::process_update(Event::gen_t current_gen,
				    const Event::gen_t *new_poisoned_generations,
				    int new_poisoned_count)
  {
    // this event had better not belong to us...
    assert(owner != gasnet_mynode());

    // the result of the update may trigger multiple generations worth of waiters - keep their
    //  generation IDs straight (we'll look up the poison bits later)
    std::map<Event::gen_t, std::vector<EventWaiter *> > to_wake;

    {
      AutoHSLLock a(mutex);

#define CHECK_POISONED_GENS
#ifdef CHECK_POISONED_GENS
      if(new_poisoned_count > 0) {
	// should be an incremental update
	assert(num_poisoned_generations <= new_poisoned_count);
	if(num_poisoned_generations > 0)
	  assert(memcmp(poisoned_generations, new_poisoned_generations, 
			new_poisoned_count * sizeof(Event::gen_t)) == 0);
      } else {
	// we shouldn't have any local ones either
	assert(num_poisoned_generations == 0);
      }
#endif

      // this might be old news if we had subscribed to an event and then triggered it ourselves
      if(current_gen <= generation)
	return;

      // first thing - update the poisoned generation list
      if(new_poisoned_count > 0) {
	if(!poisoned_generations)
	  poisoned_generations = new Event::gen_t[POISONED_GENERATION_LIMIT];
	if(num_poisoned_generations < new_poisoned_count) {
	  assert(new_poisoned_count <= POISONED_GENERATION_LIMIT);
	  num_poisoned_generations = new_poisoned_count;
	  memcpy(poisoned_generations, new_poisoned_generations,
		 new_poisoned_count * sizeof(Event::gen_t));
	}
      }

      // grab any/all waiters - start with current generation
      if(!current_local_waiters.empty())
	to_wake[generation + 1].swap(current_local_waiters);

      // now any future waiters up to and including the triggered gen
      if(!future_local_waiters.empty()) {
	std::map<Event::gen_t, std::vector<EventWaiter *> >::iterator it = future_local_waiters.begin();
	while((it != future_local_waiters.end()) && (it->first <= current_gen)) {
	  to_wake[it->first].swap(it->second);
	  future_local_waiters.erase(it);
	  it = future_local_waiters.begin();
	}

	// and see if there's a future list that's now current
	if((it != future_local_waiters.end()) && (it->first == (current_gen + 1))) {
	  current_local_waiters.swap(it->second);
	  future_local_waiters.erase(it);
	}
      }

      // next, clear out any local triggers that have been ack'd
      if(has_local_triggers) {
	std::map<Event::gen_t, bool>::iterator it = local_triggers.begin();
	while((it != local_triggers.end()) && (it->first <= current_gen)) {
	  assert(it->second == is_generation_poisoned(it->first));
	  local_triggers.erase(it);
	  it = local_triggers.begin();
	}
	has_local_triggers = !local_triggers.empty();
      }

      // finally, update the generation count, representing that we have complete information to that point
      __sync_synchronize();
      generation = current_gen;
    }

    // now trigger anybody that needs to be triggered
    if(!to_wake.empty()) {
      for(std::map<Event::gen_t, std::vector<EventWaiter *> >::const_iterator it = to_wake.begin();
	  it != to_wake.end();
	  it++) {
	Event e = make_event(it->first);
	bool poisoned = is_generation_poisoned(it->first);
	for(std::vector<EventWaiter *>::const_iterator it2 = it->second.begin();
	    it2 != it->second.end();
	    it2++) {
	  bool nuke = (*it2)->event_triggered(e, poisoned);
	  if(nuke) {
	    //printf("deleting: "); (*it)->print_info(); fflush(stdout);
	    delete (*it2);
	  }
	}
      }
    }
  }

    /*static*/ void EventUpdateMessage::handle_request(EventUpdateMessage::RequestArgs args,
						       const void *data, size_t datalen)
    {
      const Event::gen_t *new_poisoned_gens = (const Event::gen_t *)data;
      int new_poisoned_count = datalen / sizeof(Event::gen_t);
      assert((new_poisoned_count * sizeof(Event::gen_t)) == datalen);  // no remainders or overflow please

      log_event.debug() << "event update: event=" << args.event
			<< " poisoned=" << ArrayOstreamHelper<Event::gen_t>(new_poisoned_gens, new_poisoned_count);

      GenEventImpl *impl = get_runtime()->get_genevent_impl(args.event);
      impl->process_update(args.event.gen, new_poisoned_gens, new_poisoned_count);
    }


  /*static*/ Barrier::timestamp_t BarrierImpl::barrier_adjustment_timestamp;



    bool GenEventImpl::has_triggered(Event::gen_t needed_gen, bool& poisoned)
    {
#ifdef EVENT_TRACING
      {
        EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
        item.event_id = me.id;
        item.event_gen = needed_gen;
        item.action = EventTraceItem::ACT_QUERY;
      }
#endif
      // lock-free check
      if(needed_gen <= generation) {
	// it is safe to call is_generation_poisoned after just a memory barrier - no lock
	__sync_synchronize();
	poisoned = is_generation_poisoned(needed_gen);
	return true;
      }

      // if the above check fails, we have to see if we have performed any local triggers -
      // if not, we can internally-consistently say that the event hasn't triggered from our
      // perspective yet
      if(!has_local_triggers) {
	poisoned = false;
	return false;
      }

      // both easy cases failed, so take the lock that lets us see which local triggers exist
      // this prevents us from ever answering "no" on the current node if the trigger occurred here
      bool locally_triggered = false;
      poisoned = false;
      {
	AutoHSLLock a(mutex);

	std::map<Event::gen_t, bool>::const_iterator it = local_triggers.find(needed_gen);
	if(it != local_triggers.end()) {
	  locally_triggered = true;
	  poisoned = it->second;
	}
      }
      return locally_triggered;
    }

    class PthreadCondWaiter : public EventWaiter {
    public:
      PthreadCondWaiter(GASNetCondVar &_cv)
        : cv(_cv)
	, poisoned(false)
      {
      }
      virtual ~PthreadCondWaiter(void) 
      {
      }

      virtual bool event_triggered(Event e, bool _poisoned)
      {
	// record whether event was poisoned - owner will inspect once awake
	poisoned = _poisoned;

        // Need to hold the lock to avoid the race
        AutoHSLLock(cv.mutex);
	cv.signal();
        // we're allocated on caller's stack, so deleting would be bad
        return false;
      }

      virtual void print(std::ostream& os) const
      {
	os << "external waiter";
      }

    public:
      GASNetCondVar &cv;
      bool poisoned;
    };

    void GenEventImpl::external_wait(Event::gen_t gen_needed, bool& poisoned)
    {
      GASNetCondVar cv(mutex);
      PthreadCondWaiter w(cv);
      add_waiter(gen_needed, &w);
      {
	AutoHSLLock a(mutex);

	// re-check condition before going to sleep
	while(gen_needed > generation) {
	  // now just sleep on the condition variable - hope we wake up
	  cv.wait();
	}
      }
      poisoned = w.poisoned;
    }

    void GenEventImpl::trigger(Event::gen_t gen_triggered, int trigger_node, bool poisoned)
    {
      Event e = make_event(gen_triggered);
      log_event.debug() << "event triggered: event=" << e << " by node " << trigger_node
			<< " (poisoned=" << poisoned << ")";

#ifdef EVENT_TRACING
      {
        EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
        item.event_id = me.id;
        item.event_gen = gen_triggered;
        item.action = EventTraceItem::ACT_TRIGGER;
      }
#endif

      std::vector<EventWaiter *> to_wake;

      if(gasnet_mynode() == owner) {
	// we own this event

	NodeSet to_update;
	bool free_event = false;

	{
	  AutoHSLLock a(mutex);

	  // must always be the next generation
	  assert(gen_triggered == (generation + 1));

	  to_wake.swap(current_local_waiters);
	  assert(future_local_waiters.empty()); // no future waiters here

	  to_update.swap(remote_waiters);

	  // update poisoned generation list
	  if(poisoned) {
	    if(!poisoned_generations)
	      poisoned_generations = new Event::gen_t[POISONED_GENERATION_LIMIT];
	    assert(num_poisoned_generations < POISONED_GENERATION_LIMIT);
	    poisoned_generations[num_poisoned_generations++] = gen_triggered;
	  }

	  // update generation last, with a synchronization to make sure poisoned generation
	  // list is valid to any observer of this update
	  __sync_synchronize();
	  generation = gen_triggered;

	  // we'll free the event unless it's maxed out on poisoned generations
	  free_event = (num_poisoned_generations < POISONED_GENERATION_LIMIT);
	}

	// any remote nodes to notify?
	if(!to_update.empty())
	  EventUpdateMessage::broadcast_request(to_update, 
						make_event(gen_triggered),
						num_poisoned_generations,
						poisoned_generations);

	// free event?
	if(free_event)
	  get_runtime()->local_event_free_list->free_entry(this);
      } else {
	// we're triggering somebody else's event, so the first thing to do is tell them
	assert(trigger_node == gasnet_mynode());
	EventTriggerMessage::send_request(owner, make_event(gen_triggered), poisoned);

	// we might need to subscribe to intermediate generations
	bool subscribe_needed = false;
	Event::gen_t previous_subscribe_gen = 0;

	// now update our version of the data structure
	{
	  AutoHSLLock a(mutex);

	  // is this the "next" version?
	  if(gen_triggered == (generation + 1)) {
	    // yes, so we have complete information and can update the state directly
	    to_wake.swap(current_local_waiters);
	    // any future waiters?
	    if(!future_local_waiters.empty()) {
	      std::map<Event::gen_t, std::vector<EventWaiter *> >::iterator it = future_local_waiters.begin();
	      log_event.debug() << "future waiters non-empty: first=" << it->first << " (= " << (generation + 1) << "?)";
	      if(it->first == (generation + 1)) {
		current_local_waiters.swap(it->second);
		future_local_waiters.erase(it);
	      }
	    }
	    // if this event was poisoned, record it in the local triggers since we only
	    //  update the official poison list on owner update messages
	    if(poisoned) {
	      local_triggers[gen_triggered] = true;
	      has_local_triggers = true;
	    }

	    // update generation last, with a synchronization to make sure poisoned generation
	    // list is valid to any observer of this update
	    generation = gen_triggered;
	    __sync_synchronize();
	  } else {
	    // we can't update the main state because there are generations that we know
	    //  have triggered, but we do not know if they are poisoned, so look in the
	    //  future waiter list to see who we can wake, and update the local trigger
	    //  list

	    // should be in the future, not the past
	    assert(gen_triggered > (generation + 1));

	    std::map<Event::gen_t, std::vector<EventWaiter *> >::iterator it = future_local_waiters.find(gen_triggered);
	    if(it != future_local_waiters.end()) {
	      to_wake.swap(it->second);
	      future_local_waiters.erase(it);
	    }

	    local_triggers[gen_triggered] = poisoned;
	    has_local_triggers = true;

	    subscribe_needed = true;
	    previous_subscribe_gen = gen_subscribed;
	    gen_subscribed = gen_triggered;
	  }

	  if(subscribe_needed)
	    EventSubscribeMessage::send_request(owner,
						make_event(gen_triggered),
						previous_subscribe_gen);
	}
      }

      // finally, trigger any local waiters
      if(!to_wake.empty()) {
	Event e = make_event(gen_triggered);
	for(std::vector<EventWaiter *>::iterator it = to_wake.begin();
	    it != to_wake.end();
	    it++) {
	  bool nuke = (*it)->event_triggered(e, poisoned);
          if(nuke) {
            //printf("deleting: "); (*it)->print_info(); fflush(stdout);
            delete (*it);
          }
        }
      }
    }

    /*static*/ BarrierImpl *BarrierImpl::create_barrier(unsigned expected_arrivals,
							ReductionOpID redopid,
							const void *initial_value /*= 0*/,
							size_t initial_value_size /*= 0*/)
    {
      BarrierImpl *impl = get_runtime()->local_barrier_free_list->alloc_entry();
      assert(impl);
      assert(impl->me.type() == ID::ID_BARRIER);

      // set the arrival count
      impl->base_arrival_count = expected_arrivals;

      if(redopid == 0) {
	assert(initial_value_size == 0);
	impl->redop_id = 0;
	impl->redop = 0;
	impl->initial_value = 0;
	impl->value_capacity = 0;
	impl->final_values = 0;
      } else {
	impl->redop_id = redopid;  // keep the ID too so we can share it
	impl->redop = get_runtime()->reduce_op_table[redopid];

	assert(initial_value != 0);
	assert(initial_value_size == impl->redop->sizeof_lhs);

	impl->initial_value = (char *)malloc(initial_value_size);
	memcpy(impl->initial_value, initial_value, initial_value_size);

	impl->value_capacity = 0;
	impl->final_values = 0;
      }

      // and let the barrier rearm as many times as necessary without being released
      impl->free_generation = (unsigned)-1;

      log_barrier.info("barrier created: " IDFMT "/%d base_count=%d redop=%d",
		       impl->me.id(), impl->generation, impl->base_arrival_count, redopid);
#ifdef EVENT_TRACING
      {
	EventTraceItem &item = Tracer<EventTraceItem>::trace_item();
	item.event_id = impl->me.id();
	item.event_gen = impl->me.gen;
	item.action = EventTraceItem::ACT_CREATE;
      }
#endif
      return impl;
    }

    BarrierImpl::BarrierImpl(void)
      : me((ID::IDType)-1), owner(-1)
    {
      generation = 0;
      gen_subscribed = 0;
      first_generation = free_generation = 0;
      next_free = 0;
      remote_subscribe_gens.clear();
      remote_trigger_gens.clear();
      base_arrival_count = 0;
      redop = 0;
      initial_value = 0;
      value_capacity = 0;
      final_values = 0;
    }

    void BarrierImpl::init(ID _me, unsigned _init_owner)
    {
      me = _me;
      owner = _init_owner;
      generation = 0;
      gen_subscribed = 0;
      first_generation = free_generation = 0;
      next_free = 0;
      remote_subscribe_gens.clear();
      remote_trigger_gens.clear();
      base_arrival_count = 0;
      redop = 0;
      initial_value = 0;
      value_capacity = 0;
      final_values = 0;
    }

    /*static*/ void BarrierAdjustMessage::handle_request(RequestArgs args, const void *data, size_t datalen)
    {
      log_barrier.info("received barrier arrival: delta=%d in=" IDFMT "/%d out=" IDFMT "/%d (%llx)",
		       args.delta, args.wait_on.id, args.wait_on.gen, args.barrier.id, args.barrier.gen, args.barrier.timestamp);
      BarrierImpl *impl = get_runtime()->get_barrier_impl(args.barrier);
      impl->adjust_arrival(args.barrier.gen, args.delta, args.barrier.timestamp, args.wait_on,
			   datalen ? data : 0, datalen);
    }

    /*static*/ void BarrierAdjustMessage::send_request(gasnet_node_t target, Barrier barrier, int delta, Event wait_on,
						       const void *data, size_t datalen)
    {
      RequestArgs args;
      
      args.barrier = barrier;
      args.delta = delta;
      args.wait_on = wait_on;
      
      Message::request(target, args, data, datalen, PAYLOAD_COPY);
    }

    /*static*/ void BarrierSubscribeMessage::send_request(gasnet_node_t target, ID::IDType barrier_id, Event::gen_t subscribe_gen)
    {
      RequestArgs args;

      args.node = gasnet_mynode();
      args.barrier_id = barrier_id;
      args.subscribe_gen = subscribe_gen;
      
      Message::request(target, args);
    }

    /*static*/ void BarrierTriggerMessage::send_request(gasnet_node_t target, ID::IDType barrier_id,
							Event::gen_t trigger_gen, Event::gen_t previous_gen,
							Event::gen_t first_generation, ReductionOpID redop_id,
							const void *data, size_t datalen)
    {
      RequestArgs args;

      args.node = gasnet_mynode();
      args.barrier_id = barrier_id;
      args.trigger_gen = trigger_gen;
      args.previous_gen = previous_gen;
      args.first_generation = first_generation;
      args.redop_id = redop_id;

      Message::request(target, args, data, datalen, PAYLOAD_COPY);
    }

// like strdup, but works on arbitrary byte arrays
static void *bytedup(const void *data, size_t datalen)
{
  if(datalen == 0) return 0;
  void *dst = malloc(datalen);
  assert(dst != 0);
  memcpy(dst, data, datalen);
  return dst;
}

    class DeferredBarrierArrival : public EventWaiter {
    public:
      DeferredBarrierArrival(Barrier _barrier, int _delta, const void *_data, size_t _datalen)
	: barrier(_barrier), delta(_delta), data(bytedup(_data, _datalen)), datalen(_datalen)
      {}

      virtual ~DeferredBarrierArrival(void)
      {
	if(data)
	  free(data);
      }

      virtual bool event_triggered(Event e, bool poisoned)
      {
	// TODO: handle poison
	assert(poisoned == POISON_FIXME);
	log_barrier.info("deferred barrier arrival: " IDFMT "/%d (%llx), delta=%d",
			 barrier.id, barrier.gen, barrier.timestamp, delta);
	BarrierImpl *impl = get_runtime()->get_barrier_impl(barrier);
	impl->adjust_arrival(barrier.gen, delta, barrier.timestamp, Event::NO_EVENT, data, datalen);
        return true;
      }

      virtual void print(std::ostream& os) const
      {
	os << "deferred arrival: barrier=" << barrier << " (" << barrier.timestamp << ")"
	   << ", delta=" << delta << " datalen=" << datalen;
      }

    protected:
      Barrier barrier;
      int delta;
      void *data;
      size_t datalen;
    };

    BarrierImpl::Generation::Generation(void) : unguarded_delta(0) {}
    BarrierImpl::Generation::~Generation(void)
      {
        for(std::map<int, PerNodeUpdates *>::iterator it = pernode.begin();
            it != pernode.end();
            it++)
          delete (it->second);
      }

    void BarrierImpl::Generation::handle_adjustment(Barrier::timestamp_t ts, int delta)
      {
	if(ts == 0) {
	  // simple case - apply delta directly
	  unguarded_delta += delta;
	  return;
	}

        int node = ts >> BARRIER_TIMESTAMP_NODEID_SHIFT;
        PerNodeUpdates *pn;
        std::map<int, PerNodeUpdates *>::iterator it = pernode.find(node);
        if(it != pernode.end()) {
          pn = it->second;
        } else {
          pn = new PerNodeUpdates;
          pernode[node] = pn;
        }
        if(delta > 0) {
          // TODO: really need two timestamps to properly order increments
          unguarded_delta += delta;
          pn->last_ts = ts;
          std::map<Barrier::timestamp_t, int>::iterator it2 = pn->pending.begin();
          while((it2 != pn->pending.end()) && (it2->first <= pn->last_ts)) {
            log_barrier.info("applying pending delta: %llx/%d", it2->first, it2->second);
            unguarded_delta += it2->second;
            pn->pending.erase(it2);
            it2 = pn->pending.begin();
          }
        } else {
          // if the timestamp is late enough, we can apply this directly
          if(ts <= pn->last_ts) {
            log_barrier.info("adjustment can be applied immediately: %llx/%d (%llx)",
                             ts, delta, pn->last_ts);
            unguarded_delta += delta;
          } else {
            log_barrier.info("adjustment must be deferred: %llx/%d (%llx)",
                             ts, delta, pn->last_ts);
            pn->pending[ts] += delta;
          }
        }
      }

    struct RemoteNotification {
      unsigned node;
      Event::gen_t trigger_gen, previous_gen;
    };

    // used to adjust a barrier's arrival count either up or down
    // if delta > 0, timestamp is current time (on requesting node)
    // if delta < 0, timestamp says which positive adjustment this arrival must wait for
    void BarrierImpl::adjust_arrival(Event::gen_t barrier_gen, int delta, 
				     Barrier::timestamp_t timestamp, Event wait_on,
				     const void *reduce_value, size_t reduce_value_size)
    {
      if(!wait_on.has_triggered()) {
	// deferred arrival
	Barrier b = make_barrier(barrier_gen, timestamp);
#ifndef DEFER_ARRIVALS_LOCALLY
        if(owner != gasnet_mynode()) {
	  // let deferral happen on owner node (saves latency if wait_on event
          //   gets triggered there)
          //printf("sending deferred arrival to %d for " IDFMT "/%d (" IDFMT "/%d)\n",
          //       owner, e.id, e.gen, wait_on.id, wait_on.gen);
	  log_barrier.info("forwarding deferred barrier arrival: delta=%d in=" IDFMT "/%d out=" IDFMT "/%d (%llx)",
			   delta, wait_on.id, wait_on.gen, b.id, b.gen, b.timestamp);
	  BarrierAdjustMessage::send_request(owner, b, delta, wait_on, reduce_value, reduce_value_size);
	  return;
        }
#endif
	log_barrier.info("deferring barrier arrival: delta=%d in=" IDFMT "/%d out=" IDFMT "/%d (%llx)",
			 delta, wait_on.id, wait_on.gen, me.id(), barrier_gen, timestamp);
	EventImpl::add_waiter(wait_on, new DeferredBarrierArrival(b, delta, 
								  reduce_value, reduce_value_size));
	return;
      }

      log_barrier.info("barrier adjustment: event=" IDFMT "/%d delta=%d ts=%llx", 
		       me.id(), barrier_gen, delta, timestamp);

#ifdef DEBUG_BARRIER_REDUCTIONS
      if(reduce_value_size) {
        char buffer[129];
	for(size_t i = 0; (i < reduce_value_size) && (i < 64); i++)
	  sprintf(buffer+2*i, "%02x", ((const unsigned char *)reduce_value)[i]);
	log_barrier.info("barrier reduction: event=" IDFMT "/%d size=%zd data=%s",
	                 me.id(), barrier_gen, reduce_value_size, buffer);
      }
#endif

      if(owner != gasnet_mynode()) {
	// all adjustments handled by owner node
	Barrier b = make_barrier(barrier_gen, timestamp);
	BarrierAdjustMessage::send_request(owner, b, delta, Event::NO_EVENT, reduce_value, reduce_value_size);
	return;
      }

      // can't actually trigger while holding the lock, so remember which generation(s),
      //  if any, to trigger and do it at the end
      Event::gen_t trigger_gen = 0;
      std::vector<EventWaiter *> local_notifications;
      std::vector<RemoteNotification> remote_notifications;
      Event::gen_t oldest_previous = 0;
      void *final_values_copy = 0;
      {
	AutoHSLLock a(mutex);

	// sanity checks - is this a valid barrier?
	assert(generation < free_generation);
	assert(base_arrival_count > 0);

	// update whatever generation we're told to
	{
	  assert(barrier_gen > generation);
	  Generation *g;
	  std::map<Event::gen_t, Generation *>::iterator it = generations.find(barrier_gen);
	  if(it != generations.end()) {
	    g = it->second;
	  } else {
	    g = new Generation;
	    generations[barrier_gen] = g;
	    log_barrier.info("added tracker for barrier " IDFMT ", generation %d",
			     me.id(), barrier_gen);
	  }

	  g->handle_adjustment(timestamp, delta);
	}

	// if the update was to the next generation, it may cause one or more generations
	//  to trigger
	if(barrier_gen == (generation + 1)) {
	  std::map<Event::gen_t, Generation *>::iterator it = generations.begin();
	  while((it != generations.end()) &&
		(it->first == (generation + 1)) &&
		((base_arrival_count + it->second->unguarded_delta) == 0)) {
	    // keep the list of local waiters to wake up once we release the lock
	    local_notifications.insert(local_notifications.end(), 
				       it->second->local_waiters.begin(), it->second->local_waiters.end());
	    trigger_gen = generation = it->first;
	    delete it->second;
	    generations.erase(it);
	    it = generations.begin();
	  }

	  // if any triggers occurred, figure out which remote nodes need notifications
	  //  (i.e. any who have subscribed)
	  if(generation >= barrier_gen) {
	    std::map<unsigned, Event::gen_t>::iterator it = remote_subscribe_gens.begin();
	    while(it != remote_subscribe_gens.end()) {
	      RemoteNotification rn;
	      rn.node = it->first;
	      if(it->second <= generation) {
		// we have fulfilled the entire subscription
		rn.trigger_gen = it->second;
		std::map<unsigned, Event::gen_t>::iterator to_nuke = it++;
		remote_subscribe_gens.erase(to_nuke);
	      } else {
		// subscription remains valid
		rn.trigger_gen = generation;
		it++;
	      }
	      // also figure out what the previous generation this node knew about was
	      {
		std::map<unsigned, Event::gen_t>::iterator it2 = remote_trigger_gens.find(rn.node);
		if(it2 != remote_trigger_gens.end()) {
		  rn.previous_gen = it2->second;
		  it2->second = rn.trigger_gen;
		} else {
		  rn.previous_gen = first_generation;
		  remote_trigger_gens[rn.node] = rn.trigger_gen;
		}
	      }
	      if(remote_notifications.empty() || (rn.previous_gen < oldest_previous))
		oldest_previous = rn.previous_gen;
	      remote_notifications.push_back(rn);
	    }
	  }
	}

	// do we have reduction data to apply?  we can do this even if the actual adjustment is
	//  being held - no need to have lots of reduce values lying around
	if(reduce_value_size > 0) {
	  assert(redop != 0);
	  assert(redop->sizeof_rhs == reduce_value_size);

	  // do we have space for this reduction result yet?
	  int rel_gen = barrier_gen - first_generation;
	  assert(rel_gen > 0);

	  if((size_t)rel_gen > value_capacity) {
	    size_t new_capacity = rel_gen;
	    final_values = (char *)realloc(final_values, new_capacity * redop->sizeof_lhs);
	    while(value_capacity < new_capacity) {
	      memcpy(final_values + (value_capacity * redop->sizeof_lhs), initial_value, redop->sizeof_lhs);
	      value_capacity += 1;
	    }
	  }

	  redop->apply(final_values + ((rel_gen - 1) * redop->sizeof_lhs), reduce_value, 1, true);
	}

	// do this AFTER we actually update the reduction value above :)
	// if any remote notifications are going to occur and we have reduction values, make a copy so
	//  we have something stable after we let go of the lock
	if(trigger_gen && redop) {
	  int rel_gen = oldest_previous + 1 - first_generation;
	  assert(rel_gen > 0);
	  int count = trigger_gen - oldest_previous;
	  final_values_copy = bytedup(final_values + ((rel_gen - 1) * redop->sizeof_lhs),
				      count * redop->sizeof_lhs);
	}
      }

      if(trigger_gen != 0) {
	log_barrier.info("barrier trigger: event=" IDFMT "/%d", 
			 me.id(), trigger_gen);

	// notify local waiters first
	Barrier b = make_barrier(trigger_gen);
	for(std::vector<EventWaiter *>::const_iterator it = local_notifications.begin();
	    it != local_notifications.end();
	    it++) {
	  bool nuke = (*it)->event_triggered(b, POISON_FIXME);
	  if(nuke)
	    delete (*it);
	}

	// now do remote notifications
	for(std::vector<RemoteNotification>::const_iterator it = remote_notifications.begin();
	    it != remote_notifications.end();
	    it++) {
	  log_barrier.info("sending remote trigger notification: " IDFMT "/%d -> %d, dest=%d",
			   me.id(), (*it).previous_gen, (*it).trigger_gen, (*it).node);
	  void *data = 0;
	  size_t datalen = 0;
	  if(final_values_copy) {
	    data = (char *)final_values_copy + (((*it).previous_gen - oldest_previous) * redop->sizeof_lhs);
	    datalen = ((*it).trigger_gen - (*it).previous_gen) * redop->sizeof_lhs;
	  }
	  BarrierTriggerMessage::send_request((*it).node, me.id(), (*it).trigger_gen, (*it).previous_gen,
					      first_generation, redop_id, data, datalen);
	}
      }

      // free our copy of the final values, if we had one
      if(final_values_copy)
	free(final_values_copy);
    }

    bool BarrierImpl::has_triggered(Event::gen_t needed_gen, bool& poisoned)
    {
      poisoned = POISON_FIXME;

      // no need to take lock to check current generation
      if(needed_gen <= generation) return true;

      // if we're not the owner, subscribe if we haven't already
      if(owner != gasnet_mynode()) {
	Event::gen_t previous_subscription;
	// take lock to avoid duplicate subscriptions
	{
	  AutoHSLLock a(mutex);
	  previous_subscription = gen_subscribed;
	  if(gen_subscribed < needed_gen)
	    gen_subscribed = needed_gen;
	}

	if(previous_subscription < needed_gen) {
	  log_barrier.info("subscribing to barrier " IDFMT "/%d", me.id(), needed_gen);
	  BarrierSubscribeMessage::send_request(owner, me.id(), needed_gen);
	}
      }

      // whether or not we subscribed, the answer for now is "no"
      return false;
    }

    void BarrierImpl::external_wait(Event::gen_t needed_gen, bool& poisoned)
    {
      poisoned = POISON_FIXME;
      assert(0);
    }

    bool BarrierImpl::add_waiter(Event::gen_t needed_gen, EventWaiter *waiter/*, bool pre_subscribed = false*/)
    {
      bool trigger_now = false;
      {
	AutoHSLLock a(mutex);

	if(needed_gen > generation) {
	  Generation *g;
	  std::map<Event::gen_t, Generation *>::iterator it = generations.find(needed_gen);
	  if(it != generations.end()) {
	    g = it->second;
	  } else {
	    g = new Generation;
	    generations[needed_gen] = g;
	    log_barrier.info("added tracker for barrier " IDFMT ", generation %d",
			     me.id(), needed_gen);
	  }
	  g->local_waiters.push_back(waiter);

	  // a call to has_triggered should have already handled the necessary subscription
	  assert((owner == gasnet_mynode()) || (gen_subscribed >= needed_gen));
	} else {
	  // needed generation has already occurred - trigger this waiter once we let go of lock
	  trigger_now = true;
	}
      }

      if(trigger_now) {
	Barrier b = make_barrier(needed_gen);
	bool nuke = waiter->event_triggered(b, POISON_FIXME);
	if(nuke)
	  delete waiter;
      }

      return true;
    }

    /*static*/ void BarrierSubscribeMessage::handle_request(BarrierSubscribeMessage::RequestArgs args)
    {
      Barrier b;
      b.id = args.barrier_id;
      b.gen = args.subscribe_gen;
      BarrierImpl *impl = get_runtime()->get_barrier_impl(b);

      // take the lock and add the subscribing node - notice if they need to be notified for
      //  any generations that have already triggered
      Event::gen_t trigger_gen = 0;
      Event::gen_t previous_gen = 0;
      void *final_values_copy = 0;
      size_t final_values_size = 0;
      {
	AutoHSLLock a(impl->mutex);

	// make sure the subscription is for this "lifetime" of the barrier
	assert(args.subscribe_gen > impl->first_generation);

	bool already_subscribed = false;
	{
	  std::map<unsigned, Event::gen_t>::iterator it = impl->remote_subscribe_gens.find(args.node);
	  if(it != impl->remote_subscribe_gens.end()) {
	    // a valid subscription should always be for a generation that hasn't
	    //  triggered yet
	    assert(it->second > impl->generation);
	    if(it->second >= args.subscribe_gen)
	      already_subscribed = true;
	    else
	      it->second = args.subscribe_gen;
	  } else {
	    // new subscription - don't reset remote_trigger_gens because the node may have
	    //  been subscribed in the past
	    // NOTE: remote_subscribe_gens should only hold subscriptions for
	    //  generations that haven't triggered, so if we're subscribing to 
	    //  an old generation, don't add it
	    if(args.subscribe_gen > impl->generation)
	      impl->remote_subscribe_gens[args.node] = args.subscribe_gen;
	  }
	}

	// as long as we're not already subscribed to this generation, check to see if
	//  any trigger notifications are needed
	if(!already_subscribed && (impl->generation > impl->first_generation)) {
	  std::map<unsigned, Event::gen_t>::iterator it = impl->remote_trigger_gens.find(args.node);
	  if((it == impl->remote_trigger_gens.end()) or (it->second < impl->generation)) {
	    previous_gen = ((it == impl->remote_trigger_gens.end()) ?
			      impl->first_generation :
			      it->second);
	    trigger_gen = impl->generation;
	    impl->remote_trigger_gens[args.node] = impl->generation;

	    if(impl->redop) {
	      int rel_gen = previous_gen + 1 - impl->first_generation;
	      assert(rel_gen > 0);
	      final_values_size = (trigger_gen - previous_gen) * impl->redop->sizeof_lhs;
	      final_values_copy = bytedup(impl->final_values + ((rel_gen - 1) * impl->redop->sizeof_lhs),
					  final_values_size);
	    }
	  }
	}
      }

      // send trigger message outside of lock, if needed
      if(trigger_gen > 0) {
	log_barrier.info("sending immediate barrier trigger: " IDFMT "/%d -> %d",
			 args.barrier_id, previous_gen, trigger_gen);
	BarrierTriggerMessage::send_request(args.node, args.barrier_id, trigger_gen, previous_gen,
					    impl->first_generation, impl->redop_id,
					    final_values_copy, final_values_size);
      }

      if(final_values_copy)
	free(final_values_copy);
    }

    /*static*/ void BarrierTriggerMessage::handle_request(BarrierTriggerMessage::RequestArgs args,
							  const void *data, size_t datalen)
    {
      log_barrier.info("received remote barrier trigger: " IDFMT "/%d -> %d",
		       args.barrier_id, args.previous_gen, args.trigger_gen);

      Barrier b;
      b.id = args.barrier_id;
      b.gen = args.trigger_gen;
      BarrierImpl *impl = get_runtime()->get_barrier_impl(b);

      // we'll probably end up with a list of local waiters to notify
      std::vector<EventWaiter *> local_notifications;
      {
	AutoHSLLock a(impl->mutex);

	// it's theoretically possible for multiple trigger messages to arrive out
	//  of order, so check if this message triggers the oldest possible range
	if(args.previous_gen == impl->generation) {
	  // see if we can pick up any of the held triggers too
	  while(!impl->held_triggers.empty()) {
	    std::map<Event::gen_t, Event::gen_t>::iterator it = impl->held_triggers.begin();
	    // if it's not contiguous, we're done
	    if(it->first != args.trigger_gen) break;
	    // it is contiguous, so absorb it into this message and remove the held trigger
	    log_barrier.info("collapsing future trigger: " IDFMT "/%d -> %d -> %d",
			     args.barrier_id, args.previous_gen, args.trigger_gen, it->second);
	    args.trigger_gen = it->second;
	    impl->held_triggers.erase(it);
	  }

	  impl->generation = args.trigger_gen;

	  // now iterate through any generations up to and including the latest triggered
	  //  generation, and accumulate local waiters to notify
	  while(!impl->generations.empty()) {
	    std::map<Event::gen_t, BarrierImpl::Generation *>::iterator it = impl->generations.begin();
	    if(it->first > args.trigger_gen) break;

	    local_notifications.insert(local_notifications.end(),
				       it->second->local_waiters.begin(),
				       it->second->local_waiters.end());
	    delete it->second;
	    impl->generations.erase(it);
	  }
	} else {
	  // hold this trigger until we get messages for the earlier generation(s)
	  log_barrier.info("holding future trigger: " IDFMT "/%d (%d -> %d)",
			   args.barrier_id, impl->generation, 
			   args.previous_gen, args.trigger_gen);
	  impl->held_triggers[args.previous_gen] = args.trigger_gen;
	}

	// is there any data we need to store?
	if(datalen) {
	  assert(args.redop_id != 0);

	  // TODO: deal with invalidation of previous instance of a barrier
	  impl->redop_id = args.redop_id;
	  impl->redop = get_runtime()->reduce_op_table[args.redop_id];
	  impl->first_generation = args.first_generation;

	  int rel_gen = args.trigger_gen - impl->first_generation;
	  assert(rel_gen > 0);
	  if(impl->value_capacity < (size_t)rel_gen) {
	    size_t new_capacity = rel_gen;
	    impl->final_values = (char *)realloc(impl->final_values, new_capacity * impl->redop->sizeof_lhs);
	    // no need to initialize new entries - we'll overwrite them now or when data does show up
	    impl->value_capacity = new_capacity;
	  }
	  assert(datalen == (impl->redop->sizeof_lhs * (args.trigger_gen - args.previous_gen)));
	  memcpy(impl->final_values + ((rel_gen - 1) * impl->redop->sizeof_lhs), data, datalen);
	}
      }

      // with lock released, perform any local notifications
      for(std::vector<EventWaiter *>::const_iterator it = local_notifications.begin();
	  it != local_notifications.end();
	  it++) {
	bool nuke = (*it)->event_triggered(b, POISON_FIXME);
	if(nuke)
	  delete (*it);
      }
    }

    bool BarrierImpl::get_result(Event::gen_t result_gen, void *value, size_t value_size)
    {
      // take the lock so we can safely see how many results (if any) are on hand
      AutoHSLLock al(mutex);

      // generation hasn't triggered yet?
      if(result_gen > generation) return false;

      // if it has triggered, we should have the data
      int rel_gen = result_gen - first_generation;
      assert(rel_gen > 0);
      assert((size_t)rel_gen <= value_capacity);

      assert(redop != 0);
      assert(value_size == redop->sizeof_lhs);
      assert(value != 0);
      memcpy(value, final_values + ((rel_gen - 1) * redop->sizeof_lhs), redop->sizeof_lhs);
      return true;
    }

}; // namespace Realm

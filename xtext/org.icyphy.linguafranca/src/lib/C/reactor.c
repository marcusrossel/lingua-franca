/*
 FIXME: License, copyright, authors.
 */

#include "reactor.h"

/** Global constants. */
bool False = false;
bool True = true;

// Indicator of whether to wait for physical time to match logical time.
// By default, execution will wait. The command-line argument -fast will
// eliminate the wait and allow logical time to exceed physical time.
bool fast = false;

// Current time.
// This is not in scope for reactors.
instant_t current_time = 0LL;

// Indicator that the execution should stop after the completion of the
// current logical time. This can be set to true by calling the stop()
// function in a reaction.
bool stop_requested = false;

// Duration, or -1 if no stop time has been given.
instant_t duration = -1LL;

// Stop time, or 0 if no stop time has been given.
instant_t stop_time = 0LL;

// Indicator of whether the wait command-line option was given.
bool wait_specified = false;

/////////////////////////////
// The following functions are in scope for all reactors:

// Return the current logical time.
long long get_logical_time() {
    return current_time;
}

// Stop execution at the conclusion of the current logical time.
void stop() {
    stop_requested = true;
}

/////////////////////////////
// The following is not in scope for reactors:

// Priority queues.
pqueue_t* event_q;     // For sorting by time.
pqueue_t* reaction_q;  // For sorting by index (topological sort)
pqueue_t* recycle_q;   // For recycling malloc'd events.

handle_t __handle = 0;
struct timespec physicalStartTime;

// ********** Priority Queue Support Start

// Compare two priorities.
static int cmp_pri(pqueue_pri_t next, pqueue_pri_t curr) {
    return (next > curr);
}
// Compare two events.
static int eql_evt(void* next, void* curr) {
    return (((event_t*)next)->trigger == ((event_t*)curr)->trigger);
}
// Compare two reactions.
static int eql_rct(void* next, void* curr) {
    return (next == curr);
}
// Get priorities based on time.
// Used for sorting event_t structs.
static pqueue_pri_t get_evt_pri(void *a) {
    return (pqueue_pri_t)(((event_t*) a)->time);
}
// Get priorities based on indices, which reflect topological sort.
// Used for sorting reaction_t structs.
static pqueue_pri_t get_rct_pri(void *a) {
    return ((reaction_t*) a)->index;
}

// Get position in the queue of the specified event.
static size_t get_evt_pos(void *a) {
    return ((event_t*) a)->pos;
}

// Get position in the queue of the specified event.
static size_t get_rct_pos(void *a) {
    return ((reaction_t*) a)->pos;
}

// Set position of the specified event.
static void set_evt_pos(void *a, size_t pos) {
    ((event_t*) a)->pos = pos;
}

// Set position of the specified event.
static void set_rct_pos(void *a, size_t pos) {
    ((reaction_t*) a)->pos = pos;
}

// ********** Priority Queue Support End

// Schedule the specified trigger at current_time plus the
// offset of the specified trigger plus the delay.
handle_t __schedule(trigger_t* trigger, interval_t delay) {
    // Recycle event_t structs, if possible.
    event_t* e = pqueue_pop(recycle_q);
    if (e == NULL) {
        e = malloc(sizeof(struct event_t));
    }
    e->time = current_time + trigger->offset + delay;
    e->trigger = trigger;

    // NOTE: There is no need for an explicit microstep because
    // when this is called, all events at the current tag
    // (time and microstep) have been pulled from the queue,
    // and any new events added at this tag will go into the reaction_q
    // rather than the event_q, so anything put in the event_q with this
    // same time will automatically be executed at the next microstep.
    pqueue_insert(event_q, e);
    // FIXME: make a record of handle and implement unschedule.
    return __handle++;
}

// Schedule the specified trigger at current_time plus the
// offset declared in the trigger plus the extra_delay.
handle_t schedule(trigger_t* trigger, interval_t extra_delay) {
    return __schedule(trigger, trigger->offset + extra_delay);
}

// Advance logical time to the lesser of the specified time, the
// stop time, if a stop time has been given, or the time of a call to schedule()
// in an asynchronous callback. If the -fast command-line option
// was not given, then wait until physical time matches or exceeds the start time of
// execution plus the current_time plus the specified logical time.  If this is not
// interrupted, then advance current_time by the specified logical_delay. 
// Return 0 if time advanced to the time of the event and -1 if the wait
// was interrupted or if the stop time was reached.
int wait_until(instant_t logical_time_ns) {
    int return_value = 0;
    if (stop_time > 0LL && logical_time_ns > stop_time) {
        logical_time_ns = stop_time;
        // Indicate on return that the time of the event was not reached.
        // We still wait for time to elapse in case asynchronous events come in.
        return_value = -1;
    }
    if (!fast) {
        // printf("-------- Waiting for logical time %lld.\n", logical_time_ns);
    
        // Get the current physical time.
        struct timespec current_physical_time;
        clock_gettime(CLOCK_REALTIME, &current_physical_time);
    
        long long ns_to_wait = logical_time_ns
                - (current_physical_time.tv_sec * BILLION
                + current_physical_time.tv_nsec);
    
        if (ns_to_wait <= 0) {
            // Advance current time.
            current_time = logical_time_ns;
            return return_value;
        }
    
        // timespec is seconds and nanoseconds.
        struct timespec wait_time = {(time_t)ns_to_wait / BILLION, (long)ns_to_wait % BILLION};
        // printf("-------- Waiting %lld seconds, %lld nanoseconds.\n", ns_to_wait / BILLION, ns_to_wait % BILLION);
        struct timespec remaining_time;
        // FIXME: If the wait time is less than the time resolution, don't sleep.
        if (nanosleep(&wait_time, &remaining_time) != 0) {
            // Sleep was interrupted.
            // May have been an asynchronous call to schedule(), or
            // it may have been a control-C to stop the process.
            // Set current time to match physical time, but not less than
            // current logical time nor more than next time in the event queue.
            clock_gettime(CLOCK_REALTIME, &current_physical_time);
            long long current_physical_time_ns 
                    = current_physical_time.tv_sec * BILLION
                    + current_physical_time.tv_nsec;
            if (current_physical_time_ns > current_time) {
                if (current_physical_time_ns < logical_time_ns) {
                    current_time = current_physical_time_ns;
                    return -1;
                }
            } else {
                // Current physical time does not exceed current logical
                // time, so do not advance current time.
                return -1;
            }
        }
    }
    // Advance current time.
    current_time = logical_time_ns;
    return return_value;
}

// Wait until physical time matches or exceeds the time of the least tag
// on the event queue. If theres is no event in the queue, return 0.
// After this wait, advance current_time to match
// this tag. Then pop the next event(s) from the
// event queue that all have the same tag, and extract from those events
// the reactions that are to be invoked at this logical time.
// Sort those reactions by index (determined by a topological sort)
// and then execute the reactions in order. Each reaction may produce
// outputs, which places additional reactions into the index-ordered
// priority queue. All of those will also be executed in order of indices.
// If the -stop option has been given on the command line, then return
// 0 when the logical time duration matches the specified duration.
// Also return 0 if there are no more events in the queue and
// the wait command-line option has not been given.
// Otherwise, return 1.
int next() {
    event_t* event = pqueue_peek(event_q);
    // If there is no next event and -wait has been specified
    // on the command line, then we will wait the maximum time possible.
    instant_t next_time = LLONG_MAX;
    if (event == NULL) {
        // No event in the queue.
        if (!wait_specified) {
            return 0;
        }
    } else {
        next_time = event->time;
    }
    // Wait until physical time >= event.time.
    if (wait_until(next_time) < 0) {
        // Sleep was interrupted or the stop time has been reached.
        // Time has not advanced to the time of the event.
        // There may be a new earlier event on the queue.
        event_t* new_event = pqueue_peek(event_q);
        if (new_event == event) {
            // There is no new event. If the stop time has been reached,
            // or if the maximum time has been reached (unlikely), then return.
            if (current_time == stop_time || new_event == NULL) {
                return 0;
            }
        }
    }
    
    // Invoke code that must execute before starting a new logical time round,
    // such as initializing outputs to be absent.
    __start_time_step();
    
    // Pop all events from event_q with timestamp equal to current_time
    // stick them into reaction.
    do {
        event = pqueue_pop(event_q);
        for (int i = 0; i < event->trigger->number_of_reactions; i++) {
            // printf("Pushed on reaction_q: %p\n", event->trigger->reactions[i]);
            // printf("Pushed reaction args: %p\n", event->trigger->reactions[i]->args);
            pqueue_insert(reaction_q, event->trigger->reactions[i]);
        }
        if (event->trigger->period > 0) {
            // Reschedule the trigger.
            // Note that the delay here may be negative because the __schedule
            // function will add the trigger->offset, which we don't want at this point.
            __schedule(event->trigger, event->trigger->period - event->trigger->offset);
        }
          
        // Recycle this event instead of freeing it.
        // So that sorting doesn't cost anything, give all recycled events the
        // same zero time stamp.
        event->time = 0LL;
        pqueue_insert(recycle_q, event);

        event = pqueue_peek(event_q);
    } while(event != NULL && event->time == current_time);

    // Handle reactions.
    while(pqueue_size(reaction_q) > 0) {
        reaction_t* reaction = pqueue_pop(reaction_q);
        // printf("Popped from reaction_q: %p\n", reaction);
        // printf("Popped reaction function: %p\n", reaction->function);
        
        // If the reaction has a deadline, compare to current physical time
        // and invoke the deadline violation reaction before the reaction function
        // if a violation has occurred.
        if (reaction->deadline > 0LL) {
            // Get the current physical time.
            struct timespec current_physical_time;
            clock_gettime(CLOCK_REALTIME, &current_physical_time);
            // Convert to instant_t.
            instant_t physical_time = 
                    current_physical_time.tv_sec * BILLION
                    + current_physical_time.tv_nsec;
            // Check for deadline violation.
            if (physical_time > current_time + reaction->deadline) {
                // Deadline violation has occurred.
                // Invoke the violation reactions, if there are any.
                trigger_t* trigger = reaction->deadline_violation;
                if (trigger != NULL) {
                    for (int i = 0; i < trigger->number_of_reactions; i++) {
                        trigger->reactions[i]->function(trigger->reactions[i]->self);
                    }
                }
            }
        }
        
        // Invoke the reaction function.
        reaction->function(reaction->self);

        // If the reaction produced outputs, put the resulting triggered
        // reactions into the queue.
        for(int i=0; i < reaction->num_outputs; i++) {
            if (*(reaction->output_produced[i])) {
                trigger_t** triggerArray = (reaction->triggers)[i];
                for (int j=0; j < reaction->triggered_sizes[i]; j++) {
                    trigger_t* trigger = triggerArray[j];
                    for (int k=0; k < trigger->number_of_reactions; k++) {
                        reaction_t* reaction = trigger->reactions[k];
                        pqueue_insert(reaction_q, trigger->reactions[k]);
                    }
                }
            }
        }
    }
    if (current_time == stop_time) {
        return 0;
    }
    return 1;
}

// Print a usage message.
void usage(char* command) {
    printf("\nCommand-line arguments: \n\n");
    printf("  -fast\n");
    printf("   Do not wait for physical time to match logical time.\n\n");
    printf("  -stop <duration> <units>\n");
    printf("   Stop after the specified amount of logical time, where units are one of\n");
    printf("   nsec, usec, msec, sec, minute, hour, day, week, or the plurals of those.\n\n");
    printf("  -wait\n");
    printf("   Do not stop execution even if there are no events to process. Just wait.\n\n");
}

// Process the command-line arguments.
// If the command line arguments are not understood, then
// print a usage message and return 0.
// Otherwise, return 1.
int process_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-fast") == 0) {
            fast = true;
        } else if (strcmp(argv[i], "-stop") == 0) {
            if (argc < i + 3) {
                usage(argv[0]);
                return 0;
            }
            i++;
            char* time_spec = argv[i++];
            char* units = argv[i];
            duration = atoll(time_spec);
            // A parse error returns 0LL, so check to see whether that is what is meant.
            if (duration == 0LL && strncmp(time_spec, "0", 1) != 0) {
                // Parse error.
                printf("Error: invalid time value: %s", time_spec);
                usage(argv[0]);
                return 0;
            }
            if (strncmp(units, "sec", 3) == 0) {
                duration = SEC(duration);
            } else if (strncmp(units, "msec", 4) == 0) {
                duration = MSEC(duration);
            } else if (strncmp(units, "usec", 4) == 0) {
                duration = USEC(duration);
            } else if (strncmp(units, "nsec", 4) == 0) {
                duration = NSEC(duration);
            } else if (strncmp(units, "minute", 6) == 0) {
                duration = MINUTE(duration);
            } else if (strncmp(units, "hour", 4) == 0) {
                duration = HOUR(duration);
            } else if (strncmp(units, "day", 3) == 0) {
                duration = DAY(duration);
            } else if (strncmp(units, "week", 4) == 0) {
                duration = WEEK(duration);
            } else {
                // Invalid units.
                printf("Error: invalid time units: %s", units);
                usage(argv[0]);
                return 0;
            }
        } else if (strcmp(argv[i], "-wait") == 0) {
            wait_specified = true;
        } else {
            usage(argv[0]);
            return 0;
        }
    }
    return 1;
}

// Initialize the priority queues and set logical time to match
// physical time. This also prints a message reporting the start time.
void initialize() {
#if _WIN32 || WIN32
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        NtDelayExecution = (NtDelayExecution_t *)GetProcAddress(ntdll, "NtDelayExecution");
        NtQueryPerformanceCounter = (NtQueryPerformanceCounter_t *)GetProcAddress(ntdll, "NtQueryPerformanceCounter");
        NtQuerySystemTime = (NtQuerySystemTime_t *)GetProcAddress(ntdll, "NtQuerySystemTime");
    }
#endif

    // Initialize our priority queues.
    event_q = pqueue_init(INITIAL_EVENT_QUEUE_SIZE, cmp_pri, get_evt_pri,
            get_evt_pos, set_evt_pos, eql_evt);
    reaction_q = pqueue_init(INITIAL_REACT_QUEUE_SIZE, cmp_pri, get_rct_pri,
            get_rct_pos, set_rct_pos, eql_rct);
	// NOTE: The recycle queue does not need to be sorted. But here it is.
    recycle_q = pqueue_init(INITIAL_EVENT_QUEUE_SIZE, cmp_pri, get_evt_pri,
            get_evt_pos, set_evt_pos, eql_evt);

    // Initialize the trigger table.
    __initialize_trigger_objects();

    // Initialize logical time to match physical time.
    clock_gettime(CLOCK_REALTIME, &physicalStartTime);
    printf("Start execution at time %splus %ld nanoseconds.\n",
    ctime(&physicalStartTime.tv_sec), physicalStartTime.tv_nsec);
    current_time = physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec;
    
    if (duration >= 0LL) {
        // A duration has been specified. Calculate the stop time.
        stop_time = current_time + duration;
    }
}

// Print elapsed logical and physical times.
void wrapup() {
    interval_t elapsed_logical_time
        = current_time - (physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec);
    printf("Elapsed logical time (in nsec): %lld\n", elapsed_logical_time);
    
    struct timespec physicalEndTime;
    clock_gettime(CLOCK_REALTIME, &physicalEndTime);
    interval_t elapsed_physical_time
        = (physicalEndTime.tv_sec * BILLION + physicalEndTime.tv_nsec)
        - (physicalStartTime.tv_sec * BILLION + physicalStartTime.tv_nsec);
    printf("Elapsed physical time (in nsec): %lld\n", elapsed_physical_time);
}

// ********** Start Windows Support
// Windows is not POSIX, so we include here compatibility definitions.
#if _WIN32 || WIN32
NtDelayExecution_t *NtDelayExecution = NULL;
NtQueryPerformanceCounter_t *NtQueryPerformanceCounter = NULL;
NtQuerySystemTime_t *NtQuerySystemTime = NULL;
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    int result = -1;
    int days_from_1601_to_1970 = 134774 /* there were no leap seconds during this time, so life is easy */;
    long long timestamp, counts, counts_per_sec;
    switch (clk_id) {
    case CLOCK_REALTIME:
        NtQuerySystemTime((PLARGE_INTEGER)&timestamp);
        timestamp -= days_from_1601_to_1970 * 24LL * 60 * 60 * 1000 * 1000 * 10;
        tp->tv_sec = (time_t)(timestamp / (BILLION / 100));
        tp->tv_nsec = (long)((timestamp % (BILLION / 100)) * 100);
        result = 0;
        break;
    case CLOCK_MONOTONIC:
        if ((*NtQueryPerformanceCounter)((PLARGE_INTEGER)&counts, (PLARGE_INTEGER)&counts_per_sec) == 0) {
            tp->tv_sec = counts / counts_per_sec;
            tp->tv_nsec = (long)((counts % counts_per_sec) * BILLION / counts_per_sec);
            result = 0;
        } else {
            errno = EINVAL;
            result = -1;
        }
        break;
    default:
        errno = EINVAL;
        result = -1;
        break;
    }
    return result;
}
int nanosleep(const struct timespec *req, struct timespec *rem) {
    unsigned char alertable = rem ? 1 : 0;
    long long duration = -(req->tv_sec * (BILLION / 100) + req->tv_nsec / 100);
    NTSTATUS status = (*NtDelayExecution)(alertable, (PLARGE_INTEGER)&duration);
    int result = status == 0 ? 0 : -1;
    if (alertable) {
        if (status < 0) {
            errno = EINVAL;
        } else if (status > 0 && clock_gettime(CLOCK_MONOTONIC, rem) == 0) {
            errno = EINTR;
        }
    }
    return result;
}
#endif
// ********** End Windows Support

// Patmos does not have an epoch, so it does not have clock_gettime
// clock() looks like not working, use the hardware counter of Patmos
#ifdef __PATMOS__
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    // TODO: use all 64 bits of the timer
    int timestamp = TIMER_US_LOW;
// printf("Time %d\n", timestamp);
    tp->tv_sec = timestamp/1000000;
    tp->tv_nsec = (timestamp%1000000) * 1000;
// printf("clock_gettime: %lld %ld\n", tp->tv_sec, tp->tv_nsec);
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {

    // We could use our deadline device here
    int timestamp = TIMER_US_LOW;
// printf("nanosleep: %lld %ld\n", req->tv_sec, req->tv_nsec);
    
    timestamp += req->tv_sec * 1000000 + req->tv_nsec / 1000;
// printf("sleep to %d\n", timestamp);
    while (timestamp - TIMER_US_LOW > 0) {
        ;
// printf("time %d\n", TIMER_US_LOW);
    }
    if (rem != 0) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;

    }
    return 0;
}
#endif

int main(int argc, char* argv[]) {
    if (process_args(argc, argv)) {
        initialize();
        __start_timers();
        while (next() != 0 && !stop_requested);
        wrapup();
    	return 0;
    } else {
    	return -1;
    }
}
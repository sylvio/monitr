/*
 * Copyright (c) 2013, Yahoo! Inc. All rights reserved.
 * Copyrights licensed under the New BSD License.
 * See the accompanying LICENSE file for terms.
 */

#include <sys/syscall.h>

#include <sys/types.h>
#include <sys/select.h>
#include <sys/prctl.h>  // to set thread name
#include <dirent.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>

#include <iostream>
#include <fstream>
#include <algorithm>

#include "monitor.h"

#ifdef __APPLE__
    #include <sys/sysctl.h>
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#else
    extern char **environ;
#endif

using namespace std;

// This is the default IPC path where the stats are written to
// Could use the setter method to change this
static string _ipcMonitorPath = "/tmp/nodejs.mon";

// Normally reports will be sent every REPORT_INTERVAL_MS
// However, if there is no receiver on the other end (i.e. sendmsg()
// returns -1), then the reporting thread will wait MAX_INACTIVITY_RETRIES
// before trying again.
static const int REPORT_INTERVAL_MS = 1000;
static const int MAX_INACTIVITY_RETRIES = 5;

/* globals used for signal catching, etc */
static volatile sig_atomic_t hup_fired = 0;
static siginfo_t savedSigInfo;
static sigset_t savedBlockSet;
static int sigpipefd_w = -1;
static int sigpipefd_r = -1;


namespace ynode {

// our singleton instance
NodeMonitor* NodeMonitor::instance_ = NULL;

/**
 * obtain reference to process.monitor from global object
 *
 * Preconditions:  process.monitor exists and is an object
 *
 * Must be called on the JS thread (i.e. inside a N-API callback).
 **/
static Napi::Object getProcessMonitor(Napi::Env env) {
    Napi::Object global = env.Global();
    Napi::Value process = global.Get("process");
    assert(process.IsObject());

    // monitr javascript interface library must create process.monitor
    Napi::Value monitor = process.As<Napi::Object>().Get("monitor");
    assert(monitor.IsObject());

    return monitor.As<Napi::Object>();
}


void RegisterSignalHandler(int signal, void (*handler)(int, siginfo_t *, void *)) {

    sigset_t blockset;
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGHUP);
    // block SIGHUP until we get to pselect() call to avoid race condition
    // See http://lwn.net/Articles/176911/
    sigprocmask(SIG_BLOCK, &blockset, &savedBlockSet);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO; // Tell sigaction() to use the sa_sigaction field, not sa_handler.
    sigemptyset(&sa.sa_mask); // Allow other signals to run during SIGHUP handler
    if (sigaction(signal, &sa, NULL) < 0) {
        perror("sigaction");
    }
}

/** Sleep waiting for some event on our pipe from the signal handler
 *  \param ms max number of milliseconds to wait
 **/
static void doSleep(int ms) {
    int res;
    struct timespec timeout;

    timeout.tv_sec = ms / 1000;
    timeout.tv_nsec = (ms % 1000) * 1000 * 1000;

    // put this thread to sleep until timeout, or a SIGHUP occurs
    sigset_t blockset;
    blockset = savedBlockSet;
    // ensure we don't block SIGHUP in pselect in case signal delivered to this thread
    sigdelset(&blockset, SIGHUP);

    fd_set readfds;
    FD_ZERO(&readfds );
    FD_SET(sigpipefd_r, &readfds);

    res = pselect(sigpipefd_r + 1, &readfds, NULL, NULL, &timeout, &blockset);

    // Did we exit due to a "signal" sent via the pipe (likely from another thread)
    if (res >= 0) {
        if (FD_ISSET(sigpipefd_r, &readfds)) {
            char c[100];
            while ((read(sigpipefd_r, &c, sizeof(c))) == sizeof(c)) {
              /* Flush the pipe! */
            }
        }
    }
    else {
        perror("pselect");
    }
}

static void Interrupter(v8::Isolate* isolate, void *data) {
    const int kMaxFrames = 64;
    v8::Local<v8::StackTrace> stack = v8::StackTrace::CurrentStackTrace(isolate, kMaxFrames);
    const int frameCount = stack->GetFrameCount();

    fprintf(stderr, "Interrupted: (frames: %d of %d)\n", std::min(frameCount, kMaxFrames), frameCount);
    for (int i = 0; i < stack->GetFrameCount(); ++i ) {
        v8::Local<v8::StackFrame> frame = stack->GetFrame(
#if defined(V8_MAJOR_VERSION) && (V8_MAJOR_VERSION >= 7)
             isolate, 
#endif
             i );
        int line = frame->GetLineNumber();
        int col = frame->GetColumn();
        v8::String::Utf8Value fn(isolate, frame->GetFunctionName());
        v8::String::Utf8Value script(isolate, frame->GetScriptName());

        fprintf(stderr, "    at %s (%s:%d:%d)\n",
                fn.length() == 0 ? "<anonymous>" : *fn, *script, line, col);
    }
}

/**
 * Thread which reports the status of the current process via UDP messages
 */
void* monitorNodeThread(void *arg) {
    int errorCounter = 0;

    // set the thread name so it's easy to distinguish when debugging
    int rc = pthread_setname_np(pthread_self(), "monitr");
    if (0 != rc) perror("pthread_setname_np");

    NodeMonitor& monitor = NodeMonitor::getInstance();
    doSleep(REPORT_INTERVAL_MS);
    while (!monitor.isStopRequested()) {
        if (hup_fired) {
            siginfo_t *siginfo = &savedSigInfo;
            fprintf(stderr, "Process %d received SIGHUP from pid %d\n", getpid(), siginfo->si_pid);

            monitor.getIsolate()->RequestInterrupt(Interrupter, 0);
            hup_fired = 0;
        }
        if (!errorCounter) {
            if (!monitor.sendReport()) {
                // slow down reporting if nobody is listening
                ++errorCounter;
            }
        } else {
            ++errorCounter;
            if (errorCounter >= MAX_INACTIVITY_RETRIES) {
                errorCounter = 0;
            }
        }
        if (monitor.isStopRequested()) {
            break;
        }
        doSleep(REPORT_INTERVAL_MS);
    }
    return NULL;
}

// N-API getter for process.monitor.gc.count
static Napi::Value GetterGCCount(const Napi::CallbackInfo& info) {
    GCUsageTracker& tracker = NodeMonitor::getInstance().getGCUsageTracker();
    return Napi::Number::New(info.Env(), static_cast<double>(tracker.totalCollections()));
}

// N-API getter for process.monitor.gc.elapsed (milliseconds)
static Napi::Value GetterGCElapsed(const Napi::CallbackInfo& info) {
    GCUsageTracker& tracker = NodeMonitor::getInstance().getGCUsageTracker();
    return Napi::Number::New(info.Env(), tracker.totalElapsedTime() / (1000.0 * 1000.0));
}

// V8 GC prologue/epilogue callbacks.  N-API has no abstraction for these,
// so we register them directly on the isolate (the "hybrid" part).
static void startGC(v8::Isolate* /*isolate*/, v8::GCType type, v8::GCCallbackFlags /*flags*/) {
    NodeMonitor::getInstance().getGCUsageTracker().StartGC(type);
}

static void stopGC(v8::Isolate* /*isolate*/, v8::GCType type, v8::GCCallbackFlags /*flags*/) {
    NodeMonitor::getInstance().getGCUsageTracker().StopGC(type);
}

void NodeMonitor::InstallGCEventCallbacks() {
    isolate_->AddGCPrologueCallback(startGC);
    isolate_->AddGCEpilogueCallback(stopGC);
}

void NodeMonitor::UninstallGCEventCallbacks() {
    isolate_->RemoveGCPrologueCallback(startGC);
    isolate_->RemoveGCEpilogueCallback(stopGC);
}

/**
 * Set up the singleton instance variable
 */
void NodeMonitor::Initialize(Napi::Env env) {

    // only one instance is allowed per process
    // \todo change this to be one per *isolate*
    if (instance_) {
        return;
    }
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    assert(0 != isolate);
    instance_ = new NodeMonitor(isolate); // calls protected constructor

    instance_->InitializeProcessMonitorGCObject(env);
}

/**
 * Initialize the unix domain socket and
 * message which will transfer/contain the reports
 */
void NodeMonitor::InitializeIPC() {

    ipcSocket_ = socket(PF_UNIX, SOCK_DGRAM, 0);
    if (ipcSocket_ != -1) {
        fcntl(ipcSocket_, F_SETFD, FD_CLOEXEC);
    }

    memset(&ipcAddr_, 0, sizeof(ipcAddr_));
    ipcAddr_.sun_family = AF_UNIX;

    strncpy(ipcAddr_.sun_path, _ipcMonitorPath.c_str(),
            sizeof(ipcAddr_.sun_path));
    ipcAddrLen_ = sizeof(ipcAddr_.sun_family) + strlen(ipcAddr_.sun_path) + 1;

    memset(&msg_, 0, sizeof(msg_));

    msg_.msg_name = &ipcAddr_;
    msg_.msg_namelen = ipcAddrLen_;
    msg_.msg_iovlen = 1;
}


/**
 * Set up process.monitor.gc object and accessors for count/elapsed
 * These can be read from Javascript user code space
 **/
void NodeMonitor::InitializeProcessMonitorGCObject(Napi::Env env) {
    Napi::HandleScope scope(env);

    Napi::Object monitor = getProcessMonitor(env);

    // Create an object called "gc" with accessors for count/elapsed
    // * count is # of times GC has run in this process
    // * elapsed is the total duration for GC in milliseconds
    //
    // The accessors are read-only (getter, no setter).
    Napi::Object gcObj = Napi::Object::New(env);
    gcObj.DefineProperties({
        Napi::PropertyDescriptor::Accessor<GetterGCCount>("count"),
        Napi::PropertyDescriptor::Accessor<GetterGCElapsed>("elapsed"),
    });
    monitor.Set("gc", gcObj);
}

/**
 * Activate the monitor along with any required initialization
 *
 * Installs various callbacks/object setup that are valid only after
 * the monitor is started.
 * Spawns a thread to monitor stats every REPORT_INTERVAL_MS
 */
void NodeMonitor::Start(Napi::Env env) {

    assert( 0 != instance_ );

    // No need to do anything if we're already running
    if ( running_ ) {
        return;
    }
    running_ = true;
    stopRequested_ = false;

    InstallGCEventCallbacks();
    InitializeIPC();

    /* Use a pipe to let the signal handler (which will likely be
       executed in another thread) break out of pselect().
       This is the standard DJ Bernstein pipe for handling signals
       in multi-thread programs technique - http://cr.yp.to/docs/selfpipe.html */
    {
        /* \todo - make these local to the NodeMonitor rather than global */
        int fd[2];
        if (pipe(fd)) {
            perror("Can't create pipe");
        }
        sigpipefd_r = fd[0];
        sigpipefd_w = fd[1];
        fcntl(sigpipefd_r, F_SETFL, fcntl(sigpipefd_r, F_GETFL) | O_NONBLOCK );
        fcntl(sigpipefd_r, F_SETFD, FD_CLOEXEC );
        fcntl(sigpipefd_w, F_SETFL, fcntl(sigpipefd_w, F_GETFL) | O_NONBLOCK );
        fcntl(sigpipefd_w, F_SETFD, FD_CLOEXEC );
    }


    // Create a thread-safe function so the monitr pthread can schedule
    // setStatistics() back onto the JS thread.  This is the modern N-API
    // replacement for the previous uv_async_t handle + uv_async_send.
    //
    // The JS callback is a no-op: the real work happens in the native
    // [](Napi::Env, Napi::Function) callback passed to NonBlockingCall().
    tsfn_ = Napi::ThreadSafeFunction::New(
        env,
        Napi::Function::New(env, [](const Napi::CallbackInfo&) {}),
        "monitr",
        0,   // unlimited queue
        1);  // one producer thread (the monitr pthread)

    // Don't let the monitor keep the event loop (and hence the process) alive.
    tsfn_.Unref(env);

    // Go ahead and create the monitr pthread
    {
        int rc;
        rc = pthread_create(&tmonitor_, NULL, monitorNodeThread, NULL);
        if (0 != rc) perror("pthread_create");
    }
}


/**
 * Calculate and update statistics about current NodeJS process
 *
 * This function is invoked by libuv when "signalled" from the
 * separate monitr thread, thus ensuring it runs inside the main libuv
 * event loop (i.e. the same nodejs thread that runs v8).  Note: This signal
 * is invoked by the uv_async_send() function which lets libuv know to
 * invoke this callback at the next possible occasion.
 *
 * By doing so, we can call pure Javascript functions to obtain values
 * that are collected within the Javascript layer itself,
 * e.g. process.monitor.getRequestCount()
 *
 * The statistics themselves will then be sent via a UDP datagram
 * at the conclusion of the next reporting interval (REPORT_INTERVAL_MS)
 * by the monitr pthread
 **/
void NodeMonitor::setStatistics(Napi::Env env) {

    pending_ = 0;
    loop_timestamp_ = uv_hrtime();
    loop_count_++;

    {   // obtain heap memory usage ratio (V8 direct - no N-API equivalent)
        v8::HeapStatistics v8stats;
        isolate_->GetHeapStatistics(&v8stats);

        stats_.pmem_ = (v8stats.used_heap_size() / (double) v8stats.total_heap_size());
    }

    {   // Obtains the CPU usage
        float scpu = 0.0;
        float ucpu = 0.0;
        long int uticks = 0;
        long int sticks = 0;

        cpuTrackerSync_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

        // Get total number of requests since monitr started
        unsigned int totalReqs = getIntFunction(env, "getTotalRequestCount");
        unsigned int reqDelta = totalReqs - stats_.lastRequests_;

        // Update the number of requests processed
        // and the ratio CPU/req.
        stats_.lastRequests_ = totalReqs;
        stats_.lastCpuPerReq_ = (reqDelta <= 0) ? 0 : (scpu + ucpu) / reqDelta;
        stats_.lastJiffiesPerReq_ = (reqDelta <= 0) ? 0
            : ((float) (sticks + uticks)) / reqDelta;

        // Request delta - requests since last check.
        stats_.lastReqDelta_ = reqDelta;
    }

    {
        struct timeval cur_time = { 0, 0 };
        // Get the current time
        gettimeofday(&cur_time, NULL);

        // milliseconds
        long timeDelta = (cur_time.tv_sec * 1000 + cur_time.tv_usec / 1000)
            - (stats_.lastTime_.tv_sec * 1000 + stats_.lastTime_.tv_usec / 1000);

        // Update time
        stats_.lastTime_.tv_sec = cur_time.tv_sec;
        stats_.lastTime_.tv_usec = cur_time.tv_usec;

        // Last RPS
        stats_.lastRPS_ = (int) (stats_.lastReqDelta_ / ( timeDelta / 1000.0));

        // Get currently open requests
        stats_.currentOpenReqs_ = getIntFunction(env, "getRequestCount");

        // currently open connections
        stats_.currentOpenConns_ = getIntFunction(env, "getOpenConnections");

        // Kb of transferred data
        float dataTransferred = ((float) (getIntFunction(env, "getTransferred"))) / 1024;

        stats_.lastKBytesSecond = (dataTransferred - stats_.lastKBytesTransfered_) / (((double) timeDelta) / 1000);
        stats_.lastKBytesTransfered_ = dataTransferred;
    }

    stats_.healthIsDown_ = getBooleanFunction(env, "isDown");
    stats_.healthStatusCode_ = getIntFunction(env, "getStatusCode");
    stats_.healthStatusTimestamp_ = (time_t) getIntFunction(env, "getStatusTimestamp");

}

CpuUsageTracker::CpuUsageTracker() {
    memset(&lastUsage_, 0, sizeof(CpuUsage));
    memset(&currentUsage_, 0, sizeof(CpuUsage));

    // Read it once, to be able to compare
    float ucpu_usage = 0.0f, scpu_usage = 0.0f;
    long int uticks, sticks;
    GetCurrent(&ucpu_usage, &scpu_usage, &uticks, &sticks);
}

int CpuUsageTracker::GetCurrent(float* ucpu_usage, float* scpu_usage, long int * uticks, long int* sticks) {
    int err = 0;
    if (!(err = ReadCpuUsage(&currentUsage_))) {
        *ucpu_usage = 0.0f;
        *scpu_usage = 0.0f;
        *uticks = 0;
        *sticks = 0;

        if (lastUsage_.init) {
            CalculateCpuUsage(&currentUsage_, &lastUsage_, ucpu_usage, scpu_usage, uticks, sticks);
        }

        // Copy the results
        lastUsage_ = currentUsage_;
        lastUsage_.init = true;
    }
    return err;
}

// return 0 on success, -1 on error
int CpuUsageTracker::ReadCpuUsage(CpuUsage* result) {

    //convert  pid to string
    char pid_s[20];
    pid_t pid = getpid();

    snprintf(pid_s, sizeof(pid_s), "%d", pid);
    char stat_filepath[30] = "/proc/";
    strncat(stat_filepath, pid_s, sizeof(stat_filepath) - strlen(stat_filepath) - 1);
    strncat(stat_filepath, "/stat", sizeof(stat_filepath) - strlen(stat_filepath) - 1);

    //open /proc/pid/stat
    FILE *fpstat = fopen(stat_filepath, "r");
    if (fpstat == NULL) {
        printf("FOPEN ERROR pid stat %s:\n", stat_filepath);
        return -1;
    }

    //open /proc/stat
    FILE *fstat = fopen("/proc/stat", "r");
    if (fstat == NULL) {
        printf("FOPEN ERROR");
        fclose(fstat);
        return -1;
    }
    memset(result, 0, sizeof(CpuUsage));

    //read values from /proc/pid/stat
    if (fscanf(
        fpstat,
        "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %ld %ld",
        &result->utime_ticks, &result->stime_ticks, &result->cutime_ticks,
        &result->cstime_ticks) == EOF) {
        fclose(fpstat);
        fclose(fstat);
        return -1;
    }
    fclose(fpstat);

    //read+calc cpu total time from /proc/stat, on linux 2.6.35-23 x86_64 the cpu row has 10values could differ on different architectures :/
    long unsigned int cpu_time[10] = { 0 };
    memset(cpu_time, 0, sizeof(cpu_time));
    if (fscanf(fstat, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
        &cpu_time[0], &cpu_time[1], &cpu_time[2], &cpu_time[3],
        &cpu_time[4], &cpu_time[5], &cpu_time[6], &cpu_time[7],
        &cpu_time[8], &cpu_time[9]) == EOF) {
        fclose(fstat);
        return -1;
    }
    fclose(fstat);

    for (int i = 0; i < 10; i++) {
        result->cpu_total_time += cpu_time[i];
    }

    return 0;
}

void CpuUsageTracker::CalculateCpuUsage(CpuUsage* cur_usage,
    CpuUsage* last_usage, float* ucpu_usage, float* scpu_usage,
    long int * uticks, long int* sticks) {
    long unsigned int curTotalDiff = cur_usage->cpu_total_time - last_usage->cpu_total_time;

    if (curTotalDiff > 0) {
        *ucpu_usage = 100 * ((((cur_usage->utime_ticks
            + cur_usage->cutime_ticks) - (last_usage->utime_ticks
            + last_usage->cutime_ticks))) / ((float) curTotalDiff));

        *scpu_usage = 100 * ((((cur_usage->stime_ticks
            + cur_usage->cstime_ticks) - (last_usage->stime_ticks
            + last_usage->cstime_ticks))) / ((float) curTotalDiff));
    } else {
        *ucpu_usage = 0.0f;
        *scpu_usage = 0.0f;
    }
    *uticks = ((cur_usage->utime_ticks + cur_usage->cutime_ticks)
        - (last_usage->utime_ticks + last_usage->cutime_ticks));
    *sticks = ((cur_usage->stime_ticks + cur_usage->cstime_ticks)
        - (last_usage->stime_ticks + last_usage->cstime_ticks));
}

// Invoke a zero-arg function on process.monitor by name.
// Returns the function's result, or null if it doesn't exist.
// Must be called on the JS thread.
static Napi::Value callFunction(Napi::Env env, const char* funcName) {
    Napi::Object monitor = getProcessMonitor(env);

    // Does funcName function exist on process.monitor object?
    Napi::Value fval = monitor.Get(funcName);
    if (fval.IsFunction()) {
        return fval.As<Napi::Function>().Call(env.Global(), {});
    }
    return env.Null();
}


GCUsageTracker::GCUsage::GCUsage() {
    int rc = uv_mutex_init(&lock_);
    if (0 != rc) {
        perror("GCUsage: could not initialize uv_mutex");
    }

    bzero( &stats_, sizeof(GCStat) );
    startTime_ = 0;
}

GCUsageTracker::GCUsage::~GCUsage() {
    uv_mutex_destroy(&lock_);
}

/** Start() is called at the start of a GC event.
 *
 *  It runs inside the v8 isolate, so it is able to make calls to
 *  Javascript functions, etc if required
 **/
void GCUsageTracker::GCUsage::Start() {
    startTime_ = uv_hrtime();
}

/** Stop() is called at the end of a GC event.
 *
 *  It runs inside the v8 isolate, so it is able to make
 *  calls to Javascript functions, etc if required
 **/
uint64_t GCUsageTracker::GCUsage::Stop() {
    assert(0 != startTime_);
    uint64_t elapsed = uv_hrtime() - startTime_;
    {
        // We need this lock to prevent the profiling monitr
        // thread from potentially clearing the stats_ instance
        // variable at the same time as we update it
        ScopedUVLock scope( &lock_ );

        stats_.numCalls++;
        stats_.cumulativeTime += elapsed;
        if (elapsed > stats_.maxTime) {
            stats_.maxTime = elapsed;
        }
    }
    return elapsed;
}


/** EndInterval() returns the last period of GC stats
 *
 *  It is called by the monitr profiling thread, so it is
 *  not running inside the v8 isolate.  This means it needs
 *  to be careful not to allocate memory on the v8 heap.
 **/
const GCStat GCUsageTracker::GCUsage::EndInterval() {
    GCStat lastStat;
    {
        ScopedUVLock scope( &lock_ );

        lastStat = stats_;
        // now clear out stats since we've saved the last values
        bzero( &stats_, sizeof(GCStat) );

        // but don't clear out the startTime_ since we may be in
        // the middle of another GC when this EndInterval is called
        // by the profiling thread
    }
    return lastStat;
}

// calls a Javascript function which returns an integer result
int NodeMonitor::getIntFunction(Napi::Env env, const char* funcName) {
    Napi::HandleScope scope(env);
    Napi::Value res = callFunction(env, funcName);
    if (res.IsNumber()) {
        return res.As<Napi::Number>().Uint32Value();
    }
    return 0;
}

// calls a Javascript function which returns a boolean result
bool NodeMonitor::getBooleanFunction(Napi::Env env, const char* funcName) {
    Napi::HandleScope scope(env);
    Napi::Value res = callFunction(env, funcName);
    if (res.IsBoolean()) {
        return res.As<Napi::Boolean>().Value();
    }
    return false;
}

NodeMonitor& NodeMonitor::getInstance() {
    assert( 0 != instance_ );
    return *instance_;
}

/**
 * Sends UDP JSON datagram approximately once per REPORT_INTERVAL_MS
 *
 * Executed in the monitr pthread, *not* from a pthread executing v8
 * Therefore, we can't directly access any Javascript functions/vars
 * and we need to ensure we don't stomp on any variables being modified
 * by the running v8 Javascript thread
 **/
bool NodeMonitor::sendReport() {
    static pid_t pid = getpid();
    static double minOverHead = 0;

    // See how many reports have been processed since last call to this function
    unsigned int diff_count = loop_count_ - last_loop_count_ + 1;
    last_loop_count_ = loop_count_;

    // The difference between current and previous time (in milliseconds)
    // ({loop,start}_timestamp_ is set using libuv's uv_hrtime())
    double ts_diff = (loop_timestamp_ - start_timestamp_) / 1.0e6;

    // Obtains the CPU usage
    float scpu = 0.0f;
    float ucpu = 0.0f;
    float cpusum = 0.0f;
    long int uticks = 0;
    long int sticks = 0;

    cpuTracker_.GetCurrent(&ucpu, &scpu, &uticks, &sticks);

    cpusum = ucpu + scpu;

    // Obtain the time elapsed since last event
    if (ts_diff >= 0) {
        if (minOverHead == 0.0 || minOverHead > ts_diff) {
            minOverHead = ts_diff;
        }
    } else {
        ts_diff = -ts_diff;
    }

    consumption_ = cpusum;
    Statistics& stats = stats_;

    const int k_MAX_BUFLENGTH = 100;
    char buffer[k_MAX_BUFLENGTH];

    // data must result in a valid JSON object, although we don't validate this!
    string data = "{\"status\":{";
    snprintf(buffer, sizeof(buffer), "\"cluster\":%d,", getpgid(0));
    data.append(buffer);
    snprintf(buffer, sizeof(buffer), "\"pid\":%d,", pid);
    data.append(buffer);
    snprintf(buffer, sizeof(buffer), "\"cpu\":%.2f,", ucpu + scpu);

    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"user_cpu\":%.2f,", ucpu);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"sys_cpu\":%.2f,", scpu);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"cpuperreq\":%.6f,", stats.lastCpuPerReq_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"jiffyperreq\":%.6f,", stats.lastJiffiesPerReq_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"events\":%d,", diff_count);
    data.append(buffer);

    snprintf(buffer, sizeof(buffer), "\"elapsed\":%.2f,", ts_diff);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    snprintf(buffer, sizeof(buffer), "\"ts\":%.2f,", loop_timestamp_ / 1.0e6);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // memory
    snprintf(buffer, sizeof(buffer), "\"mem\":%.2f,", stats.pmem_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // requests served since beginning
    snprintf(buffer, sizeof(buffer), "\"reqstotal\":%d,", stats.lastRequests_);
    data.append(buffer);

    // RPS
    snprintf(buffer, sizeof(buffer), "\"rps\":%d,", stats.lastRPS_);
    data.append(buffer);

    // open requests
    snprintf(buffer, sizeof(buffer), "\"oreqs\":%d,", stats.currentOpenReqs_);
    data.append(buffer);

    // startTime of the process.
    snprintf(buffer, sizeof(buffer), "\"utcstart\":%d,", (int) startTime);
    data.append(buffer);

    // open connections
    snprintf(buffer, sizeof(buffer), "\"oconns\":%d,", stats.currentOpenConns_);
    data.append(buffer);

    // Kb transferred
    snprintf(buffer, sizeof(buffer), "\"kb_trans\":%.2f,", stats.lastKBytesTransfered_);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    // Kb transferred per second
    snprintf(buffer, sizeof(buffer), "\"kbs_out\":%.2f,", stats.lastKBytesSecond);
    if (!strstr(buffer, "nan")) {
        data.append(buffer);
    }

    if (stats.healthStatusTimestamp_ != 0) {
        snprintf(buffer, sizeof(buffer), "\"health_status_timestamp\":%ld,", stats.healthStatusTimestamp_);
        data.append(buffer);

        //Add the rest health statistics only if health timestamp is not 0
        snprintf(buffer, sizeof(buffer), "\"health_is_down\":%s,", (stats.healthIsDown_ ? "true" : "false"));
        data.append(buffer);

        snprintf(buffer, sizeof(buffer), "\"health_status_code\":%d,", stats.healthStatusCode_);
        data.append(buffer);
    }

    // append gc stats
    {
        GCUsageTracker& tracker = getGCUsageTracker();

        snprintf(buffer, sizeof(buffer), "\"gc\":{" );
        data.append(buffer);

        for (int i=0; i < GCUsageTracker::kNumGCTypes; ++i) {
            v8::GCType type = GCUsageTracker::indexTov8GCType(i);
            const GCStat stat = tracker.EndInterval( type );

            snprintf(buffer, sizeof(buffer), "\"%s\":{", GCUsageTracker::indexToString(i) );
            data.append(buffer);

            snprintf(buffer, sizeof(buffer), "\"count\":%lu,\"elapsed_ms\":%1.3f,\"max_ms\":%1.3f},",
                     stat.numCalls, stat.cumulativeTime / 1.0e6, stat.maxTime / 1.0e6 );
            data.append(buffer);
        }

        data.erase(data.size() - 1); //get rid of last comma

        // end the object literal
        snprintf(buffer, sizeof(buffer), "}," );
        data.append(buffer);

    }

    data.erase(data.size() - 1);; //get rid of last comma

    data.append("}}");

    // Construct the datagram pointing to the message
    struct iovec vec;

    vec.iov_base = (void *) data.c_str();
    vec.iov_len = strlen((char *) vec.iov_base);
    msg_.msg_iov = &vec;

    // Send it
    int rc = sendmsg(ipcSocket_, &msg_, MSG_DONTWAIT);

    if (!pending_) {
        // Initialization of start_timestamp_ first time through
        start_timestamp_ = uv_hrtime();
        pending_ = 1;
    }

    // Schedule setStatistics() to run on the JS thread.  This replaces the
    // old uv_async_send(): the TSFN marshals us back onto the event loop
    // where it is safe to call into V8/JS.
    tsfn_.NonBlockingCall([](Napi::Env env, Napi::Function /*jsCallback*/) {
        NodeMonitor::getInstance().setStatistics(env);
    });
    return (rc != -1);
}

/**
 * Stop monitoring
 *
 * Uninstall any callbacks for GC, stop the thread and close socket
 */
void NodeMonitor::Stop() {
    assert( 0 != instance_ );

    if ( !running_ ) {
        return;
    }

    UninstallGCEventCallbacks();

    // Ask the monitor thread to exit and wake it from its pselect() sleep
    // by writing to the self-pipe, then wait for it to finish.
    stopRequested_ = true;
    if (sigpipefd_w != -1) {
        char c = 0;
        ssize_t rc = write(sigpipefd_w, &c, 1);
        (void) rc;  // best-effort wakeup
    }
    pthread_join(tmonitor_, NULL);

    // The producer thread is gone, so it is now safe to release the TSFN.
    if (tsfn_) {
        tsfn_.Release();
        tsfn_ = Napi::ThreadSafeFunction();
    }

    close(ipcSocket_);
    ipcSocket_ = -1;

    // Close the self-pipe so a subsequent Start() can recreate it cleanly.
    if (sigpipefd_r != -1) { close(sigpipefd_r); sigpipefd_r = -1; }
    if (sigpipefd_w != -1) { close(sigpipefd_w); sigpipefd_w = -1; }

    running_ = false;
}

/**
 * Create a new monitor instance based on the v8 isolate
 */
NodeMonitor::NodeMonitor(v8::Isolate* isolate) :
    running_(false),
    stopRequested_(false),
    startTime(0),
    gcTracker_(),
    tmonitor_((pthread_t) NULL),
    isolate_(isolate),
    loop_count_(0),
    last_loop_count_(0),
    consumption_(0.0),
    pending_(0),
    ipcAddrLen_(0),
    ipcSocket_(-1)
{
    loop_timestamp_ = start_timestamp_ = uv_hrtime();
    startTime = time(NULL);
    memset(&stats_, 0, sizeof(Statistics));
    memset(&ipcAddr_, 0,sizeof(struct sockaddr_un));
}

NodeMonitor::~NodeMonitor() {
}

static void SignalHangupActionHandler(int signo, siginfo_t* siginfo,  void* context) {
    savedSigInfo = *siginfo;
    hup_fired = 1;
    char c = 0;

    // enforce serial write via signal pipe
    // This will be handled by the monitor thread which is running a pselect() to
    // listen to any events on the other end of this pipe file descriptor pair.
    ssize_t rc = write(sigpipefd_w, &c, 1);
    if (rc < 0) {
        perror("write");
    }
}


// Javascript interface routines
// Javascript getter/setters
static Napi::Value GetterIPCMonitorPath(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), _ipcMonitorPath);
}


// methods that can be called from Javascript
static Napi::Value SetterIPCMonitorPath(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 ||
        (!info[0].IsString() && !info[0].IsUndefined() && !info[0].IsNull())) {
        Napi::TypeError::New(env, "setIpcMonitorPath: expected a string path")
            .ThrowAsJavaScriptException();
        return env.Undefined();
    }
    if (info[0].IsString()) {
        _ipcMonitorPath = info[0].As<Napi::String>().Utf8Value();
    }
    return env.Undefined();
}

static Napi::Value StartMonitor(const Napi::CallbackInfo& info) {
    NodeMonitor::getInstance().Start(info.Env());
    return info.Env().Undefined();
}

static Napi::Value StopMonitor(const Napi::CallbackInfo& info) {
    NodeMonitor::getInstance().Stop();
    return info.Env().Undefined();
}


// main module initialization
static Napi::Object init(Napi::Env env, Napi::Object exports) {
    exports.DefineProperty(
        Napi::PropertyDescriptor::Accessor<GetterIPCMonitorPath>("ipcMonitorPath"));
    exports.Set("setIpcMonitorPath", Napi::Function::New(env, SetterIPCMonitorPath));
    exports.Set("start", Napi::Function::New(env, StartMonitor));
    exports.Set("stop", Napi::Function::New(env, StopMonitor));

    NodeMonitor::Initialize(env);
    RegisterSignalHandler(SIGHUP, SignalHangupActionHandler);

    return exports;
}

NODE_API_MODULE(monitor, init)
}

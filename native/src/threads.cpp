// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's threads, and the objects they synchronize with: mutexes, condition variables,
// read-write locks and semaphores.
//
// The PS4's are FreeBSD's. Each object the game declares (a ScePthreadMutex, pthread_cond_t,
// SceKernelSema, ...) is one pointer-sized handle; init writes into it a pointer to the system's
// own object. Here that object is the loader's, allocated in its own area (SYNC_BASE), so the
// game's heap is laid out as before. A handle still 0 (a statically initialized object) gets
// its object on first use.
//
// The scePthread* calls return SCE_KERNEL_ERROR_* (0x80020000 + the FreeBSD errno); the POSIX
// pthread_* calls return the errno itself.

#include <chrono>
#include <cstring>
#include <format>
#include <new>
#include <thread>

#include "loader.hpp"

namespace mensis {
namespace {

template <typename T> T* ptr(u64 a) { return reinterpret_cast<T*>(a); }

// FreeBSD's errno values
constexpr int E_PERM = 1, E_DEADLK = 11, E_BUSY = 16, E_INVAL = 22, E_AGAIN = 35,
              E_TIMEDOUT = 60, E_CANCELED = 85;

i64 sce(int e) { return e ? i64(std::int32_t(0x80020000u | u32(e))) : 0; }

template <typename T> T* make() {
    return new (ptr<void>(sync_alloc(sizeof(T), alignof(T)))) T();
}

// The object behind a handle, made on first use if the handle is still empty. Objects carry a
// tag, so a handle holding anything else (an uninitialized one) gets a fresh object too.
template <typename T> T* object(u64 handle_at) {
    static std::mutex lock;
    u64* h = ptr<u64>(handle_at);
    auto valid = [&] {
        return *h >= SYNC_BASE && *h < SYNC_BASE + SYNC_RESERVE && ptr<T>(*h)->tag == T::TAG;
    };
    if (valid()) return ptr<T>(*h);
    std::lock_guard g(lock);
    if (!valid()) *h = reinterpret_cast<u64>(make<T>());
    return ptr<T>(*h);
}

GameThread* me() { return current_thread(); }

std::chrono::microseconds usec(u64 n) { return std::chrono::microseconds(n); }

// ---- threads ----

struct ThreadAttr {
    static constexpr u32 TAG = 0x41545448;  // "HTTA"
    u32 tag = TAG;
    u64 stack_size = 1 << 20;
    int detach = 0, policy = 0, priority = 700, inherit = 0;
    u64 affinity = 0x7f, guard = 0x1000;
};

GameThread* by_handle(u64 h) {
    std::lock_guard lock(M.log_lock);
    for (auto& t : M.threads)
        if (t->handle == h) return t.get();
    return nullptr;
}

// scePthreadCreate(ScePthread *thread, const ScePthreadAttr *attr, entry, arg, name)
i64 s_create(const u64* a, std::string& note) {
    ThreadAttr defaults;
    const ThreadAttr* attr = a[1] ? object<ThreadAttr>(a[1]) : &defaults;
    std::string name = a[4] ? ptr<char>(a[4]) : "";
    GameThread* t = new_thread(name, a[2], a[3], 0, attr->stack_size);
    t->detached = attr->detach != 0;
    *ptr<u64>(a[0]) = t->handle;
    t->host = std::thread(run_thread, t);
    t->host.detach();
    note = std::format("thread {}: '{}', stack {:#x}", t->id, name, attr->stack_size);
    return 0;
}

// pthread_create(pthread_t *thread, const pthread_attr_t *attr, entry, arg)
i64 s_pthread_create(const u64* a, std::string& note) {
    u64 b[6] = {a[0], a[1], a[2], a[3], 0, 0};
    return s_create(b, note);
}

// pthread_create_name_np(pthread_t *thread, const pthread_attr_t *attr, entry, arg, name):
// FreeBSD's, with a name, in scePthreadCreate's order
i64 s_pthread_create_name(const u64* a, std::string& note) { return s_create(a, note); }

i64 s_self(const u64*, std::string&) { return i64(me()->handle); }

// scePthreadExit(void *value): back out of the thread
i64 s_exit(const u64* a, std::string&) {
    GameThread* t = me();
    {
        std::lock_guard lock(t->m);
        t->exited = true;
        t->result = a[0];
    }
    if (t->id == 0) stop("the main thread called scePthreadExit");
    mensis_restore(t->enter.saved_rsp);
}

// scePthreadJoin(ScePthread thread, void **value)
i64 join(const u64* a, bool posix) {
    GameThread* t = by_handle(a[0]);
    if (!t) return posix ? E_INVAL : sce(E_INVAL);
    std::unique_lock lock(t->m);
    t->cv.wait(lock, [&] { return t->finished; });
    if (a[1]) *ptr<u64>(a[1]) = t->result;
    return 0;
}
i64 s_join(const u64* a, std::string&) { return join(a, false); }
i64 s_pthread_join(const u64* a, std::string&) { return join(a, true); }

i64 s_detach(const u64* a, std::string&) {
    if (GameThread* t = by_handle(a[0])) t->detached = true;
    return 0;
}

i64 s_yield(const u64*, std::string&) {
    std::this_thread::yield();
    return 0;
}

// scePthreadGetprio(thread, int *prio): the PS4's default priority
i64 s_getprio(const u64* a, std::string&) {
    *ptr<int>(a[1]) = 700;
    return 0;
}

i64 s_rename(const u64* a, std::string&) {
    if (GameThread* t = by_handle(a[0])) {
        std::lock_guard lock(M.log_lock);
        t->name = a[1] ? ptr<char>(a[1]) : "";
    }
    return 0;
}

// sceKernelUsleep(microseconds)
i64 s_usleep(const u64* a, std::string&) {
    std::this_thread::sleep_for(usec(u32(a[0])));
    return 0;
}

i64 s_attr_init(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(make<ThreadAttr>());
    return 0;
}
i64 s_attr_destroy(const u64*, std::string&) { return 0; }
i64 s_attr_setstacksize(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->stack_size = a[1];
    return 0;
}
i64 s_attr_getstacksize(const u64* a, std::string&) {
    *ptr<u64>(a[1]) = object<ThreadAttr>(a[0])->stack_size;
    return 0;
}
i64 s_attr_setdetachstate(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->detach = int(a[1]);
    return 0;
}
i64 s_attr_getdetachstate(const u64* a, std::string&) {
    *ptr<int>(a[1]) = object<ThreadAttr>(a[0])->detach;
    return 0;
}
i64 s_attr_setschedpolicy(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->policy = int(a[1]);
    return 0;
}
// scePthreadAttrSetschedparam(attr*, const SceKernelSchedParam *): { int priority }
i64 s_attr_setschedparam(const u64* a, std::string&) {
    if (a[1]) object<ThreadAttr>(a[0])->priority = *ptr<int>(a[1]);
    return 0;
}
i64 s_attr_getschedparam(const u64* a, std::string&) {
    *ptr<int>(a[1]) = object<ThreadAttr>(a[0])->priority;
    return 0;
}
i64 s_attr_setinheritsched(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->inherit = int(a[1]);
    return 0;
}
i64 s_attr_setaffinity(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->affinity = a[1];
    return 0;
}
i64 s_attr_setguardsize(const u64* a, std::string&) {
    object<ThreadAttr>(a[0])->guard = a[1];
    return 0;
}
// scePthreadAttrGet(ScePthread thread, ScePthreadAttr *attr): describe a running thread
i64 s_attr_get(const u64* a, std::string&) {
    object<ThreadAttr>(a[1]);
    return 0;
}

// ---- mutexes ----

// FreeBSD's types: 1 error-checking (the default), 2 recursive, 3 normal, 4 adaptive
struct MutexAttr {
    static constexpr u32 TAG = 0x5441544d;  // "MTAT"
    u32 tag = TAG;
    int type = 1, protocol = 0;
};

struct Mutex {
    static constexpr u32 TAG = 0x5854554d;  // "MUTX"
    u32 tag = TAG;
    int type = 1;
    std::mutex m;
    std::condition_variable cv;
    GameThread* owner = nullptr;
    int count = 0;
};

// 0, or an errno. timeout < 0: wait for ever; 0: don't wait.
int lock(Mutex* x, i64 timeout_us) {
    GameThread* self = me();
    std::unique_lock g(x->m);
    if (x->owner == self) {
        if (x->type == 2) {
            x->count++;
            return 0;
        }
        return timeout_us == 0 ? E_BUSY : E_DEADLK;
    }
    auto free = [&] { return x->owner == nullptr; };
    if (timeout_us == 0) {
        if (!free()) return E_BUSY;
    } else if (timeout_us < 0) {
        x->cv.wait(g, free);
    } else if (!x->cv.wait_for(g, usec(u64(timeout_us)), free)) {
        return E_TIMEDOUT;
    }
    x->owner = self;
    x->count = 1;
    return 0;
}

int unlock(Mutex* x) {
    std::lock_guard g(x->m);
    if (x->owner != me()) return E_PERM;
    if (--x->count == 0) {
        x->owner = nullptr;
        x->cv.notify_one();
    }
    return 0;
}

// For condition waits: let go however many times it's held, and take it back as many.
int release_all(Mutex* x) {
    std::lock_guard g(x->m);
    int n = x->count;
    x->owner = nullptr;
    x->count = 0;
    x->cv.notify_one();
    return n;
}
void reacquire(Mutex* x, int n) {
    std::unique_lock g(x->m);
    x->cv.wait(g, [&] { return x->owner == nullptr; });
    x->owner = me();
    x->count = n;
}

i64 mutexattr_init(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(make<MutexAttr>());
    return 0;
}
i64 mutexattr_settype(const u64* a, std::string&) {
    object<MutexAttr>(a[0])->type = int(a[1]);
    return 0;
}
i64 mutexattr_gettype(const u64* a, std::string&) {
    *ptr<int>(a[1]) = object<MutexAttr>(a[0])->type;
    return 0;
}
i64 mutexattr_setprotocol(const u64* a, std::string&) {
    object<MutexAttr>(a[0])->protocol = int(a[1]);
    return 0;
}
i64 nothing(const u64*, std::string&) { return 0; }

// scePthreadMutexInit(ScePthreadMutex *m, const ScePthreadMutexattr *attr, const char *name)
i64 mutex_init(const u64* a, std::string& note) {
    Mutex* x = make<Mutex>();
    if (a[1] && *ptr<u64>(a[1])) x->type = object<MutexAttr>(a[1])->type;
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(x);
    if (a[2]) note = std::format("'{}'", ptr<char>(a[2]));
    return 0;
}
i64 sce_mutex_lock(const u64* a, std::string&) { return sce(lock(object<Mutex>(a[0]), -1)); }
i64 sce_mutex_trylock(const u64* a, std::string&) { return sce(lock(object<Mutex>(a[0]), 0)); }
// scePthreadMutexTimedlock(m*, microseconds)
i64 sce_mutex_timedlock(const u64* a, std::string&) {
    return sce(lock(object<Mutex>(a[0]), i64(u32(a[1]))));
}
i64 sce_mutex_unlock(const u64* a, std::string&) { return sce(unlock(object<Mutex>(a[0]))); }
i64 px_mutex_lock(const u64* a, std::string&) { return lock(object<Mutex>(a[0]), -1); }
i64 px_mutex_trylock(const u64* a, std::string&) { return lock(object<Mutex>(a[0]), 0); }
i64 px_mutex_unlock(const u64* a, std::string&) { return unlock(object<Mutex>(a[0])); }

// ---- condition variables ----

struct Cond {
    static constexpr u32 TAG = 0x444e4f43;  // "COND"
    u32 tag = TAG;
    std::mutex m;
    std::condition_variable cv;
    u64 seq = 0;  // bumped by every signal; a waiter returns once it has moved
};

// 0, or an errno
int wait(Cond* c, Mutex* x, i64 timeout_us) {
    std::unique_lock g(c->m);
    {
        std::lock_guard xg(x->m);
        if (x->owner != me()) return E_PERM;
    }
    int held = release_all(x);
    u64 seq = c->seq;
    bool woken = true;
    if (timeout_us < 0)
        c->cv.wait(g, [&] { return c->seq != seq; });
    else
        woken = c->cv.wait_for(g, usec(u64(timeout_us)), [&] { return c->seq != seq; });
    g.unlock();
    reacquire(x, held);
    return woken ? 0 : E_TIMEDOUT;
}

i64 cond_init(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(make<Cond>());
    return 0;
}
i64 cond_signal(const u64* a, std::string&) {
    Cond* c = object<Cond>(a[0]);
    std::lock_guard g(c->m);
    c->seq++;
    c->cv.notify_one();
    return 0;
}
i64 cond_broadcast(const u64* a, std::string&) {
    Cond* c = object<Cond>(a[0]);
    std::lock_guard g(c->m);
    c->seq++;
    c->cv.notify_all();
    return 0;
}
i64 sce_cond_wait(const u64* a, std::string&) {
    return sce(wait(object<Cond>(a[0]), object<Mutex>(a[1]), -1));
}
// scePthreadCondTimedwait(cond*, mutex*, microseconds)
i64 sce_cond_timedwait(const u64* a, std::string&) {
    return sce(wait(object<Cond>(a[0]), object<Mutex>(a[1]), i64(u32(a[2]))));
}
i64 px_cond_wait(const u64* a, std::string&) {
    return wait(object<Cond>(a[0]), object<Mutex>(a[1]), -1);
}
// pthread_cond_timedwait(cond*, mutex*, const struct timespec *abstime): against the real clock
i64 px_cond_timedwait(const u64* a, std::string&) {
    const i64* ts = ptr<i64>(a[2]);
    auto now = std::chrono::system_clock::now().time_since_epoch();
    i64 deadline_us = ts[0] * 1000000 + ts[1] / 1000;
    i64 now_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return wait(object<Cond>(a[0]), object<Mutex>(a[1]), std::max<i64>(deadline_us - now_us, 0));
}

// ---- read-write locks ----

struct Rwlock {
    static constexpr u32 TAG = 0x4b4c5752;  // "RWLK"
    u32 tag = TAG;
    std::mutex m;
    std::condition_variable cv;
    int readers = 0;
    GameThread* writer = nullptr;
    int writes = 0;  // a writer may take it again
};

int rdlock(Rwlock* r, bool try_) {
    std::unique_lock g(r->m);
    if (r->writer == me()) return E_DEADLK;
    auto free = [&] { return r->writer == nullptr; };
    if (try_ && !free()) return E_BUSY;
    r->cv.wait(g, free);
    r->readers++;
    return 0;
}
int wrlock(Rwlock* r, bool try_) {
    std::unique_lock g(r->m);
    if (r->writer == me()) {
        r->writes++;
        return 0;
    }
    auto free = [&] { return r->writer == nullptr && r->readers == 0; };
    if (try_ && !free()) return E_BUSY;
    r->cv.wait(g, free);
    r->writer = me();
    r->writes = 1;
    return 0;
}
int rwunlock(Rwlock* r) {
    std::lock_guard g(r->m);
    if (r->writer == me()) {
        if (--r->writes == 0) r->writer = nullptr;
    } else if (r->readers > 0) {
        r->readers--;
    } else {
        return E_PERM;
    }
    r->cv.notify_all();
    return 0;
}

i64 rwlock_init(const u64* a, std::string&) {
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(make<Rwlock>());
    return 0;
}
i64 sce_rdlock(const u64* a, std::string&) { return sce(rdlock(object<Rwlock>(a[0]), false)); }
i64 sce_wrlock(const u64* a, std::string&) { return sce(wrlock(object<Rwlock>(a[0]), false)); }
i64 sce_tryrdlock(const u64* a, std::string&) { return sce(rdlock(object<Rwlock>(a[0]), true)); }
i64 sce_trywrlock(const u64* a, std::string&) { return sce(wrlock(object<Rwlock>(a[0]), true)); }
i64 sce_rwunlock(const u64* a, std::string&) { return sce(rwunlock(object<Rwlock>(a[0]))); }

// ---- semaphores ----

struct Sema {
    static constexpr u32 TAG = 0x414d4553;  // "SEMA"
    u32 tag = TAG;
    std::mutex m;
    std::condition_variable cv;
    int count = 0, max = 0;
    u64 cancels = 0;  // bumped by sceKernelCancelSema
    int waiting = 0;
};

// sceKernelCreateSema(SceKernelSema *sem, const char *name, attr, int init, int max, opt*)
i64 sema_create(const u64* a, std::string& note) {
    Sema* s = make<Sema>();
    s->count = int(a[3]);
    s->max = int(a[4]);
    *ptr<u64>(a[0]) = reinterpret_cast<u64>(s);
    note = std::format("'{}' {}/{}", a[1] ? ptr<char>(a[1]) : "", s->count, s->max);
    return 0;
}

Sema* sema(u64 h) {
    Sema* s = ptr<Sema>(h);
    return h >= SYNC_BASE && h < SYNC_BASE + SYNC_RESERVE && s->tag == Sema::TAG ? s : nullptr;
}

// sceKernelWaitSema(sem, int need, u32 *timeout_us)
i64 sema_wait(const u64* a, std::string&) {
    Sema* s = sema(a[0]);
    if (!s) return sce(E_INVAL);
    int need = int(a[1]);
    u32* timeout = a[2] ? ptr<u32>(a[2]) : nullptr;
    std::unique_lock g(s->m);
    u64 cancels = s->cancels;
    auto ready = [&] { return s->count >= need || s->cancels != cancels; };
    s->waiting++;
    bool ok = true;
    if (!timeout) {
        s->cv.wait(g, ready);
    } else {
        auto start = std::chrono::steady_clock::now();
        ok = s->cv.wait_for(g, usec(*timeout), ready);
        auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start).count();
        *timeout = ok ? u32(std::max<i64>(i64(*timeout) - spent, 0)) : 0;
    }
    s->waiting--;
    if (!ok) return sce(E_TIMEDOUT);
    if (s->cancels != cancels) return sce(E_CANCELED);
    s->count -= need;
    return 0;
}

// sceKernelSignalSema(sem, int count)
i64 sema_signal(const u64* a, std::string&) {
    Sema* s = sema(a[0]);
    if (!s) return sce(E_INVAL);
    std::lock_guard g(s->m);
    if (s->count + int(a[1]) > s->max) return sce(E_INVAL);
    s->count += int(a[1]);
    s->cv.notify_all();
    return 0;
}

// sceKernelPollSema(sem, int need)
i64 sema_poll(const u64* a, std::string&) {
    Sema* s = sema(a[0]);
    if (!s) return sce(E_INVAL);
    std::lock_guard g(s->m);
    if (s->count < int(a[1])) return sce(E_AGAIN);
    s->count -= int(a[1]);
    return 0;
}

// sceKernelCancelSema(sem, int set_count, int *waiting)
i64 sema_cancel(const u64* a, std::string&) {
    Sema* s = sema(a[0]);
    if (!s) return sce(E_INVAL);
    std::lock_guard g(s->m);
    if (a[2]) *ptr<int>(a[2]) = s->waiting;
    if (int(a[1]) >= 0) s->count = int(a[1]);
    s->cancels++;
    s->cv.notify_all();
    return 0;
}

}  // namespace

void add_thread_standins(std::unordered_map<std::string, Standin>& t) {
    std::pair<const char*, Standin> list[] = {
        {"scePthreadCreate", s_create},
        {"pthread_create", s_pthread_create},
        {"pthread_create_name_np", s_pthread_create_name},
        {"scePthreadSelf", s_self},
        {"pthread_self", s_self},
        {"scePthreadExit", s_exit},
        {"pthread_exit", s_exit},
        {"scePthreadJoin", s_join},
        {"pthread_join", s_pthread_join},
        {"scePthreadDetach", s_detach},
        {"pthread_detach", s_detach},
        {"scePthreadYield", s_yield},
        {"pthread_yield", s_yield},
        {"sched_yield", s_yield},
        {"scePthreadGetprio", s_getprio},
        {"scePthreadSetprio", nothing},
        {"scePthreadSetaffinity", nothing},
        {"scePthreadRename", s_rename},
        {"sceKernelUsleep", s_usleep},
        {"usleep", s_usleep},
        {"scePthreadAttrInit", s_attr_init},
        {"scePthreadAttrDestroy", s_attr_destroy},
        {"scePthreadAttrSetstacksize", s_attr_setstacksize},
        {"scePthreadAttrGetstacksize", s_attr_getstacksize},
        {"scePthreadAttrSetdetachstate", s_attr_setdetachstate},
        {"scePthreadAttrGetdetachstate", s_attr_getdetachstate},
        {"scePthreadAttrSetschedpolicy", s_attr_setschedpolicy},
        {"scePthreadAttrSetschedparam", s_attr_setschedparam},
        {"scePthreadAttrGetschedparam", s_attr_getschedparam},
        {"scePthreadAttrSetinheritsched", s_attr_setinheritsched},
        {"scePthreadAttrSetaffinity", s_attr_setaffinity},
        {"scePthreadAttrSetguardsize", s_attr_setguardsize},
        {"scePthreadAttrGet", s_attr_get},
        {"pthread_attr_init", s_attr_init},
        {"pthread_attr_destroy", s_attr_destroy},
        {"pthread_attr_setstacksize", s_attr_setstacksize},
        {"pthread_attr_setdetachstate", s_attr_setdetachstate},

        {"scePthreadMutexattrInit", mutexattr_init},
        {"scePthreadMutexattrDestroy", nothing},
        {"scePthreadMutexattrSettype", mutexattr_settype},
        {"scePthreadMutexattrGettype", mutexattr_gettype},
        {"scePthreadMutexattrSetprotocol", mutexattr_setprotocol},
        {"scePthreadMutexInit", mutex_init},
        {"scePthreadMutexDestroy", nothing},
        {"scePthreadMutexLock", sce_mutex_lock},
        {"scePthreadMutexTrylock", sce_mutex_trylock},
        {"scePthreadMutexTimedlock", sce_mutex_timedlock},
        {"scePthreadMutexUnlock", sce_mutex_unlock},
        {"pthread_mutexattr_init", mutexattr_init},
        {"pthread_mutexattr_destroy", nothing},
        {"pthread_mutexattr_settype", mutexattr_settype},
        {"pthread_mutexattr_gettype", mutexattr_gettype},
        {"pthread_mutex_init", mutex_init},
        {"pthread_mutex_destroy", nothing},
        {"pthread_mutex_lock", px_mutex_lock},
        {"pthread_mutex_trylock", px_mutex_trylock},
        {"pthread_mutex_unlock", px_mutex_unlock},

        {"scePthreadCondattrInit", nothing},
        {"scePthreadCondattrDestroy", nothing},
        {"scePthreadCondInit", cond_init},
        {"scePthreadCondDestroy", nothing},
        {"scePthreadCondWait", sce_cond_wait},
        {"scePthreadCondTimedwait", sce_cond_timedwait},
        {"scePthreadCondSignal", cond_signal},
        {"scePthreadCondBroadcast", cond_broadcast},
        {"pthread_condattr_init", nothing},
        {"pthread_condattr_destroy", nothing},
        {"pthread_cond_init", cond_init},
        {"pthread_cond_destroy", nothing},
        {"pthread_cond_wait", px_cond_wait},
        {"pthread_cond_timedwait", px_cond_timedwait},
        {"pthread_cond_signal", cond_signal},
        {"pthread_cond_broadcast", cond_broadcast},

        {"scePthreadRwlockattrInit", nothing},
        {"scePthreadRwlockattrDestroy", nothing},
        {"scePthreadRwlockInit", rwlock_init},
        {"scePthreadRwlockDestroy", nothing},
        {"scePthreadRwlockRdlock", sce_rdlock},
        {"scePthreadRwlockWrlock", sce_wrlock},
        {"scePthreadRwlockTryrdlock", sce_tryrdlock},
        {"scePthreadRwlockTrywrlock", sce_trywrlock},
        {"scePthreadRwlockUnlock", sce_rwunlock},

        {"sceKernelCreateSema", sema_create},
        {"sceKernelDeleteSema", nothing},
        {"sceKernelWaitSema", sema_wait},
        {"sceKernelSignalSema", sema_signal},
        {"sceKernelPollSema", sema_poll},
        {"sceKernelCancelSema", sema_cancel},
    };
    for (auto [name, fn] : list) t.insert_or_assign(name, fn);
}

}  // namespace mensis

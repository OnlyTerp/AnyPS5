#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREAD_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_HOSTTHREAD_HPP

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace HostThread {

using Entry = void* (*)(void* argument);

inline bool StartDetached(std::size_t stackBytes, Entry entry, void* argument) {
#ifdef _WIN32
    if (stackBytes > std::numeric_limits<unsigned>::max()) throw std::invalid_argument("host thread stack size exceeds the Windows limit");
    struct Start {
        Entry entry;
        void* argument;
    };
    auto start = std::make_unique<Start>(Start{entry, argument});
    const auto handle = _beginthreadex(nullptr, static_cast<unsigned>(stackBytes), [](void* opaque) -> unsigned {
        const std::unique_ptr<Start> owned(static_cast<Start*>(opaque));
        owned->entry(owned->argument);
        return 0;
    }, start.get(), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle == 0) return false;
    start.release();
    CloseHandle(reinterpret_cast<HANDLE>(handle));
    return true;
#else
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_t thread;
    const bool started = pthread_attr_setstacksize(&attributes, stackBytes) == 0 && pthread_create(&thread, &attributes, entry, argument) == 0;
    pthread_attr_destroy(&attributes);
    if (started) pthread_detach(thread);
    return started;
#endif
}

}

#endif

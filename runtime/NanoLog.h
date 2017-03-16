/* Copyright (c) 2016-2017 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef NANO_LOG_H
#define NANO_LOG_H

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <aio.h>
#include <assert.h>

#include "Config.h"
#include "Common.h"
#include "Fence.h"
#include "Util.h"

// These header files are needed for the in-lined runtime code. They are
// included here so that the user of the NanoLog system only has to
// include one file.
#include <string.h>         /* strlen + memcpy */
#include "Log.h"            //TODO(syang0) We don't need the ENTIRE log.h...

/**
 * NanoLog provides runtime support to the C++ code generated by the
 * Preprocessor component and serves as the main interface for the user.
 * Its main responsibilities are to manage fast thread-local storage to stage
 * uncompressed log messages and manage a background thread to compress the
 * log messages to an output file.
 */
class NanoLog {
public:
    // User API
    static void printStats();
    static void printConfig();
    static void preallocate();
    static void setLogFile(const char* filename);
    static void sync();

    /// Internal API after this point
    /**
     * Allocate thread-local space for the generated C++ code to store an
     * uncompressed log message, but do not make it available for compression
     * yet. The caller should invoke finishAlloc() to make the space visible
     * to the compression thread and this function shall not be invoked
     * again until the corresponding finishAlloc() is invoked first.
     *
     * Note this will block of the buffer is full.
     *
     * \param nbytes
     *      number of bytes to allocate in the
     *
     * \return
     *      pointer to the allocated space
     */
    static inline char*
    __internal_reserveAlloc(size_t nbytes)
    {
        if (stagingBuffer == nullptr)
            nanoLogSingleton.ensureStagingBufferAllocated();

        return stagingBuffer->reserveProducerSpace(nbytes);
    }

    /**
     * Complement to reserveAlloc, makes the bytes previously reserveAlloc()-ed
     * visible to the compression/output thread.
     *
     * \param nbytes
     *      Number of bytes to make visible
     */
    static inline void
    __internal_finishAlloc(size_t nbytes){
        stagingBuffer->finishReservation(nbytes);
    }

PRIVATE:
    // Forward Declarations
    class StagingBuffer;
    class StagingBufferDestroyer;

    // Storage for staging uncompressed log statements for compression
    static __thread StagingBuffer* stagingBuffer;

    // Destroys the __thread StagingBuffer upon its own destruction, which
    // is synchronized with thread death
    static thread_local StagingBufferDestroyer sbc;

    // Singleton NanoLog that manages the thread-local structures and
    // background output thread.
    static NanoLog nanoLogSingleton;

    NanoLog();
    ~NanoLog();

    void compressionThreadMain();
    void printStatsInternal();
    void setLogFile_internal(const char* filename);
    void waitForAIO();

    /**
     * Allocates thread-local structures if they weren't already allocated.
     * This is used by the generated C++ code to ensure it has space to
     * log uncompressed messages to and by the user if they wish to
     * preallocate the data structures on thread creation.
     */
    inline void
    ensureStagingBufferAllocated()
    {
        if (stagingBuffer == nullptr) {
            std::unique_lock<std::mutex> guard(bufferMutex);
            uint32_t bufferId = nextBufferId++;

            // Unlocked for the expensive StagingBuffer allocation
            guard.unlock();
            stagingBuffer = new StagingBuffer(bufferId);
            guard.lock();

            threadBuffers.push_back(stagingBuffer);
        }
    }

    // Globally the thread-local stagingBuffers
    std::vector<StagingBuffer*> threadBuffers;

    // Stores the id for the next StagingBuffer to be allocated. The ids are
    // unique for this execution for each StagingBuffer allocation.
    uint32_t nextBufferId = 1;

    // Protects reads and writes to threadBuffers
    std::mutex bufferMutex;

    // Background thread that polls the various staging buffers, compresses
    // the staged log messages, and outputs it to a file.
    std::thread compressionThread;

    // Indicates there's an operation in aioCb that should be waited on
    bool hasOutstandingOperation;

    // Flag signaling the compressionThread to stop running
    bool compressionThreadShouldExit;

    // Indicates that a sync request has been made but is not completed
    // by the background thread yet.
    bool syncRequested;

    // Protects the condition variables below
    std::mutex condMutex;

    // Signal for when the compression thread should wakeup
    std::condition_variable workAdded;

    // Signaled when the LogCompressor makes a complete pass through all the
    // thread staging buffers and finds no log messages to output.
    std::condition_variable hintQueueEmptied;

    // File handle for the output file; should only be opened once at the
    // construction of the LogCompressor
    int outputFd;

    // POSIX AIO structure used to communicate async IO requests
    struct aiocb aioCb;

    // Used to stage the compressed log messages before passing it on to the
    // POSIX AIO library.

    // Dynamically allocated buffer to stage compressed log message before
    // handing it over to the POSIX AIO library for output.
    char *compressingBuffer;

    // Dynamically allocated double buffer that is swapped with the
    // compressingBuffer when the latter is passed to the POSIX AIO library.
    char *outputDoubleBuffer;

    // Marks the rdtsc() when the current compression thread first started
    // running. A value of 0 indicates the compression thread is not running.
    uint64_t cycleAtThreadStart;

    // Metric: Number of cycles compression thread is alive
    uint64_t cyclesAwake;

    // Metric: Amount of time spent compressing the dynamic log data
    uint64_t cyclesCompressing;

    // Metric: Amount of time spent scanning the buffers for work and
    // compressing events found.
    uint64_t cyclesScanningAndCompressing;

    // Metric: Amount of time spent on fsync() and writes. Note that if posix
    // AIO is used, the only the amount of time it takes to submit the job is
    // recorded.
    uint64_t cyclesAioAndFsync;

    // Metric: Number of bytes read in from the staging buffers
    uint64_t totalBytesRead;

    // Metric: Number of bytes written to the output file (includes padding)
    uint64_t totalBytesWritten;

    // Metric: Number of pad bytes written to round the file to the nearest 512B
    uint64_t padBytesWritten;

    // Metric: Number of events compressed and outputted.
    uint64_t eventsProcessed;

    // Metric: Number of times an AIO write was completed.
    uint32_t numAioWritesCompleted;

    /**
     * Implements a circular FIFO producer/consumer byte queue that is used
     * to hold the dynamic information of a NanoLog log statement (producer)
     * as it waits for compression via the NanoLog background thread
     * (consumer). There exists a StagingBuffer for every thread that uses
     * the NanoLog system.
     */
    class StagingBuffer {
    public:
        /**
         * Attempt to reserve contiguous space for the producer without
         * making it visible to the consumer. The caller should invoke
         * finishReservation() before invoking reserveProducerSpace()
         * again to make the bytes reserved visible to the consumer.
         *
         * This mechanism is in place to allow the producer to initialize
         * the contents of the reservation before exposing it to the consumer.
         * This function will block behind the consumer if there's not
         * enough space.
         *
         * \param nbytes
         *      Number of bytes to allocate
         *
         * \return
         *      Pointer to at least nbytes of contiguous space
         */
        inline char*
        reserveProducerSpace(size_t nbytes) {
            // Fast in-line path
            if (nbytes < minFreeSpace)
                return producerPos;

            // Slow allocation
            return reserveSpaceInternal(nbytes);
        }

        /**
         * Complement to reserveProducerSpace that makes nbytes starting from
         * the return of reserveProducerSpace visible to the consumer.
         *
         * \param nbytes
         *      Number of bytes to expose to the consumer
         */
        inline void
        finishReservation(size_t nbytes) {
            assert(nbytes < minFreeSpace);
            assert(producerPos + nbytes <
                                storage + NanoLogConfig::STAGING_BUFFER_SIZE);

            Fence::sfence(); // Ensures producer finishes writes before bump
            minFreeSpace -= nbytes;
            producerPos += nbytes;
        }

        char* peek(uint64_t *bytesAvailable);

        /**
         * Consumes the next nbytes in the StagingBuffer and frees it back
         * for the producer to reuse. nbytes must be less than what is
         * returned by peek().
         *
         * \param nbytes
         *      Number of bytes to return back to the producer
         */
        inline void
        consume(uint64_t nbytes) {
            Fence::lfence(); // Make sure consumer reads finish before bump
            consumerPos += nbytes;
        }

        /**
         * Returns true if it's safe for the compression thread to delete
         * the StagingBuffer and remove it from the global vector.
         *
         * \return
         *      true if its safe to delete the StagingBuffer
         */
        bool
        checkCanDelete()
        {
            return shouldDeallocate && consumerPos == producerPos;
        }


        uint32_t getId() {
            return id;
        }

        StagingBuffer(uint32_t bufferId)
            : producerPos(storage)
            , endOfRecordedSpace(storage + NanoLogConfig::STAGING_BUFFER_SIZE)
            , minFreeSpace(NanoLogConfig::STAGING_BUFFER_SIZE)
            , cyclesProducerBlocked(0)
            , cacheLineSpacer()
            , consumerPos(storage)
            , shouldDeallocate(false)
            , id(bufferId)
            , storage()
        {
            // Empty function, but causes the C++ runtime to instantiate the
            // sbc thread_local (see documentation in function).
            sbc.stagingBufferCreated();
        }

        ~StagingBuffer() {
        }
    PRIVATE:
        char* reserveSpaceInternal(size_t nbytes, bool blocking=true);

        // Position within storage[] where the producer may place new data
        char *producerPos;

        // Marks the end of valid data for the consumer. Set by the producer
        // on a roll-over
        char *endOfRecordedSpace;

        // Lower bound on the number of bytes the producer can allocate
        // without rolling over the producerPos or stalling behind the consumer
        uint64_t minFreeSpace;

        // Number of cycles producer was blocked while waiting for space to
        // free up in the StagingBuffer for an allocation.
        uint64_t cyclesProducerBlocked;

        // An extra cache-line to separate the variables that are primarily
        // updated/read by the producer (above) from the ones by the
        // consumer(below)
        char cacheLineSpacer[PerfUtils::Util::BYTES_PER_CACHE_LINE];

        // Position within the storage buffer where the consumer will consume
        // the next bytes from. This value is only updated by the consumer.
        char *consumerPos;

        // Indicates that the thread owning this StagingBuffer has been
        // destructed (i.e. no more messages will be logged to it) and thus
        // should be cleaned up once the buffer has been emptied by the
        // compression thread.
        bool shouldDeallocate;

        // Uniquely identifies this StagingBuffer for this execution. It's
        // similar to ThreadId, but is only assigned to threads that NANO_LOG).
        uint32_t id;

        // Backing store used to implement the circular queue
        char storage[NanoLogConfig::STAGING_BUFFER_SIZE];

        friend StagingBufferDestroyer;

        DISALLOW_COPY_AND_ASSIGN(StagingBuffer);
    };

    // This class is intended to be instantiated as a C++ thread_local to
    // synchronize marking the thread local stagingBuffer for deletion with
    // thread death.
    //
    // The reason why this class exists rather than wrapping the stagingBuffer
    // in a unique_ptr or declaring the stagingBuffer itself to be thread_local
    // is because of performance. Dereferencing the former costs 10 ns and the
    // latter allocates large amounts of resources for every thread that is
    // created, which is wasteful for threads that do not use the NanoLog.
    class StagingBufferDestroyer {
    public:
        // TODO(syang0) I wonder if it'll be better if stagingBuffer was
        // actually a thread_local wrapper with dereference operators
        // implemented.

        explicit StagingBufferDestroyer() {
        }

        // Weird C++ hack; C++ thread_local are instantiated upon first use
        // thus the StagingBuffer has to invoke this function in order
        // to instantiate this object.
        void stagingBufferCreated() { }

        virtual ~StagingBufferDestroyer() {
            if (stagingBuffer != nullptr) {
                stagingBuffer->shouldDeallocate = true;
                stagingBuffer = nullptr;
            }
        }
    };

    DISALLOW_COPY_AND_ASSIGN(NanoLog);
};  // NanoLog

// MUST appear at the very end of the NanoLog.h file, right before the
// last #endif. It serves a marker for the preprocessor for where it can
// start injecting inlined, generated functions.
static const int __internal_dummy_variable_marker_for_code_injection = 0;

#endif /* NANO_LOG_H */


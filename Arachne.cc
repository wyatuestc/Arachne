#include <stdio.h>
#include <thread>
#include "string.h"
#include "Arachne.h"
#include "Cycles.h"

namespace Arachne {

using PerfUtils::Cycles;

enum InitializationState {
    NOT_INITIALIZED,
    INITIALIZING,
    INITIALIZED
};

void schedulerMainLoop();


InitializationState initializationState = NOT_INITIALIZED;
unsigned numCores = 1;

/**
 * Work to do on each thread. 
 * TODO(hq6): If vector is too slow, we may switch to a linked list.  */
std::vector<std::deque<UserContext*> > workQueues;


/**
 * Threads that are sleeping and waiting.
 */
static std::vector<std::deque<UserContext*> > sleepQueues;

/**
 * Protect each work queue.
 */
SpinLock *workQueueLocks;
thread_local int kernelThreadId;
std::vector<std::deque<void*> > stackPool;

/**
 * These two values store the place in the kernel thread to return to when a
 * user task either yields or completes.
 */
thread_local void* libraryStackPointer;
thread_local UserContext *running;

/**
 * When a user thread finishes and it is not the last runnable thread, it sets
 * this pointer to indicate that the structure must be cleaned up.
 */
thread_local UserContext* oldContext;

/**
  * This structure holds the flags, functions, and arguments for the main functions of user threads passed across cores.
  * TODO: In a NUMA world, it may make more sense if this is TaskBox** to allow
  * cores on different NUMA nodes to allocate from the memory that is closer to
  * them.
  */
TaskBox* taskBoxes;


/**
 * This function will allocate stacks and create kernel threads pinned to particular cores.
 *
 * Note that the setting of initializationState should probably use memory
 * barriers, but we are currently assuming that the application will only call
 * it from a single thread of execution.
 *
 * Calling the function twice is a no-op.
 */
void threadInit() {
    if (initializationState != NOT_INITIALIZED) {
        return;
    }
    initializationState = INITIALIZING;

    // Allocate stacks. Note that number of cores is actually number of
    // hyperthreaded cores, rather than necessarily real CPU cores.
    numCores = std::thread::hardware_concurrency(); 
//    printf("numCores = %u\n", numCores);
    workQueueLocks = new SpinLock[numCores];
    taskBoxes = new TaskBox[numCores];
    for (unsigned int i = 0; i < numCores; i++) {
        workQueues.push_back(std::deque<UserContext*>());
        sleepQueues.push_back(std::deque<UserContext*>());

        // Initialize stack pool for each kernel thread
        stackPool.push_back(std::deque<void*>());
        for (int j = 0; j < stackPoolSize; j++)
            stackPool[i].push_back(malloc(stackSize));

        // Leave one thread for the main thread
        if (i != numCores - 1) {
            std::thread(threadMainFunction, i).detach();
        }

    }

    // Set the kernelThreadId for the main thread
    kernelThreadId = numCores - 1;

    initializationState = INITIALIZED;
}

/**
 * Main function for a kernel-level thread participating in the thread pool. 
 */
void threadMainFunction(int id) {
    // Switch to a user stack for symmetry, so that we can deallocate it ourselves later.
    // If we are the last user thread on this core, we will never return since we will simply poll for work.
    // If we are not the last, we will context switch out of this function and have our
    // stack deallocated by the thread we swap to (perhaps in the unblock
    // function), the next time Arachne gets control.
    // The original thread given by the kernel is simply discarded in the
    // current implementation.
    oldContext = NULL;
    kernelThreadId = id;
    auto stack = stackPool[kernelThreadId].front();
    stackPool[kernelThreadId].pop_front();
    running = new UserContext;
    running->stack = stack;
    asm("movq %0, %%rsp" :: "r" (stack));

    schedulerMainLoop();
}

/**
 * When there is a need for a new runnable thread, either because there are
 * none, or because there are more tasks than current threads, this function is
 * invoked to allocate a new stack and switch to it on the current core.
 */
void createNewRunnableThread() {
    void** saved = &running->sp;
    auto stack = stackPool[kernelThreadId].front();
    stackPool[kernelThreadId].pop_front();

    running = new UserContext;
    running->stack = stack;

    // Set up the stack to return to the main thread function.
    running->sp = (char*) running->stack + stackSize - 64; 
    *(void**) running->sp = (void*) schedulerMainLoop;
    savecontext(&running->sp);

    // Switch to the new thread and start polling for work there.
    swapcontext(saved, &running->sp);
}

/**
 * Load a new context without saving anything.
 */
void  __attribute__ ((noinline))  setcontext(void **saved) {

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp");

    asm("popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
 * Save a context of the currently executing process.
 */
void  __attribute__ ((noinline))  savecontext(void **target) {
    // Load the new stack pointer, push the registers, and then restore the old stack pointer.

    asm("movq %rsp, %r11\n\t"
        "movq (%rdi), %rsp\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq  %rsp, (%rdi)\n\t"
        "movq %r11, %rsp"
        );
}

/**
 * Save one set of registers and load another set.
 * %rdi, %rsi are the two addresses of where stack pointers are stored.
 *
 * Load from saved and store into target.
 */
void  __attribute__ ((noinline))  swapcontext(void **saved, void **target) {

    // Save the registers and store the stack pointer
    asm("pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rbx\n\t"
        "pushq %rbp\n\t"
        "movq  %rsp, (%rsi)");

    // Load the stack pointer and restore the registers
    asm("movq (%rdi), %rsp\n\t"
        "popq %rbp\n\t"
        "popq %rbx\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12");
}

/**
  * This function runs the scheduler on a fresh stack when there are no
  * runnable threads on our core.
  * If other threads become runnable during this call, then this loop will exit
  * by arranging for the destruction of the current thread and switching to one
  * of the other runnable threads.
  */
void schedulerMainLoop() {
    // At most one user thread on each core should be going through this loop
    // at any given time.  Most threads should be inside runThread, and only
    // re-enter the thread library by making an API call into Arachne.
    while (true) {
        // Poll for work on my taskBox and take it off first so that we avoid
        // blocking other create requests onto our core.
        if (taskBoxes[kernelThreadId].state.loadState.load() == FILLED) {

            // Copy the task onto the local stack
            auto task = taskBoxes[kernelThreadId].getTask();

            auto expectedTaskState = FILLED; // Because of compare_exchange_strong requires a reference
            taskBoxes[kernelThreadId].state.loadState.compare_exchange_strong(expectedTaskState, EMPTY);
            reinterpret_cast<TaskBase*>(&task)->runThread();
        }
        
        checkSleepQueue();
        {
            // Determine whether we are the last runnable thread.
            std::lock_guard<SpinLock> guard(workQueueLocks[kernelThreadId]);
            if (workQueues[kernelThreadId].empty()) continue;
            else { // Arrange for our stack to be deallocated, and swap out
                // Only ever need to delete oldContext if we are about to overwrite it.
                if (oldContext != NULL) {
                    stackPool[kernelThreadId].push_front(oldContext->stack);
                    delete oldContext;
                }
                oldContext = running;
                running = workQueues[kernelThreadId].front();
                workQueues[kernelThreadId].pop_front();
                // We expect this new thread to free our stack and also clean
                // up our UserContext before any other thread can replace
                // oldContext.
                guard.~lock_guard<SpinLock>();
                setcontext(&running->sp);
            }
        }

//        // Resume right after here when user task finishes or yields
//        // Check if the currently running user thread is finished and recycle
//        // its stack if it is.
//        if (running->finished) {
//            stackPool[kernelThreadId].push_front(running->stack);
//            delete running;
//        }
    }
}

/**
 * Restore control back to the thread library.
 * Assume we are the running process in the current kernel thread if we are calling
 * yield.
 */
void yield() {

    // Poll for incoming task.
    if (taskBoxes[kernelThreadId].state.loadState.load() == FILLED) {
        createNewRunnableThread();
    }
    checkSleepQueue();
    workQueueLocks[kernelThreadId].lock();
    if (workQueues[kernelThreadId].empty()) {
        workQueueLocks[kernelThreadId].unlock();
        return; // Yield is noop if there is no longer work to be done.
    }

    void** saved = &running->sp;

    workQueues[kernelThreadId].push_back(running);
    running = workQueues[kernelThreadId].front();
    workQueues[kernelThreadId].pop_front();
    workQueueLocks[kernelThreadId].unlock();

    // Loop until we find a thread to run, in case there are new threads that
    // do not have a thread.
    swapcontext(&running->sp, saved);
}
void checkSleepQueue() {
    uint64_t currentCycles = Cycles::rdtsc();
    auto& sleepQueue = sleepQueues[kernelThreadId];

    workQueueLocks[kernelThreadId].lock();
    // Assume sorted and move it off the list
    while (sleepQueue.size() > 0 && sleepQueue[0]->wakeUpTimeInCycles < currentCycles) {
        // Move onto the ready queue
        workQueues[kernelThreadId].push_back(sleepQueue[0]);
        sleepQueue.pop_front();  
    }
    workQueueLocks[kernelThreadId].unlock();
}

// Sleep for at least the argument number of ns.
// We keep in core-resident to avoid cross-core cache coherency concerns.
// It must be the case that this function returns after at least ns have
// passed.
void sleep(uint64_t ns) {
    running->wakeUpTimeInCycles = Cycles::rdtsc() + Cycles::fromNanoseconds(ns);

    auto& sleepQueue = sleepQueues[kernelThreadId];
    // TODO(hq6): Sort this by wake-up time using possibly binary search
    if (sleepQueue.size() == 0) sleepQueue.push_back(running);
    else {
        auto it = sleepQueue.begin();
        for (; it != sleepQueue.end() ; it++)
            if ((*it)->wakeUpTimeInCycles > running->wakeUpTimeInCycles) {
                sleepQueue.insert(it, running);
                break;
            }
        // Insert now
        if (it == sleepQueue.end()) {
            sleepQueue.push_back(running);
        }
    }

    // Poll for incoming task.
    if (taskBoxes[kernelThreadId].state.loadState.load() == FILLED) {
        createNewRunnableThread();
    }
        
    checkSleepQueue();

    workQueueLocks[kernelThreadId].lock();
    if (workQueues[kernelThreadId].empty()) {
        workQueueLocks[kernelThreadId].unlock();
        // Create an idle thread that runs the main scheduler loop and swap to it, so
        // we're ready for new work with a new stack as soon as it becomes
        // available.
        createNewRunnableThread();

        // Return after swapcontext returns because that means we have awoken from our sleep
        return;
    }

    // There are other runnable threads, so we simply switch to the first one.
    void** saved = &running->sp;
    running = workQueues[kernelThreadId].front();
    workQueues[kernelThreadId].pop_front();
    workQueueLocks[kernelThreadId].unlock();
    swapcontext(&running->sp, saved);
}



/**
 * This is a special function to allow the main thread to join the thread pool
 * after seeding initial tasks for itself and possibly other threads.
 *
 * The other way of implementing this is to merge this function with the
 * threadInit, and ask the user to provide an initial task to run in the main
 * thread, which will presumably spawn other tasks.
 */
void mainThreadJoinPool() {
    threadMainFunction(numCores - 1);
}

}

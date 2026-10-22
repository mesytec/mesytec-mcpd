#include "mcpd_py_lib.h"

#include <optional>
#include <pybind11/eval.h>
#include <pybind11/gil_safe_call_once.h>

#include <mesytec-mcpd/util/logging.h>

namespace py = pybind11;

namespace mesytec::mcpd::py_lib
{

namespace
{
// WorkerBase methods may be called with or without the GIL held (python
// bindings vs. C++ tests). They must not hold it while waiting on the mutex or
// joining the worker, because the worker thread needs it to put packets into
// the queue.
struct ReleaseGilIfHeld
{
    std::optional<py::gil_scoped_release> release;
    ReleaseGilIfHeld()
    {
        if (PyGILState_Check())
            release.emplace();
    }
};

// put(), get() and shutdown() as implemented in python 3.13.
const char *QueueBackportSource = R"(
import queue
from time import monotonic as time


class ShutDown(Exception):
    """Raised when put/get with shut-down queue."""


class Queue(queue.Queue):
    def __init__(self, maxsize=0):
        super().__init__(maxsize)
        self.is_shutdown = False

    def put(self, item, block=True, timeout=None):
        with self.not_full:
            if self.is_shutdown:
                raise ShutDown
            if self.maxsize > 0:
                if not block:
                    if self._qsize() >= self.maxsize:
                        raise queue.Full
                elif timeout is None:
                    while self._qsize() >= self.maxsize:
                        self.not_full.wait()
                        if self.is_shutdown:
                            raise ShutDown
                elif timeout < 0:
                    raise ValueError("'timeout' must be a non-negative number")
                else:
                    endtime = time() + timeout
                    while self._qsize() >= self.maxsize:
                        remaining = endtime - time()
                        if remaining <= 0.0:
                            raise queue.Full
                        self.not_full.wait(remaining)
                        if self.is_shutdown:
                            raise ShutDown
            self._put(item)
            self.unfinished_tasks += 1
            self.not_empty.notify()

    def get(self, block=True, timeout=None):
        with self.not_empty:
            if self.is_shutdown and not self._qsize():
                raise ShutDown
            if not block:
                if not self._qsize():
                    raise queue.Empty
            elif timeout is None:
                while not self._qsize():
                    self.not_empty.wait()
                    if self.is_shutdown and not self._qsize():
                        raise ShutDown
            elif timeout < 0:
                raise ValueError("'timeout' must be a non-negative number")
            else:
                endtime = time() + timeout
                while not self._qsize():
                    remaining = endtime - time()
                    if remaining <= 0.0:
                        raise queue.Empty
                    self.not_empty.wait(remaining)
                    if self.is_shutdown and not self._qsize():
                        raise ShutDown
            item = self._get()
            self.not_full.notify()
            return item

    def shutdown(self, immediate=False):
        with self.mutex:
            self.is_shutdown = True
            if immediate:
                while self._qsize():
                    self._get()
                    if self.unfinished_tasks > 0:
                        self.unfinished_tasks -= 1
                self.all_tasks_done.notify_all()
            self.not_empty.notify_all()
            self.not_full.notify_all()
)";
} // namespace

const QueueTypes &queueTypes()
{
    PYBIND11_CONSTINIT static py::gil_safe_call_once_and_store<QueueTypes> storage;

    return storage
        .call_once_and_store_result(
            []
            {
                py::object mod = py::module_::import("queue");

                if (!py::hasattr(mod, "ShutDown"))
                {
                    mod = py::module_::import("types").attr("ModuleType")(
                        "mesytec_mcpd._queue_backport");
                    py::exec(QueueBackportSource, mod.attr("__dict__"));
                }

                return QueueTypes{mod.attr("Queue"), mod.attr("ShutDown")};
            })
        .get_stored();
}

PyQueueTap::PyQueueTap(py::handle queue, bool blocking)
    : queue_(queue)
    , blocking_(blocking)
{
}

void PyQueueTap::consume(const ReceivedPacket &rp)
{
    if (shutDown_)
        return;

    py::gil_scoped_acquire gil;

    try
    {
        AugmentedDataPacket aug{rp.packet, rp.srcAddr, rp.srcPort, {}};
        queue_.attr("put")(std::move(aug), blocking_);
    }
    catch (py::error_already_set &e)
    {
        if (e.matches(queueTypes().shutDown))
            shutDown_ = true;
        else if (e.matches(py::module_::import("queue").attr("Full")))
            ++dropped_;
        else
            throw;
    }
}

void PyQueueTap::finished()
{
    py::gil_scoped_acquire gil;
    queue_.attr("shutdown")(false);
}

WorkerBase::WorkerBase(size_t queueSize)
    : queueSize_(queueSize)
    , queue_(queueTypes().queue(queueSize))
{
}

WorkerBase::~WorkerBase()
{
    ReleaseGilIfHeld release;

    try
    {
        stop(true);
    }
    catch (const std::exception &e)
    {
        spdlog::error("{}: exception from stop(): {}", PRETTY_FUNCTION, e.what());
    }
}

bool WorkerBase::start()
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);

    if (worker_ && worker_->isRunning())
    {
        spdlog::warn("{}: already running, not starting again", PRETTY_FUNCTION);
        return false;
    }

    auto source = makeSource();

    {
        py::gil_scoped_acquire gil;
        if (queueUsed_)
            queue_ = queueTypes().queue(queueSize_);
        queueUsed_ = true;
    }

    // Join a previous worker that ended by itself (end of replay, exception).
    worker_.reset();

    stats_ = std::make_shared<SourceStatsCollector>();
    tap_ = std::make_shared<PyQueueTap>(queue_, blockingQueue());
    worker_ = std::make_unique<ReadoutWorker>(
        std::move(source), nullptr, std::vector<std::shared_ptr<PacketConsumer>>{stats_, tap_});
    worker_->start();
    return true;
}

bool WorkerBase::stop(bool immediate)
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!worker_)
        return false;

    const bool wasRunning = worker_->isRunning();

    {
        // Wakes up a worker blocked in queue.put() and consumers blocked in get().
        py::gil_scoped_acquire gil;
        queue_.attr("shutdown")(immediate);
    }

    worker_->stop();
    return wasRunning;
}

bool WorkerBase::isRunning() const
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_ && worker_->isRunning();
}

bool WorkerBase::hasException() const
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_ && worker_->hasException();
}

void WorkerBase::rethrowException()
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_)
        worker_->rethrowException();
}

Counters WorkerBase::getCounters() const
{
    ReleaseGilIfHeld release;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!worker_)
        return {};

    const auto wc = worker_->getCounters();
    const auto totals = stats_->getTotals();

    Counters result;
    result.packets = wc.packets;
    result.bytes = wc.bytes;
    result.timeouts = wc.timeouts;
    result.events = totals.events;
    result.packetsLost = totals.packetsLost;
    result.packetsDropped = tap_->dropped();
    return result;
}

Readout::Readout(int listenPort, size_t queueSize)
    : WorkerBase(queueSize)
    , listenPort_(listenPort)
{
}

std::unique_ptr<PacketSource> Readout::makeSource()
{
    auto source = std::make_unique<UdpPacketSource>(listenPort_);
    localPort_ = source->localPort();
    return source;
}

Replay::Replay(size_t queueSize)
    : WorkerBase(queueSize)
{
}

Replay::Replay(const std::string &filename, size_t queueSize)
    : WorkerBase(queueSize)
    , filename_(filename)
{
}

std::unique_ptr<PacketSource> Replay::makeSource()
{
    return std::make_unique<ListfilePacketSource>(filename_);
}

} // namespace mesytec::mcpd::py_lib

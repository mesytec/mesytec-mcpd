#ifndef E7B93B2B_DB43_49A2_A29F_480864086D05
#define E7B93B2B_DB43_49A2_A29F_480864086D05

#include <atomic>
#include <memory>
#include <mutex>
#include <pybind11/pybind11.h>
#include <vector>

#include <mesytec-mcpd/mesytec-mcpd.h>

namespace mesytec::mcpd::py_lib
{

namespace py = pybind11;

struct Counters
{
    u64 packets = 0u;
    u64 bytes = 0u;
    u64 timeouts = 0u;
    u64 events = 0u;
    u64 packetsLost = 0u;
    u64 packetsDropped = 0u;
};

const size_t DefaultQueueSize = 1000;

// queue.Queue and queue.ShutDown on python >= 3.13. On older versions a
// queue.Queue subclass backporting shutdown() and its ShutDown exception.
struct QueueTypes
{
    py::object queue;
    py::object shutDown;
};

const QueueTypes &queueTypes();

struct AugmentedDataPacket
{
    DataPacket packet;
    u32 srcAddr;
    u16 srcPort;
    mutable std::vector<u64> rawEvents; // buffer for raw event data, allocated on demand

    py::buffer_info getBufferInfo() const
    {
        if (rawEvents.empty())
        {
            const auto eventCount = get_event_count(packet);
            rawEvents.reserve(eventCount);

            for (size_t i = 0; i < eventCount; ++i)
                rawEvents.push_back(get_event(packet, i));
        }

        return py::buffer_info(
            rawEvents.data(),           // Pointer to buffer
            sizeof(u64),               // Size of one scalar
            py::format_descriptor<u64>::format(), // Python struct-style format descriptor
            1,                         // Number of dimensions
            {rawEvents.size()},        // Buffer dimensions
            {sizeof(u64)}              // Strides (in bytes) for each index
        );
    }
};

// Puts every packet into a python queue.Queue as an AugmentedDataPacket. With
// blocking=false full-queue drops are counted, blocking=true applies
// backpressure (used for replays). Shuts the queue down when the worker loop
// ends so that consumers blocked in get() wake up.
class PyQueueTap: public PacketConsumer
{
  public:
    // The queue object must outlive the tap.
    PyQueueTap(py::handle queue, bool blocking);

    void consume(const ReceivedPacket &rp) override;
    void finished() override;

    u64 dropped() const { return dropped_; }

  private:
    py::handle queue_;
    bool blocking_;
    bool shutDown_ = false; // only accessed from the worker thread
    std::atomic<u64> dropped_ = 0;
};

// Python facing readout/replay: a ReadoutWorker feeding a queue.Queue. A new
// queue is created for each run after the first one because a shut down
// queue cannot be reused: call get_queue() after start().
class WorkerBase
{
  public:
    explicit WorkerBase(size_t queueSize = DefaultQueueSize);
    virtual ~WorkerBase();

    // Returns false if already running. Throws if the source cannot be set up.
    bool start();
    // Returns false if not running. immediate=true discards queued packets.
    bool stop(bool immediate = false);
    bool isRunning() const;
    bool hasException() const;
    void rethrowException();

    py::object getQueue() const { return queue_; }

    Counters getCounters() const;

  protected:
    virtual std::unique_ptr<PacketSource> makeSource() = 0;
    virtual bool blockingQueue() const = 0;

  private:
    WorkerBase(const WorkerBase &) = delete;
    WorkerBase &operator=(const WorkerBase &) = delete;

    size_t queueSize_;
    py::object queue_;
    bool queueUsed_ = false;
    mutable std::mutex mutex_; // serializes start/stop
    std::shared_ptr<SourceStatsCollector> stats_;
    std::shared_ptr<PyQueueTap> tap_;
    std::unique_ptr<ReadoutWorker> worker_;
};

class Readout: public WorkerBase
{
  public:
    explicit Readout(int listenPort = McpdDefaultPort, size_t queueSize = DefaultQueueSize);

    // Port bound by the last start(). Useful with listenPort=0.
    u16 localPort() const { return localPort_; }

  protected:
    std::unique_ptr<PacketSource> makeSource() override;
    bool blockingQueue() const override { return false; }

  private:
    int listenPort_;
    std::atomic<u16> localPort_ = 0;
};

class Replay: public WorkerBase
{
  public:
    explicit Replay(size_t queueSize = DefaultQueueSize);
    explicit Replay(const std::string &filename, size_t queueSize = DefaultQueueSize);

  protected:
    std::unique_ptr<PacketSource> makeSource() override;
    bool blockingQueue() const override { return true; }

  private:
    std::string filename_;
};

} // namespace mesytec::mcpd::py_lib

#endif /* E7B93B2B_DB43_49A2_A29F_480864086D05 */

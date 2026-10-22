#include <pybind11/native_enum.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "mcpd_py_commands.h"
#include "mcpd_py_lib.h"
#include "util/logging.h"
#include "util/pybind11_log.h"
#include <mesytec-mcpd/mesytec-mcpd.h>

namespace py = pybind11;
using namespace mesytec::mcpd;

void init_logging()
{
    // mesytec-mcpd links privately against spdlog and so do we. This means we
    // have two copies of spdlog around. The code below sets logger and log
    // level for both instances.
    #if 0
    auto logger = pybind11_log::init_mt("mcpd_py"); // this currently deadlocks :), :(
    spdlog::set_default_logger(logger);
    mesytec::mcpd::set_default_logger(logger);
    #endif
    spdlog::set_level(spdlog::level::info);
    mesytec::mcpd::set_global_log_level(spdlog::level::info);
}

using namespace py_lib;

void init_py_module(py::module_ &m)
{
    m.doc() = "driver library for the mesytec PSD system (MCPD, MPSD, MDLL) - python bindings";
    m.attr("__version__") = library_version();

    // The queue type used by Readout/Replay and the exception raised by
    // get()/put() after shutdown(). Stdlib versions on python >= 3.13.
    m.attr("Queue") = queueTypes().queue;
    m.attr("QueueShutDown") = queueTypes().shutDown;

    m.def("init", []() { init_logging(); });
    m.def(
        "set_log_level",
        [](const std::string &levelName)
        {
            auto level = log_level_from_string(levelName);
            spdlog::set_level(level.value_or(spdlog::level::info));
            mesytec::mcpd::set_global_log_level(level.value_or(spdlog::level::info));
        },
        py::arg("levelName"));

    py::class_<DecodedEvent::Neutron>(m, "Neutron")
        .def(py::init<>())
        .def_readonly("mdpsd_id", &DecodedEvent::Neutron::mpsdId)
        .def_readonly("channel", &DecodedEvent::Neutron::channel)
        .def_readonly("amplitude", &DecodedEvent::Neutron::amplitude)
        .def_readonly("position", &DecodedEvent::Neutron::position);

    py::class_<DecodedEvent::MdllNeutron>(m, "MdllNeutron")
        .def(py::init<>())
        .def_readonly("amplitude", &DecodedEvent::MdllNeutron::amplitude)
        .def_readonly("x_pos", &DecodedEvent::MdllNeutron::xPos)
        .def_readonly("y_pos", &DecodedEvent::MdllNeutron::yPos);

    py::class_<DecodedEvent::Trigger>(m, "Trigger")
        .def(py::init<>())
        .def_readonly("trigger_id", &DecodedEvent::Trigger::triggerId)
        .def_readonly("data_id", &DecodedEvent::Trigger::dataId)
        .def_readonly("value", &DecodedEvent::Trigger::value);

    py::native_enum<EventType>(m, "EventType", "enum.Enum")
        .value("NeutronEvent", EventType::Neutron)
        .value("TriggerEvent", EventType::Trigger)
        .value("MdllNeutronEvent", EventType::MdllNeutron)
        .export_values()
        .finalize();

    py::class_<DecodedEvent>(m, "DecodedEvent")
        .def(py::init<>())
        .def_readonly("deviceId", &DecodedEvent::deviceId)
        .def_readonly("type", &DecodedEvent::type)
        .def_readonly("timestamp", &DecodedEvent::timestamp)                // full stamp
        .def_readonly("event_timestamp", &DecodedEvent::event_timestamp)    // relative event stamp
        .def_readonly("packet_timestamp", &DecodedEvent::packet_timestamp)  // packet header stamp
        .def("neutron",
             [](const DecodedEvent &event) -> py::object
             {
                 if (event.type == EventType::Neutron)
                     return py::cast(event.neutron);
                 else
                     return py::none();
             })
        .def("trigger",
             [](const DecodedEvent &event) -> py::object
             {
                 if (event.type == EventType::Trigger)
                     return py::cast(event.trigger);
                 else
                     return py::none();
             })
        .def("mdll_neutron",
             [](const DecodedEvent &event) -> py::object
             {
                 if (event.type == EventType::MdllNeutron)
                     return py::cast(event.mdllNeutron);
                 else
                     return py::none();
             })
        .def("__str__", [](const DecodedEvent &event) { return to_string(event); });

    py::class_<DataPacket>(m, "DataPacket")
        .def(py::init<>())
        .def_readonly("runId", &DataPacket::runId)
        .def_readonly("device_status", &DataPacket::deviceStatus)
        .def_readonly("device_id", &DataPacket::deviceId)
        .def_readonly("buffer_type", &DataPacket::bufferType)
        .def_readonly("buffer_length", &DataPacket::bufferLength)
        .def_readonly("buffer_number", &DataPacket::bufferNumber)
        .def_property_readonly("time",
                               [](const DataPacket &packet)
                               {
                                   return py::array_t<u16>(
                                       {3},             // shape
                                       {sizeof(u16)},   // stride
                                       packet.time,     // pointer to data
                                       py::cast(packet) // base object to keep alive
                                   );
                               })

        .def_property_readonly("packet_timestamp", [](const DataPacket &packet)
                               { return get_header_timestamp(packet); })

        .def_property_readonly("params",
                               [](const DataPacket &packet)
                               {
                                   return py::array_t<u16>(
                                       {McpdParamCount, McpdParamWords},            // shape
                                       {sizeof(u16) * McpdParamWords, sizeof(u16)}, // stride
                                       &packet.param[0][0], // pointer to data
                                       py::cast(packet)     // base object to keep alive
                                   );
                               })

        .def_property_readonly("data",
                               [](const DataPacket &packet)
                               {
                                   return py::array_t<u16>(
                                       {get_data_length(packet)}, // shape
                                       {sizeof(u16)},             // stride
                                       packet.data,               // pointer to data
                                       py::cast(packet)           // base object to keep alive
                                   );
                               })

        .def("__str__", [](const DataPacket &packet) { return to_string(packet); })

        .def("event_count", [](const DataPacket &packet) { return get_event_count(packet); })

        .def("decode_event", [](const DataPacket &packet, size_t eventNum)
             { return decode_event(packet, eventNum); })

        .def("get_decoded_events",
             [](const DataPacket &packet)
             {
                 const auto eventCount = get_event_count(packet);
                 std::vector<DecodedEvent> events;
                 events.reserve(eventCount);

                 for (size_t i = 0; i < eventCount; ++i)
                     events.push_back(decode_event(packet, i));

                 return events;
             })

        .def("get_raw_events",
             [](const DataPacket &packet)
             {
                 const auto eventCount = get_event_count(packet);
                 auto result = py::array_t<u64>(eventCount); // allocates storage
                 py::buffer_info info = result.request();
                 u64 *ptr = static_cast<u64 *>(info.ptr);

                 for (size_t i = 0; i < eventCount; ++i)
                     ptr[i] = get_event(packet, i);

                 return result;
             })

        .def("get_raw_words",
             [](const DataPacket &packet)
             {
                auto data = reinterpret_cast<const u16 *>(&packet);
                auto wordCount = packet.bufferLength;
                auto result = py::array_t<u16>(wordCount); // allocates storage
                py::buffer_info info = result.request();
                u16 *ptr = static_cast<u16 *>(info.ptr);

                for (size_t i = 0; i < wordCount; ++i)
                    ptr[i] = data[i];

                return result;
             })

             ;


    py::class_<AugmentedDataPacket>(m, "AugmentedDataPacket", py::buffer_protocol())
        .def(py::init<>())
        .def_readonly("packet", &AugmentedDataPacket::packet)
        .def_readonly("src_addr", &AugmentedDataPacket::srcAddr)
        .def_readonly("src_port", &AugmentedDataPacket::srcPort)
        .def_buffer(&AugmentedDataPacket::getBufferInfo);

    py::class_<Counters>(m, "Counters")
        .def(py::init<>())
        .def_readonly("packets", &Counters::packets)
        .def_readonly("bytes", &Counters::bytes)
        .def_readonly("timeouts", &Counters::timeouts)
        .def_readonly("events", &Counters::events)
        .def_readonly("packets_lost", &Counters::packetsLost)
        .def_readonly("packets_dropped", &Counters::packetsDropped)
        .def("__repr__", [] (const Counters &counters)
             {
                 return "mesytec_mcpd_py.Counters(packets=" + std::to_string(counters.packets)
                     + ", bytes=" + std::to_string(counters.bytes)
                     + ", timeouts=" + std::to_string(counters.timeouts)
                     + ", events=" + std::to_string(counters.events)
                     + ", packets_lost=" + std::to_string(counters.packetsLost)
                     + ", packets_dropped=" + std::to_string(counters.packetsDropped) + ")";
             });

    py::class_<WorkerBase>(m, "WorkerBase")
        .def("start", &WorkerBase::start)
        .def("stop", &WorkerBase::stop, py::arg("immediate") = false)
        .def("is_running", &WorkerBase::isRunning)
        .def("has_exception", &WorkerBase::hasException)
        .def("rethrow_exception", &WorkerBase::rethrowException)
        .def("get_queue", &WorkerBase::getQueue)
        .def("get_counters", &WorkerBase::getCounters);

    py::class_<Readout, WorkerBase>(m, "Readout")
        .def(py::init<int, size_t>(), py::arg("listenPort") = McpdDefaultPort,
             py::arg("queue_size") = py_lib::DefaultQueueSize)
        .def_property_readonly("local_port", &Readout::localPort);

    py::class_<Replay, WorkerBase>(m, "Replay")
        .def(py::init<size_t>(), py::arg("queue_size") = py_lib::DefaultQueueSize)
        .def(py::init<const std::string &, size_t>(),
             py::arg("filename"),
             py::arg("queue_size") = py_lib::DefaultQueueSize);

    py::class_<ReadoutWorkerCounters>(m, "ReadoutWorkerCounters")
        .def(py::init<>())
        .def_readonly("packets", &ReadoutWorkerCounters::packets)
        .def_readonly("bytes", &ReadoutWorkerCounters::bytes)
        .def_readonly("timeouts", &ReadoutWorkerCounters::timeouts)
        .def_readonly("invalid_packets", &ReadoutWorkerCounters::invalidPackets)
        .def_readonly("listfile_bytes", &ReadoutWorkerCounters::listfileBytes);

    py::class_<SourceStats>(m, "SourceStats")
        .def(py::init<>())
        .def_readonly("buffer_type", &SourceStats::bufferType)
        .def_readonly("packets", &SourceStats::packets)
        .def_readonly("bytes", &SourceStats::bytes)
        .def_readonly("events", &SourceStats::events)
        .def_readonly("neutron_events", &SourceStats::neutronEvents)
        .def_readonly("trigger_events", &SourceStats::triggerEvents)
        .def_readonly("packets_lost", &SourceStats::packetsLost)
        .def_readonly("buffer_number_jumps", &SourceStats::bufferNumberJumps)
        .def_readonly("last_buffer_number", &SourceStats::lastBufferNumber)
        .def_readonly("last_device_status", &SourceStats::lastDeviceStatus)
        .def_readonly("last_run_id", &SourceStats::lastRunId)
        .def_readonly("last_header_timestamp", &SourceStats::lastHeaderTimestamp)
        .def_property_readonly("last_params", [](const SourceStats &s)
                               { return std::make_tuple(s.lastParams[0], s.lastParams[1],
                                                        s.lastParams[2], s.lastParams[3]); });

    m.def("buffer_number_gap", &buffer_number_gap, py::arg("last"), py::arg("current"),
          "Packets lost between two consecutive buffer numbers, None if the number repeated "
          "or went backwards.");

    // Moves the vector into a numpy array without copying.
    auto to_array = [](std::vector<u64> &&v, std::vector<py::ssize_t> shape)
    {
        auto heap = new std::vector<u64>(std::move(v));
        py::capsule owner(heap, [](void *p) { delete static_cast<std::vector<u64> *>(p); });
        return py::array_t<u64>(shape, heap->data(), owner);
    };

    // Sources are exposed as (src_addr: int, device_id: int) tuples. src_addr is 0 for replays.
    py::class_<Daq>(m, "Daq",
                    "Readout/replay with listfile writing, per source stats and MDLL/MCPD "
                    "histograms. No python code runs in the data path.")
        .def(py::init<u16>(), py::arg("listen_port") = McpdDefaultPort)
        .def("start_readout", &Daq::startReadout, py::arg("listfile") = std::string(),
             py::arg("overwrite") = false, py::call_guard<py::gil_scoped_release>())
        .def("start_replay", &Daq::startReplay, py::arg("listfile"),
             py::call_guard<py::gil_scoped_release>())
        .def("stop", &Daq::stop, py::call_guard<py::gil_scoped_release>())
        .def("is_running", &Daq::isRunning)
        .def("has_exception", &Daq::hasException)
        .def("rethrow_exception", &Daq::rethrowException)
        .def_property_readonly("listen_port", &Daq::listenPort)
        .def_property_readonly("local_port", &Daq::localPort)
        .def_property_readonly("socket_receive_buffer_size", &Daq::socketReceiveBufferSize)
        .def("get_counters", &Daq::getCounters, py::call_guard<py::gil_scoped_release>())
        .def("get_source_stats", [](Daq &daq)
            {
                std::map<SourceKey, SourceStats> stats;
                {
                    py::gil_scoped_release release;
                    stats = daq.stats().getStats();
                }
                py::dict result;
                for (const auto &[key, s]: stats)
                    result[py::make_tuple(key.srcAddr, key.deviceId)] = s;
                return result;
            })
        .def("get_mdll_histograms", [to_array](Daq &daq, u32 srcAddr, u8 deviceId) -> py::object
            {
                std::optional<MdllHistograms> h;
                {
                    py::gil_scoped_release release;
                    h = daq.mdllHistos().getHistograms({srcAddr, deviceId});
                }
                if (!h)
                    return py::none();

                py::dict result;
                result["amplitude"] = to_array(std::move(h->amplitude), {MdllHistograms::AmplitudeBins});
                result["x"] = to_array(std::move(h->x), {MdllHistograms::XBins});
                result["y"] = to_array(std::move(h->y), {MdllHistograms::YBins});
                result["xy"] = to_array(std::move(h->xy), {MdllHistograms::YBins, MdllHistograms::XBins});
                return result;
            }, py::arg("src_addr"), py::arg("device_id"),
            "Copies of the MDLL histograms of a source as a dict of numpy arrays "
            "(amplitude, x, y, xy[y, x]) or None if the source is unknown.")
        .def("get_mcpd_histograms", [to_array](Daq &daq, u32 srcAddr, u8 deviceId) -> py::object
            {
                std::optional<McpdHistograms> h;
                {
                    py::gil_scoped_release release;
                    h = daq.mcpdHistos().getHistograms({srcAddr, deviceId});
                }
                if (!h)
                    return py::none();

                constexpr auto M = McpdHistograms::MpsdCount;
                constexpr auto C = McpdHistograms::ChannelCount;
                py::dict result;
                result["amplitude"] = to_array(std::move(h->amplitude), {M, C, McpdHistograms::AmplitudeBins});
                result["position"] = to_array(std::move(h->position), {M, C, McpdHistograms::PositionBins});
                return result;
            }, py::arg("src_addr"), py::arg("device_id"),
            "Copies of the MCPD/MPSD histograms of a source as a dict of numpy arrays "
            "(amplitude[mpsd, channel, bin], position[mpsd, channel, bin]) or None if the "
            "source is unknown.")
        .def("clear_histograms", &Daq::clearHistograms, py::call_guard<py::gil_scoped_release>())
        .def("reset_stats", &Daq::resetStats, py::call_guard<py::gil_scoped_release>())
        .def("clear_sources", &Daq::clearSources, py::call_guard<py::gil_scoped_release>());

    m.def("format_ipv4", &format_ipv4, py::arg("addr"), "Format a host byte order IPv4 address.");

    // Command/setup API: a thin, exception-based wrapper around the
    // mcpd_*/mdll_*/mpsd_*/mstd_* free functions, one verb per mcpd-cli
    // subcommand.
    py::register_exception<py_lib::McpdError>(m, "McpdError");

    py::native_enum<TimingRole>(m, "TimingRole", "enum.Enum")
        .value("Slave", TimingRole::Slave)
        .value("Master", TimingRole::Master)
        .finalize();

    py::native_enum<BusTermination>(m, "BusTermination", "enum.Enum")
        .value("Off", BusTermination::Off)
        .value("On", BusTermination::On)
        .finalize();

    py::native_enum<CellName>(m, "CellName", "enum.Enum")
        .value("Monitor0", CellName::Monitor0)
        .value("Monitor1", CellName::Monitor1)
        .value("Monitor2", CellName::Monitor2)
        .value("Monitor3", CellName::Monitor3)
        .value("DigitalIn1", CellName::DigitalIn1)
        .value("DigitalIn2", CellName::DigitalIn2)
        .value("ADC1", CellName::ADC1)
        .value("ADC2", CellName::ADC2)
        .finalize();

    py::native_enum<TriggerSource>(m, "TriggerSource", "enum.Enum")
        .value("NoTrigger", TriggerSource::NoTrigger)
        .value("AuxTimer0", TriggerSource::AuxTimer0)
        .value("AuxTimer1", TriggerSource::AuxTimer1)
        .value("AuxTimer2", TriggerSource::AuxTimer2)
        .value("AuxTimer3", TriggerSource::AuxTimer3)
        .value("RisingEdgeRearInput1", TriggerSource::RisingEdgeRearInput1)
        .value("RisingEdgeRearInput2", TriggerSource::RisingEdgeRearInput2)
        .value("CompareRegister", TriggerSource::CompareRegister)
        .finalize();

    py::native_enum<DataSource>(m, "DataSource", "enum.Enum")
        .value("Monitor0", DataSource::Monitor0)
        .value("Monitor1", DataSource::Monitor1)
        .value("Monitor2", DataSource::Monitor2)
        .value("Monitor3", DataSource::Monitor3)
        .value("DigitalIn1", DataSource::DigitalIn1)
        .value("DigitalIn2", DataSource::DigitalIn2)
        .value("AllDigitalAndAdcInputs", DataSource::AllDigitalAndAdcInputs)
        .value("EventCounter", DataSource::EventCounter)
        .value("MasterClock", DataSource::MasterClock)
        .finalize();

    py::native_enum<ChannelPosition>(m, "ChannelPosition", "enum.Enum")
        .value("Left", ChannelPosition::Left)
        .value("Right", ChannelPosition::Right)
        .value("Center", ChannelPosition::Center)
        .finalize();

    py::native_enum<PulserState>(m, "PulserState", "enum.Enum")
        .value("Off", PulserState::Off)
        .value("On", PulserState::On)
        .finalize();

    py::native_enum<MpsdMode>(m, "MpsdMode", "enum.Enum")
        .value("Position", MpsdMode::Position)
        .value("Amplitude", MpsdMode::Amplitude)
        .finalize();

    py::native_enum<MdllChannelPosition>(m, "MdllChannelPosition", "enum.Enum")
        .value("LowerLeft", MdllChannelPosition::LowerLeft)
        .value("Middle", MdllChannelPosition::Middle)
        .value("UpperRight", MdllChannelPosition::UpperRight)
        .finalize();

    py::native_enum<MdllTxDataSet>(m, "MdllTxDataSet", "enum.Enum")
        .value("Default", MdllTxDataSet::Default)
        .value("Timings", MdllTxDataSet::Timings)
        .finalize();

    py::class_<McpdVersionInfo>(m, "McpdVersionInfo")
        .def(py::init<>())
        .def_property_readonly("cpu", [](const McpdVersionInfo &vi)
                               { return std::make_tuple(vi.cpu[0], vi.cpu[1]); })
        .def_property_readonly("fpga", [](const McpdVersionInfo &vi)
                               { return std::make_tuple(vi.fpga[0], vi.fpga[1]); });

    py::class_<McpdParams>(m, "McpdParams")
        .def(py::init<>())
        .def_property_readonly("adc", [](const McpdParams &p)
                               { return std::make_tuple(p.adc[0], p.adc[1]); })
        .def_property_readonly("dac", [](const McpdParams &p)
                               { return std::make_tuple(p.dac[0], p.dac[1]); })
        .def_readonly("ttl_out", &McpdParams::ttlOut)
        .def_readonly("ttl_in", &McpdParams::ttlIn)
        .def_property_readonly("event_counters",
                               [](const McpdParams &p)
                               {
                                   return std::make_tuple(
                                       p.eventCounters[0], p.eventCounters[1],
                                       p.eventCounters[2]);
                               })
        .def_property_readonly("params",
                               [](const McpdParams &p)
                               {
                                   std::array<u64, McpdParamCount> result = {};
                                   for (size_t i = 0; i < McpdParamCount; ++i)
                                       result[i] = to_48bit_value(p.params[i]);
                                   return result;
                               });

    py::class_<BusCapabilities>(m, "BusCapabilities")
        .def(py::init<>())
        .def_readonly("available", &BusCapabilities::available)
        .def_readonly("selected", &BusCapabilities::selected);

    py::class_<MpsdParameters>(m, "MpsdParameters")
        .def(py::init<>())
        .def_readonly("mpsd_id", &MpsdParameters::mpsdId)
        .def_readonly("bus_tx_caps", &MpsdParameters::busTxCaps)
        .def_readonly("tx_format", &MpsdParameters::txFormat)
        .def_readonly("firmware_revision", &MpsdParameters::firmwareRevision);

    // McpdConnection performs blocking network I/O (UDP send/recv with
    // retries, up to ~2.5s per call) directly in the calling C++ function
    // without releasing the GIL itself (unlike Readout/Replay, this class has
    // no pybind11 dependency of its own). Release the GIL for the duration of
    // every blocking call here so other Python threads (e.g. a test fake
    // device, or NICOS's poller) keep running while a command is in flight.
    using GilRelease = py::call_guard<py::gil_scoped_release>;

    py::class_<py_lib::McpdConnection>(m, "McpdConnection")
        .def(
            py::init<const std::string &, unsigned, u16>(), py::arg("address"),
            py::arg("mcpd_id") = 0, py::arg("port") = McpdDefaultPort, GilRelease())
        .def("close", &py_lib::McpdConnection::close)
        .def("is_open", &py_lib::McpdConnection::is_open)
        .def_property(
            "mcpd_id", &py_lib::McpdConnection::mcpd_id, &py_lib::McpdConnection::set_mcpd_id)

        .def("get_version", &py_lib::McpdConnection::get_version, GilRelease())
        .def("set_id", &py_lib::McpdConnection::set_id, py::arg("new_id"), GilRelease())
        .def(
            "set_ip_address", &py_lib::McpdConnection::set_ip_address, py::arg("address"),
            GilRelease())
        .def(
            "set_data_dest_port", &py_lib::McpdConnection::set_data_dest_port, py::arg("port"),
            GilRelease())
        .def(
            "set_ip_address_and_data_dest", &py_lib::McpdConnection::set_ip_address_and_data_dest,
            py::arg("address"), py::arg("data_dest_address"), py::arg("data_dest_port"),
            GilRelease())
        .def("set_run_id", &py_lib::McpdConnection::set_run_id, py::arg("run_id"), GilRelease())
        .def("reset_daq", &py_lib::McpdConnection::reset_daq, GilRelease())
        .def("start_daq", &py_lib::McpdConnection::start_daq, GilRelease())
        .def("stop_daq", &py_lib::McpdConnection::stop_daq, GilRelease())
        .def("continue_daq", &py_lib::McpdConnection::continue_daq, GilRelease())
        .def("get_all_parameters", &py_lib::McpdConnection::get_all_parameters, GilRelease())
        .def("get_bus_capabilities", &py_lib::McpdConnection::get_bus_capabilities, GilRelease())
        .def(
            "set_bus_capabilities", &py_lib::McpdConnection::set_bus_capabilities,
            py::arg("cap_bits"), GilRelease())
        .def(
            "set_timing_options", &py_lib::McpdConnection::set_timing_options, py::arg("role"),
            py::arg("term"), py::arg("ext_sync") = false, GilRelease())
        .def(
            "set_master_clock_value", &py_lib::McpdConnection::set_master_clock_value,
            py::arg("clock"), GilRelease())
        .def(
            "setup_cell", &py_lib::McpdConnection::setup_cell, py::arg("cell"), py::arg("trigger_source"),
            py::arg("compare_register_bit_value"), GilRelease())
        .def(
            "setup_auxtimer", &py_lib::McpdConnection::setup_auxtimer, py::arg("timer_id"),
            py::arg("compare_register_value"), GilRelease())
        .def(
            "set_param_source", &py_lib::McpdConnection::set_param_source, py::arg("param"),
            py::arg("source"), GilRelease())
        .def(
            "set_dac_output_values", &py_lib::McpdConnection::set_dac_output_values, py::arg("dac0_value"),
            py::arg("dac1_value"), GilRelease())
        .def("scan_busses", &py_lib::McpdConnection::scan_busses, GilRelease())
        .def(
            "write_register", &py_lib::McpdConnection::write_register, py::arg("address"),
            py::arg("value"), GilRelease())
        .def(
            "read_register", &py_lib::McpdConnection::read_register, py::arg("address"),
            GilRelease())
        .def(
            "read_peripheral_register", &py_lib::McpdConnection::read_peripheral_register,
            py::arg("mpsd_id"), py::arg("register_number"), GilRelease())
        .def(
            "write_peripheral_register", &py_lib::McpdConnection::write_peripheral_register,
            py::arg("mpsd_id"), py::arg("register_number"), py::arg("register_value"), GilRelease())

        .def(
            "mpsd_set_gain", &py_lib::McpdConnection::mpsd_set_gain, py::arg("mpsd_id"), py::arg("channel"),
            py::arg("gain"), GilRelease())
        .def(
            "mpsd_set_threshold", &py_lib::McpdConnection::mpsd_set_threshold, py::arg("mpsd_id"),
            py::arg("threshold"), GilRelease())
        .def(
            "mpsd_set_pulser", &py_lib::McpdConnection::mpsd_set_pulser, py::arg("mpsd_id"),
            py::arg("channel"), py::arg("position"), py::arg("amplitude"), py::arg("state"),
            GilRelease())
        .def(
            "mpsd_set_mode", &py_lib::McpdConnection::mpsd_set_mode, py::arg("mpsd_id"),
            py::arg("mode"), GilRelease())
        .def(
            "mpsd_set_tx_format", &py_lib::McpdConnection::mpsd_set_tx_format, py::arg("mpsd_id"),
            py::arg("tx_format"), GilRelease())
        .def(
            "mpsd_get_params", &py_lib::McpdConnection::mpsd_get_params, py::arg("mpsd_id"),
            GilRelease())

        .def(
            "mstd_set_gain", &py_lib::McpdConnection::mstd_set_gain, py::arg("mstd_id"), py::arg("channel"),
            py::arg("gain"), GilRelease())

        .def(
            "mdll_set_thresholds", &py_lib::McpdConnection::mdll_set_thresholds, py::arg("threshold_x"),
            py::arg("threshold_y"), py::arg("threshold_anode"), GilRelease())
        .def(
            "mdll_set_spectrum", &py_lib::McpdConnection::mdll_set_spectrum, py::arg("shift_x"),
            py::arg("shift_y"), py::arg("scale_x"), py::arg("scale_y"), GilRelease())
        .def(
            "mdll_set_pulser", &py_lib::McpdConnection::mdll_set_pulser, py::arg("enable"),
            py::arg("amplitude"), py::arg("position"), GilRelease())
        .def(
            "mdll_set_tx_data_set", &py_lib::McpdConnection::mdll_set_tx_data_set,
            py::arg("data_set"), GilRelease())
        .def(
            "mdll_set_timing_window", &py_lib::McpdConnection::mdll_set_timing_window,
            py::arg("t_sum_limit_x_low"), py::arg("t_sum_limit_x_high"),
            py::arg("t_sum_limit_y_low"), py::arg("t_sum_limit_y_high"), GilRelease())
        .def(
            "mdll_set_energy_window", &py_lib::McpdConnection::mdll_set_energy_window,
            py::arg("lower_threshold"), py::arg("upper_threshold"), GilRelease());

    m.def(
        "find_mcpd_id", &py_lib::find_mcpd_id, py::arg("address"),
        py::arg("port") = McpdDefaultPort, GilRelease(),
        "Probe mcpd_id values 0..255 against 'address' until one responds "
        "(MCPD-8_v1 only).");

    // Event field constants (maximum values)
    namespace ec = event_constants;

    py::module_ constants = m.def_submodule("constants", "Event field ranges");

    constants.attr("event_type_shift") = ec::IdShift;
    constants.attr("event_type_mask") = ec::IdMask;
    constants.attr("event_type_bits") = ec::IdBits;
    constants.attr("timestamp_max") = (1u << ec::TimestampBits) - 1;

    py::module_ mdll_neutron =
        constants.def_submodule("mdll_neutron", "MDLL neutron event field ranges");
    mdll_neutron.attr("amplitude_max") = (1u << ec::mdll_neutron::AmplitudeBits) - 1;
    mdll_neutron.attr("x_pos_max") = (1u << ec::mdll_neutron::xPosBits) - 1;
    mdll_neutron.attr("y_pos_max") = (1u << ec::mdll_neutron::yPosBits) - 1;

    py::module_ neutron = constants.def_submodule("neutron", "MPSD neutron event field ranges");
    neutron.attr("mpsd_id_max") = (1u << ec::neutron::MpsdIdBits) - 1;
    neutron.attr("channel_max") = (1u << ec::neutron::ChannelBits) - 1;
    neutron.attr("amplitude_max") = (1u << ec::neutron::AmplitudeBits) - 1;
    neutron.attr("position_max") = (1u << ec::neutron::PositionBits) - 1;

    py::module_ trigger = constants.def_submodule("trigger", "Trigger event field ranges");
    trigger.attr("trigger_id_max") = (1u << ec::trigger::TriggerIdBits) - 1;
    trigger.attr("data_id_max") = (1u << ec::trigger::DataIdBits) - 1;
    trigger.attr("data_max") = (1u << ec::trigger::DataBits) - 1;

    py::module_ buffer_types = constants.def_submodule("buffer_types", "Data buffer types");
    buffer_types.attr("CommandPacketBufferType") = CommandPacketBufferType;
    buffer_types.attr("McpdDataBufferType") = McpdDataBufferType;
    buffer_types.attr("MdllDataBufferType") = MdllDataBufferType;
}

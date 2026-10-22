"""Tests for the queue based python Readout/Replay classes."""

import threading
import time

import pytest

import mesytec_mcpd as mcpd
from fake_data import PACKET_SIZE, DataSender, build_packet, mdll_neutron


def drain(q, n, timeout=2.0):
    result = []
    deadline = time.monotonic() + timeout
    while len(result) < n:
        result.append(q.get(timeout=max(deadline - time.monotonic(), 0.01)))
    return result


def test_readout_queue_and_counters():
    rdo = mcpd.Readout(listenPort=0)
    assert rdo.start()
    port = rdo.local_port

    with DataSender(port, "127.0.0.2") as s:
        s.send([mdll_neutron(1, 2, 3)] * 4, buffer_number=0)
        s.send([mdll_neutron(1, 2, 3)] * 4, buffer_number=2)
        packets = drain(rdo.get_queue(), 2)

    assert packets[0].src_addr == s.src_addr
    assert packets[0].packet.event_count() == 4
    assert packets[1].packet.buffer_number == 2

    c = rdo.get_counters()
    assert (c.packets, c.events, c.packets_lost, c.packets_dropped) == (2, 8, 1, 0)

    assert rdo.stop()
    assert not rdo.is_running()
    with pytest.raises(mcpd.QueueShutDown):
        rdo.get_queue().get(timeout=0.1)


def test_readout_restart_gets_new_queue():
    rdo = mcpd.Readout(listenPort=0)
    for _ in range(3):
        rdo.start()
        port = rdo.local_port
        with DataSender(port) as s:
            s.send([])
            assert len(drain(rdo.get_queue(), 1)) == 1
        rdo.stop()


def test_readout_counts_drops_when_queue_full():
    rdo = mcpd.Readout(listenPort=0, queue_size=2)
    rdo.start()
    port = rdo.local_port
    with DataSender(port) as s:
        for _ in range(10):
            s.send([])
        deadline = time.monotonic() + 2
        while rdo.get_counters().packets < 10 and time.monotonic() < deadline:
            time.sleep(0.01)
    c = rdo.get_counters()
    rdo.stop()
    assert (c.packets, c.packets_dropped) == (10, 8)


def _write_listfile(path, n):
    with open(path, "wb") as f:
        for i in range(n):
            f.write(build_packet([mdll_neutron(1, 2, 3)], buffer_number=i).ljust(PACKET_SIZE, b"\0"))


def test_replay_backpressure(tmp_path):
    listfile = str(tmp_path / "r.mcpdlst")
    _write_listfile(listfile, 50)

    rp = mcpd.Replay(filename=listfile, queue_size=4)
    rp.start()
    q = rp.get_queue()
    got = []
    while True:
        try:
            got.append(q.get(timeout=2))
        except mcpd.QueueShutDown:
            break
    assert [p.packet.buffer_number for p in got] == list(range(50))
    assert not rp.is_running()
    c = rp.get_counters()
    assert (c.packets, c.events, c.packets_lost, c.packets_dropped) == (50, 50, 0, 0)
    rp.stop()


def test_replay_stop_while_blocked(tmp_path):
    listfile = str(tmp_path / "r.mcpdlst")
    _write_listfile(listfile, 50)

    rp = mcpd.Replay(filename=listfile, queue_size=2)
    rp.start()
    time.sleep(0.1)  # worker is now blocked in queue.put()
    t = threading.Thread(target=rp.stop)
    t.start()
    t.join(timeout=2)
    assert not t.is_alive()
    assert not rp.is_running()


def test_replay_missing_file():
    rp = mcpd.Replay(filename="/nonexistent/x.mcpdlst")
    with pytest.raises(RuntimeError, match="Failed to open"):
        rp.start()
    assert not rp.is_running()


def test_readout_restart_on_fixed_port():
    import socket

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("0.0.0.0", 0))
        port = sock.getsockname()[1]

    rdo = mcpd.Readout(listenPort=port)
    for _ in range(3):
        rdo.start()
        rdo.stop()

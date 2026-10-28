import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"evaluations"))
sys.path.insert(0,str(ROOT/"realnet"))
import metrics
import utils
import sift
import scenarios
import netshape
import remote
import session

class MetricsTests(unittest.TestCase):
    def events(self):
        return [{"type":"FRAME_SENT","fid":1,"ts_microsecs":100000,"frame_interval_us":100000,"bytes":1000,"target_mbps":99,"sensible_us":80000},
                {"type":"FRAME_SENT","fid":2,"ts_microsecs":200000,"frame_interval_us":100000,"bytes":1000,"target_mbps":99,"sensible_us":80000},
                {"type":"FRAME_DELIVERED","fid":1,"ts_microsecs":210000,"bytes":1000,"delay_ms":110},
                {"type":"PROBE_SUMMARY","fid":1,"expected":4,"received":3},
                {"type":"PROBE_SUMMARY","fid":2,"expected":4,"received":0},
                {"type":"FRAME_LOSS","fid":2,"ts_microsecs":490000}]
    def test_measure_delivery_not_target_and_include_failed_frames(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"log";path.write_text("\n".join(map(json.dumps,self.events())))
            b,r,t=metrics.parse_jsonl(path);summary=metrics.summarise(b,r,t)
        self.assertEqual(summary["avg_bitrate"],.04)
        self.assertEqual(summary["frame_losses"],1)
        self.assertEqual(summary["stall_100ms"],1)
        self.assertEqual(summary["stall_200ms"],.5)
        self.assertEqual(r.times,[0,100])
        self.assertEqual(t.times,[110])
    def test_failed_flow_stays_in_fairness(self):
        self.assertEqual(metrics.jain([10,0]),.5)
        self.assertEqual(metrics.jain([0,0]),0)
    def test_convergence_rejects_stable_wrong_rate(self):
        self.assertIsNone(metrics.convergence_ms(metrics.Series([1]*12,times=range(12)),0,10))
        self.assertEqual(metrics.convergence_ms(metrics.Series([10]*12,times=range(12)),0,10),0)
    def test_convergence_stays_within_phase(self):
        series=metrics.Series([1]*10+[10]*10,times=range(20))
        self.assertIsNone(metrics.convergence_ms(series,0,10,end_ms=10))
    def test_nonfinite_trace_capacity_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                utils.const_trace(Path(directory)/"trace",float("inf"),1)
    def test_probe_loss_covers_all_outcomes(self):
        self.assertEqual(metrics.probe_summary(self.events())["loss_rate"],5/8)
    def test_trace_units_and_step_boundary(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"trace";utils.const_trace(path,12,1)
            self.assertEqual(len(path.read_text().splitlines()),1000)
            utils.phases_trace(path,[(12,.5),(24,1)])
            self.assertEqual(len(path.read_text().splitlines()),1500)

class WireUnitTests(unittest.TestCase):
    def test_frame_wire_bytes_matches_protocol(self):
        # 5 packets -> groups of 4 and 1: one parity; 4 probes.
        self.assertEqual(metrics.frame_wire_bytes(5*1200),6*1274+4*74)
        self.assertEqual(metrics.frame_wire_bytes(1),1274+4*74)
    def test_payload_equivalent_fits_capacity(self):
        for capacity in (5,20,50):
            payload=metrics.payload_equivalent(capacity)
            wire=metrics.frame_wire_bytes(payload*metrics.FRAME_US/8)*8/metrics.FRAME_US
            self.assertLessEqual(wire,capacity)
            self.assertTrue(.6*capacity<payload<.8*capacity)
    def test_wire_throughput_counts_overhead_and_retries(self):
        events=[{"type":"FRAME_SENT","fid":1,"ts_microsecs":0,"frame_interval_us":1000,"bytes":4800},
                {"type":"FRAME_DELIVERED","fid":1,"ts_microsecs":500},
                {"type":"PACKET_RETRANSMITTED","fid":1,"ts_microsecs":600}]
        self.assertAlmostEqual(metrics.wire_throughput(events),8*(metrics.frame_wire_bytes(4800)+1274)/1000)
    def test_smoothed_convergence_tolerates_fallback_dips(self):
        # Every 4th frame is a 15% fallback: raw never holds 10 frames within 5%.
        values=[10*(.85 if i%4==0 else 1) for i in range(60)]
        series=metrics.Series(values,times=[i*28.6 for i in range(60)])
        self.assertIsNone(metrics.convergence_ms(series,0,9.6))
        self.assertIsNotNone(metrics.convergence_ms(series,0,9.6,smooth_ms=500))

class SiftTests(unittest.TestCase):
    def frame(self,fid,bur=.5):
        return [{"type":"FRAME_SENT","fid":fid},{"type":"FRAME_DELIVERED","fid":fid},
                {"type":"CONTROL_SAMPLE","fid":fid,"n_probes":4,"pacing":1.25,"bur":bur},
                {"type":"PROBE_SUMMARY","fid":fid,"expected":4,"received":4}]
    def test_clean_log_passes(self):
        self.assertEqual(sift.check(self.frame(1)+self.frame(2)),[])
    def test_missing_outcome_and_summary_flagged(self):
        bad=sift.check([{"type":"FRAME_SENT","fid":1}])
        self.assertIn("sent frames lack final outcomes",bad)
        self.assertIn("sent frames lack probe summaries",bad)
    def test_drain_needs_three_congested_samples(self):
        events=self.frame(1,1.5)+self.frame(2,.9)+self.frame(3,1.5)
        events.append({"type":"DRAIN_START","fid":3,"drain_rate":1.0})
        self.assertIn("drain 3 lacks three congested samples",sift.check(events))
        events=self.frame(1,1.5)+self.frame(2,1.2)+self.frame(3,1.5)+[{"type":"DRAIN_START","fid":3,"drain_rate":1.0}]
        self.assertEqual(sift.check(events),[])

class ScenarioTests(unittest.TestCase):
    def test_trace_duration(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"t";path.write_text("0\n500\n1999\n")
            self.assertEqual(scenarios.trace_duration(path),2)
            path.write_text("5\n3\n")
            with self.assertRaises(ValueError):scenarios.trace_duration(path)

class TraceConversionTests(unittest.TestCase):
    def test_no_silent_clipping_and_atomic_failure(self):
        import importlib.util
        spec=importlib.util.spec_from_file_location("trace_converter", ROOT/"zeus_traces"/"convert.py")
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/"input";output=Path(directory)/"output"
            source.write_text("0 30\n1 2\n")
            self.assertEqual(module.convert(source,output),0)
            self.assertEqual(len(output.read_text().splitlines()),32)
            previous=output.read_text();source.write_text("1 -2\n")
            with self.assertRaises(ValueError):module.convert(source,output)
            self.assertEqual(output.read_text(),previous)

class ShapingTests(unittest.TestCase):
    def test_preserve_custom_root(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        with patch.object(shape,"_qdiscs",return_value=[{"kind":"fq_codel","handle":"8001:","root":True}]),patch("netshape._tc") as tc:
            with self.assertRaises(RuntimeError):shape.apply()
            tc.assert_not_called()
    def test_preserve_configured_child_under_default_root(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        tree=[{"kind":"mq","handle":"0:","root":True},{"kind":"fq","handle":"8002:","parent":":1"}]
        with patch.object(shape,"_qdiscs",return_value=tree),patch("netshape._tc") as tc:
            with self.assertRaises(RuntimeError):shape.apply()
            tc.assert_not_called()
    def test_kernel_default_mq_tree_is_shaped_and_restored(self):
        # WSL2 eth0 and most NICs: mq root with per-queue pfifo_fast, all handle 0:.
        default=[{"kind":"mq","handle":"0:","root":True}]+[{"kind":"pfifo_fast","handle":"0:","parent":f":{i}"} for i in range(1,5)]
        ours=[{"kind":"prio","handle":"7d1:","root":True},{"kind":"netem","handle":"7d4:","parent":"7d1:4"}]
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        with patch.object(shape,"_qdiscs",side_effect=[default,ours,default]),patch("netshape._tc") as tc:
            shape.apply();shape.clear()
            self.assertIn("root handle 7d1: prio",tc.call_args_list[0].args[0])
            self.assertIn("qdisc del",tc.call_args_list[-1].args[0])
    def test_restore_failure_is_reported(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        ours=[{"kind":"prio","handle":"7d1:","root":True}]
        with patch.object(shape,"_qdiscs",side_effect=[[],ours,[{"kind":"htb","handle":"1:","root":True}]]),patch("netshape._tc"):
            shape.apply()
            with self.assertRaises(RuntimeError):shape.clear()
    def test_cleanup_only_after_owned_root_created(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        with patch("netshape._tc") as tc:shape.clear();tc.assert_not_called()
    def test_ports_and_protocol_filters(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700,9701])
        with patch.object(shape,"_qdiscs",return_value=[{"kind":"noqueue","handle":"0:","root":True}]),patch("netshape._tc") as tc:
            shape.apply()
            filters=[call.args[0] for call in tc.call_args_list if "filter add" in call.args[0]]
            self.assertEqual(len(filters),2)
            self.assertTrue(all("protocol 17 0xff" in x for x in filters))
            self.assertIn("dport 9701 0xffff",filters[1])
    def test_jitter_not_silently_omitted(self):
        with self.assertRaises(ValueError):netshape._netem_opts(20,0,40,0)
        self.assertIn("40ms",netshape._netem_opts(20,40,40,0))
    def test_jitter_without_rate_rejected(self):
        with self.assertRaises(ValueError):netshape._netem_opts(None,40,40,0)
    def test_queue_limit(self):
        self.assertTrue(netshape._netem_opts(20,0,0,0,8).startswith("limit 8 "))
        with self.assertRaises(ValueError):netshape._netem_opts(20,0,0,0,0)
    def test_partial_setup_cleanup(self):
        shape=netshape.Shaper("test0","192.0.2.1",ports=[9700])
        with patch.object(shape,"_qdiscs",side_effect=[[],[{"kind":"prio","handle":"7d1:","root":True}],[]]),patch("netshape._tc",side_effect=[None,RuntimeError("netem failed"),None]) as tc:
            with self.assertRaises(RuntimeError):shape.apply()
            self.assertIn("qdisc del",tc.call_args_list[-1].args[0])
    def test_remote_repo_quoted(self):
        with patch.object(remote.config,"REMOTE_REPO","~/repo with 'quotes"):
            value=remote.repo_path()
            self.assertTrue(value.startswith('"$HOME"/'))
            self.assertIn("'\"'\"'",value)

if __name__=="__main__":unittest.main()

# Zeus 5G traces

These traces come from the ZEUS dataset (https://github.com/comnetsAD/zeus/tree/main/traces/5G%20new%20format). Thanks to its authors.

Each `.csv` file has one row per millisecond: a timestamp and the number of packets the link could carry in that millisecond. Each `.up` file is the same trace converted to mahimahi's format, one line per 1500-byte delivery opportunity, so a row `12 3` becomes three lines reading `12`.

To convert a trace again:

```bash
python3 zeus_traces/convert.py zeus_traces/5GCityDrive.csv zeus_traces/5GCityDrive.up
```

The converter keeps every packet by default. `--max-packets-per-ms N` caps each millisecond at N packets and prints how many packets were dropped by the cap. The output is written to a temporary file first, so a bad input never leaves a half-written trace behind.

The experiments use the same trace for both directions:

```bash
python3 evaluations/zeus_batch.py --dur 30
python3 evaluations/zeus_tests.py --trace 5GCityDrive.up --full
```

`--full` runs each trace for its whole length instead of `--dur` seconds.

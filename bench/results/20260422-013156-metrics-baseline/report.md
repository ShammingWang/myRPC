# Benchmark Report

## Metadata

- Date: `2026-04-22T01:31:56`
- Commit: `2e2ca2d`
- Label: `metrics-baseline`
- Machine: `Linux 6.6.87.2-microsoft-standard-WSL2, x86_64, cpu_count=16`
- Method: `EchoService.Echo`
- Duration: `6.0s`
- Warmup: `2.0s`
- Repeats: `3`

## Throughput And Tail Latency

| Connections | IO Threads | Payload | QPS | P50 | P99 | P999 | Avg | QPS vs Prev | P99 vs Prev |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 50 | 8 | 16 B | 51446.08 | 0.892 ms | 2.270 ms | 2.910 ms | 0.968 ms | n/a | n/a |
| 100 | 8 | 16 B | 50126.80 | 1.863 ms | 4.108 ms | 5.121 ms | 1.988 ms | n/a | n/a |
| 200 | 8 | 16 B | 49612.25 | 3.903 ms | 6.921 ms | 8.658 ms | 4.017 ms | n/a | n/a |
| 400 | 8 | 16 B | 48915.61 | 7.984 ms | 12.639 ms | 17.014 ms | 8.134 ms | n/a | n/a |

## IO Thread Scaling

| IO Threads | Connections | Payload | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 200 | 16 B | 13762.52 | 14.783 ms | 18.536 ms | 39.084 ms | 14.488 ms |
| 2 | 200 | 16 B | 25891.30 | 7.892 ms | 10.536 ms | 12.703 ms | 7.702 ms |
| 4 | 200 | 16 B | 42035.48 | 4.664 ms | 6.628 ms | 7.462 ms | 4.743 ms |
| 8 | 200 | 16 B | 49795.07 | 3.881 ms | 6.926 ms | 8.715 ms | 4.002 ms |
| 16 | 200 | 16 B | 48297.39 | 3.832 ms | 9.661 ms | 12.582 ms | 4.125 ms |

## Payload Scaling

| Payload | IO Threads | Connections | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 B | 8 | 200 | 49347.38 | 3.913 ms | 7.051 ms | 8.841 ms | 4.038 ms |
| 256 B | 8 | 200 | 49273.98 | 3.925 ms | 6.947 ms | 8.898 ms | 4.044 ms |
| 4096 B | 8 | 200 | 46624.84 | 4.147 ms | 7.379 ms | 9.451 ms | 4.274 ms |
| 16384 B | 8 | 200 | 32366.39 | 5.795 ms | 14.367 ms | 21.720 ms | 6.281 ms |

## Notes

- `QPS/P50/P99/P999/Avg` are the arithmetic mean across the measured repeats.
- `QPS vs Prev` and `P99 vs Prev` are computed only when `--compare-with` points to an older suite CSV.
- Per-run raw outputs are stored in `raw_runs.json`; the aggregated machine-readable summary is stored in `summary.csv`.

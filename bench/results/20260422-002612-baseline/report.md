# Benchmark Report

## Metadata

- Date: `2026-04-22T00:26:12`
- Commit: `45774b1`
- Label: `baseline`
- Machine: `Linux 6.6.87.2-microsoft-standard-WSL2, x86_64, cpu_count=16`
- Method: `EchoService.Echo`
- Duration: `6.0s`
- Warmup: `2.0s`
- Repeats: `3`

## Throughput And Tail Latency

| Connections | IO Threads | Payload | QPS | P50 | P99 | P999 | Avg | QPS vs Prev | P99 vs Prev |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 50 | 8 | 16 B | 49971.95 | 0.911 ms | 2.434 ms | 3.480 ms | 0.997 ms | n/a | n/a |
| 100 | 8 | 16 B | 49550.16 | 1.874 ms | 4.344 ms | 5.577 ms | 2.012 ms | n/a | n/a |
| 200 | 8 | 16 B | 48352.41 | 3.970 ms | 7.570 ms | 10.243 ms | 4.122 ms | n/a | n/a |
| 400 | 8 | 16 B | 45450.79 | 8.540 ms | 14.735 ms | 41.226 ms | 8.755 ms | n/a | n/a |

## IO Thread Scaling

| IO Threads | Connections | Payload | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 200 | 16 B | 13505.81 | 15.073 ms | 17.978 ms | 46.373 ms | 14.764 ms |
| 2 | 200 | 16 B | 26172.10 | 7.822 ms | 10.723 ms | 13.363 ms | 7.622 ms |
| 4 | 200 | 16 B | 40202.77 | 4.825 ms | 7.986 ms | 11.390 ms | 4.960 ms |
| 8 | 200 | 16 B | 45556.10 | 4.187 ms | 8.501 ms | 12.636 ms | 4.382 ms |
| 16 | 200 | 16 B | 46283.88 | 3.979 ms | 10.370 ms | 15.527 ms | 4.309 ms |

## Payload Scaling

| Payload | IO Threads | Connections | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 B | 8 | 200 | 46620.25 | 4.102 ms | 8.023 ms | 11.143 ms | 4.274 ms |
| 256 B | 8 | 200 | 44404.89 | 4.243 ms | 9.325 ms | 14.192 ms | 4.488 ms |
| 4096 B | 8 | 200 | 43932.83 | 4.355 ms | 8.455 ms | 11.844 ms | 4.535 ms |
| 16384 B | 8 | 200 | 39262.00 | 4.900 ms | 8.919 ms | 12.212 ms | 5.076 ms |

## Notes

- `QPS/P50/P99/P999/Avg` are the arithmetic mean across the measured repeats.
- `QPS vs Prev` and `P99 vs Prev` are computed only when `--compare-with` points to an older suite CSV.
- Per-run raw outputs are stored in `raw_runs.json`; the aggregated machine-readable summary is stored in `summary.csv`.

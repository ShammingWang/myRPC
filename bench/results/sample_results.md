# Sample Results

`bench/run_suite.py` 生成的正式报告会放到 `bench/results/<timestamp>/report.md`。下面这个示例展示推荐的归档格式。

## Metadata

- Date: `2026-04-22T20:00:00`
- Commit: `example123`
- Label: `multi-io-baseline`
- Machine: `Linux example-host, x86_64, cpu_count=16`
- Method: `EchoService.Echo`
- Duration: `6.0s`
- Warmup: `2.0s`
- Repeats: `3`

## Throughput And Tail Latency

| Connections | IO Threads | Payload | QPS | P50 | P99 | P999 | Avg | QPS vs Prev | P99 vs Prev |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 50 | 8 | 16 B | TBD | TBD | TBD | TBD | TBD | n/a | n/a |
| 100 | 8 | 16 B | TBD | TBD | TBD | TBD | TBD | n/a | n/a |
| 200 | 8 | 16 B | TBD | TBD | TBD | TBD | TBD | n/a | n/a |
| 400 | 8 | 16 B | TBD | TBD | TBD | TBD | TBD | n/a | n/a |

## IO Thread Scaling

| IO Threads | Connections | Payload | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 200 | 16 B | TBD | TBD | TBD | TBD | TBD |
| 2 | 200 | 16 B | TBD | TBD | TBD | TBD | TBD |
| 4 | 200 | 16 B | TBD | TBD | TBD | TBD | TBD |
| 8 | 200 | 16 B | TBD | TBD | TBD | TBD | TBD |
| 16 | 200 | 16 B | TBD | TBD | TBD | TBD | TBD |

## Payload Scaling

| Payload | IO Threads | Connections | QPS | P50 | P99 | P999 | Avg |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 B | 8 | 200 | TBD | TBD | TBD | TBD | TBD |
| 256 B | 8 | 200 | TBD | TBD | TBD | TBD | TBD |
| 4096 B | 8 | 200 | TBD | TBD | TBD | TBD | TBD |
| 16384 B | 8 | 200 | TBD | TBD | TBD | TBD | TBD |

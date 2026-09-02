# Native runtime benchmarks

`make bench` compiles every `tools/bench/*.art` with `tmp/ncomp`, runs host and
native three times, and prints the median wall time. Numbers below are from an
Apple M-series laptop, Arturo 0.10.0; ratio > 1 means native is faster.

## Baseline (before the runtime performance work)

    program            host_ms native_ms   ratio native_MB
    closures                28        24   1.17x         1
    dict_insert             58      7982   0.01x        54
    dict_iter               88       660   0.13x       118
    fib                    241       474   0.51x       217
    loop_sum                97       268   0.36x       135
    map_select             117       477   0.25x       273
    nested_loops            92        96   0.96x         1
    sort                   179     18899   0.01x        69
    string_append           41       132   0.31x       472
    while_counter          393       786   0.50x       267

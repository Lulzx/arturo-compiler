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

## Phase 1: cached dispatch and integer fast paths

Measured from commit `7af19ea` on the same machine. This phase targets runtime
dispatch overhead; the allocation- and algorithm-heavy cases remain dominated
by later work.

    program            host_ms native_ms   ratio native_MB
    closures               100       292   0.34x       130
    dict_insert             83      8928   0.01x        45
    dict_iter              114       811   0.14x       116
    fib                    283       555   0.51x       217
    loop_sum               110       330   0.33x       135
    map_select             137       563   0.24x       272
    nested_loops           106        43   2.47x         1
    sort                   202     21771   0.01x        70
    string_append           43       125   0.34x        47
    while_counter          397       761   0.52x       267

## Phase 3: dictionary index and stable merge sort

Open-addressed dictionary lookup makes repeated insertion near-linear, while a
stable merge sort removes the quadratic collection-sort path. Object-defined
comparators retain insertion sort because comparator calls are observable.
`ARTURO_NATIVE_DICTCHECK=1` cross-checks indexed results against a linear scan.

    program            host_ms native_ms   ratio native_MB
    closures                80       179   0.45x       130
    dict_insert             58        91   0.64x        57
    dict_iter               81       171   0.47x       118
    fib                    229       330   0.69x       217
    loop_sum                88       210   0.42x       134
    map_select             107       438   0.24x       273
    nested_loops            84        32   2.62x         1
    sort                   163       139   1.17x        69
    string_append           37       121   0.31x       472
    while_counter          368       494   0.74x       267

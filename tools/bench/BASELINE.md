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

## Phase 2: arena allocation, interned names, and reclaimed frames

Phase 2 was implemented after Phase 3 in this branch. Normal builds use 1 MiB
chunks with size-class freelists and tracked large allocations; ASan or
`ARTURO_TRACKED_HEAP` builds retain real `malloc`/`free`. Environment names are
interned, six bindings are inline, escaping closure chains are marked captured,
and uncaptured function/action/loop frames are reclaimed. Stack buffers cover
the common call and block-expression paths. `ARTURO_NATIVE_HEAPCHECK=1` disables
frame reclamation for differential diagnosis.

    program            host_ms native_ms   ratio native_MB
    closures                82        49   1.67x        14
    dict_insert             61        48   1.27x        14
    dict_iter               81        46   1.76x        11
    fib                    233        93   2.51x         1
    loop_sum                94        49   1.92x         1
    map_select             111        91   1.22x        44
    nested_loops            86        25   3.44x         1
    sort                   174        76   2.29x        25
    string_append           39       106   0.37x       465
    while_counter          387       153   2.53x         1

## Phase 4: compiled higher-order action bodies

Literal action blocks for `map`, `select`, `filter`, `every?`, `some?`,
`chunk`, `cluster`, `gather`, `arrange`, `enumerate`, `maximum`, `minimum`,
and `fold` now execute pre-lowered IR. Stored action values and synthetic
thick-arrow actions retain the token-block path. The full benchmark run below
had a noisy `map_select` sample (100 ms); two immediately repeated focused runs
were 67 ms and 68 ms native with output parity.

    program            host_ms native_ms   ratio native_MB
    closures                87        46   1.89x        14
    dict_insert             63        46   1.37x        14
    dict_iter               84        49   1.71x        10
    fib                    248        99   2.51x         1
    loop_sum                98        50   1.96x         1
    map_select             117        68   1.72x        44
    nested_loops           124        42   2.95x         1
    sort                   185        61   3.03x        25
    string_append           42       110   0.38x       465
    while_counter          403       159   2.53x         1

## Phase 5: single-copy string append

String append now computes both lengths once, allocates the final buffer once,
copies with `memcpy`, and transfers that buffer directly into the result value.
This removes the previous `v_str` copy. The immutable-value arena still retains
prior growing-string values until process exit, so peak RSS is unchanged; an
alias-safe in-place growth scheme requires ownership tracking.

    program            host_ms native_ms   ratio native_MB
    closures                86        48   1.79x        14
    dict_insert             63        52   1.21x        14
    dict_iter               85        48   1.77x        11
    fib                    249        91   2.74x         1
    loop_sum                96        50   1.92x         1
    map_select             117        67   1.75x        44
    nested_loops            91        29   3.14x         1
    sort                   181        61   2.97x        26
    string_append           40        77   0.52x       465
    while_counter          406       158   2.57x         1

## Phase 6: alias-safe string growth

Strings carry a header (len/cap/owned/shared) so appends avoid `strlen` and new
buffers carry geometric capacity. Heap stores (env/block/dict) mark a second
owner as shared (sticky, no release tracking; the arena still reclaims at
exit); IR literals start shared. The value-path append always copies to
preserve `a: b ++ c`, while `OP_DEFINE`/`OP_LET` detect `s: s ++ x` and grow an
exclusive `s` in place (amortized O(1)). Peak RSS on string_append falls from
465 MB to ~4 MB; all ten benchmarks are now faster than the host.

    program            host_ms native_ms   ratio native_MB
    closures                87        47   1.85x        14
    dict_insert             66        50   1.32x        16
    dict_iter               86        48   1.79x        14
    fib                    240        92   2.61x         1
    loop_sum                97        54   1.80x         1
    map_select             118        66   1.79x        44
    nested_loops            90        27   3.33x         1
    sort                   182        63   2.89x        27
    string_append           42        39   1.08x         4
    while_counter          414       173   2.39x         1

# Runtime Worker Placement Benchmark

Date: 2026-05-14

Host: Windows, MinGW Makefiles, GCC 12.2.0, `CMAKE_BUILD_TYPE=Release`

The current benchmark is a core microbenchmark. It is useful as a regression guard for foundational runtime changes, but it does not yet measure the target HTTP worker-pool throughput limit. Add HTTP/reuse-port benchmark cases after the endpoint worker proof lands.

Tables below report median `ops_per_s` from three consecutive runs.

## Commands

Legacy baseline:

```powershell
git worktree add ..\webserver-legacy-runtime-service-bound codex/legacy-runtime-service-bound
git submodule update --init --recursive
cmake -G "MinGW Makefiles" -S . -B build-mingw
cmake --build build-mingw --target core_runtime_benchmark -j 4
1..3 | ForEach-Object { .\build-mingw\test\benchmark\core_runtime_benchmark.exe }
```

Current branch:

```powershell
cmake -G "MinGW Makefiles" -S . -B build-mingw-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw-release --target core_runtime_benchmark -j 4
1..3 | ForEach-Object { .\build-mingw-release\test\benchmark\core_runtime_benchmark.exe }
```

## Results

| Benchmark | Legacy ops/s | Current ops/s | Notes |
| --- | ---: | ---: | --- |
| `byte_buffer_append_copy` | 7.60023e+06 | 7.73386e+06 | No meaningful change. |
| `buffer_chain_push_pop` | 7.54058e+06 | 7.57389e+06 | No meaningful change. |
| `event_bus_publish` | 1.18239e+07 | 1.1795e+07 | No meaningful change. |

Raw legacy output:

```text
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=25.8457    ops_per_s=7.73823e+06    MiB_per_s=7556.87
buffer_chain_push_pop          ops=800000       elapsed_ms=105.964    ops_per_s=7.54971e+06    MiB_per_s=3686.38
event_bus_publish              ops=300000       elapsed_ms=25.2235    ops_per_s=1.18937e+07
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=26.8354    ops_per_s=7.45284e+06    MiB_per_s=7278.17
buffer_chain_push_pop          ops=800000       elapsed_ms=110.627    ops_per_s=7.23153e+06    MiB_per_s=3531.02
event_bus_publish              ops=300000       elapsed_ms=25.3724    ops_per_s=1.18239e+07
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=26.315     ops_per_s=7.60023e+06    MiB_per_s=7422.1
buffer_chain_push_pop          ops=800000       elapsed_ms=106.093    ops_per_s=7.54058e+06    MiB_per_s=3681.92
event_bus_publish              ops=300000       elapsed_ms=25.3761    ops_per_s=1.18221e+07
```

Raw current output:

```text
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=25.8603    ops_per_s=7.73386e+06    MiB_per_s=7552.6
buffer_chain_push_pop          ops=800000       elapsed_ms=105.626    ops_per_s=7.57389e+06    MiB_per_s=3698.19
event_bus_publish              ops=300000       elapsed_ms=25.5132    ops_per_s=1.17586e+07
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=28.0572    ops_per_s=7.1283e+06     MiB_per_s=6961.23
buffer_chain_push_pop          ops=800000       elapsed_ms=113.505    ops_per_s=7.04814e+06    MiB_per_s=3441.47
event_bus_publish              ops=300000       elapsed_ms=25.1555    ops_per_s=1.19258e+07
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=25.6377    ops_per_s=7.80101e+06    MiB_per_s=7618.18
buffer_chain_push_pop          ops=800000       elapsed_ms=105.501    ops_per_s=7.58284e+06    MiB_per_s=3702.56
event_bus_publish              ops=300000       elapsed_ms=25.4346    ops_per_s=1.1795e+07
```

## Local Verification After HTTP Worker Placement Proof

Date: 2026-05-15

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

This run is a strict post-change smoke benchmark from the verified debug tree. It is not directly comparable to the release baseline table above, but it now covers more than buffer/event-bus micro-ops:

- runtime callback queue drain
- coroutine suspend/resume chain lifecycle
- detached coroutine creation/completion lifecycle
- TCP connection object create/abort lifecycle
- async listener accept/read/write/close loopback roundtrip

Command:

```powershell
1..3 | ForEach-Object { .\build\test\benchmark\core_runtime_benchmark.exe }
```

Median `ops_per_s` from three consecutive runs:

| Benchmark | Debug ops/s |
| --- | ---: |
| `byte_buffer_append_copy` | 2.60604e+06 |
| `buffer_chain_push_pop` | 1.15406e+06 |
| `event_bus_publish` | 1.08548e+06 |
| `runtime_dispatch_callbacks` | 3.10828e+06 |
| `coroutine_schedule_chain` | 7.76574e+05 |
| `detached_coroutine_lifecycle` | 2.0875e+06 |
| `tcp_connection_create_abort` | 2.20356e+04 |
| `async_listener_echo_roundtrip` | 3.78679e+03 |

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=76.7449    ops_per_s=2.60604e+06    MiB_per_s=2544.96
buffer_chain_push_pop          ops=800000       elapsed_ms=693.206    ops_per_s=1.15406e+06    MiB_per_s=563.505
event_bus_publish              ops=300000       elapsed_ms=276.375    ops_per_s=1.08548e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=110.011    ops_per_s=2.727e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=196.763    ops_per_s=762337
detached_coroutine_lifecycle   ops=50000        elapsed_ms=23.9521    ops_per_s=2.0875e+06
tcp_connection_create_abort    ops=20000        elapsed_ms=907.623    ops_per_s=22035.6
async_listener_echo_roundtrip  ops=1000         elapsed_ms=264.076    ops_per_s=3786.79        MiB_per_s=1.84902
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=96.634     ops_per_s=2.06966e+06    MiB_per_s=2021.16
buffer_chain_push_pop          ops=800000       elapsed_ms=804.939    ops_per_s=993864         MiB_per_s=485.285
event_bus_publish              ops=300000       elapsed_ms=246.877    ops_per_s=1.21518e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=96.5165    ops_per_s=3.10828e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=174.008    ops_per_s=862027
detached_coroutine_lifecycle   ops=50000        elapsed_ms=20.2255    ops_per_s=2.47213e+06
tcp_connection_create_abort    ops=20000        elapsed_ms=993.903    ops_per_s=20122.7
async_listener_echo_roundtrip  ops=1000         elapsed_ms=240.113    ops_per_s=4164.71        MiB_per_s=2.03355
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=73.9467    ops_per_s=2.70465e+06    MiB_per_s=2641.26
buffer_chain_push_pop          ops=800000       elapsed_ms=691.631    ops_per_s=1.15669e+06    MiB_per_s=564.788
event_bus_publish              ops=300000       elapsed_ms=302.778    ops_per_s=990824
runtime_dispatch_callbacks     ops=300000       elapsed_ms=95.5048    ops_per_s=3.1412e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=193.156    ops_per_s=776574
detached_coroutine_lifecycle   ops=50000        elapsed_ms=31.7374    ops_per_s=1.57543e+06
tcp_connection_create_abort    ops=20000        elapsed_ms=870.955    ops_per_s=22963.3
async_listener_echo_roundtrip  ops=1000         elapsed_ms=298.801    ops_per_s=3346.7         MiB_per_s=1.63413
```

## Local Verification After Benchmark Coverage Expansion

Date: 2026-05-16

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

This run extends the debug benchmark coverage to include:

- timer-backed detached coroutine lifecycle (`sleep_for` suspend/resume/destruction)
- persistent echo processing capacity on one connection
- concurrent short-connection echo throughput and accepted connection lifecycle

Command:

```powershell
cmake --build build --target core_runtime_benchmark --config Debug -j 4
1..3 | ForEach-Object { .\build\test\benchmark\core_runtime_benchmark.exe }
```

Median `ops_per_s` from three consecutive runs:

| Benchmark | Debug ops/s |
| --- | ---: |
| `byte_buffer_append_copy` | 2.74022e+06 |
| `buffer_chain_push_pop` | 1.20114e+06 |
| `event_bus_publish` | 1.25607e+06 |
| `runtime_dispatch_callbacks` | 3.57296e+06 |
| `coroutine_schedule_chain` | 6.44905e+05 |
| `detached_coroutine_lifecycle` | 2.79439e+06 |
| `timer_coroutine_lifecycle` | 2.58715e+05 |
| `tcp_connection_create_abort` | 2.461e+04 |
| `async_listener_echo_roundtrip` | 4.11608e+03 |
| `async_listener_persistent_echo` | 1.21855e+04 |
| `async_listener_concurrent_echo` | 4.66701e+03 |

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=83.4785    ops_per_s=2.39583e+06    MiB_per_s=2339.67
buffer_chain_push_pop          ops=800000       elapsed_ms=640.081    ops_per_s=1.24984e+06    MiB_per_s=610.274
event_bus_publish              ops=300000       elapsed_ms=229.584    ops_per_s=1.30671e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=83.964     ops_per_s=3.57296e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=232.592    ops_per_s=644905
detached_coroutine_lifecycle   ops=50000        elapsed_ms=20.191     ops_per_s=2.47635e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=78.7694    ops_per_s=253906
tcp_connection_create_abort    ops=20000        elapsed_ms=833.459    ops_per_s=23996.4
async_listener_echo_roundtrip  ops=1000         elapsed_ms=233.537    ops_per_s=4281.98        MiB_per_s=2.09081
async_listener_persistent_echo ops=5000         elapsed_ms=415.09     ops_per_s=12045.6        MiB_per_s=11.7633
async_listener_concurrent_echo ops=2000         elapsed_ms=450.036    ops_per_s=4444.09        MiB_per_s=8.67986
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=71.3458    ops_per_s=2.80325e+06    MiB_per_s=2737.55
buffer_chain_push_pop          ops=800000       elapsed_ms=666.034    ops_per_s=1.20114e+06    MiB_per_s=586.494
event_bus_publish              ops=300000       elapsed_ms=239.316    ops_per_s=1.25357e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=83.9995    ops_per_s=3.57145e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=251.893    ops_per_s=595491
detached_coroutine_lifecycle   ops=50000        elapsed_ms=17.893     ops_per_s=2.79439e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=77.3052    ops_per_s=258715
tcp_connection_create_abort    ops=20000        elapsed_ms=795.75     ops_per_s=25133.5
async_listener_echo_roundtrip  ops=1000         elapsed_ms=253.616    ops_per_s=3942.96        MiB_per_s=1.92527
async_listener_persistent_echo ops=5000         elapsed_ms=410.322    ops_per_s=12185.5        MiB_per_s=11.8999
async_listener_concurrent_echo ops=2000         elapsed_ms=415.764    ops_per_s=4810.42        MiB_per_s=9.39535
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=72.9868    ops_per_s=2.74022e+06    MiB_per_s=2676
buffer_chain_push_pop          ops=800000       elapsed_ms=686.036    ops_per_s=1.16612e+06    MiB_per_s=569.394
event_bus_publish              ops=300000       elapsed_ms=238.84     ops_per_s=1.25607e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=83.4352    ops_per_s=3.5956e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=230.995    ops_per_s=649365
detached_coroutine_lifecycle   ops=50000        elapsed_ms=17.1548    ops_per_s=2.91464e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=69.3829    ops_per_s=288255
tcp_connection_create_abort    ops=20000        elapsed_ms=812.677    ops_per_s=24610
async_listener_echo_roundtrip  ops=1000         elapsed_ms=242.95     ops_per_s=4116.08        MiB_per_s=2.00981
async_listener_persistent_echo ops=5000         elapsed_ms=405.396    ops_per_s=12333.6        MiB_per_s=12.0445
async_listener_concurrent_echo ops=2000         elapsed_ms=428.54     ops_per_s=4667.01        MiB_per_s=9.11525
```

## Local Verification After HTTP Static Write-Timeout Regression

Date: 2026-05-16

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

Command:

```powershell
.\build\test\benchmark\core_runtime_benchmark.exe
```

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=71.4439    ops_per_s=2.7994e+06     MiB_per_s=2733.79
buffer_chain_push_pop          ops=800000       elapsed_ms=648.802    ops_per_s=1.23304e+06    MiB_per_s=602.071
event_bus_publish              ops=300000       elapsed_ms=225.373    ops_per_s=1.33113e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=90.1644    ops_per_s=3.32726e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=239.542    ops_per_s=626194
detached_coroutine_lifecycle   ops=50000        elapsed_ms=18.0068    ops_per_s=2.77673e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=68.3132    ops_per_s=292769
tcp_connection_create_abort    ops=20000        elapsed_ms=823.948    ops_per_s=24273.4
async_listener_echo_roundtrip  ops=1000         elapsed_ms=270.316    ops_per_s=3699.37        MiB_per_s=1.80633
async_listener_persistent_echo ops=5000         elapsed_ms=405.843    ops_per_s=12320          MiB_per_s=12.0313
async_listener_concurrent_echo ops=2000         elapsed_ms=559.196    ops_per_s=3576.56        MiB_per_s=6.98547
```

## Local Verification After In-Process Runtime Worker Path

Date: 2026-05-16

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

Command:

```powershell
.\build\test\benchmark\core_runtime_benchmark.exe
```

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection
byte_buffer_append_copy        ops=200000       elapsed_ms=72.171     ops_per_s=2.7712e+06     MiB_per_s=2706.25
buffer_chain_push_pop          ops=800000       elapsed_ms=639.931    ops_per_s=1.25013e+06    MiB_per_s=610.417
event_bus_publish              ops=300000       elapsed_ms=245.679    ops_per_s=1.22111e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=80.364     ops_per_s=3.73301e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=219.389    ops_per_s=683715
detached_coroutine_lifecycle   ops=50000        elapsed_ms=17.906     ops_per_s=2.79236e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=68.7281    ops_per_s=291002
tcp_connection_create_abort    ops=20000        elapsed_ms=813.961    ops_per_s=24571.2
async_listener_echo_roundtrip  ops=1000         elapsed_ms=253.149    ops_per_s=3950.24        MiB_per_s=1.92883
async_listener_persistent_echo ops=5000         elapsed_ms=402.752    ops_per_s=12414.6        MiB_per_s=12.1236
async_listener_concurrent_echo ops=2000         elapsed_ms=553.363    ops_per_s=3614.26        MiB_per_s=7.0591
```

## Local Verification After In-Process Worker Recovery and Lifecycle Benchmark

Date: 2026-05-16

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

Command:

```powershell
.\build\test\benchmark\core_runtime_benchmark.exe
```

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection,worker_lifecycle
byte_buffer_append_copy        ops=200000       elapsed_ms=74.7614    ops_per_s=2.67518e+06    MiB_per_s=2612.48
buffer_chain_push_pop          ops=800000       elapsed_ms=650.273    ops_per_s=1.23025e+06    MiB_per_s=600.709
event_bus_publish              ops=300000       elapsed_ms=210.034    ops_per_s=1.42834e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=78.7749    ops_per_s=3.80832e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=225.006    ops_per_s=666650
detached_coroutine_lifecycle   ops=50000        elapsed_ms=18.0685    ops_per_s=2.76725e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=80.1404    ops_per_s=249562
tcp_connection_create_abort    ops=20000        elapsed_ms=833.184    ops_per_s=24004.3
async_listener_echo_roundtrip  ops=1000         elapsed_ms=220.608    ops_per_s=4532.93        MiB_per_s=2.21334
async_listener_persistent_echo ops=5000         elapsed_ms=409.638    ops_per_s=12205.9        MiB_per_s=11.9198
async_listener_concurrent_echo ops=2000         elapsed_ms=522.325    ops_per_s=3829.03        MiB_per_s=7.47858
in_process_worker_lifecycle    ops=100          elapsed_ms=566.791    ops_per_s=176.432
```

## Local Verification After Worker-Local Plugin Runtime Adapter

Date: 2026-05-17

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

Command:

```powershell
.\build\test\benchmark\core_runtime_benchmark.exe
```

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection,worker_lifecycle
byte_buffer_append_copy        ops=200000       elapsed_ms=69.9506    ops_per_s=2.85916e+06    MiB_per_s=2792.15
buffer_chain_push_pop          ops=800000       elapsed_ms=632.446    ops_per_s=1.26493e+06    MiB_per_s=617.642
event_bus_publish              ops=300000       elapsed_ms=207.717    ops_per_s=1.44427e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=77.4656    ops_per_s=3.87269e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=223.304    ops_per_s=671731
detached_coroutine_lifecycle   ops=50000        elapsed_ms=17.1022    ops_per_s=2.9236e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=64.6767    ops_per_s=309230
tcp_connection_create_abort    ops=20000        elapsed_ms=755.756    ops_per_s=26463.6
async_listener_echo_roundtrip  ops=1000         elapsed_ms=207.962    ops_per_s=4808.58        MiB_per_s=2.34794
async_listener_persistent_echo ops=5000         elapsed_ms=387.56     ops_per_s=12901.2        MiB_per_s=12.5989
async_listener_concurrent_echo ops=2000         elapsed_ms=401.556    ops_per_s=4980.63        MiB_per_s=9.72779
in_process_worker_lifecycle    ops=100          elapsed_ms=454.575    ops_per_s=219.985
```

## Local Verification After EndpointManager Planning Layer

Date: 2026-05-17

Host: Windows, MinGW Makefiles, GCC 12.2.0, existing `build` tree (`CMAKE_BUILD_TYPE=Debug`)

Command:

```powershell
.\build\test\benchmark\core_runtime_benchmark.exe
```

Raw debug output:

```text
core runtime benchmark
build=manual chrono=steady_clock scope=buffer,event_bus,runtime,coroutine,connection,worker_lifecycle
byte_buffer_append_copy        ops=200000       elapsed_ms=69.4653    ops_per_s=2.87914e+06    MiB_per_s=2811.66
buffer_chain_push_pop          ops=800000       elapsed_ms=585.939    ops_per_s=1.36533e+06    MiB_per_s=666.665
event_bus_publish              ops=300000       elapsed_ms=219.073    ops_per_s=1.36941e+06
runtime_dispatch_callbacks     ops=300000       elapsed_ms=77.5635    ops_per_s=3.8678e+06
coroutine_schedule_chain       ops=150000       elapsed_ms=220.967    ops_per_s=678835
detached_coroutine_lifecycle   ops=50000        elapsed_ms=16.3947    ops_per_s=3.04977e+06
timer_coroutine_lifecycle      ops=20000        elapsed_ms=63.6322    ops_per_s=314306
tcp_connection_create_abort    ops=20000        elapsed_ms=772.187    ops_per_s=25900.5
async_listener_echo_roundtrip  ops=1000         elapsed_ms=210.464    ops_per_s=4751.4         MiB_per_s=2.32002
async_listener_persistent_echo ops=5000         elapsed_ms=384.577    ops_per_s=13001.3        MiB_per_s=12.6966
async_listener_concurrent_echo ops=2000         elapsed_ms=356.664    ops_per_s=5607.52        MiB_per_s=10.9522
in_process_worker_lifecycle    ops=100          elapsed_ms=368.905    ops_per_s=271.073
```

## Linux HTTP Worker-Pool Throughput Check

Date: 2026-05-17

Host: Linux, Ubuntu 20.04, kernel `5.15.0-125-generic`, x86_64, 4 logical CPUs.

Build:

- Yuan benchmark: `build-release-gcc14`, `CMAKE_BUILD_TYPE=Release`, GCC/G++ 14.3.0.
- libuv: local `/home/yuan/codes/repos/network/libuv/build-release`, `CMAKE_BUILD_TYPE=Release`.
- workflow: local `/home/yuan/codes/repos/network/workflow/build-release`, `CMAKE_BUILD_TYPE=Release`.
- libevent: local `/home/yuan/codes/repos/network/libevent/build` debug static libraries. A Release configure attempt was blocked by this host's CMake 3.16 missing `CheckLinkerFlag`, so the libevent comparison is conservative and not strictly release-to-release.

Benchmark shape:

- one process
- 4 server workers/listeners
- loopback HTTP/1.1 keep-alive
- `GET /bench`
- fixed `Content-Length: 2` body: `OK`
- same built-in load generator for every implementation

Commands:

```sh
cmake -S . -B build-release-gcc14 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++-14 -DCMAKE_C_COMPILER=/usr/bin/gcc-14
cmake --build build-release-gcc14 --target \
  http_worker_pool_benchmark test_http_shared_reuse_port \
  test_endpoint_manager test_in_process_worker_runtime -j 4

./build-release-gcc14/test/protocol/http/test_http_shared_reuse_port
./build-release-gcc14/test/core/test_endpoint_manager
./build-release-gcc14/test/core/test_in_process_worker_runtime

./build-release-gcc14/test/benchmark/http_worker_pool_benchmark yuan 4 3 1,2,4,8,16,32,64,128,256
./build-release-gcc14/test/benchmark/http_worker_pool_benchmark libevent 4 3 1,2,4,8,16,32,64,128,256
./build-release-gcc14/test/benchmark/http_worker_pool_benchmark libuv 4 3 1,2,4,8,16,32,64,128,256
./build-release-gcc14/test/benchmark/http_worker_pool_benchmark workflow 4 3 1,2,4,8,16,32,64,128,256
```

Peak observed RPS:

| Implementation | Best concurrency | Best RPS | Notes |
| --- | ---: | ---: | --- |
| `yuan` | 16 | 59,511 | 3-second sweep; focused 5-second retest peaked at concurrency 12 with 67,746 RPS, with repeated concurrency-12 runs at 67,552 / 61,266 / 58,259 RPS. Treat the current practical ceiling as roughly 60k-68k RPS on this host. |
| `libevent` | 8 | 116,418 | Uses existing debug static libevent build; release build unavailable on this host. |
| `libuv` | 256 | 169,606 | Release build. A short 256/512/1024 follow-up peaked at 256 with 140,316 RPS, so 256 is near the knee for this client/server shape. |
| `workflow` | 256 | 105,102 | Release build. A short 256/512/1024 follow-up peaked at 512 with 103,747 RPS. |

Raw Yuan output:

```text
http worker-pool benchmark implementation=yuan workers=4 duration_s=3
concurrency,requests,requests_per_second
1,49404,16467
2,107494,35827.9
4,126671,42215.5
8,139901,46621.7
16,178630,59510.6
32,133436,44413.7
64,133565,44402.7
128,91586,30134.6
256,93325,30626.3
best_concurrency=16 best_requests_per_second=59510.6
```

Raw comparison output:

```text
http worker-pool benchmark implementation=libevent workers=4 duration_s=3
concurrency,requests,requests_per_second
1,72925,24306.8
2,106406,35458
4,197151,65709
8,349336,116418
16,283410,94435.4
32,305959,101934
64,303171,100915
128,314490,103865
256,298892,98648.6
best_concurrency=8 best_requests_per_second=116418

http worker-pool benchmark implementation=libuv workers=4 duration_s=3
concurrency,requests,requests_per_second
1,124774,41587.9
2,187014,62330.3
4,246153,82043.8
8,353406,117776
16,404244,134710
32,463629,154469
64,473788,157778
128,502211,167079
256,516395,169606
best_concurrency=256 best_requests_per_second=169606

http worker-pool benchmark implementation=workflow workers=4 duration_s=3
concurrency,requests,requests_per_second
1,38379,12791.6
2,70742,23578
4,115913,38631.7
8,162644,54201.5
16,186900,62277.5
32,241253,80367.6
64,265515,88343.5
128,299647,99091.9
256,317374,105102
best_concurrency=256 best_requests_per_second=105102
```

Interpretation:

The current Yuan HTTP worker-pool path is functional on Linux with `reuse_port`, but its throughput ceiling is well below the simpler libuv/libevent responders under the same load generator. The drop after the 12-16 concurrency range points at per-request HTTP stack/runtime overhead rather than listener scaling failure. Next profiling should focus on the HTTP request-complete path, route dispatch, response send/close scheduling, and per-request allocations/timers before changing the worker placement model again.

## Final Local Runtime-Worker Refactor Smoke

Date: 2026-05-17

Build targets:

```sh
cmake --build build --target \
  test_endpoint_manager test_in_process_worker_runtime test_plugin_governance \
  test_http_shared_reuse_port core_runtime_benchmark http_worker_pool_benchmark -j 4
```

Regression subset:

```sh
ctest -R "^(logger_auto|net_logger_auto|coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|timer_backends|supervisor_reason|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics|async_listener_shutdown|compressed_trie|udp_kcp_adapter|service_definition_registration|worker_plan|endpoint_manager|in_process_worker_runtime|plugin_governance|http_shared_reuse_port)$" --output-on-failure --timeout 180
```

Result: 25/25 passed.

Local benchmark smoke:

```text
core_runtime_benchmark: completed buffer/event/runtime/coroutine/connection/worker lifecycle coverage.
http_worker_pool_benchmark yuan workers=4 duration_s=3: best_concurrency=16 best_requests_per_second=29859.5
```

This final smoke was run from the normal local build tree, so it is a functional/regression check rather than a release-grade throughput comparison. The release comparison above remains the useful HTTP CPU-tail reference.

## Windows MinGW Core Throughput Pass

Date: 2026-05-18

Host/build:

- Windows, MinGW GCC 12.2.0, `build-mingw-release`, `CMAKE_BUILD_TYPE=Release`.
- External root: `D:/code/repos/network`.
- libuv was built locally with MinGW static release output at `D:/code/repos/network/libuv/build-release/libuv.a`.
- workflow was not included because its CMake currently rejects MinGW.
- libevent was not included because the available `libevent-2.1.8-stable/lib/*.lib` artifacts are MSVC/Windows libraries and the benchmark's libevent implementation currently targets the pthread-style path.
- Windows does not support the current `reuse_port` worker-pool path, so HTTP comparison below is single worker only.

Changes validated in this pass:

- `BufferChain` now uses `std::deque` for FIFO storage, removing `vector::erase(begin)` from real output queue pop paths.
- `HttpService::stop()` dispatches `HttpServer::stop()` onto the service runtime thread before host shutdown, preventing concurrent `sessions_` mutation during high-concurrency shutdown.
- `PollPoller` now maintains fd-to-index state for O(1) channel updates/removals instead of scanning the poll fd vector on each write-interest change.
- Core now has small Windows IOCP primitives used by the benchmark path, covering completion-port lifecycle, socket association, posted completions, blocking waits, overlapped socket creation, `AcceptEx` function loading/posting/address parsing, `WSARecv`/`WSASend` posting, and IO cancellation.
- The benchmark CMake links libuv with Windows system libraries when building on Win32.

Regression subset:

```text
ctest -R "^(event_token|tcp_close_semantics|async_facades|buffer_model|byte_buffer_reader|http_shared_reuse_port)$" --output-on-failure --timeout 180
100% tests passed, 0 tests failed out of 6

ctest -R "^(iocp_completion_port|event_token|tcp_close_semantics|async_facades|buffer_model|byte_buffer_reader|http_shared_reuse_port)$" --output-on-failure --timeout 180
100% tests passed, 0 tests failed out of 7
```

Core benchmark observations:

- `buffer_chain_push_pop` improved from roughly 7.5-7.7M ops/s to roughly 8.2-8.6M ops/s immediately after the deque change.
- TCP echo and HTTP throughput did not materially improve from the poller maintenance change on this host; the remaining gap is more likely the Windows `WSAPoll` model plus HTTP request/response stack overhead.

HTTP worker-pool benchmark, single worker, sequential 3 second sweep:

```text
http_worker_pool_benchmark yuan 1 3 1,2,4,8,16,24,32,64,128
best_concurrency=16 best_requests_per_second=29434.8

http_worker_pool_benchmark libuv 1 3 1,2,4,8,16,24,32,64,128
best_concurrency=16 best_requests_per_second=89632.9

http_worker_pool_benchmark iocp 1 3 1,2,4,8,16,24,32,64,128
best_concurrency=16 best_requests_per_second=94606.6

http_worker_pool_benchmark iocp 4 3 1,2,4,8,16,24,32,64,128,256
best_concurrency=24 best_requests_per_second=229683

http_worker_pool_benchmark iocp 4 2 1,2,4,8,16,24,32,64,128
best_concurrency=24 best_requests_per_second=217270

http_worker_pool_benchmark iocp 1 3 1,2,4,8,16,24,32,64,128
best_concurrency=16 best_requests_per_second=93745.5

http_worker_pool_benchmark iocp 4 3 1,2,4,8,16,24,32,64,128,256
best_concurrency=16 best_requests_per_second=230366

http_worker_pool_benchmark iocp 4 3 1,2,4,8,16,24,32,64,128,256
best_concurrency=32 best_requests_per_second=228239
```

The benchmark-only IOCP server is intentionally minimal: it now pre-posts overlapped `AcceptEx` operations and uses core IOCP helpers for `WSARecv`/`WSASend` completion workers. It avoids the Yuan HTTP stack and therefore is not a drop-in production optimization, but it is a useful direction check. Single-worker IOCP is close to libuv on the same machine, and four IOCP completion workers scale to roughly 2.6x the single-worker libuv baseline. Current `yuan 4` and `libuv 4` Windows runs fail to start through the benchmark's worker-pool/reuse-port path, so the multi-worker comparison is IOCP-only for now.

Practical blocker summary:

- Windows core networking still uses the readiness-style poller path. The remaining HTTP gap is dominated by the platform backend and request/response stack path, not by `BufferChain` FIFO behavior.
- The first real core IOCP step should now move from primitives to a native backend/runtime adapter with operation lifetime, cancellation, accepted-socket handoff, and wakeup semantics. The benchmark POC proves direction, but it should not be copied into production as an HTTP-specific fast path.
- A production IOCP backend should reuse the current `IocpCompletionPort`, `IocpAcceptEx`, and `IocpTcpIo` primitives, keep per-operation ownership explicit, and share the existing runtime dispatch contract so TCP, HTTP, and later SSL/UDP integrations do not diverge.

Before the stop-path fix, `http_worker_pool_benchmark yuan 1 1 24/32` intermittently crashed with Windows heap corruption during `HttpServer::stop()`. After dispatching stop onto the runtime thread, concurrency 24/32/64 completed cleanly.

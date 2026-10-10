#pragma once
#include "guard.h"
#include "providers.h"

namespace as {

class Sampler {
public:
    explicit Sampler(Roots r = {});
    SnapshotPtr sample();

private:
    Roots r_;
    CpuProvider cpu_;
    MemoryProvider mem_;
    DiskProvider disks_;
    NetProvider nets_;
    GpuProvider gpus_;
    SensorProvider sensors_;
    ProcessProvider procs_;
    CgroupProvider cgroups_;
    ThermalGuard guard_;
    uint64_t generation_ = 0;
    SnapshotPtr last_;
    double last_proc_scan_ = -1e9;
    double last_units_ = -1e9;
};

double monotonic_seconds();
double wall_seconds();
double thread_cpu_seconds();  // CPU time used by the calling thread

}  // namespace as

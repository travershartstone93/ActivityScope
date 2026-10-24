#pragma once
#include "guard.h"
#include "providers.h"

namespace as {

class Sampler {
public:
    explicit Sampler(Roots r = {});
    // processes: scan the processes now, not only when due (every 3 s while the machine is calm).
    SnapshotPtr sample(bool processes = false);

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
    double last_statvfs_ = -1e9;
    Reading<double> root_free_ = Reading<double>::none("not read yet");
};

double monotonic_seconds();
double wall_seconds();
double thread_cpu_seconds();  // CPU time used by the calling thread

}  // namespace as

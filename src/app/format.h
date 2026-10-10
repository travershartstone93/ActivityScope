#pragma once
#include <QString>
#include <cmath>

#include "events.h"
#include "reading.h"

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline QString bytes(double b) { return qs(as::human_bytes(b)); }
inline QString rate(double bps) { return bps < 1000 ? QStringLiteral("0 KB/s") : bytes(bps) + "/s"; }
inline QString pct(double v, int decimals = 1) { return QString::number(v, 'f', decimals) + "%"; }
inline QString duration(double s) { return qs(as::human_duration(s)); }
inline QString clockText(double epoch) { return qs(as::clock_time(epoch)); }
inline QString mhz(double m) { return m >= 1000 ? QString::number(m / 1000, 'f', 2) + " GHz" : QString::number(m, 'f', 0) + " MHz"; }
inline QString celsius(double c) { return QString::number(c, 'f', 1) + " °C"; }

template <class T, class F>
QString orWhy(const as::Reading<T>& r, F f) {
    return r.ok() ? f(r.get()) : QStringLiteral("Unavailable");
}

// Plain names for the kernel's sensor chip names.
inline QString chipName(const std::string& chip) {
    if (chip == "coretemp" || chip == "k10temp" || chip == "zenpower") return "CPU";
    if (chip == "acpitz") return "Motherboard";
    if (chip.rfind("nvme", 0) == 0) return "SSD";
    if (chip.rfind("pch_", 0) == 0) return "Chipset";
    if (chip.rfind("iwlwifi", 0) == 0 || chip.rfind("ath", 0) == 0 || chip.rfind("mt7", 0) == 0) return "Wi-Fi";
    if (chip == "amdgpu" || chip == "nouveau" || chip == "radeon") return "GPU";
    if (chip.rfind("BAT", 0) == 0) return "Battery";
    return QString::fromStdString(chip);
}

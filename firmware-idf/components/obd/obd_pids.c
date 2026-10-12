/* obd_pids.c — tabela podstawowych PID-ow OBD-II (tryb 01) + dekodery. */
#include "obd.h"

/* Dekodery (wg SAE J1979) */
static float d_u8(const uint8_t *d)   { return (float)d[0]; }
static float d_u16(const uint8_t *d)  { return (float)(d[0] * 256u + d[1]); }
static float d_pct(const uint8_t *d)  { return (float)d[0] * 100.0f / 255.0f; }
static float d_temp(const uint8_t *d) { return (float)d[0] - 40.0f; }
static float d_rpm(const uint8_t *d)  { return (float)(d[0] * 256u + d[1]) / 4.0f; }
static float d_kph(const uint8_t *d)  { return (float)d[0]; }
static float d_kpa(const uint8_t *d)  { return (float)d[0]; }
static float d_trim(const uint8_t *d) { return (float)d[0] / 1.28f - 100.0f; }
static float d_timing(const uint8_t *d) { return (float)d[0] / 2.0f - 64.0f; }
static float d_maf(const uint8_t *d)  { return (float)(d[0] * 256u + d[1]) / 100.0f; }
static float d_volt(const uint8_t *d) { return (float)(d[0] * 256u + d[1]) / 1000.0f; }
static float d_lambda(const uint8_t *d) { return 2.0f / 65536.0f * (float)(d[0] * 256u + d[1]); }
static float d_fuel_rate(const uint8_t *d) { return (float)(d[0] * 256u + d[1]) / 20.0f; }
static float d_sec(const uint8_t *d)  { return (float)(d[0] * 256u + d[1]); }
static float d_km(const uint8_t *d)   { return (float)(d[0] * 256u + d[1]); }

#define P(pid, name, unit, nb, fn) { 0x##pid, name, unit, nb, fn }

static const obd_pid_t TABLE[] = {
    P(04, "EngineLoad",       "%",      1, d_pct),
    P(05, "CoolantTemp",      "C",      1, d_temp),
    P(0A, "FuelPressure",     "kPa",    1, d_kpa),
    P(0B, "IntakeMAP",        "kPa",    1, d_kpa),
    P(0C, "EngineRPM",        "rpm",    2, d_rpm),
    P(0D, "VehicleSpeed",     "km/h",   1, d_kph),
    P(0F, "IntakeTemp",       "C",      1, d_temp),
    P(10, "MAF",              "g/s",    2, d_maf),
    P(11, "ThrottlePos",      "%",      1, d_pct),
    P(1F, "RunTime",          "s",      2, d_sec),
    P(21, "DistWithMIL",      "km",     2, d_km),
    P(2F, "FuelLevel",        "%",      1, d_pct),
    P(31, "DistSinceClear",   "km",     2, d_km),
    P(33, "BaroPressure",     "kPa",    1, d_kpa),
    P(42, "ModuleVoltage",    "V",      2, d_volt),
    P(44, "CommandedLambda",  "lambda", 2, d_lambda),
    P(46, "AmbientTemp",      "C",      1, d_temp),
    P(51, "FuelType",         "code",   1, d_u8),
    P(5C, "OilTemp",          "C",      1, d_temp),
    P(5E, "FuelRate",         "L/h",    2, d_fuel_rate),
    P(03, "FuelSystemStatus", "code",   2, d_u16),
};

const obd_pid_t *obd_pid_table(size_t *count)
{
    if (count) {
        *count = sizeof(TABLE) / sizeof(TABLE[0]);
    }
    return TABLE;
}

const obd_pid_t *obd_find_pid(uint8_t pid)
{
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (TABLE[i].pid == pid) {
            return &TABLE[i];
        }
    }
    return NULL;
}

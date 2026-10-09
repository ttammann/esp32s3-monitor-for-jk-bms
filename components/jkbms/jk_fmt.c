#include "jk_fmt.h"
#include "strbuf.h"

#define CELLS_PER_ROW 8

int jk_fmt_cell_rows(uint32_t cell_present)
{
    int rows = 0;
    for (int i = 0; i < JK55_MAX_CELLS; i += CELLS_PER_ROW) {
        if (!(cell_present >> i)) {
            break;                       // no populated cells left
        }
        rows++;
    }
    return rows;
}

int jk_fmt_cell_row(char *out, size_t on, const uint16_t *cell_mv,
                    size_t ncells, int row)
{
    if (out == NULL || on == 0) {
        return -1;
    }
    out[0] = '\0';
    if (cell_mv == NULL || row < 0) {
        return -1;
    }
    const size_t first = (size_t)row * CELLS_PER_ROW;
    if (first >= ncells) {
        return -1;
    }
    int w = 0;
    for (size_t i = first; i < first + CELLS_PER_ROW && i < ncells; i++) {
        w = sb_addf(out, on, w, "%s%4u", i == first ? "" : " ",
                    (unsigned)cell_mv[i]);
    }
    return sb_overflowed(on, w) ? -1 : w;
}

void jk_fmt_hex(char *out, size_t on, const uint8_t *b, size_t n)
{
    if (out == NULL || on == 0) {
        return;
    }
    out[0] = '\0';
    if (b == NULL) {
        return;
    }
    int w = 0;
    for (size_t i = 0; i < n; i++) {
        w = sb_addf(out, on, w, "%s%02X", i ? " " : "", (unsigned)b[i]);
        if (sb_overflowed(on, w)) {
            break;                       // buffer full; leave what fits
        }
    }
}

uint16_t jk_fmt_cell_at(const jk55_dynamic_t *d, uint8_t idx)
{
    if (d == NULL || idx >= JK55_MAX_CELLS) {
        return 0;
    }
    return d->cell_mv[idx];
}

int jk_fmt_state_json(char *buf, size_t n, const jk55_dynamic_t *d,
                      int rssi, int64_t uptime_s, uint32_t frame_count)
{
    if (buf == NULL || d == NULL) {
        return -1;
    }
    const uint16_t min_mv = jk_fmt_cell_at(d, d->cell_min_idx);
    const uint16_t max_mv = jk_fmt_cell_at(d, d->cell_max_idx);

    // The worst wire resistance across populated cells, not all sixteen values:
    // one number that goes up when a crimp is failing is actionable, and
    // sixteen entities that never move are not.
    int16_t wire_max = 0;
    for (int i = 0; i < JK55_MAX_CELLS; i++) {
        if (((d->cell_present >> i) & 1u) && d->wire_res_mohm[i] > wire_max) {
            wire_max = d->wire_res_mohm[i];
        }
    }

    // Signed, because a magnitude is fine for a gauge but useless to anything
    // integrating it: Home Assistant's energy dashboard cannot tell charge
    // from discharge and would count a discharge as energy going in. The sign
    // rule lives in jk55_power_mw_signed() so the console line cannot drift
    // from this one.
    const double power_w = jk55_power_mw_signed(d) / 1000.0;

    int w = sb_addf(buf, n, 0,
        "{\"pack_v\":%.3f,\"current_a\":%.3f,\"power_w\":%.1f,"
        "\"soc\":%u,\"soh\":%u,"
        "\"cell_min_v\":%.3f,\"cell_max_v\":%.3f,"
        "\"cell_avg_v\":%.3f,\"cell_delta_v\":%.3f,"
        "\"cell_min_idx\":%u,\"cell_max_idx\":%u,\"cell_count\":%u,"
        "\"temp_mos\":%.1f,\"temp1\":%.1f,\"temp2\":%.1f,"
        "\"balance_a\":%.3f,\"alarms\":%lu,\"wire_res_max_mohm\":%d,"
        "\"chg_mos\":%s,\"dsg_mos\":%s,\"bal_mos\":%s,"
        "\"remain_ah\":%.3f,\"full_ah\":%.3f,"
        "\"cycles\":%lu,\"cycle_ah\":%.3f,\"runtime_s\":%lu,"
        "\"frames\":%lu,\"rssi\":%d,\"uptime_s\":%lld,"
        "\"cells\":[",
        d->pack_mv / 1000.0, d->current_ma / 1000.0, power_w,
        (unsigned)d->soc_pct, (unsigned)d->soh_pct,
        min_mv / 1000.0, max_mv / 1000.0,
        d->cell_avg_mv / 1000.0, d->cell_delta_mv / 1000.0,
        (unsigned)(d->cell_min_idx + 1), (unsigned)(d->cell_max_idx + 1),
        (unsigned)d->cell_count,
        d->mos_temp_dc / 10.0, d->t1_dc / 10.0, d->t2_dc / 10.0,
        d->balance_ma / 1000.0, (unsigned long)d->alarms, (int)wire_max,
        d->chg_mos ? "true" : "false", d->dsg_mos ? "true" : "false",
        d->bal_mos ? "true" : "false",
        d->remain_mah / 1000.0, d->full_mah / 1000.0,
        (unsigned long)d->cycles, d->cycle_mah / 1000.0,
        (unsigned long)d->runtime_s,
        (unsigned long)frame_count,
        rssi, (long long)uptime_s);

    // Indexed by physical position, up to the highest cell the pack reports
    // present. Publishing 32 slots for a 16S pack would give Home Assistant
    // sixteen entities stuck at 0.000 V; compacting out the holes instead
    // would shift every later cell onto its neighbour's entity, so a weak
    // cell 9 would be reported as cell 5. Holes are null, which the discovery
    // template turns into an unknown entity.
    int i = 0;
    for (uint32_t m = d->cell_present; m; m >>= 1, i++) {
        w = sb_addf(buf, n, w, i ? "," : "");
        w = (m & 1u) ? sb_addf(buf, n, w, "%.3f", d->cell_mv[i] / 1000.0)
                     : sb_addf(buf, n, w, "null");
    }
    w = sb_addf(buf, n, w, "]}");
    return sb_overflowed(n, w) ? -1 : w;
}

int jk_fmt_settings_json(char *buf, size_t n, const jk55_settings_t *s)
{
    if (buf == NULL || s == NULL) {
        return -1;
    }
    int w = sb_addf(buf, n, 0,
        "{\"cell_count\":%lu,\"design_ah\":%.3f,\"device_addr\":%lu,"
        "\"cell_ovp_v\":%.3f,\"cell_ovp_recover_v\":%.3f,"
        "\"cell_uvp_v\":%.3f,\"cell_uvp_recover_v\":%.3f,"
        "\"balance_start_v\":%.3f,\"balance_trigger_v\":%.3f,"
        "\"charge_limit_a\":%.3f,\"discharge_limit_a\":%.3f,"
        "\"balance_limit_a\":%.3f,"
        "\"soc100_v\":%.3f,\"soc0_v\":%.3f,"
        "\"float_v\":%.3f,\"charge_max_v\":%.3f,"
        "\"chg_otp_c\":%.1f,\"chg_utp_c\":%.1f,"
        "\"dsg_otp_c\":%.1f,\"mos_otp_c\":%.1f,"
        "\"charge_enabled\":%s,\"discharge_enabled\":%s,"
        "\"balance_enabled\":%s}",
        (unsigned long)s->cell_count, s->design_capacity_mah / 1000.0,
        (unsigned long)s->device_addr,
        s->cell_ovp_mv / 1000.0, s->cell_ovp_recover_mv / 1000.0,
        s->cell_uvp_mv / 1000.0, s->cell_uvp_recover_mv / 1000.0,
        s->balance_start_mv / 1000.0, s->balance_trigger_mv / 1000.0,
        s->charge_current_ma / 1000.0, s->discharge_current_ma / 1000.0,
        s->max_balance_current_ma / 1000.0,
        s->soc100_mv / 1000.0, s->soc0_mv / 1000.0,
        s->float_mv / 1000.0, s->charge_max_mv / 1000.0,
        s->charge_otp_dc / 10.0, s->charge_utp_dc / 10.0,
        s->discharge_otp_dc / 10.0, s->mos_otp_dc / 10.0,
        s->charge_enabled ? "true" : "false",
        s->discharge_enabled ? "true" : "false",
        s->balance_enabled ? "true" : "false");
    return sb_overflowed(n, w) ? -1 : w;
}

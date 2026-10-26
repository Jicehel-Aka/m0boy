/* Plays a Mega Drive YM2612 write log (md_pc --ym-log) through m0boy's
 * YM2612 (targets/megadrive/runtime/md_ym2612.c) and through ymfm, sample
 * by sample at fM/144, and reports how closely the two outputs agree:
 * the signal-to-difference ratio after the best gain and a lag of a few
 * samples (the chips' pipelines differ), over the whole log and per window.
 *
 *   md_ym_compare LOG [--samples N] [--min-snr DB] [--window N] [--dump FILE] [--exact]
 *
 * Exits 1 when the whole-log ratio of either side falls under --min-snr,
 * or with --exact when any sample differs from ymfm's once scaled as ymfm
 * scales its 9-bit sum. */
#include "ymfm_opn.h"
extern "C" {
#include "md_ym2612.h"
}
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct Write { uint64_t sample; unsigned bank, reg, value; };

double ratio_db(const std::vector<double> &ref, const std::vector<double> &ours, int lag,
                size_t from, size_t to, double *gain_out) {
    double rr = 0, ro = 0, oo = 0;
    for (size_t i = from; i < to; ++i) {
        const long j = long(i) + lag;
        if (j < 0 || size_t(j) >= ours.size()) continue;
        rr += ref[i] * ref[i]; ro += ref[i] * ours[size_t(j)]; oo += ours[size_t(j)] * ours[size_t(j)];
    }
    if (rr == 0 || oo == 0) { if (gain_out) *gain_out = 0; return rr == oo ? 200.0 : 0.0; }
    const double g = ro / oo;   /* least-squares gain applied to ours */
    double err = 0;
    for (size_t i = from; i < to; ++i) {
        const long j = long(i) + lag;
        const double o = (j < 0 || size_t(j) >= ours.size()) ? 0 : ours[size_t(j)];
        err += (ref[i] - g * o) * (ref[i] - g * o);
    }
    if (gain_out) *gain_out = g;
    return err == 0 ? 200.0 : 10.0 * std::log10(rr / err);
}
}

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: md_ym_compare LOG [--samples N] [--min-snr DB] [--window N]\n"); return 2; }
    uint64_t samples = 0;
    double min_snr = -1e9;
    size_t window = 53267 / 2;
    const char *dump = nullptr;
    bool exact = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--samples") && i + 1 < argc) samples = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--min-snr") && i + 1 < argc) min_snr = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) window = std::strtoul(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!std::strcmp(argv[i], "--exact")) exact = true;
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    std::FILE *f = std::fopen(argv[1], "r");
    if (!f) { std::perror(argv[1]); return 2; }
    std::vector<Write> log;
    unsigned long long at; unsigned bank, reg, value;
    while (std::fscanf(f, "%llu %u %x %x", &at, &bank, &reg, &value) == 4) log.push_back({at, bank, reg, value});
    std::fclose(f);
    if (log.empty()) { std::fprintf(stderr, "empty log\n"); return 2; }
    if (!samples) samples = log.back().sample + 53267;

    ymfm::ymfm_interface intf;
    ymfm::ym2612 ref_chip(intf);
    ref_chip.reset();
    MdYm2612 ours_chip;
    md_ym_reset(&ours_chip);
    std::vector<double> ref[2], ours[2];
    size_t next = 0;
    for (uint64_t n = 0; n < samples; ++n) {
        for (; next < log.size() && log[next].sample <= n; ++next) {
            const Write &w = log[next];
            ref_chip.write(w.bank * 2u, uint8_t(w.reg));
            ref_chip.write(w.bank * 2u + 1u, uint8_t(w.value));
            md_ym_write(&ours_chip, w.bank, uint8_t(w.reg), uint8_t(w.value));
        }
        ymfm::ym2612::output_data out;
        ref_chip.generate(&out);
        int l, r;
        md_ym_sample(&ours_chip, &l, &r);
        ref[0].push_back(out.data[0]); ref[1].push_back(out.data[1]);
        ours[0].push_back(l); ours[1].push_back(r);
    }
    if (dump) {   /* sample, reference L, ours L, reference R, ours R */
        std::FILE *d = std::fopen(dump, "w");
        if (!d) { std::perror(dump); return 2; }
        for (size_t i = 0; i < ref[0].size(); ++i)
            std::fprintf(d, "%zu %.0f %.0f %.0f %.0f\n", i, ref[0][i], ours[0][i], ref[1][i], ours[1][i]);
        std::fclose(d);
    }
    int status = 0;
    for (unsigned side = 0; side < 2; ++side) {
        int best_lag = 0;
        double best = -1e9, gain = 0;
        for (int lag = -6; lag <= 6; ++lag) {
            double g;
            const double db = ratio_db(ref[side], ours[side], lag, 0, ref[side].size(), &g);
            if (db > best) { best = db; best_lag = lag; gain = g; }
        }
        double worst = 1e9;
        size_t worst_at = 0, silent = 0, windows = 0;
        for (size_t from = 0; from + window <= ref[side].size(); from += window) {
            double energy = 0;
            for (size_t i = from; i < from + window; ++i) energy += ref[side][i] * ref[side][i];
            if (energy / double(window) < 1e4) { ++silent; continue; }   /* below the ladder offset's level */
            ++windows;
            const double db = ratio_db(ref[side], ours[side], best_lag, from, from + window, nullptr);
            if (db < worst) { worst = db; worst_at = from; }
        }
        /* exact: ours is a sum of 9-bit DAC levels times 32; ymfm scales
         * the same sum by 128 * 64 / (6 * 65) */
        size_t differ = 0;
        for (size_t i = 0; i < ref[side].size(); ++i)
            if (ref[side][i] != double((int32_t(ours[side][i]) / 32 * 128) * 64 / (6 * 65))) ++differ;
        std::printf("%s: snr=%.1fdB lag=%d gain=%.3f windows=%zu silent=%zu worst=%.1fdB@%.2fs differ=%zu/%zu\n",
                    side ? "right" : "left", best, best_lag, gain, windows, silent,
                    windows ? worst : 0.0, double(worst_at) / 53267.0, differ, ref[side].size());
        if (best < min_snr || (exact && differ)) status = 1;
    }
    return status;
}

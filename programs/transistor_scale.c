/* ============================================================================
 * transistor_scale.c — MOSFET Transistor Parameter Simulator
 *
 * Models a single transistor at the physical level:
 *   - Channel width, oxide thickness, doping concentration
 *   - Threshold voltage (Vth) from physical parameters
 *   - Drain current (Id) in saturation region
 *   - SIMD sweep of 8 configurations in parallel
 *
 * Build: gcc -O3 -mavx2 -march=native -o transistor_scale transistor_scale.c -lm
 * Run:   ./transistor_scale
 * ============================================================================
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <immintrin.h>

// Physical constants
#define EPSILON_OX  3.45e-11  // SiO2 permittivity (F/m)
#define EPSILON_SI  1.04e-10  // Silicon permittivity (F/m)
#define Q           1.60e-19  // Electron charge (C)
#define KT_Q        0.0259    // Thermal voltage at 300K (V)
#define NI          1.5e10    // Intrinsic carrier concentration (cm^-3)

// ─── Threshold voltage: Vth = Vfb + 2*phi_f + gamma * sqrt(2*phi_f) ───
static double compute_vth(double tox, double nd, double phi_ms) {
    double cox = EPSILON_OX / tox;
    double phi_f = KT_Q * log(nd / NI);
    double gamma = sqrt(2 * Q * EPSILON_SI * nd) / cox;
    return phi_ms + 2 * phi_f + gamma * sqrt(2 * phi_f);
}

// ─── Drain current in saturation: Id = (W/2L) * mu * Cox * (Vgs - Vth)^2 ───
static double compute_idsat(double w, double l, double mu, double cox, double vgs, double vth) {
    double vov = vgs - vth;
    if (vov < 0) return 0;
    return (w / (2 * l)) * mu * cox * vov * vov;
}

// ─── SIMD: compute 4 Vth values in parallel ───
// Packed as 4x float. We use floats for SIMD, doubles for precision.
static void compute_vth_simd(float *tox, float *nd, float *phi_ms, float *vth_out) {
    // Load 4 values
    __m128 t = _mm_loadu_ps(tox);
    __m128 n = _mm_loadu_ps(nd);
    __m128 p = _mm_loadu_ps(phi_ms);
    
    // cox = EPSILON_OX / tox
    __m128 cox = _mm_div_ps(_mm_set1_ps(EPSILON_OX), t);
    
    // phi_f = KT_Q * log(nd / NI) — approximated as polynomial
    // log(x) ≈ 2 * (x-1)/(x+1) for x near 1, but nd/NI is huge
    // Use scalar for log since AVX2 log is expensive. We'll compute per-lane.
    float nd_arr[4], p_arr[4], cox_arr[4], vth_arr[4];
    _mm_storeu_ps(nd_arr, n);
    _mm_storeu_ps(p_arr, p);
    _mm_storeu_ps(cox_arr, cox);
    
    for (int i = 0; i < 4; i++) {
        double nd_d = nd_arr[i];
        double phi_f = KT_Q * log(nd_d / NI);
        double gamma = sqrt(2 * Q * EPSILON_SI * nd_d) / cox_arr[i];
        vth_arr[i] = p_arr[i] + 2 * phi_f + gamma * sqrt(2 * phi_f);
    }
    
    _mm_storeu_ps(vth_out, _mm_loadu_ps(vth_arr));
}

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   TRANSISTOR SCALE — Physical Parameter Simulator   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    
    // ─── 8 transistor variants ───
    // Technology nodes: 180nm, 130nm, 90nm, 65nm, 45nm, 32nm, 22nm, 14nm
    struct {
        const char *name;
        double tox;      // Oxide thickness (m)
        double nd;       // Doping concentration (cm^-3)
        double phi_ms;   // Work function difference (V)
        double w;        // Channel width (m)
        double l;        // Channel length (m)
        double mu;       // Mobility (cm^2/V·s)
    } variants[] = {
        {"180nm", 4.0e-9,  1e17, -0.1, 1e-6, 180e-9, 450},
        {"130nm", 3.0e-9,  2e17, -0.1, 8e-7, 130e-9, 420},
        {"90nm",  2.4e-9,  3e17, -0.1, 6e-7, 90e-9,  380},
        {"65nm",  2.0e-9,  5e17, -0.1, 5e-7, 65e-9,  340},
        {"45nm",  1.8e-9,  8e17, -0.1, 4e-7, 45e-9,  300},
        {"32nm",  1.5e-9,  1e18, -0.1, 3e-7, 32e-9,  260},
        {"22nm",  1.2e-9,  2e18, -0.1, 2e-7, 22e-9,  220},
        {"14nm",  1.0e-9,  4e18, -0.1, 1.5e-7,14e-9, 180},
        {"7nm",   0.7e-9,  8e18, -0.1, 1.0e-7,7e-9,  140},
        {"5nm",   0.5e-9,  1.2e19,-0.1, 8.0e-8,5e-9,  110},
        {"3nm",   0.4e-9,  2e19, -0.1, 6.0e-8,3e-9,  85},
        {"2nm",   0.3e-9,  3e19, -0.1, 5.0e-8,2e-9,  65},
        {"1nm",   0.2e-9,  5e19, -0.1, 4.0e-8,1e-9,  45},
        {"0.7nm", 0.15e-9, 8e19, -0.1, 3.0e-8,0.7e-9,30},
        {"0.5nm", 0.1e-9,  1e20, -0.1, 2.5e-8,0.5e-9,20},
        {"0.3nm", 0.08e-9, 2e20, -0.1, 2.0e-8,0.3e-9,12},
        {"0.1nm", 0.05e-9, 5e20, -0.1, 1.5e-8,0.1e-9,5},
    };
    
    int n = sizeof(variants) / sizeof(variants[0]);
    
    printf("-- Technology Node Comparison (180nm down to 0.1nm) --\n\n");
    printf("%-8s %-12s %-12s %-12s %-12s %-12s %-12s\n",
           "Node", "Vth (V)", "Id_sat(uA)", "Cox (fF)", "phi_f(V)", "gamma", "gm (mS)");
    printf("──────────────────────────────────────────────────────────────────────────────\n");
    
    for (int i = 0; i < n; i++) {
        double *v = (double *)&variants[i];
        double tox = variants[i].tox;
        double nd  = variants[i].nd;
        double pms = variants[i].phi_ms;
        double w   = variants[i].w;
        double l   = variants[i].l;
        double mu  = variants[i].mu * 1e-4;  // cm^2/V·s → m^2/V·s
        
        double cox = EPSILON_OX / tox;
        double vth = compute_vth(tox, nd, pms);
        double vgs = 1.0;  // Standard gate drive
        double ids = compute_idsat(w, l, mu, cox, vgs, vth) * 1e6;  // A → uA
        double phi_f = KT_Q * log(nd / NI);
        double gamma = sqrt(2 * Q * EPSILON_SI * nd) / cox;
        double gm = (w / l) * mu * cox * (vgs - vth) * 1e3;  // S → mS
        
        printf("%-8s %-12.4f %-12.2f %-12.4f %-12.4f %-12.4f %-12.4f\n",
               variants[i].name, vth, ids, cox * 1e15, phi_f, gamma, gm);
    }
    
    // ─── SIMD sweep: 8 variants in 2 batches of 4 ───
    printf("\n── SIMD Accelerated Sweep (2×4 configurations) ──\n\n");
    
    for (int batch = 0; batch < 2; batch++) {
        float tox_f[4], nd_f[4], pms_f[4], vth_f[4];
        for (int j = 0; j < 4; j++) {
            int idx = batch * 4 + j;
            tox_f[j] = variants[idx].tox;
            nd_f[j]  = variants[idx].nd;
            pms_f[j] = variants[idx].phi_ms;
        }
        
        compute_vth_simd(tox_f, nd_f, pms_f, vth_f);
        
        printf("  Batch %d: ", batch + 1);
        for (int j = 0; j < 4; j++) {
            printf("%s(Vth=%.3f) ", variants[batch * 4 + j].name, vth_f[j]);
        }
        printf("\n");
    }
    
    // ─── Parameter sweep: vary tox and see Vth change ───
    printf("\n── Oxide Thickness Sweep (fixed doping Nd=1e17) ──\n\n");
    printf("%-12s %-12s %-12s %-12s\n", "tox (nm)", "Vth (V)", "Cox (fF)", "Id_sat(uA)");
    printf("──────────────────────────────────────────────\n");
    
    double sweep_nd = 1e17;
    double sweep_pms = -0.1;
    double sweep_w = 1e-6, sweep_l = 180e-9, sweep_mu = 450 * 1e-4;
    double sweep_vgs = 1.0;
    
    for (int tox_nm = 10; tox_nm >= 1; tox_nm--) {
        double tox = tox_nm * 1e-9;
        double cox = EPSILON_OX / tox;
        double vth = compute_vth(tox, sweep_nd, sweep_pms);
        double ids = compute_idsat(sweep_w, sweep_l, sweep_mu, cox, sweep_vgs, vth) * 1e6;
        
        printf("%-12d %-12.4f %-12.4f %-12.2f\n", tox_nm, vth, cox * 1e15, ids);
    }
    
    // ─── Doping sweep ───
    printf("\n── Doping Sweep (fixed tox=4nm) ──\n\n");
    printf("%-12s %-12s %-12s\n", "Nd (cm^-3)", "Vth (V)", "Id_sat(uA)");
    printf("─────────────────────────────────────\n");
    
    double dope_tox = 4e-9;
    double dope_nd[] = {1e16, 5e16, 1e17, 5e17, 1e18, 5e18, 1e19};
    
    for (int i = 0; i < 7; i++) {
        double nd_d = dope_nd[i];
        double cox_d = EPSILON_OX / dope_tox;
        double vth_d = compute_vth(dope_tox, nd_d, sweep_pms);
        double ids_d = compute_idsat(sweep_w, sweep_l, sweep_mu, cox_d, sweep_vgs, vth_d) * 1e6;
        
        printf("%-12.0e %-12.4f %-12.2f\n", nd_d, vth_d, ids_d);
    }
    
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║   TRANSISTOR SIMULATION COMPLETE                   ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    
    return 0;
}

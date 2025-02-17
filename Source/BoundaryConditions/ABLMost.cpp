#include <ABLMost.H>
#include <MOSTAverage.H>

using namespace amrex;

/**
 * Wrapper to update ustar and tstar for Monin Obukhov similarity theory.
 *
 * @param[in] lev Current level
 * @param[in] max_iters maximum iterations to use
 */
void
ABLMost::update_fluxes (const int& lev,
                        const Real& time,
                        int max_iters)
{
    // Update SST data if we have a valid pointer
    if (m_sst_lev[lev][0]) time_interp_tsurf(lev, time);

    // Compute plane averages for all vars (regardless of flux type)
    m_ma.compute_averages(lev);

    // Iterate the fluxes if moeng type
    if (flux_type == FluxCalcType::MOENG) {
        if (theta_type == ThetaCalcType::HEAT_FLUX) {
            if (rough_type == RoughCalcType::CONSTANT) {
                surface_flux most_flux(m_ma.get_zref(), surf_temp_flux);
                compute_fluxes(lev, max_iters, most_flux);
            } else if (rough_type == RoughCalcType::CHARNOCK) {
                surface_flux_charnock most_flux(m_ma.get_zref(), surf_temp_flux, cnk_a);
                compute_fluxes(lev, max_iters, most_flux);
            } else {
                surface_flux_mod_charnock most_flux(m_ma.get_zref(), surf_temp_flux, depth);
                compute_fluxes(lev, max_iters, most_flux);
            }
        } else if (theta_type == ThetaCalcType::SURFACE_TEMPERATURE) {
            update_surf_temp(time);
            if (rough_type == RoughCalcType::CONSTANT) {
                surface_temp most_flux(m_ma.get_zref(), surf_temp_flux);
                compute_fluxes(lev, max_iters, most_flux);
            } else if (rough_type == RoughCalcType::CHARNOCK) {
                surface_temp_charnock most_flux(m_ma.get_zref(), surf_temp_flux, cnk_a);
                compute_fluxes(lev, max_iters, most_flux);
            } else {
                surface_temp_mod_charnock most_flux(m_ma.get_zref(), surf_temp_flux, depth);
                compute_fluxes(lev, max_iters, most_flux);
            }
        } else {
            if (rough_type == RoughCalcType::CONSTANT) {
                adiabatic most_flux(m_ma.get_zref(), surf_temp_flux);
                compute_fluxes(lev, max_iters, most_flux);
            } else if (rough_type == RoughCalcType::CHARNOCK) {
                adiabatic_charnock most_flux(m_ma.get_zref(), surf_temp_flux, cnk_a);
                compute_fluxes(lev, max_iters, most_flux);
            } else {
                adiabatic_mod_charnock most_flux(m_ma.get_zref(), surf_temp_flux, depth);
                compute_fluxes(lev, max_iters, most_flux);
            }
        } // theta flux
    } // Moeng flux
}


/**
 * Function to compute the fluxes (u^star and t^star) for Monin Obukhov similarity theory.
 *
 * @param[in] lev Current level
 * @param[in] max_iters maximum iterations to use
 * @param[in] most_flux structure to iteratively compute ustar and tstar
 */
template <typename FluxIter>
void
ABLMost::compute_fluxes (const int& lev,
                         const int& max_iters,
                         const FluxIter& most_flux)
{
    // Pointers to the computed averages
    const auto *const tm_ptr  = m_ma.get_average(lev,2);
    const auto *const umm_ptr = m_ma.get_average(lev,3);

    for (MFIter mfi(*u_star[lev]); mfi.isValid(); ++mfi)
    {
        Box gtbx = mfi.growntilebox();

        auto u_star_arr = u_star[lev]->array(mfi);
        auto t_star_arr = t_star[lev]->array(mfi);
        auto t_surf_arr = t_surf[lev]->array(mfi);
        auto olen_arr   = olen[lev]->array(mfi);

        const auto tm_arr  = tm_ptr->array(mfi);
        const auto umm_arr = umm_ptr->array(mfi);
        const auto z0_arr  = z_0[lev].array();

        ParallelFor(gtbx, [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
        {
            most_flux.iterate_flux(i, j, k, max_iters, z0_arr, umm_arr, tm_arr,
                                   u_star_arr, t_star_arr, t_surf_arr, olen_arr);
        });
    }
}


/**
 * Wrapper to impose Monin Obukhov similarity theory fluxes by populating ghost cells.
 *
 * @param[in] lev Current level
 * @param[in,out] mfs Multifabs to populate
 * @param[in] eddyDiffs Diffusion coefficients from turbulence model
 */
void
ABLMost::impose_most_bcs (const int& lev,
                          const Vector<MultiFab*>& mfs,
                          MultiFab* eddyDiffs,
                          MultiFab* z_phys)
{
    const int zlo = 0;
    if (flux_type == FluxCalcType::DONELAN) {
        donelan_flux flux_comp(zlo);
        compute_most_bcs(lev, mfs, eddyDiffs, z_phys, m_geom[lev].CellSize(2), flux_comp);
    } else {
        moeng_flux flux_comp(zlo);
        compute_most_bcs(lev, mfs, eddyDiffs, z_phys, m_geom[lev].CellSize(2), flux_comp);
    }
}


/**
 * Function to calculate MOST fluxes for populating ghost cells.
 *
 * @param[in] lev Current level
 * @param[in,out] mfs Multifabs to populate
 * @param[in] eddyDiffs Diffusion coefficients from turbulence model
 * @param[in] flux_comp structure to compute fluxes
 */
template<typename FluxCalc>
void
ABLMost::compute_most_bcs (const int& lev,
                           const Vector<MultiFab*>& mfs,
                           MultiFab* eddyDiffs,
                           MultiFab* z_phys,
                           const Real& dz_no_terrain,
                           const FluxCalc& flux_comp)
{
    const int zlo   = 0;
    const int icomp = 0;
    for (MFIter mfi(*mfs[0]); mfi.isValid(); ++mfi)
    {
        // Get field arrays
        const auto cons_arr  = mfs[Vars::cons]->array(mfi);
        const auto velx_arr  = mfs[Vars::xvel]->array(mfi);
        const auto vely_arr  = mfs[Vars::yvel]->array(mfi);
        const auto  eta_arr  = eddyDiffs->array(mfi);
        const auto zphys_arr = (z_phys) ? z_phys->const_array(mfi) : Array4<const Real>{};

        // Get average arrays
        const auto *const u_mean     = m_ma.get_average(lev,0);
        const auto *const v_mean     = m_ma.get_average(lev,1);
        const auto *const t_mean     = m_ma.get_average(lev,2);
        const auto *const u_mag_mean = m_ma.get_average(lev,3);

        const auto um_arr  = u_mean->array(mfi);
        const auto vm_arr  = v_mean->array(mfi);
        const auto tm_arr  = t_mean->array(mfi);
        const auto umm_arr = u_mag_mean->array(mfi);

        // Get derived arrays
        const auto u_star_arr = u_star[lev]->array(mfi);
        const auto t_star_arr = t_star[lev]->array(mfi);
        const auto t_surf_arr = t_surf[lev]->array(mfi);

        for (int var_idx = 0; var_idx < Vars::NumTypes; ++var_idx)
        {
            const Box& bx = (*mfs[var_idx])[mfi].box();
            auto dest_arr = (*mfs[var_idx])[mfi].array();

            if (var_idx == Vars::cons) {
                Box b2d = bx;
                b2d.setBig(2,zlo-1);
                int n = RhoTheta_comp;

                ParallelFor(b2d, [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    Real dz = (zphys_arr) ? ( zphys_arr(i,j,zlo) - zphys_arr(i,j,zlo-1) ) : dz_no_terrain;
                    flux_comp.compute_t_flux(i, j, k, n, icomp, dz,
                                             cons_arr, velx_arr, vely_arr, eta_arr,
                                             umm_arr, tm_arr, u_star_arr, t_star_arr, t_surf_arr,
                                             dest_arr);
                });

            } else if (var_idx == Vars::xvel || var_idx == Vars::xmom) {

                Box xb2d = surroundingNodes(bx,0);
                xb2d.setBig(2,zlo-1);

                ParallelFor(xb2d, [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    Real dz = (zphys_arr) ? ( zphys_arr(i,j,zlo) - zphys_arr(i,j,zlo-1) ) : dz_no_terrain;
                    flux_comp.compute_u_flux(i, j, k, icomp, var_idx, dz,
                                             cons_arr, velx_arr, vely_arr, eta_arr,
                                             umm_arr, um_arr, u_star_arr,
                                             dest_arr);
                });

            } else if (var_idx == Vars::yvel || var_idx == Vars::ymom) {

                Box yb2d = surroundingNodes(bx,1);
                yb2d.setBig(2,zlo-1);

                ParallelFor(yb2d, [=] AMREX_GPU_DEVICE (int i, int j, int k)
                {
                    Real dz = (zphys_arr) ? ( zphys_arr(i,j,zlo) - zphys_arr(i,j,zlo-1) ) : dz_no_terrain;
                    flux_comp.compute_v_flux(i, j, k, icomp, var_idx, dz,
                                             cons_arr, velx_arr, vely_arr, eta_arr,
                                             umm_arr, vm_arr, u_star_arr,
                                             dest_arr);
                });
            }
        } // var_idx
    } // mfiter
}

void
ABLMost::time_interp_tsurf(const int& lev,
                           const Real& time)
{
    // Time interpolation
    Real dT = m_bdy_time_interval;
    Real time_since_start = time - m_start_bdy_time;
    int n_time = static_cast<int>( time_since_start /  dT);
    amrex::Real alpha = (time_since_start - n_time * dT) / dT;
    AMREX_ALWAYS_ASSERT( alpha >= 0. && alpha <= 1.0);
    amrex::Real oma   = 1.0 - alpha;
    AMREX_ALWAYS_ASSERT( (n_time >= 0) && (n_time < (m_sst_lev[lev].size()-1)));

    // Populate t_surf
    for (MFIter mfi(*t_surf[lev]); mfi.isValid(); ++mfi)
    {
        Box gtbx = mfi.growntilebox();

        auto t_surf_arr = t_surf[lev]->array(mfi);
        const auto sst_hi_arr = m_sst_lev[lev][n_time+1]->const_array(mfi);
        const auto sst_lo_arr = m_sst_lev[lev][n_time  ]->const_array(mfi);

        ParallelFor(gtbx, [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
        {
            t_surf_arr(i,j,k) = oma   * sst_lo_arr(i,j,k)
                              + alpha * sst_hi_arr(i,j,k);
        });
    }
}

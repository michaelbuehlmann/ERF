/**
 *  Wrapper for calling the routine that creates the fast RHS
 */
auto fast_rhs_fun = [&](int fast_step, int /*n_sub*/, int nrk,
                        Vector<MultiFab>& S_slow_rhs,
                        const Vector<MultiFab>& S_old,
                        Vector<MultiFab>& S_stage,
                        Vector<MultiFab>& S_data,
                        Vector<MultiFab>& S_scratch,
                        const Real dtau,
                        const Real inv_fac,
                        const Real old_substep_time,
                        const Real new_substep_time)
    {
        BL_PROFILE("fast_rhs_fun");
        if (verbose) amrex::Print() << "Calling fast rhs at level " << level << " with dt = " << dtau << std::endl;

        const bool l_use_moisture = (solverChoice.moisture_type != MoistureType::None);

        // Define beta_s here so that it is consistent between where we make the fast coefficients
        //    and where we use them
        // Per p2902 of Klemp-Skamarock-Dudhia-2007
        // beta_s = -1.0 : fully explicit
        // beta_s =  1.0 : fully implicit
        Real beta_s = 0.1;

        // *************************************************************************
        // Set up flux registers if using two_way coupling
        // *************************************************************************
        const bool l_reflux = (solverChoice.coupling_type == CouplingType::TwoWay);

        YAFluxRegister* fr_as_crse = nullptr;
        YAFluxRegister* fr_as_fine = nullptr;
        if (l_reflux) {
            if (level < finest_level) {
                fr_as_crse = getAdvFluxReg(level+1);
            }
            if (level > 0) {
                fr_as_fine = getAdvFluxReg(level);
            }
        }

        // Moving terrain
        std::unique_ptr<MultiFab> z_t_pert;
        if ( solverChoice.use_terrain &&  (solverChoice.terrain_type == TerrainType::Moving) )
        {
            // Make "old" fast geom -- store in z_phys_nd for convenience
            if (verbose) Print() << "Making geometry at start of substep time: " << old_substep_time << std::endl;
            prob->init_custom_terrain(fine_geom,*z_phys_nd[level],old_substep_time);
            init_terrain_grid  (fine_geom,*z_phys_nd[level], zlevels_stag);
            make_J             (fine_geom,*z_phys_nd[level], *detJ_cc[level]);

            // Make "new" fast geom
            if (verbose) Print() << "Making geometry for end of substep time :" << new_substep_time << std::endl;
            prob->init_custom_terrain(fine_geom,*z_phys_nd_new[level],new_substep_time);
            init_terrain_grid  (fine_geom,*z_phys_nd_new[level], zlevels_stag);
            make_J             (fine_geom,*z_phys_nd_new[level], *detJ_cc_new[level]);

            Real inv_dt   = 1./dtau;

            z_t_pert = std::make_unique<MultiFab>(S_data[IntVar::zmom].boxArray(), S_data[IntVar::zmom].DistributionMap(), 1, 1);

            for (MFIter mfi(*z_t_rk[level],TilingIfNotGPU()); mfi.isValid(); ++mfi)
            {
                Box gbx = mfi.growntilebox(IntVect(1,1,0));

                const Array4<Real      >& z_t_arr      =  z_t_rk[level]->array(mfi);
                const Array4<Real      >& zp_t_arr     =  z_t_pert->array(mfi);

                const Array4<Real const>& z_nd_new_arr =  z_phys_nd_new[level]->const_array(mfi);
                const Array4<Real const>& z_nd_old_arr =  z_phys_nd[level]->const_array(mfi);

                // Loop over horizontal plane
                amrex::ParallelFor(gbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    // Evaluate between RK stages assuming the geometry is linear between old and new time
                    zp_t_arr(i,j,k) = 0.25 * inv_dt * (z_nd_new_arr(i+1,j+1,k) - z_nd_old_arr(i+1,j+1,k)
                                                      +z_nd_new_arr(i  ,j+1,k) - z_nd_old_arr(  i,j+1,k)
                                                      +z_nd_new_arr(i+1,j  ,k) - z_nd_old_arr(i+1,j  ,k)
                                                      +z_nd_new_arr(i  ,j  ,k) - z_nd_old_arr(i  ,j  ,k));
                    // Convert to perturbation: z"_t(t) = z_t(t) - z_t^{RK}
                    zp_t_arr(i,j,k) -= z_t_arr(i,j,k);
                });
            } // mfi

            // We have to call this each step since it depends on the substep time now
            // Note we pass in the *old* detJ here
            make_fast_coeffs(level, fast_coeffs, S_stage, S_prim, pi_stage, fine_geom,
                             l_use_moisture, solverChoice.use_terrain, solverChoice.gravity, solverChoice.c_p,
                             detJ_cc[level], r0, pi0, dtau, beta_s);

            if (fast_step == 0) {
                // If this is the first substep we pass in S_old as the previous step's solution
                erf_fast_rhs_MT(fast_step, nrk, level, finest_level,
                                S_slow_rhs, S_old, S_stage, S_prim, pi_stage, fast_coeffs,
                                S_data, S_scratch, fine_geom,
                                solverChoice.gravity, solverChoice.use_lagged_delta_rt,
                                Omega, z_t_rk[level], z_t_pert.get(),
                                z_phys_nd[level], z_phys_nd_new[level], z_phys_nd_src[level],
                                  detJ_cc[level],   detJ_cc_new[level],   detJ_cc_src[level],
                                dtau, beta_s, inv_fac,
                                mapfac_m[level], mapfac_u[level], mapfac_v[level],
                                fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            } else {
                // If this is not the first substep we pass in S_data as the previous step's solution
                erf_fast_rhs_MT(fast_step, nrk, level, finest_level,
                                S_slow_rhs, S_data, S_stage, S_prim, pi_stage, fast_coeffs,
                                S_data, S_scratch, fine_geom,
                                solverChoice.gravity, solverChoice.use_lagged_delta_rt,
                                Omega, z_t_rk[level], z_t_pert.get(),
                                z_phys_nd[level], z_phys_nd_new[level], z_phys_nd_src[level],
                                  detJ_cc[level],   detJ_cc_new[level],   detJ_cc_src[level],
                                dtau, beta_s, inv_fac,
                                mapfac_m[level], mapfac_u[level], mapfac_v[level],
                                fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            }
        } else if (solverChoice.use_terrain && solverChoice.terrain_type == TerrainType::Static) {
            if (fast_step == 0) {

                // If this is the first substep we make the coefficients since they are based only on stage data
                make_fast_coeffs(level, fast_coeffs, S_stage, S_prim, pi_stage, fine_geom,
                                 l_use_moisture, solverChoice.use_terrain, solverChoice.gravity, solverChoice.c_p,
                                 detJ_cc[level], r0, pi0, dtau, beta_s);

                // If this is the first substep we pass in S_old as the previous step's solution
                erf_fast_rhs_T(fast_step, nrk, level, finest_level,
                               S_slow_rhs, S_old, S_stage, S_prim, pi_stage, fast_coeffs,
                               S_data, S_scratch, fine_geom, solverChoice.gravity, Omega,
                               z_phys_nd[level], detJ_cc[level], dtau, beta_s, inv_fac,
                               mapfac_m[level], mapfac_u[level], mapfac_v[level],
                               fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            } else {
                // If this is not the first substep we pass in S_data as the previous step's solution
                erf_fast_rhs_T(fast_step, nrk, level, finest_level,
                               S_slow_rhs, S_data, S_stage, S_prim, pi_stage, fast_coeffs,
                               S_data, S_scratch, fine_geom, solverChoice.gravity, Omega,
                               z_phys_nd[level], detJ_cc[level], dtau, beta_s, inv_fac,
                               mapfac_m[level], mapfac_u[level], mapfac_v[level],
                               fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            }
        } else {
            if (fast_step == 0) {

                // If this is the first substep we make the coefficients since they are based only on stage data
                make_fast_coeffs(level, fast_coeffs, S_stage, S_prim, pi_stage, fine_geom,
                                 l_use_moisture, solverChoice.use_terrain, solverChoice.gravity, solverChoice.c_p,
                                 detJ_cc[level], r0, pi0, dtau, beta_s);

                // If this is the first substep we pass in S_old as the previous step's solution
                erf_fast_rhs_N(fast_step, nrk, level, finest_level,
                               S_slow_rhs, S_old, S_stage, S_prim, pi_stage, fast_coeffs,
                               S_data, S_scratch, fine_geom, solverChoice.gravity,
                               dtau, beta_s, inv_fac,
                               mapfac_m[level], mapfac_u[level], mapfac_v[level],
                               fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            } else {
                // If this is not the first substep we pass in S_data as the previous step's solution
                erf_fast_rhs_N(fast_step, nrk, level, finest_level,
                               S_slow_rhs, S_data, S_stage, S_prim, pi_stage, fast_coeffs,
                               S_data, S_scratch, fine_geom, solverChoice.gravity,
                               dtau, beta_s, inv_fac,
                               mapfac_m[level], mapfac_u[level], mapfac_v[level],
                               fr_as_crse, fr_as_fine, l_use_moisture, l_reflux);
            }
        }

        // Even if we update all the conserved variables we don't need
        // to fillpatch the slow ones every acoustic substep
        bool fast_only          = true;
        bool vel_and_mom_synced = false;
        int ng_cons    = 1;
        int ng_vel     = 1;
        apply_bcs(S_data, new_substep_time, ng_cons, ng_vel, fast_only, vel_and_mom_synced);
    };

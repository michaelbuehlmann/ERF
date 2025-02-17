#ifndef ABLMOST_H
#define ABLMOST_H

#include <AMReX_Geometry.H>
#include <AMReX_ParmParse.H>
#include <AMReX_FArrayBox.H>
#include <AMReX_MultiFab.H>
#include <AMReX_iMultiFab.H>

#include <IndexDefines.H>
#include <ERF_Constants.H>
#include <MOSTAverage.H>
#include <MOSTStress.H>

/** Monin-Obukhov surface layer profile
 *
 * van der Laan, P., Kelly, M. C., & Sørensen, N. N. (2017). A new k-epsilon
 * model consistent with Monin-Obukhov similarity theory. Wind Energy,
 * 20(3), 479–489. https://doi.org/10.1002/we.2017
 *
 * Consistent with Dyer (1974) formulation from page 57, Chapter 2, Modeling
 * the vertical ABL structure in Modelling of Atmospheric Flow Fields,
 * Demetri P Lalas and Corrado F Ratto, January 1996,
 * https://doi.org/10.1142/2975.
 */
class ABLMost
{

public:

    // Constructor
    explicit ABLMost (const amrex::Vector<amrex::Geometry>& geom,
                      amrex::Vector<amrex::Vector<amrex::MultiFab>>& vars_old,
                      amrex::Vector<std::unique_ptr<amrex::MultiFab>>& Theta_prim,
                      amrex::Vector<std::unique_ptr<amrex::MultiFab>>& z_phys_nd,
                      amrex::Vector<amrex::Vector<std::unique_ptr<amrex::MultiFab>>>& sst_lev,
                      amrex::Vector<amrex::Vector<std::unique_ptr<amrex::iMultiFab>>>& lmask_lev,
                      amrex::Real start_bdy_time = 0.0,
                      amrex::Real bdy_time_interval = 0.0)
    : m_start_bdy_time(start_bdy_time),
      m_bdy_time_interval(bdy_time_interval),
      m_geom(geom),
      m_ma(geom,vars_old,Theta_prim,z_phys_nd)
    {
        amrex::ParmParse pp("erf");
        pp.query("most.z0", z0_const);

        // Specify how to compute the flux
        std::string flux_string{"moeng"};
        pp.query("most.flux_type", flux_string);
        if (flux_string == "donelan") {
            flux_type = FluxCalcType::DONELAN;
        } else if (flux_string == "moeng") {
            flux_type = FluxCalcType::MOENG;
        } else {
            amrex::Abort("Undefined MOST flux type!");
        }

        // Specify surface temperature or surface flux
        auto erf_st = pp.query("most.surf_temp", surf_temp);
        if (erf_st) {
            theta_type = ThetaCalcType::SURFACE_TEMPERATURE;
            pp.query("most.surf_heating_rate", surf_heating_rate); // [K/h]
            surf_heating_rate = surf_heating_rate / 3600.0; // [K/s]
            if (pp.query("most.surf_temp_flux", surf_temp_flux)) {
                amrex::Abort("Can only specify one of surf_temp_flux or surf_heating_rate");
            }
        } else {
            pp.query("most.surf_temp_flux", surf_temp_flux);
            if (pp.query("most.surf_heating_rate", surf_heating_rate)) {
                amrex::Abort("Can only specify one of surf_temp_flux or surf_heating_rate");
            }
            if (std::abs(surf_temp_flux) > std::numeric_limits<amrex::Real>::epsilon()) {
                theta_type = ThetaCalcType::HEAT_FLUX;
            } else {
                theta_type = ThetaCalcType::ADIABATIC;
            }
        }

        // Specify how to compute the flux
        std::string rough_string{"constant"};
        pp.query("most.roughness_type", rough_string);
        if (rough_string == "constant") {
            rough_type = RoughCalcType::CONSTANT;
        } else if (rough_string == "charnock") {
            rough_type = RoughCalcType::CHARNOCK;
            pp.query("most.charnock_constant",cnk_a);
        } else if (rough_string == "modified_charnock") {
            rough_type = RoughCalcType::MODIFIED_CHARNOCK;
            pp.query("most.modified_charnock_depth",depth);
        } else {
            amrex::Abort("Undefined MOST roughness type!");
        }

        // Size the MOST params for all levels
        int nlevs = m_geom.size();
        z_0.resize(nlevs);
        u_star.resize(nlevs);
        t_star.resize(nlevs);
        t_surf.resize(nlevs);
        olen.resize(nlevs);

        // Get pointers to SST and LANDMASK data
        m_sst_lev.resize(nlevs);
        m_lmask_lev.resize(nlevs);
        for (int lev(0); lev<nlevs; ++lev) {
            int nt_tot = sst_lev[lev].size();
            m_sst_lev[lev].resize(nt_tot);
            m_lmask_lev[lev].resize(nt_tot);
            for (int nt(0); nt<nt_tot; ++nt) {
                m_sst_lev[lev][nt]   = sst_lev[lev][nt].get();
                m_lmask_lev[lev][nt] = lmask_lev[lev][nt].get();
            }
        }

        for (int lev = 0; lev < nlevs; lev++) {
            // Attributes for MFs and FABs
            //--------------------------------------------------------
            auto& mf = vars_old[lev][Vars::cons];
            // Create a 2D ba, dm, & ghost cells
            amrex::BoxArray ba  = mf.boxArray();
            amrex::BoxList bl2d = ba.boxList();
            for (auto& b : bl2d) {
                b.setRange(2,0);
            }
            amrex::BoxArray ba2d(std::move(bl2d));
            const amrex::DistributionMapping& dm = mf.DistributionMap();
            const int ncomp   = 1;
            amrex::IntVect ng = mf.nGrowVect(); ng[2]=0;

            // Z0 heights FAB
            //--------------------------------------------------------
            amrex::Box bx = amrex::grow(m_geom[lev].Domain(),ng);
            bx.setSmall(2,0);
            bx.setBig(2,0);
            z_0[lev].resize(bx,1);
            z_0[lev].setVal<amrex::RunOn::Device>(z0_const);

            // 2D MFs for U*, T*, T_surf
            //--------------------------------------------------------
            u_star[lev] = std::make_unique<amrex::MultiFab>(ba2d,dm,ncomp,ng);
            u_star[lev]->setVal(1.E34);

            t_star[lev] = std::make_unique<amrex::MultiFab>(ba2d,dm,ncomp,ng);
            t_star[lev]->setVal(1.E34);

            olen[lev] = std::make_unique<amrex::MultiFab>(ba2d,dm,ncomp,ng);
            olen[lev]->setVal(1.E34);

            t_surf[lev] = std::make_unique<amrex::MultiFab>(ba2d,dm,ncomp,ng);

            if (m_sst_lev[lev][0]) { // Valid SST data at t==0
                theta_type = ThetaCalcType::SURFACE_TEMPERATURE;
                amrex::MultiFab::Copy(*(t_surf[lev]), *(m_sst_lev[lev][0]), 0, 0, 1, ng);
            } else if (erf_st) {     // Constant temp
                t_surf[lev]->setVal(surf_temp);
            } else {
                t_surf[lev]->setVal(0.0);
            }
        }// lev
    }

    void
    update_fluxes (const int& lev,
                   const amrex::Real& time,
                   int max_iters = 25);

    template <typename FluxIter>
    void
    compute_fluxes (const int& lev,
                    const int& max_iters,
                    const FluxIter& most_flux);

    void
    impose_most_bcs (const int& lev,
                     const amrex::Vector<amrex::MultiFab*>& mfs,
                     amrex::MultiFab* eddyDiffs,
                     amrex::MultiFab* z_phys);

    template<typename FluxCalc>
    void
    compute_most_bcs (const int& lev,
                      const amrex::Vector<amrex::MultiFab*>& mfs,
                      amrex::MultiFab* eddyDiffs,
                      amrex::MultiFab* z_phys,
                      const amrex::Real& dz_no_terrain,
                      const FluxCalc& flux_comp);

    void
    time_interp_tsurf(const int& lev,
                      const amrex::Real& time);

    void
    update_surf_temp (const amrex::Real& time)
    {
        if (surf_heating_rate != 0) {
            int nlevs = m_geom.size();
            for (int lev = 0; lev < nlevs; lev++)
            {
               t_surf[lev]->setVal(surf_temp + surf_heating_rate*time);
               amrex::Print() << "Surface temp at t=" << time
                   << ": "
                   << surf_temp + surf_heating_rate*time
                   << std::endl;
            }
        }
    }

    void
    update_mac_ptrs (const int& lev,
                     amrex::Vector<amrex::Vector<amrex::MultiFab>>& vars_old,
                     amrex::Vector<std::unique_ptr<amrex::MultiFab>>& Theta_prim)
    { m_ma.update_field_ptrs(lev,vars_old,Theta_prim); }

    const amrex::MultiFab*
    get_u_star (const int& lev) { return u_star[lev].get(); }

    const amrex::MultiFab*
    get_t_star (const int& lev) { return t_star[lev].get(); }

    const amrex::MultiFab*
    get_olen (const int& lev) { return olen[lev].get(); }

    const amrex::MultiFab*
    get_mac_avg (const int& lev, int comp) { return m_ma.get_average(lev,comp); }

    enum struct FluxCalcType {
        MOENG = 0,      ///< Moeng functional form
        DONELAN         ///< Donelan functional form
    };

    enum struct ThetaCalcType {
        ADIABATIC = 0,
        HEAT_FLUX,          ///< Heat-flux specified
        SURFACE_TEMPERATURE ///< Surface temperature specified
    };

    enum struct RoughCalcType {
        CONSTANT = 0,      ///< Constant z0
        CHARNOCK,
        MODIFIED_CHARNOCK
    };

    FluxCalcType  flux_type;
    ThetaCalcType theta_type;
    RoughCalcType rough_type;

private:
    amrex::Real z0_const;
    amrex::Real surf_temp;
    amrex::Real surf_heating_rate{0};
    amrex::Real surf_temp_flux{0};
    amrex::Real cnk_a{0.0185};
    amrex::Real depth{30.0};
    amrex::Real m_start_bdy_time;
    amrex::Real m_bdy_time_interval;
    amrex::Vector<amrex::Geometry>  m_geom;
    amrex::Vector<amrex::FArrayBox> z_0;

    MOSTAverage m_ma;
    amrex::Vector<std::unique_ptr<amrex::MultiFab>> u_star;
    amrex::Vector<std::unique_ptr<amrex::MultiFab>> t_star;
    amrex::Vector<std::unique_ptr<amrex::MultiFab>> olen;
    amrex::Vector<std::unique_ptr<amrex::MultiFab>> t_surf;

    amrex::Vector<amrex::Vector<amrex::MultiFab*>>  m_sst_lev;
    amrex::Vector<amrex::Vector<amrex::iMultiFab*>> m_lmask_lev;
};

#endif /* ABLMOST_H */

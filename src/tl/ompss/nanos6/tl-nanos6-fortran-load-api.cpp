/*--------------------------------------------------------------------
  (C) Copyright 2015-2015 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion
  
  This file is part of Mercurium C/C++ source-to-source compiler.
  
  See AUTHORS file in the top level directory for information
  regarding developers and contributors.
  
  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.
  
  Mercurium C/C++ source-to-source compiler is distributed in the hope
  that it will be useful, but WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the GNU Lesser General Public License for more
  details.
  
  You should have received a copy of the GNU Lesser General Public
  License along with Mercurium C/C++ source-to-source compiler; if
  not, write to the Free Software Foundation, Inc., 675 Mass Ave,
  Cambridge, MA 02139, USA.
--------------------------------------------------------------------*/


#include "tl-nanos6.hpp"
#include "tl-nanos6-interface.hpp"

#include "tl-omp-lowering-utils.hpp"

namespace TL { namespace Nanos6 {

namespace
{
const char *entry_points[] = {
    "nanos6_in_final",
    "nanos6_create_task",
    "nanos6_submit_task",
    "nanos6_taskwait",
    "nanos6_user_lock",
    "nanos6_user_unlock",
    NULL
};

// We have '__nanos6_max_dimensions' different versions for each symbol, for
// this reason they're treated a bit different
const char *multidimensional_entry_points[] =
{
    "nanos6_register_region_read_depinfo",
    "nanos6_register_region_write_depinfo",
    "nanos6_register_region_readwrite_depinfo",
    "nanos6_register_region_weak_read_depinfo",
    "nanos6_register_region_weak_write_depinfo",
    "nanos6_register_region_weak_readwrite_depinfo",
    "nanos6_register_region_commutative_depinfo",
//    "nanos6_register_region_weak_commutative_depinfo",
    "nanos6_register_region_concurrent_depinfo",
    "nanos6_register_region_reduction_depinfo",
    "nanos6_register_region_weak_reduction_depinfo",
    "nanos6_release_read_",
    "nanos6_release_write_",
    "nanos6_release_readwrite_",
    "nanos6_release_weak_read_",
    "nanos6_release_weak_write_",
    "nanos6_release_weak_readwrite_",
    "nanos6_release_commutative_",
//    "nanos6_release_weak_commutative_",
    "nanos6_release_concurrent_" ,
    NULL
};
}
    void LoweringPhase::fortran_fixup_api()
    {
        ERROR_CONDITION(!IS_FORTRAN_LANGUAGE, "This is only for Fortran", 0);
        TL::OpenMP::Lowering::Utils::Fortran::fixup_entry_points(
                entry_points, multidimensional_entry_points, nanos6_api_max_dimensions());

        if (!Interface::family_is_at_least("nanos6_reductions_api", 2)) {
            const char *reduction_entry_points[] = {
                "nanos6_get_reduction_storage1",
                NULL,
            };
            const char *empty[] = { NULL };

            TL::OpenMP::Lowering::Utils::Fortran::fixup_entry_points(
                    reduction_entry_points, empty, nanos6_api_max_dimensions());
        }

        if (Interface::family_is_at_least("nanos6_loop_api", 2)) {
            const char *loop_entry_points[] = {
                "nanos6_register_loop_bounds",
                NULL,
            };
            const char *empty[] = { NULL };

            TL::OpenMP::Lowering::Utils::Fortran::fixup_entry_points(
                    loop_entry_points, empty, nanos6_api_max_dimensions());
        }
        else
        {
            const char *loop_entry_points[] = {
                "nanos6_register_taskloop_bounds",
                NULL,
            };
            const char *empty[] = { NULL };

            TL::OpenMP::Lowering::Utils::Fortran::fixup_entry_points(
                    loop_entry_points, empty, nanos6_api_max_dimensions());
        }

        if (Interface::family_is_at_least("nanos6_lint_multidimensional_accesses_api", 1))
        {
            const char* lint_entry_points[] =
            {
                "nanos6_lint_ignore_region_begin",
                "nanos6_lint_ignore_region_end",
                "nanos6_lint_register_alloc",
                "nanos6_lint_register_free",
                NULL
            };

            const char* lint_multidimensional_entry_points[] =
            {
                "nanos6_lint_register_region_read_",
                "nanos6_lint_register_region_write_",
                "nanos6_lint_register_region_readwrite_",
                NULL
            };

            TL::OpenMP::Lowering::Utils::Fortran::fixup_entry_points(
                    lint_entry_points, lint_multidimensional_entry_points, nanos6_api_max_dimensions());
        }
    }
} }

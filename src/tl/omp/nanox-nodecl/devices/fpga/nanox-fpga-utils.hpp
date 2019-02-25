/*--------------------------------------------------------------------
  (C) Copyright 2018-2018 Barcelona Supercomputing Center
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

#ifndef NANOX_FPGA_UTILS_HPP
#define NANOX_FPGA_UTILS_HPP

#include "cxx-graphviz.h"

#include "../../../lowering-common/tl-omp-lowering-utils.hpp"

namespace TL
{
namespace Nanox
{

static std::string fpga_outline_name(const std::string &name)
{
    return "fpga_" + name;
}

UNUSED_PARAMETER static void print_ast_dot(const Nodecl::NodeclBase &node)
{
    std::cerr << std::endl << std::endl;
    ast_dump_graphviz(nodecl_get_ast(node.get_internal_nodecl()), stderr);
    std::cerr << std::endl << std::endl;
}

//Implementation from LoweringVisitor::compute_array_info (tl/omp/nanox-nodecl/tl-lower-task.cpp)
static void compute_array_info(
        Nodecl::NodeclBase ctr,
        TL::DataReference array_expr,
        TL::Type array_type,
        // Out
        TL::Type& base_type,
        TL::ObjectList<Nodecl::NodeclBase>& lower_bounds,
        TL::ObjectList<Nodecl::NodeclBase>& upper_bounds,
        TL::ObjectList<Nodecl::NodeclBase>& dims_sizes)
{
    ERROR_CONDITION(!array_type.is_array(), "Unexpected type", 0);

    TL::Type t = array_type;
    int fortran_rank = array_type.fortran_rank();

    while (t.is_array())
    {
        Nodecl::NodeclBase array_lb, array_ub;
        Nodecl::NodeclBase region_lb, region_ub;
        Nodecl::NodeclBase dim_size;

        dim_size = t.array_get_size();
        t.array_get_bounds(array_lb, array_ub);
        if (t.array_is_region())
        {
            t.array_get_region_bounds(region_lb, region_ub);
        }

        if (IS_FORTRAN_LANGUAGE
                && t.is_fortran_array())
        {
            if (array_lb.is_null())
            {
                array_lb = TL::OpenMP::Lowering::Utils::Fortran::get_lower_bound(array_expr, fortran_rank);
            }
            if (array_ub.is_null())
            {
                array_ub = TL::OpenMP::Lowering::Utils::Fortran::get_upper_bound(array_expr, fortran_rank);
            }
            if (dim_size.is_null())
            {
                dim_size = TL::OpenMP::Lowering::Utils::Fortran::get_size_for_dimension(array_expr, t, fortran_rank);
            }
        }

        // The region is the whole array
        if (region_lb.is_null())
            region_lb = array_lb;
        if (region_ub.is_null())
            region_ub = array_ub;

        // Adjust bounds to be 0-based
        Nodecl::NodeclBase adjusted_region_lb =
            (Source() << "(" << as_expression(region_lb) << ") - (" << as_expression(array_lb) << ")").
            parse_expression(ctr);
        Nodecl::NodeclBase adjusted_region_ub =
            (Source() << "(" << as_expression(region_ub) << ") - (" << as_expression(array_lb) << ")").
            parse_expression(ctr);

        lower_bounds.append(adjusted_region_lb);
        upper_bounds.append(adjusted_region_ub);
        dims_sizes.append(dim_size);

        t = t.array_element();

        fortran_rank--;
    }
    base_type = t;
}

std::string fpga_wrapper_name(const std::string &name)
{
    return name + "_hls_automatic_mcxx_wrapper";
}

} // namespace Nanox
} // namespace TL

#endif // NANOX_FPGA_UTILS_HPP

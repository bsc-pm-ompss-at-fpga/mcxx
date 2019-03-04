/*--------------------------------------------------------------------
  (C) Copyright 2018-2019 Barcelona Supercomputing Center
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

#define STR_ACCID              "accID"
#define STR_COMPONENTS_COUNT   "__mcxx_taskComponents"
#define STR_GLOB_OUTPORT       "__mcxx_outPort"
#define STR_GLOB_TWPORT        "__mcxx_twPort"
#define STR_TASKID             "__mcxx_taskId"

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

std::string get_mcxx_ptr_declaration(const TL::Type& type_to_point)
{
    return "mcxx_ptr_t<" + type_to_point.print_declarator() + ">";
}

struct ReplacePtrDeclVisitor : public Nodecl::ExhaustiveVisitor<void>
{
    private:
        static TL::Symbol declare_mcxx_ptr_variable(TL::Scope scope, const TL::Type& type_to_point)
        {
            TL::Symbol structure = get_mcxx_ptr_symbol(scope);
            //TODO: obtain the mcxx_ptr_t info from structure
            TL::Symbol field = scope.new_symbol(get_mcxx_ptr_declaration(type_to_point));
            field.get_internal_symbol()->kind = SK_VARIABLE;
            symbol_entity_specs_set_is_user_declared(field.get_internal_symbol(), 1);
            field.get_internal_symbol()->type_information = structure.get_user_defined_type().get_internal_type();
            field.get_internal_symbol()->locus = make_locus("", 0, 0);
            return field;
        }

        static TL::Symbol get_mcxx_ptr_symbol(TL::Scope scope)
        {
            std::string structure_name = "mcxx_ptr_t";
            // const locus_t* locus = make_locus("", 0, 0);

            TL::Symbol new_class_symbol = scope.new_symbol(structure_name);
            new_class_symbol.get_internal_symbol()->kind = SK_CLASS;
            type_t* new_class_type = get_new_class_type(scope.get_decl_context(), TT_STRUCT);
            symbol_entity_specs_set_is_user_declared(new_class_symbol.get_internal_symbol(), 1);
            const decl_context_t* class_context = new_class_context(new_class_symbol.get_scope().get_decl_context(),
                    new_class_symbol.get_internal_symbol());
            class_type_set_inner_context(new_class_type, class_context);
            new_class_symbol.get_internal_symbol()->type_information = new_class_type;
            new_class_symbol.get_internal_symbol()->do_not_print = 1;

            // Add members
            // TL::Scope class_scope(class_context);
            //
            // std::string field_name = "mcxx_ptr_member";
            // TL::Symbol field = class_scope.new_symbol(field_name);
            // field.get_internal_symbol()->kind = SK_VARIABLE;
            // symbol_entity_specs_set_is_user_declared(field.get_internal_symbol(), 1);
            //
            // TL::Type field_type = get_unsigned_long_int_type();
            // field.get_internal_symbol()->type_information = field_type.get_internal_type();
            //
            // symbol_entity_specs_set_is_member(field.get_internal_symbol(), 1);
            // symbol_entity_specs_set_class_type(field.get_internal_symbol(),
            //         ::get_user_defined_type(new_class_symbol.get_internal_symbol()));
            // symbol_entity_specs_set_access(field.get_internal_symbol(), AS_PUBLIC);
            //
            // field.get_internal_symbol()->locus = locus;
            //
            // class_type_add_member(new_class_type,
            //         field.get_internal_symbol(),
            //         class_scope.get_decl_context(),
            //         /* is_definition */ 1);

            // nodecl_t nodecl_output = nodecl_null();
            // finish_class_type(new_class_type,
            //         ::get_user_defined_type(new_class_symbol.get_internal_symbol()),
            //         scope.get_decl_context(),
            //         locus,
            //         &nodecl_output);
            // set_is_complete_type(new_class_type, /* is_complete */ 1);
            // set_is_complete_type(get_actual_class_type(new_class_type), /* is_complete */ 1);
            //
            // if (!nodecl_is_null(nodecl_output))
            // {
            //     std::cerr << "FIXME: finished class issues nonempty nodecl" << std::endl;
            // }

            return new_class_symbol;
        }

        static TL::Type get_user_defined_type_mcxx_ptr(TL::Scope scope)
        {
            TL::Symbol new_class_symbol = get_mcxx_ptr_symbol(scope);
            return new_class_symbol.get_user_defined_type();
        }

    public:
        ReplacePtrDeclVisitor() {}

        virtual void visit(const Nodecl::Symbol& node)
        {
            TL::Symbol sym = node.get_symbol();
            const TL::Type type = sym.get_type();
            if (!sym.get_value().is_null())
            {
                walk(sym.get_value());
            }
            //n.replace(_sym_rename_map[s]);
            const std::string sym_name = sym.get_name();
            const bool is_nanox_var = sym_name.find("nanos_") != std::string::npos;
            if (sym.is_variable() && type.is_pointer() && !is_nanox_var)
            {
                const TL::Type base_type = type.points_to();
                //const TL::Type new_type = get_user_defined_type_mcxx_ptr(sym.get_scope(), base_type);
                const TL::Type new_type = declare_mcxx_ptr_variable(sym.get_scope(), base_type).get_user_defined_type();
                sym.set_type(new_type);
            }
        }
};

Source get_mcxx_ptr_source()
{
    Source out;

    out << "template <typename T>"
        << "struct mcxx_ptr_t {"
        << "  uintptr_t val;"
        << "  mcxx_ptr_t() : val(0) {}"
        << "  mcxx_ptr_t(uintptr_t val) { this->val = val; }"
        << "  mcxx_ptr_t(mcxx_ptr_t<T> const &ref) { this->val = ref.val; }"
        << "  operator T*() const { return (T *)this->val; }"
        << "  operator uintptr_t() const { return this->val; }"
        << "  mcxx_ptr_t<T> operator + (int const val) const {"
        << "    mcxx_ptr_t<T> ret;"
        << "    ret.val = this->val + val*sizeof(T);"
        << "    return ret;"
        << "  }"
        << "  mcxx_ptr_t<T> operator + (mcxx_ptr_t<T> const &obj) const {"
        << "    return this + (int)obj.val;"
        << "  }"
        << "  mcxx_ptr_t<T> operator - (int const val) const {"
        << "    mcxx_ptr_t<T> ret;"
        << "    ret.val = this->val - val*sizeof(T);"
        << "    return ret;"
        << "  }"
        << "  mcxx_ptr_t<T> operator - (mcxx_ptr_t<T> const &obj) const {"
        << "    return this - (int)obj.val;"
        << "  }"
        << "};"
    ;

    return out;
}

Source get_aux_task_creation_source()
{
    Source out;

    //NOTE: structs and enums types cannot be declared using a typedef, they must be declared as they will be in a regular mcxx source
    out << "enum nanos_err_t {NANOS_OK = 0};"
        << "typedef nanos_wd_t nanos_wg_t;"
        << "nanos_wd_t nanos_current_wd() { return " << STR_TASKID << "; }"
        << "void nanos_handle_error(enum nanos_err_t err) {}"
        << "void write_outstream(uint64_t data, unsigned short dest, unsigned char last) {"
        << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_OUTPORT << "\n"
        //NOTE: Pack the axiData_t info: data(64bits) + dest(6bits) + last(2bit). It can be done
        //      with less bits but this way the info is HEX friendly
        << "  ap_uint<72> tmp = data;"
        << "  tmp = (tmp << 8) | ((dest & 0x3F) << 2) | (last & 0x3);"
        << "  " << STR_GLOB_OUTPORT << " = tmp;"
        << "}"
        << "void wait_tw_signal() {"
        << "  #pragma HLS INTERFACE ap_hs port=" << STR_GLOB_TWPORT << "\n"
        << "  ap_uint<2> sync = " << STR_GLOB_TWPORT << ";"
        << "}"
    ;

    return out;
}

Source get_nanos_wait_completion_source()
{
    Source out;

    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
    out << "enum nanos_err_t nanos_wg_wait_completion(nanos_wg_t uwg, bool avoid_flush) {"
        << "  if (" << STR_COMPONENTS_COUNT << " == 0) { return NANOS_OK; }"
        << "  const unsigned short TM_TW = 0x13;"
        << "  uint64_t tmp = " << STR_ACCID << ";"
        << "  tmp = tmp << 48 /*ACC_ID info uses bits [48:55]*/;"
        << "  tmp = 0x8000000100000000 | tmp | " << STR_COMPONENTS_COUNT << ";"
        << "  write_outstream(tmp /*TASKWAIT_DATA_BLOCK*/, TM_TW, 0 /*last*/);"
        << "  write_outstream(" << STR_TASKID << " /*data*/, TM_TW, 1 /*last*/);"
        << "  {\n"
        << "    #pragma HLS PROTOCOL fixed\n"
        << "    wait_tw_signal();"
        << "  }\n"
        << "  " << STR_COMPONENTS_COUNT << " = 0;"
        << "  return NANOS_OK;"
        << "}"
    ;

    return out;
}

Source get_nanos_create_wd_source()
{
    Source out;

    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
    //NOTE: structs and enums types cannot be declared using a typedef, they must be declared as they will be in a regular mcxx source
    out << "enum {NANOS_FPGA_ARCH_SMP = 0x800000, NANOS_FPGA_ARCH_FPGA = 0x400000};"
        << "enum {NANOS_ARGFLAG_DEP_OUT  = 0x08, NANOS_ARGFLAG_DEP_IN  = 0x04,"
        << "      NANOS_ARGFLAG_COPY_OUT = 0x02, NANOS_ARGFLAG_COPY_IN = 0x01,"
        << "      NANOS_ARGFLAG_NONE    = 0x00};"
        << "typedef struct nanos_fpga_copyinfo_t {"
        << "  uint64_t address;"
        << "  uint32_t flags;"
        << "  uint32_t size;"
        << "  uint32_t offset;"
        << "  uint32_t accessed_length;"
        << "} nanos_fpga_copyinfo_t;"
        << "void nanos_fpga_create_wd_async(uint32_t archMask, uint64_t type, uint16_t numDeps,"
        << "    uint16_t numArgs, uint64_t * args, uint8_t * argsFlags, uint16_t numCopies,"
        << "    struct nanos_fpga_copyinfo_t * copies) {"
        << "  #pragma HLS inline\n"
        << "  ++" << STR_COMPONENTS_COUNT << ";"
        << "  const unsigned short TM_NEW = 0x12;"
        << "  const unsigned short TM_SCHED = 0x14;"
        << "  const unsigned char hasSmpArch = (archMask & NANOS_FPGA_ARCH_SMP) != 0;"
        << "  const unsigned short DEST_ID = (numDeps == 0 && !hasSmpArch) ? TM_SCHED : TM_NEW;"
        //1st word: [ valid (8b) | arch_mask (24b) | num_args (16b) | num_copies (16b) ]
        << "  uint64_t tmp = 0x80000000 | archMask;"
        << "  tmp = (tmp << 16) | numArgs;"
        << "  tmp = (tmp << 16) | numCopies;"
        << "  write_outstream(tmp, DEST_ID, 0);"
        //2nd word: [ parent_task_id (64b) ]
        << "  write_outstream(" << STR_TASKID << ", DEST_ID, 0);"
        //3rd word: [ type_value (64b) ]
        << "  write_outstream(type, DEST_ID, 0);"
        << "  for (uint16_t idx = 0; idx < numArgs; ++idx) {"
        //arg words: [ arg_flags (8b) | arg_value (56b) ]
        << "    tmp = argsFlags[idx];"
        << "    tmp = (tmp << 56) | args[idx];"
        << "    write_outstream(tmp, DEST_ID, (idx == (numArgs - 1))&(numCopies == 0));"
        << "  }"
        << "  if (DEST_ID != TM_SCHED) {"
        << "    for (uint16_t idx = 0; idx < numCopies; ++idx) {"
        //1st copy word: [ address (64b) ]
        << "      tmp = copies[idx].address;"
        << "      write_outstream(tmp, DEST_ID, 0);"
        //2nd copy word: [ size (32b) | not_used (24b) | flags (8b) ]
        << "      tmp = copies[idx].size;"
        << "      tmp = (tmp << 32) | copies[idx].flags;"
        << "      write_outstream(tmp, DEST_ID, 0);"
        //3rd copy word: [ accessed_length (32b) | offset (32b) ]
        << "      tmp = copies[idx].accessed_length;"
        << "      tmp = (tmp << 32) | copies[idx].offset;"
        << "      write_outstream(tmp, DEST_ID, idx == (numCopies - 1));"
        << "    }"
        << "  }"
        << "}"
    ;

    return out;
}

} // namespace Nanox
} // namespace TL

#endif // NANOX_FPGA_UTILS_HPP

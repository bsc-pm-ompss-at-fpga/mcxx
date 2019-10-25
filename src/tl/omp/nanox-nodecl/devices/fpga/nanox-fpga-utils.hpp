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
#include "cxx-driver-build-info.h"
#include "tl-counters.hpp"
#include "tl-symbol-utils.hpp"
#include "../../../lowering-common/tl-omp-lowering-utils.hpp"

/*
 * NOTE: accID is composed by 2 parts:
 *  [0:3] global accelerator id (aka considering all accelerators)
 *  [4:7] ext accelerator id (aka only considering accels with task creation capabilities)
 */
#define STR_FULL_ACCID         "accID"
#define STR_GLB_ACCID          "(accID&0xF)"
#define STR_EXT_ACCID          "((accID>>4)&0xF)"
#define STR_COMPONENTS_COUNT   "__mcxx_taskComponents"
#define STR_GLOB_OUTPORT       "__mcxx_outPort"
#define STR_GLOB_TWPORT        "__mcxx_twPort"
#define STR_TASKID             "__mcxx_taskId"
#define STR_WRAPPERDATA        "mcxx_wrapper_data"
#define STR_OUTPUTSTREAM       "outStream"
#define STR_INPUTSTREAM        "inStream"
#define STR_INSTRCOUNTER       "mcxx_instr_counter"
#define STR_INSTRBUFFER        "mcxx_instr_buffer"
#define STR_INSTRBUFFER_OFFSET "__mcxx_instr_buffOffset"
#define STR_INSTRAVSLOTS       "__mcxx_instr_avSlots"
#define STR_INSTRSLOTS         "__mcxx_instr_slots"
#define STR_INSTRCURRENTSLOT   "__mcxx_instr_currentSlot"
#define STR_INSTROVERFLOW      "__mcxx_instr_numOverflow"
#define STR_EVENTTYPE          "__mcxx_eventType_t"
#define STR_EVENTSTRUCT        "__mcxx_event_t"
#define STR_PARENT_TASKID      "__mcxx_parent_taskId"

//Default instrumentation events codes
#define EV_DEVCOPYIN            78
#define EV_DEVCOPYOUT           79
#define EV_DEVEXEC              80
#define EV_INSTEVLOST           82

namespace TL
{
namespace Nanox
{

static std::string fpga_outline_name(const std::string &name)
{
    return "fpga_" + name;
}

UNUSED_PARAMETER static void print_ast_dot(const Nodecl::NodeclBase &node, const std::string path)
{
    FILE* file = fopen(path.c_str(), "w");
    ast_dump_graphviz(nodecl_get_ast(node.get_internal_nodecl()), file);
    fclose(file);
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

std::string get_mcxx_ptr_declaration(const TL::Type& type_to_point)
{
    return "mcxx_ptr_t<" + type_to_point.print_declarator() + ">";
}

void add_fpga_header(FILE* file, const std::string name, const std::string type, const std::string num_instances)
{
    fprintf(file, "\
///////////////////\n\
// Automatic IP Generated by OmpSs@FPGA compiler\n\
///////////////////\n"
    );
    fprintf(file, "// Top IP Function: %s\n", name.c_str());
    fprintf(file, "// Accelerator type: %s\n", type.c_str());
    fprintf(file, "// Num. instances: %s\n", num_instances.c_str());
    fprintf(file, "// Wrapper version: %s\n", FPGA_WRAPPER_VERSION);
    fprintf(file, "\
///////////////////\n\
#define __HLS_AUTOMATIC_MCXX__ 1\n\n\
#include <cstring>\n\
#include <stdint.h> \n\
#include <hls_stream.h>\n\
#include <ap_axi_sdata.h>\n\n"
    );
}

void add_fpga_footer(FILE* file)
{
    fprintf(file, "\n\
#undef __HLS_AUTOMATIC_MCXX__\n"
    );
}

struct ReplaceTaskCreatorSymbolsVisitor : public Nodecl::ExhaustiveVisitor<void>
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

        TL::Symbol get_nanos_fpga_current_wd_symbol(const TL::Symbol& nanos_current_wd_sym)
        {
            if (_symbol_map->map(nanos_current_wd_sym) == nanos_current_wd_sym)
            {
                _nanos_fpga_current_wd_sym = SymbolUtils::new_function_symbol(
                    nanos_current_wd_sym,
                    "nanos_fpga_current_wd",
                    TL::Type::get_unsigned_long_long_int_type(),
                    ObjectList<std::string>(),
                    ObjectList<TL::Type>());

                _symbol_map->add_map(nanos_current_wd_sym, _nanos_fpga_current_wd_sym);
            }

            return _nanos_fpga_current_wd_sym;
        }

        TL::Symbol get_nanos_fpga_wg_wait_completion_symbol(const TL::Symbol& host_sym)
        {
            if (_symbol_map->map(host_sym) == host_sym)
            {
                ObjectList<std::string> param_names;
                ObjectList<TL::Type> param_types;

                param_names.append("uwg");
                param_types.append(TL::Type::get_unsigned_long_long_int_type());

                param_names.append("avoid_flush");
                param_types.append(TL::Type::get_unsigned_char_type());

                _nanos_fpga_wg_wait_completion_sym = SymbolUtils::new_function_symbol(
                    host_sym.get_scope(),
                    "nanos_fpga_wg_wait_completion",
                    host_sym.get_type().returns(),
                    param_names,
                    param_types);

                _symbol_map->add_map(host_sym, _nanos_fpga_current_wd_sym);
            }

            return _nanos_fpga_wg_wait_completion_sym;
        }

        TL::Symbol                       _nanos_fpga_current_wd_sym;
        TL::Symbol                       _nanos_fpga_wg_wait_completion_sym;
        Nodecl::Utils::SimpleSymbolMap*  _symbol_map;

    public:
        ReplaceTaskCreatorSymbolsVisitor(Nodecl::Utils::SimpleSymbolMap * map) : _nanos_fpga_current_wd_sym(),
            _nanos_fpga_wg_wait_completion_sym(), _symbol_map(map) {}

        virtual void visit(const Nodecl::Symbol& node)
        {
            TL::Symbol sym = node.get_symbol();
            const TL::Type type = sym.get_type();
            if (!sym.get_value().is_null())
            {
                walk(sym.get_value());
            }
            //n.replace(_sym_rename_map[s]);
            const std::string type_decl =  type.get_simple_declaration(sym.get_scope(), "");
            if (sym.is_variable() && type.is_pointer() && type_decl.find("nanos_") == std::string::npos)
            {
                const TL::Type base_type = type.points_to();
                //const TL::Type new_type = get_user_defined_type_mcxx_ptr(sym.get_scope(), base_type);
                const TL::Type new_type = declare_mcxx_ptr_variable(sym.get_scope(), base_type).get_user_defined_type();
                sym.set_type(new_type);
            }
            else if (sym.is_variable() && (type_decl.find("nanos_wd_t") != std::string::npos ||
                    type_decl.find("nanos_wg_t") != std::string::npos))
            {
                // NOTE: nanos_wd_t and nanos_wg_t are defined as void pointers but this is not posible
                // inside the FPGA. Therefore, we replace the type of variables by uint64_t
                sym.set_type(TL::Type::get_unsigned_long_long_int_type()/*uint64_t*/);
            }
        }

        virtual void visit(const Nodecl::FunctionCall& node)
        {
            Nodecl::NodeclBase called = node.get_called();
            Nodecl::NodeclBase arguments = node.get_arguments();
            Nodecl::NodeclBase alternate_name = node.get_alternate_name();
            Nodecl::NodeclBase function_form = node.get_function_form();

            walk(called);
            walk(arguments);
            walk(alternate_name);
            walk(function_form);

            if (!called.is<Nodecl::Symbol>())
                return;
            Symbol sym = called.as<Nodecl::Symbol>().get_symbol();
            Nodecl::FunctionCode function_code =
                sym.get_function_code().as<Nodecl::FunctionCode>();

            if (!function_code.is_null())
            {
                Nodecl::NodeclBase function_statements = function_code.get_statements();
                walk(function_statements);
            }

            if (sym.get_name() == "nanos_current_wd")
            {
                //NOTE: Replace the called symbol: nanos_current_wd --> nanos_fpga_current_wd
                TL::Symbol new_sym = get_nanos_fpga_current_wd_symbol(sym);
                called.set_symbol(new_sym);
            }
            else if (sym.get_name() == "nanos_wg_wait_completion")
            {
                //NOTE: Replace the called symbol: nanos_wg_wait_completion --> nanos_fpga_wg_wait_completion
                TL::Symbol new_sym = get_nanos_fpga_wg_wait_completion_symbol(sym);
                called.set_symbol(new_sym);
            }
        }
};

struct FpgaTaskCodeVisitor : public Nodecl::ExhaustiveVisitor<void>
{
    private:
        const std::string                _filename;
        Nodecl::Utils::SimpleSymbolMap*  _symbol_map;

        void checkSymTypeAndEmitWarning(const TL::Symbol& sym, const Nodecl::NodeclBase& node)
        {
            static bool warning_already_shown = false;
            static bool target_is_32b = TL::Type::get_unsigned_long_int_type().get_size() == 4;
            if (warning_already_shown || !target_is_32b)
                return;

            // Emit warning if target architecture seems 32bits and type of symbol is size_t, long
            TL::Type type = sym.get_type();
            const bool type_is_size_t = type.get_simple_declaration(sym.get_scope(), "").find("size_t") != std::string::npos;
            const bool type_is_long = type.is_signed_long_int() || type.is_unsigned_long_int();
            if (sym.is_variable() && (type_is_size_t || type_is_long))
            {
                std::string text = "Found a variable of type '%s' inside an fpga task.\n";
                text += "    It is one of the following types that may cause";
                text += " problems when used in 32 bits platforms and should be avoided:";
                text += " long int, unsigned long int, size_t.\n";
                warn_printf_at(node.get_locus(), text.c_str(),
                        type.get_simple_declaration(sym.get_scope(), "").c_str());
                warning_already_shown = true;
            }
        }
    public:
        Nodecl::List                     _called_functions;
        bool                             _calls_nanos_instrument;

        FpgaTaskCodeVisitor(const std::string filename, Nodecl::Utils::SimpleSymbolMap * map) :
                _filename(filename), _symbol_map(map), _called_functions(), _calls_nanos_instrument(false) {}

        virtual void visit(const Nodecl::Symbol& node)
        {
            TL::Symbol sym = node.get_symbol();
            if (!sym.get_value().is_null())
            {
                // Recursive walk
                walk(sym.get_value());
            }

            checkSymTypeAndEmitWarning(sym, node);
        }

        virtual void visit(const Nodecl::ObjectInit& node)
        {
            TL::Symbol sym = node.get_symbol();
            if (!sym.get_value().is_null())
            {
                walk(sym.get_value());
            }

            checkSymTypeAndEmitWarning(sym, node);
        }

        virtual void visit(const Nodecl::FunctionCall& node)
        {
            Nodecl::NodeclBase called = node.get_called();
            Nodecl::NodeclBase arguments = node.get_arguments();
            Nodecl::NodeclBase alternate_name = node.get_alternate_name();
            Nodecl::NodeclBase function_form = node.get_function_form();

            walk(called);
            walk(arguments);
            walk(alternate_name);
            walk(function_form);

            if (!called.is<Nodecl::Symbol>())
                return;
            Symbol sym = called.as<Nodecl::Symbol>().get_symbol();

            _calls_nanos_instrument |= sym.get_name().find("nanos_instrument_") != std::string::npos;

            Nodecl::FunctionCode function_code = sym.get_function_code().as<Nodecl::FunctionCode>();
            if (function_code.is_null())
                return;

            Nodecl::NodeclBase function_statements = function_code.get_statements();
            walk(function_statements);

            if (_filename == function_code.get_filename() && _symbol_map->map(sym) == sym)
            {
                // Duplicate the symbol and append the function code to the list
                // FIXME: Change the name of the new symbol and replace all needed symbol calls
                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(
                    sym, sym.get_name() /*+ "_moved"*/);
                _symbol_map->add_map(sym, new_function);

                Nodecl::NodeclBase fun_code = Nodecl::Utils::deep_copy(
                    sym.get_function_code(),
                    sym.get_scope(),
                    *_symbol_map);
                new_function.set_value(fun_code);
                symbol_entity_specs_set_is_static(new_function.get_internal_symbol(), 1);

                _called_functions.append(fun_code);
            }
        }
};

//NOTE: Function code based on LoweringVisitor::declare_argument_structure
TL::Symbol declare_casting_union(TL::Type field_type, Nodecl::NodeclBase construct)
{
    // Come up with a unique name
    Counter& counter = CounterManager::get_counter("ompss-fpga-cast-union");
    std::string structure_name;

    std::stringstream ss;
    ss << "fpga_cast_union_" << (int)counter << "_t";
    counter++;

    if (IS_C_LANGUAGE)
    {
        // We need an extra 'union
        structure_name = "union " + ss.str();
    }
    else
    {
        structure_name = ss.str();
    }

    TL::Scope sc(construct.retrieve_context());

    TL::Symbol new_class_symbol = sc.new_symbol(structure_name);
    new_class_symbol.get_internal_symbol()->kind = SK_CLASS;
    type_t* new_class_type = get_new_class_type(sc.get_decl_context(), TT_UNION);
    symbol_entity_specs_set_is_user_declared(new_class_symbol.get_internal_symbol(), 1);

    const decl_context_t* class_context = new_class_context(new_class_symbol.get_scope().get_decl_context(),
            new_class_symbol.get_internal_symbol());

    TL::Scope class_scope(class_context);

    class_type_set_inner_context(new_class_type, class_context);

    new_class_symbol.get_internal_symbol()->type_information = new_class_type;
    TL::Type union_class_type(new_class_type);

    //Add the uint64_t raw member
    TL::Symbol field_raw = class_scope.new_symbol("raw");
    field_raw.get_internal_symbol()->kind = SK_VARIABLE;
    symbol_entity_specs_set_is_user_declared(field_raw.get_internal_symbol(), 1);
    field_raw.get_internal_symbol()->type_information = TL::Type::get_unsigned_long_long_int_type().get_internal_type();
    symbol_entity_specs_set_is_member(field_raw.get_internal_symbol(), 1);
    symbol_entity_specs_set_class_type(field_raw.get_internal_symbol(), ::get_user_defined_type(new_class_symbol.get_internal_symbol()));
    symbol_entity_specs_set_access(field_raw.get_internal_symbol(), AS_PUBLIC);
    field_raw.get_internal_symbol()->locus = nodecl_get_locus(construct.get_internal_nodecl());
    class_type_add_member(((TL::Type)union_class_type).get_internal_type(),
            field_raw.get_internal_symbol(),
            field_raw.get_internal_symbol()->decl_context,
            /* is_definition */ 1);

    //Add the typed member
    TL::Symbol field_typed = class_scope.new_symbol("typed");
    field_typed.get_internal_symbol()->kind = SK_VARIABLE;
    symbol_entity_specs_set_is_user_declared(field_typed.get_internal_symbol(), 1);
    if (IS_CXX_LANGUAGE || IS_C_LANGUAGE)
    {
        if (field_type.is_const())
        {
            field_type = field_type.get_unqualified_type();
        }
    }
    field_typed.get_internal_symbol()->type_information = field_type.get_internal_type();
    symbol_entity_specs_set_is_member(field_typed.get_internal_symbol(), 1);
    symbol_entity_specs_set_class_type(field_typed.get_internal_symbol(), ::get_user_defined_type(new_class_symbol.get_internal_symbol()));
    symbol_entity_specs_set_access(field_typed.get_internal_symbol(), AS_PUBLIC);
    field_typed.get_internal_symbol()->locus = nodecl_get_locus(construct.get_internal_nodecl());
    class_type_add_member(union_class_type.get_internal_type(),
            field_typed.get_internal_symbol(),
            field_typed.get_internal_symbol()->decl_context,
            /* is_definition */ 1);

    nodecl_t nodecl_output = nodecl_null();
    finish_class_type(new_class_type,
            ::get_user_defined_type(new_class_symbol.get_internal_symbol()),
            sc.get_decl_context(),
            construct.get_locus(),
            &nodecl_output);
    set_is_complete_type(new_class_type, /* is_complete */ 1);
    set_is_complete_type(get_actual_class_type(new_class_type), /* is_complete */ 1);

    if (!nodecl_is_null(nodecl_output))
    {
        std::cerr << "FIXME: finished class issues nonempty nodecl" << std::endl;
    }

    //FIXME: Check if this has to be done always
    CXX_LANGUAGE()
    {
        Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDef::make(
                Nodecl::Context::make(Nodecl::NodeclBase::null(), sc),
                new_class_symbol,
                construct.get_locus());
        Nodecl::Utils::prepend_items_before(construct, nodecl_decl);
    }

    return new_class_symbol;
}

void get_hls_wrapper_decls(
  const bool instrumentation,
  const bool user_calls_nanos_instrument,
  const bool task_creation,
  const std::string shared_memory_port_width,
  Source& wrapper_decls,
  Source& wrapper_body_pragmas)
{
    // NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
    // NOTE: The declarations of Nanos++ APIs must be coherent with the ones in the Nanos++ headers.
    //       The only declarations that changes is nanos_wd_t which is a integer type inside the FPGA
    const bool put_instr_nanos_api =
        (!IS_C_LANGUAGE && (instrumentation || user_calls_nanos_instrument)) ||
        (IS_C_LANGUAGE && instrumentation && !user_calls_nanos_instrument);

    /*** Type declarations ***/
    wrapper_decls
        << "typedef ap_axis<64,1,8,5> axiData_t;"
        << "typedef hls::stream<axiData_t> axiStream_t;"
        /*<< "typedef uint64_t nanos_wd_t;"*/;

    if (!IS_C_LANGUAGE || (IS_C_LANGUAGE && !task_creation && !user_calls_nanos_instrument && instrumentation))
    {
        // NOTE: The following declarations will be placed in the source by the codegen in C lang
        wrapper_decls
            << "enum nanos_err_t"
            << "{"
            << "  NANOS_OK = 0,"
            << "  NANOS_UNKNOWN_ERR = 1,"
            << "  NANOS_UNIMPLEMENTED = 2,"
            << "  NANOS_ENOMEM = 3,"
            << "  NANOS_INVALID_PARAM = 4,"
            << "  NANOS_INVALID_REQUEST = 5"
            << "};"
            << "typedef enum nanos_err_t nanos_err_t;";
    }

    if (put_instr_nanos_api) {
        wrapper_decls
            << "typedef unsigned int nanos_event_key_t;"
            << "typedef unsigned long long int nanos_event_value_t;";
    }

    if (instrumentation)
    {
        wrapper_decls
            << "typedef uint64_t counter_t;"
            << "struct " << STR_EVENTSTRUCT
            << "{"
            << "  uint64_t value;"
            << "  uint64_t timestamp;"
            << "  uint64_t typeAndId;"
            << "};"
            << "typedef struct " << STR_EVENTSTRUCT << " " << STR_EVENTSTRUCT << ";"
            << "enum " << STR_EVENTTYPE
            << "{"
            << "  MCXX_EVENT_TYPE_BURST_OPEN = 0,\n"
            << "  MCXX_EVENT_TYPE_BURST_CLOSE = 1,\n"
            << "  MCXX_EVENT_TYPE_POINT = 2,\n"
            << "  MCXX_EVENT_TYPE_INVALID = 0XFFFFFFFF\n"
            << "};"
            << "typedef enum " << STR_EVENTTYPE << " " << STR_EVENTTYPE << ";";
    }

    if (task_creation)
    {
        wrapper_decls
            << "template <typename T>struct mcxx_ptr_t;"
            << "template <typename T>struct mcxx_ref_t;";

        if (!IS_C_LANGUAGE)
        {
            // NOTE: The following declarations will be placed in the source by the codegen in C lang
            wrapper_decls
                /*<< "typedef nanos_wd_t nanos_wg_t;"*/
                << "enum"
                << "{"
                << "  NANOS_FPGA_ARCH_SMP = 0x800000,"
                << "  NANOS_FPGA_ARCH_FPGA = 0x400000"
                << "};"
                << "enum"
                << "{"
                << "  NANOS_ARGFLAG_DEP_OUT = 0x08,"
                << "  NANOS_ARGFLAG_DEP_IN = 0x04,"
                << "  NANOS_ARGFLAG_COPY_OUT = 0x02,"
                << "  NANOS_ARGFLAG_COPY_IN = 0x01,"
                << "  NANOS_ARGFLAG_NONE = 0x00"
                << "};"
                << "struct __attribute__ ((__packed__)) nanos_fpga_copyinfo_t"
                << "{"
                << "  uint64_t address;"
                << "  uint8_t  flags;"
                << "  uint8_t  arg_idx;"
                << "  uint16_t _padding;"
                << "  uint32_t size;"
                << "  uint32_t offset;"
                << "  uint32_t accessed_length;"
                << "};"
                << "typedef struct nanos_fpga_copyinfo_t nanos_fpga_copyinfo_t;";
        }
    }

    /*** Variable declarations ***/
    wrapper_decls
        << "extern const uint8_t " << STR_FULL_ACCID << ";"
        << "static uint64_t " << STR_TASKID << ";"
        << "static uint64_t " << STR_PARENT_TASKID << ";";

    if (instrumentation)
    {
        wrapper_decls
            << "uint64_t " << STR_INSTRBUFFER_OFFSET << ";"
            << "extern volatile counter_t * " << STR_INSTRCOUNTER << ";"
            << "extern volatile counter_t * " << STR_INSTRBUFFER << ";"
            << "unsigned short int " << STR_INSTRSLOTS << ", " << STR_INSTRCURRENTSLOT << ", " << STR_INSTROVERFLOW
            <<     ", " << STR_INSTRAVSLOTS ";";

        wrapper_body_pragmas
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRCOUNTER << " offset=direct "
            <<     "bundle=" << STR_INSTRCOUNTER << "\n"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRBUFFER << "\n";
    }

    if (task_creation)
    {
        wrapper_decls
            << "extern ap_uint<72> " << STR_GLOB_OUTPORT << ";"
            << "extern volatile ap_uint<2> " << STR_GLOB_TWPORT << ";"
            << "static ap_uint<32> " << STR_COMPONENTS_COUNT << ";";

        wrapper_body_pragmas
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_OUTPORT << "\n"
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_TWPORT << "\n";

        if (shared_memory_port_width != "")
        {
            wrapper_decls
                << "extern volatile ap_uint<" + shared_memory_port_width + "> * " + STR_WRAPPERDATA << ";";

            wrapper_body_pragmas
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n";
        }
    }

    /*** Function declarations ***/
    wrapper_decls
        << "void __mcxx_write_stream(axiStream_t &stream, uint64_t data, unsigned short dest, unsigned char last);"
        << "void __mcxx_send_finished_task_cmd(axiStream_t& stream, const uint8_t destId);";

    if (put_instr_nanos_api)
    {
        // NOTE: The following declarations will be placed in the source by the codegen in C lang
        wrapper_decls
            << "nanos_err_t nanos_instrument_burst_begin(nanos_event_key_t event, nanos_event_value_t value);"
            << "nanos_err_t nanos_instrument_burst_end(nanos_event_key_t event, nanos_event_value_t value);"
            << "nanos_err_t nanos_instrument_point_event(nanos_event_key_t event, nanos_event_value_t value);";
    }

    if (instrumentation)
    {
        wrapper_decls
            << "counter_t __mcxx_instr_get_time();"
            << "void __mcxx_instr_wait();"
            << "void __mcxx_instr_write(uint32_t event, uint64_t val, uint32_t type, const counter_t timestamp);";
    }

    if (task_creation)
    {
        if (!IS_C_LANGUAGE)
        {
            // NOTE: The following declarations will be placed in the source by the codegen in C lang
            wrapper_decls
                << "void __mcxx_write_outstream(const uint64_t data, const unsigned short dest, const unsigned char last);"
                << "void __mcxx_wait_tw_signal();"
                << "unsigned long long int nanos_fpga_current_wd();"
                << "void nanos_handle_error(nanos_err_t err);"
                << "nanos_err_t nanos_fpga_wg_wait_completion(unsigned long long int uwg, unsigned char avoid_flush);"
                << "void nanos_fpga_create_wd_async(uint32_t archMask, uint64_t type,"
                << "    uint8_t numArgs, uint64_t * args,"
                << "    uint8_t numDeps, uint64_t * deps, uint8_t * depsFlags,"
                << "    uint8_t numCopies, nanos_fpga_copyinfo_t * copies);";
        }
    }

    /*** Full mcxx_ptr_t and mcxx_ref_t definition ***/
    // NOTE: This has to be done here, otherwise the user code cannot instantiate those variable types
    if (task_creation)
    {
        Source ptr_ops;
        if (shared_memory_port_width != "")
        {
            wrapper_decls
                << "template <typename T>"
                << "struct mcxx_ref_t"
                << "{"
                << "  uintptr_t offset;"
                << "  ap_uint<" << shared_memory_port_width << "> buffer;"
                << "  mcxx_ref_t(const uintptr_t offset)"
                << "  {"
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n"
                << "    this->buffer = *(" << STR_WRAPPERDATA << " + offset/sizeof(ap_uint<" << shared_memory_port_width << ">));"
                << "    this->offset = offset;"
                << "  }"
                << "  operator T() const"
                << "  {"
                << "    union { uint64_t raw; const T typed; } cast_tmp;"
                << "    const size_t off = this->offset%sizeof(ap_uint<" << shared_memory_port_width << ">);"
                << "    cast_tmp.raw = this->buffer.range((off+1)*sizeof(const T)*8-1,off*sizeof(const T)*8);"
                << "    return cast_tmp.typed;"
                << "  }"
                << "  mcxx_ref_t<T>& operator=(const T value)"
                << "  {"
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n"
                << "    union { uint64_t raw; T typed; } cast_tmp;"
                << "    cast_tmp.typed = value;"
                << "    const size_t off = this->offset%sizeof(ap_uint<" << shared_memory_port_width << ">);"
                << "    this->buffer.range((off+1)*sizeof(T)*8-1,off*sizeof(T)*8) = cast_tmp.raw;"
                << "    *(" << STR_WRAPPERDATA << " + this->offset/sizeof(ap_uint<" << shared_memory_port_width << ">)) = this->buffer;"
                << "    return *this;"
                << "  }"
                << "  mcxx_ptr_t<T> operator&()"
                << "  {"
                << "    return new mcxx_ptr_t<T>(this->offset);"
                << "  }"
                << "};";

            ptr_ops
                << "  mcxx_ref_t<T> operator[](size_t idx)"
                << "  {"
                << "    return mcxx_ref_t<T>(this->val + idx);"
                << "  }"
                << "  mcxx_ref_t<T> operator*()"
                << "  {"
                << "    return mcxx_ref_t<T>(this->val);"
                << "  }"
                // NOTE: Not sure if the following method is well implemented
                << "  operator ap_uint<" << shared_memory_port_width << "> *() const"
                << "  {"
                << "    return (ap_uint<" << shared_memory_port_width << "> *)(" << STR_WRAPPERDATA << " + "
                <<        "this->val/sizeof(ap_uint<" << shared_memory_port_width << ">));"
                << "  }";
        }

        wrapper_decls
            << "template <typename T>"
            << "struct mcxx_ptr_t"
            << "{"
            << "  uintptr_t val;"
            << "  mcxx_ptr_t() : val(0) {}"
            << "  mcxx_ptr_t(uintptr_t val) { this->val = val; }"
            << "  mcxx_ptr_t(T* ptr) { this->val = (uintptr_t)ptr; }"
            << "  template <typename V>"
            << "  mcxx_ptr_t(mcxx_ptr_t<V> const &ref) { this->val = ref.val; }"
            << "  operator T*() const { return (T *)this->val; }"
            << "  operator uintptr_t() const { return this->val; }"
            << "  operator mcxx_ptr_t<const T>() const"
            << "  {"
            << "    mcxx_ptr_t<const T> ret;"
            << "    ret.val = this->val;"
            << "    return ret;"
            << "  }"
            << "  template <typename V>"
            << "  mcxx_ptr_t<T> operator + (V const val) const"
            << "  {"
            << "    mcxx_ptr_t<T> ret;"
            << "    ret.val = this->val + val*sizeof(T);"
            << "    return ret;"
            << "  }"
            << "  template <typename V>"
            << "  mcxx_ptr_t<T> operator - (V const val) const"
            << "  {"
            << "    mcxx_ptr_t<T> ret;"
            << "    ret.val = this->val - val*sizeof(T);"
            << "    return ret;"
            << "  }"
            << ptr_ops
            << "};";
    }
}

void get_hls_wrapper_defs(
  const bool instrumentation,
  const bool user_calls_nanos_instrument,
  const bool task_creation,
  const std::string shared_memory_port_width,
  Source& wrapper_defs)
{
    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated

    wrapper_defs
        << "void __mcxx_write_stream(axiStream_t &stream, uint64_t data, unsigned short dest, unsigned char last)"
        << "{"
        << "#pragma HLS INLINE\n"
        << "#pragma HLS INTERFACE axis port=stream\n"
        << "  axiData_t __data = {0, 0, 0, 0, 0, 0, 0};"
        << "  __data.id = " << STR_GLB_ACCID << ";"
        << "  __data.keep = 0xFF;"
        << "  __data.dest = dest;"
        << "  __data.last = last;"
        << "  __data.data = data;"
        << "  stream.write(__data);"
        << "}"

        << "void __mcxx_send_finished_task_cmd(axiStream_t& stream, const uint8_t destId)"
        << "{"
        << "#pragma HLS INTERFACE axis port=stream\n"
        << "  uint64_t header = " << STR_GLB_ACCID << ";"
        << "  header = (header << 8) | 0x03;"
        << "  __mcxx_write_stream(stream, header, destId, 0);"
        << "  __mcxx_write_stream(stream, " << STR_TASKID << ", destId, 0);"
        << "  __mcxx_write_stream(stream, " << STR_PARENT_TASKID << ", destId, 1);"
        << "}";

    if (instrumentation)
    {
        wrapper_defs
            << "counter_t __mcxx_instr_get_time()"
            << "{"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRCOUNTER << " offset=direct bundle=" << STR_INSTRCOUNTER << "\n"
            << "#pragma HLS inline\n"
            << "  return *(" << STR_INSTRCOUNTER << ");"
            << "}"

            << "void __mcxx_instr_wait()"
            << "{"
            << "#pragma HLS inline off\n"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRBUFFER << "\n"
            << "  if (" << STR_INSTRAVSLOTS << " == 1) {"
            << "    unsigned short int i = (" << STR_INSTRCURRENTSLOT << " + 1)%" << STR_INSTRSLOTS << ";"
            << "    uint64_t typeAndId = *((uint64_t*)(" << STR_INSTRBUFFER << " + ((" << STR_INSTRBUFFER_OFFSET
            <<     " + i*sizeof(" << STR_EVENTSTRUCT << ") + offsetof(" << STR_EVENTSTRUCT << ", typeAndId))/sizeof(uint64_t))));"
            << "    while (((typeAndId >> 32) == MCXX_EVENT_TYPE_INVALID) && (" << STR_INSTRAVSLOTS
            <<     " < " <<  STR_INSTRSLOTS << ")) {"
            << "      " << STR_INSTRAVSLOTS << "++;"
            << "      i = (i+1)%" << STR_INSTRSLOTS << ";"
            << "      typeAndId = *((uint64_t*)(" << STR_INSTRBUFFER << " + ((" << STR_INSTRBUFFER_OFFSET
            <<     " + i*sizeof(" << STR_EVENTSTRUCT << ") + offsetof(" << STR_EVENTSTRUCT << ", typeAndId))/sizeof(uint64_t))));"
            << "    }"
            << "    if (" << STR_INSTRAVSLOTS << " > 1 && " << STR_INSTROVERFLOW << " > 0) {"
            << "      " << STR_INSTROVERFLOW << " = 0;"
            << "      " << STR_INSTRCURRENTSLOT << "++;"
            << "    }"
            << "  }"
            << "}"

            << "void __mcxx_instr_write(uint32_t event, uint64_t val, uint32_t type, const counter_t timestamp)"
            << "{"
            //NOTE: This function must be inline to avoid issue: https://pm.bsc.es/gitlab/ompss-at-fpga/mcxx/issues/19
            << "#pragma HLS inline\n"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRBUFFER << "\n"
            << "  if (" << STR_INSTRSLOTS << " > 0) {"
            << "    __mcxx_instr_wait();"
            << "    const uint64_t slot_offset = (" << STR_INSTRBUFFER_OFFSET << " + "
            <<         STR_INSTRCURRENTSLOT << "*sizeof(" << STR_EVENTSTRUCT << "))/sizeof(uint64_t);"
            << "    " << STR_EVENTSTRUCT << " fpga_event;"
            << "    if (" << STR_INSTRAVSLOTS << " > 1) {"
            << "      " << STR_INSTRCURRENTSLOT << " = (" << STR_INSTRCURRENTSLOT << " + 1)%" << STR_INSTRSLOTS << ";"
            << "      " << STR_INSTRAVSLOTS << "--;"
            << "      fpga_event.typeAndId = ((uint64_t)type<<32) | event;"
            << "      fpga_event.value = val;"
            << "      fpga_event.timestamp = timestamp;"
            << "    }"
            << "    else if (" << STR_INSTRAVSLOTS << " == 1) {"
            << "      fpga_event.typeAndId = (((uint64_t)MCXX_EVENT_TYPE_POINT)<<32) | "
            <<     EV_INSTEVLOST << ";"
            << "      fpga_event.value = ++" << STR_INSTROVERFLOW << ";"
            << "      fpga_event.timestamp = timestamp;"
            << "    }"
            << "    memcpy((void *)(" << STR_INSTRBUFFER << " + slot_offset), &fpga_event, sizeof("
            <<     STR_EVENTSTRUCT << "));"
            << "  }"
            << "}"

            << "nanos_err_t nanos_instrument_burst_begin(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_BURST_OPEN, __mcxx_instr_get_time() );"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_burst_end(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_BURST_CLOSE, __mcxx_instr_get_time() );"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_point_event(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_POINT, __mcxx_instr_get_time() );"
            << "  return NANOS_OK;"
            << "}";
    }
    else if (user_calls_nanos_instrument)
    {
        //Define empty instrument calls when instrumentation is not enabled
        wrapper_defs
            << "nanos_err_t nanos_instrument_burst_begin(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_burst_end(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_point_event(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "  return NANOS_OK;"
            << "}";
    }

    if (task_creation)
    {
        wrapper_defs
            << "void __mcxx_write_outstream(const uint64_t data, const unsigned short dest, const unsigned char last)"
            << "{"
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_OUTPORT << " register\n"
            // NOTE: Pack the axiData_t info: data(64bits) + dest(6bits) + last(2bit). It can be done
            //       with less bits but this way the info is HEX friendly
            << "  ap_uint<72> tmp = data;"
            << "  tmp = (tmp << 8) | ((dest & 0x3F) << 2) | (last & 0x3);"
            << "  " << STR_GLOB_OUTPORT << " = tmp;"
            << "}"

            << "void __mcxx_wait_tw_signal()"
            << "{"
            << "  #pragma HLS INTERFACE ap_hs port=" << STR_GLOB_TWPORT << "\n"
            << "  ap_uint<2> sync = " << STR_GLOB_TWPORT << ";"
            << "}"

            << "unsigned long long int nanos_fpga_current_wd()"
            << "{"
            << " return " << STR_TASKID << ";"
            << "}"

            << "void nanos_handle_error(nanos_err_t err)"
            << "{}"

            << "nanos_err_t nanos_fpga_wg_wait_completion(unsigned long long int uwg, unsigned char avoid_flush)"
            << "{"
            << "  if (" << STR_COMPONENTS_COUNT << " == 0) { return NANOS_OK; }"
            << "  const unsigned short TM_TW = 0x13;"
            << "  uint64_t tmp = " << STR_EXT_ACCID << ";"
            << "  tmp = tmp << 48 /*ACC_ID info uses bits [48:55]*/;"
            << "  tmp = 0x8000000100000000 | tmp | " << STR_COMPONENTS_COUNT << ";"
            << "  __mcxx_write_outstream(tmp /*TASKWAIT_DATA_BLOCK*/, TM_TW, 0 /*last*/);"
            << "  __mcxx_write_outstream(" << STR_TASKID << " /*data*/, TM_TW, 1 /*last*/);"
            << "  {\n"
            << "#pragma HLS PROTOCOL fixed\n"
            << "    __mcxx_wait_tw_signal();"
            << "  }\n"
            << "  " << STR_COMPONENTS_COUNT << " = 0;"
            << "  return NANOS_OK;"
            << "}"

            << "void nanos_fpga_create_wd_async(uint32_t archMask, uint64_t type,"
            << "    uint8_t numArgs, uint64_t * args,"
            << "    uint8_t numDeps, uint64_t * deps, uint8_t * depsFlags,"
            << "    uint8_t numCopies, nanos_fpga_copyinfo_t * copies)"
            << "{"
            << "#pragma HLS inline\n"
            << "  ++" << STR_COMPONENTS_COUNT << ";"
            << "  const unsigned short TM_NEW = 0x12;"
            << "  const unsigned short TM_SCHED = 0x14;"
            << "  const unsigned char hasSmpArch = (archMask & NANOS_FPGA_ARCH_SMP) != 0;"
            << "  const unsigned short destId = (numDeps == 0 && !hasSmpArch) ? TM_SCHED : TM_NEW;"
            //1st word: [ valid (8b) | arch_mask (24b) | num_copies (8b) | num_deps (8b) | num_args (8b) | (8b) ]
            << "  uint64_t tmp = archMask;"
            << "  tmp = (tmp << 8) | numCopies;"
            << "  tmp = (tmp << 8) | numDeps;"
            << "  tmp = (tmp << 8) | numArgs;"
            << "  tmp = tmp << 8;"
            << "  __mcxx_write_outstream(tmp, destId, 0);"
            //2nd word: [ parent_task_id (64b) ]
            << "  __mcxx_write_outstream(" << STR_TASKID << ", destId, 0);"
            //3rd word: [ type_value (64b) ]
            << "  __mcxx_write_outstream(type, destId, 0);"
            //copy words
            << "  for (uint8_t idx = 0; idx < numCopies; ++idx) {"
            //1st copy word: [ address (64b) ]
            << "    tmp = copies[idx].address;"
            << "    __mcxx_write_outstream(tmp, destId, 0);"
            //2nd copy word: [ size (32b) | not_used (16b) | arg_idx (8b) | flags (8b) ]
            << "    tmp = copies[idx].size;"
            << "    tmp = (tmp << 24) | copies[idx].arg_idx;"
            << "    tmp = (tmp << 8) | copies[idx].flags;"
            << "    __mcxx_write_outstream(tmp, destId, 0);"
            //3rd copy word: [ accessed_length (32b) | offset (32b) ]
            << "    tmp = copies[idx].accessed_length;"
            << "    tmp = (tmp << 32) | copies[idx].offset;"
            << "    __mcxx_write_outstream(tmp, destId, idx == (numCopies - 1)&&(numDeps == 0)&&(numCopies == 0));"
            << "  }"
            << "  for (uint8_t idx = 0; idx < numDeps; ++idx) {"
            << "    tmp = depsFlags[idx];"
            << "    tmp = (tmp << 56) | deps[idx];"
            //dep words: [ arg_flags (8b) | arg_value (56b) ]
            << "    __mcxx_write_outstream(tmp, destId, (idx == (numDeps - 1))&&(numArgs == 0));"
            << "  }"
            << "  for (uint8_t idx = 0; idx < numArgs; ++idx) {"
            //arg words: [ arg_value (64b) ]
            << "    __mcxx_write_outstream(args[idx], destId, idx == (numArgs - 1));"
            << "  }"
            << "}";
    }
}

} // namespace Nanox
} // namespace TL

#endif // NANOX_FPGA_UTILS_HPP

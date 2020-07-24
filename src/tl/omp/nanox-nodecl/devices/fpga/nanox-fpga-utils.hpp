/*--------------------------------------------------------------------
  (C) Copyright 2018-2020 Barcelona Supercomputing Center
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

#define STR_ACCID              "accID"
#define STR_COMPONENTS_COUNT   "__mcxx_taskComponents"
#define STR_OUTPORT            "mcxx_outPort"
#define STR_GLOB_EINPORT       "mcxx_eInPort"
#define STR_TASKID             "__mcxx_taskId"
#define STR_PARENT_TASKID      "__mcxx_parent_taskId"
#define STR_REP_NUM            "__mcxx_periTask_repNum"
#define STR_NUM_REPS           "__mcxx_periTask_numReps"
#define STR_WRAPPERDATA        "mcxx_wrapper_data"
#define STR_INPUTSTREAM        "mcxx_inStream"
#define STR_INSTR_PORT         "mcxx_instr"
#define STR_HWCOUNTER_PORT     "mcxx_hwcounterPort"
#define STR_FREQ_PORT          "mcxx_freqPort"

//Default instrumentation events codes
#define EV_DEVCOPYIN            78
#define EV_DEVCOPYOUT           79
#define EV_DEVEXEC              80
#define EV_INSTEVLOST           82

//Ack codes
#define ACK_REJECT_CODE         0x0
#define ACK_OK_CODE             0x1
#define ACK_FINAL_CODE          0x2

//IDs of the HWR IPs
#define HWR_DEPS_ID             0x12
#define HWR_SCHED_ID            0x13
#define HWR_TASKWAIT_ID         0x14
#define HWR_LOCK_ID             0x15

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

std::string get_mcxx_ptr_declaration(TL::Scope scope, const TL::Type& type_to_point)
{
    return "mcxx_ptr_t< " + type_to_point.get_simple_declaration(scope, "") + " >";
}

void add_fpga_header(
    FILE* file,
    const bool needs_systemc_header,
    const std::string name,
    const std::string type,
    const std::string num_instances)
{
    fprintf(file, "\
///////////////////\n\
// Automatic IP Generated by OmpSs@FPGA compiler\n\
///////////////////\n\
// The below code is composed by:\n\
//  1) User source code, which may be under any license (see in original source code)\n\
//  2) OmpSs@FPGA toolchain code which is licensed under LGPLv3 terms and conditions\n\
///////////////////\n"
    );
    fprintf(file, "// Top IP Function: %s\n", name.c_str());
    fprintf(file, "// Accel. type hash: %s\n", type.c_str());
    fprintf(file, "// Num. instances: %s\n", num_instances.c_str());
    fprintf(file, "// Wrapper version: %s\n", FPGA_WRAPPER_VERSION);
    fprintf(file, "\
///////////////////\n\
#define __HLS_AUTOMATIC_MCXX__ 1\n\n"
    );

    if (needs_systemc_header)
    {
        fprintf(file, "#include <systemc.h>\n");
    }
    fprintf(file, "\
#include <cstring>\n\
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
            TL::Symbol field = scope.new_symbol(get_mcxx_ptr_declaration(scope, type_to_point));
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
                // inside the FPGA. Therefore, we replace the type of variables by unsigned long long int
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
        const std::string                _unique_suffix;
        const std::string                _filename;
        Nodecl::Utils::SimpleSymbolMap*  _symbol_map;
        //FIXME: Do not use the following set to know which symbols are a copy of originals
        std::set<scope_entry_t*>         _new_symbol_set;

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
        std::set<std::string>            _user_calls_set;

        FpgaTaskCodeVisitor(const std::string suffix, const std::string filename, Nodecl::Utils::SimpleSymbolMap * map) :
                _unique_suffix(suffix), _filename(filename), _symbol_map(map), _new_symbol_set(), _called_functions(),
                _user_calls_set() {}

        virtual void visit(const Nodecl::Symbol& node)
        {
            TL::Symbol sym = node.get_symbol();
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
            //Nodecl::NodeclBase arguments = node.get_arguments();
            //Nodecl::NodeclBase alternate_name = node.get_alternate_name();
            //Nodecl::NodeclBase function_form = node.get_function_form();

            //walk(called);
            //walk(arguments);
            //walk(alternate_name);
            //walk(function_form);

            if (!called.is<Nodecl::Symbol>())
                return;
            TL::Symbol sym = called.as<Nodecl::Symbol>().get_symbol();

            Nodecl::FunctionCode function_code = sym.get_function_code().as<Nodecl::FunctionCode>();
            if (function_code.is_null())
            {
                if (sym.get_name() == "memcpy")
                {
                    if (_symbol_map->map(sym) == sym)
                    {
                        // This is the first occurence of memcpy, create the __mcxx_memcpy symbol
                        ObjectList<std::string> param_names;
                        ObjectList<TL::Type> param_types;

                        param_names.append("dest");
                        param_types.append(TL::Type::get_void_type().get_pointer_to());

                        param_names.append("src");
                        param_types.append(TL::Type::get_void_type().get_const_type().get_pointer_to());

                        param_names.append("n");
                        param_types.append(TL::Type::get_unsigned_int_type().get_const_type());

                        _symbol_map->add_map(sym, SymbolUtils::new_function_symbol(
                            sym.get_scope(),
                            "__mcxx_memcpy",
                            TL::Type::get_void_type().get_pointer_to(),
                            param_names,
                            param_types));
                        _user_calls_set.insert("mcxx_memcpy");
                    }

                    //NOTE: Replace the called symbol: memcpy --> __mcxx_memcpy
                    called.set_symbol(_symbol_map->map(sym));
                }
                else if (sym.get_name() == "memset")
                {
                    if (_symbol_map->map(sym) == sym)
                    {
                        // This is the first occurence of memset, create the __mcxx_memset symbol
                        ObjectList<std::string> param_names;
                        ObjectList<TL::Type> param_types;

                        param_names.append("s");
                        param_types.append(TL::Type::get_void_type().get_pointer_to());

                        param_names.append("c");
                        param_types.append(TL::Type::get_int_type());

                        param_names.append("n");
                        param_types.append(TL::Type::get_unsigned_int_type());

                        _symbol_map->add_map(sym, SymbolUtils::new_function_symbol(
                            sym.get_scope(),
                            "__mcxx_memset",
                            sym.get_type().returns(),
                            param_names,
                            param_types));
                        _user_calls_set.insert("mcxx_memset");
                    }

                    //NOTE: Replace the called symbol: memset --> __mcxx_memset
                    called.set_symbol(_symbol_map->map(sym));
                }
                else if (sym.get_name() == "sqrtf")
                {
                    if (_symbol_map->map(sym) == sym)
                    {
                        // This is the first occurence of sqrtf, create the __mcxx_sqrtf symbol
                        ObjectList<std::string> param_names;
                        ObjectList<TL::Type> param_types;

                        param_names.append("x");
                        param_types.append(TL::Type::get_float_type());

                        _symbol_map->add_map(sym, SymbolUtils::new_function_symbol(
                            sym.get_scope(),
                            "__mcxx_sqrtf",
                            sym.get_type().returns(),
                            param_names,
                            param_types));
                        _user_calls_set.insert("mcxx_sqrtf");
                    }

                    //NOTE: Replace the called symbol: sqrtf --> __mcxx_sqrtf
                    called.set_symbol(_symbol_map->map(sym));
                }
                else if (sym.get_name() == "usleep")
                {
                    if (_symbol_map->map(sym) == sym)
                    {
                        // This is the first occurence of usleep, create the __mcxx_usleep symbol
                        ObjectList<std::string> param_names;
                        ObjectList<TL::Type> param_types;

                        param_names.append("usec");
                        param_types.append(TL::Type::get_unsigned_int_type());

                        _symbol_map->add_map(sym, SymbolUtils::new_function_symbol(
                            sym.get_scope(),
                            "__mcxx_usleep",
                            sym.get_type().returns(),
                            param_names,
                            param_types));
                        _user_calls_set.insert("mcxx_usleep");
                    }

                    //NOTE: Replace the called symbol: usleep --> __mcxx_usleep
                    called.set_symbol(_symbol_map->map(sym));
                }
                else if (sym.get_name().find("nanos_instrument_") != std::string::npos)
                {
                    _user_calls_set.insert("nanos_instrument");
                }
                else if (sym.get_name().find("nanos_handle_error") != std::string::npos)
                {
                    _user_calls_set.insert("nanos_handle_error");
                }
                else if (sym.get_name().find("nanos_set_lock") != std::string::npos)
                {
                    _user_calls_set.insert("nanos_set_lock");
                }
                else if (sym.get_name().find("nanos_try_lock") != std::string::npos)
                {
                    _user_calls_set.insert("nanos_try_lock");
                }
                else if (sym.get_name().find("nanos_unset_lock") != std::string::npos)
                {
                    _user_calls_set.insert("nanos_unset_lock");
                }

                return;
            }

            const std::map<TL::Symbol, TL::Symbol>* map = _symbol_map->get_simple_symbol_map();
            bool has_been_duplicated = map->find(sym) != map->end();
            const bool is_orig_symbol = _new_symbol_set.find(sym.get_internal_symbol()) == _new_symbol_set.end();
            const bool is_member = sym.is_member();

            if (_filename == function_code.get_filename() && !has_been_duplicated && is_orig_symbol && !is_member)
            {
                // Duplicate the symbol and append the function code to the list
                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(
                    sym, sym.get_name() + _unique_suffix);

                has_been_duplicated = true;
                _symbol_map->add_map(sym, new_function);

                //NOTE: _new_symbol_set should not be necessary as when the visitor founds the same symbol a
                //      second time it should point the original symbol. However, it points the copied one
                //      under some unknown circumstancies.
                _new_symbol_set.insert(new_function.get_internal_symbol());

                Nodecl::NodeclBase fun_code = Nodecl::Utils::deep_copy(
                    function_code,
                    sym.get_scope(),
                    *_symbol_map);
                symbol_entity_specs_set_is_static(new_function.get_internal_symbol(), 1);
                //called.set_symbol(new_function);

                walk(fun_code);

                //NOTE: Prepend the function code to ensure a proper declaration order in the FPGA source
                _called_functions.append(fun_code);
            }

            if (has_been_duplicated)
            {
                Nodecl::NodeclBase new_function_call = Nodecl::Utils::deep_copy(
                     node,
                     node,
                     *_symbol_map);

                node.replace(new_function_call);
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

    //Add the unsigned long long int raw member
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
  const bool task_creation,
  const bool periodic_support,
  const std::string shared_memory_port_width,
  const std::set<std::string> user_calls_set,
  Source& wrapper_decls_before_user_code,
  Source& wrapper_decls_after_user_code,
  Source& wrapper_body_pragmas)
{
    // NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
    // NOTE: The declarations of Nanos++ APIs must be coherent with the ones in the Nanos++ headers.
    //       The only declarations that changes is nanos_wd_t which is a integer type inside the FPGA
    const bool user_calls_nanos_instrument = user_calls_set.count("nanos_instrument") > 0;
    const bool user_calls_nanos_handle_err = user_calls_set.count("nanos_handle_error") > 0;
    const bool user_calls_nanos_set_lock = user_calls_set.count("nanos_set_lock") > 0;
    const bool user_calls_nanos_try_lock = user_calls_set.count("nanos_set_lock") > 0;
    const bool user_calls_nanos_unset_lock = user_calls_set.count("nanos_unset_lock") > 0;
    const bool put_instr_nanos_api =
        (!IS_C_LANGUAGE && (instrumentation || user_calls_nanos_instrument)) ||
        (IS_C_LANGUAGE && instrumentation && !user_calls_nanos_instrument);
    const bool put_nanos_err_api = !IS_C_LANGUAGE ||
        (IS_C_LANGUAGE && !task_creation && !user_calls_nanos_instrument && instrumentation && !user_calls_nanos_handle_err);
    const bool put_nanos_handle_err_api =
        !IS_C_LANGUAGE && (task_creation || user_calls_nanos_handle_err);
    bool is_nanos_err_declared = false;

    /*** Type declarations ***/
    wrapper_decls_before_user_code
        << "typedef ap_axis<64,1,8,5> axiData_t;"
        << "typedef hls::stream<axiData_t> axiStream_t;"
        /*<< "typedef unsigned long long int nanos_wd_t;"*/;

    if (put_nanos_err_api)
    {
        // NOTE: The following declarations will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
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

        is_nanos_err_declared = true;
    }

    if (put_instr_nanos_api) {
        wrapper_decls_before_user_code
            << "typedef unsigned int nanos_event_key_t;"
            << "typedef unsigned long long int nanos_event_value_t;";
    }

    if (instrumentation)
    {
        wrapper_decls_before_user_code
            << "enum __mcxx_eventType_t"
            << "{"
            << "  MCXX_EVENT_TYPE_BURST_OPEN = 0,\n"
            << "  MCXX_EVENT_TYPE_BURST_CLOSE = 1,\n"
            << "  MCXX_EVENT_TYPE_POINT = 2,\n"
            << "  MCXX_EVENT_TYPE_INVALID = 0XFFFFFFFF\n"
            << "};"
            << "typedef enum __mcxx_eventType_t __mcxx_eventType_t;"
            << "typedef ap_uint<105> __mcxx_instrData_t;";
    }

    if (task_creation)
    {
        wrapper_decls_before_user_code
            << "template <typename T>struct mcxx_ptr_t;"
            << "template <typename T>struct mcxx_ref_t;";

        if (!IS_C_LANGUAGE)
        {
            // NOTE: The following declarations will be placed in the source by the codegen in C lang
            wrapper_decls_before_user_code
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
                << "  unsigned long long int address;"
                << "  unsigned char flags;"
                << "  unsigned char arg_idx;"
                << "  unsigned short _padding;"
                << "  unsigned int size;"
                << "  unsigned int offset;"
                << "  unsigned int accessed_length;"
                << "};"
                << "typedef struct nanos_fpga_copyinfo_t nanos_fpga_copyinfo_t;";
        }
    }

    if ((user_calls_nanos_set_lock || user_calls_nanos_try_lock || user_calls_nanos_unset_lock) && !IS_C_LANGUAGE)
    {
        wrapper_decls_before_user_code
            << "typedef const unsigned char nanos_lock_t;";
    }

    /*** Variable declarations ***/
    wrapper_decls_before_user_code
        << "extern const unsigned char " << STR_ACCID << ";"
        << "static unsigned long long int " << STR_TASKID << ";"
        << "static unsigned long long int " << STR_PARENT_TASKID << ";"
        << "extern ap_uint<72> " << STR_OUTPORT << ";";

    if (task_creation || user_calls_nanos_set_lock || user_calls_nanos_try_lock || user_calls_nanos_unset_lock)
    {
        wrapper_decls_before_user_code
            << "extern volatile ap_uint<8> " << STR_GLOB_EINPORT << ";";

        wrapper_body_pragmas
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_EINPORT << "\n";
    }

    if (instrumentation)
    {
        wrapper_decls_before_user_code
            << "extern __mcxx_instrData_t " << STR_INSTR_PORT << ";";

        wrapper_body_pragmas
            << "#pragma HLS INTERFACE ap_hs port=" << STR_INSTR_PORT << "\n";
    }

    if (task_creation)
    {
        wrapper_decls_before_user_code
            << "static ap_uint<32> " << STR_COMPONENTS_COUNT << ";";

        if (shared_memory_port_width != "")
        {
            wrapper_decls_before_user_code
                << "extern volatile ap_uint<" + shared_memory_port_width + "> * " + STR_WRAPPERDATA << ";";

            wrapper_body_pragmas
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n";
        }
    }

    if (periodic_support || user_calls_set.count("mcxx_usleep") > 0)
    {
        wrapper_decls_before_user_code
            << "extern volatile unsigned long long int " << STR_HWCOUNTER_PORT << ";"
            << "extern ap_uint<10> " << STR_FREQ_PORT << ";";

        wrapper_body_pragmas
            << "#pragma HLS INTERFACE ap_none port=" << STR_HWCOUNTER_PORT << "\n"
            << "#pragma HLS INTERFACE ap_none port=" << STR_FREQ_PORT << "\n";
    }

    if (periodic_support)
    {
        wrapper_decls_before_user_code
            << "static volatile unsigned int " << STR_NUM_REPS << ";"
            << "static unsigned int " << STR_REP_NUM << ";";
    }

    if (user_calls_nanos_set_lock || user_calls_nanos_try_lock || user_calls_nanos_unset_lock)
    {
        wrapper_decls_before_user_code
            << "nanos_lock_t nanos_default_critical_lock = 0;";
    }

    /*** Function declarations ***/
    wrapper_decls_before_user_code
        << "void __mcxx_send_finished_task_cmd(const unsigned char destId);";

    if (!IS_C_LANGUAGE)
    {
        // NOTE: The following declarations will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
            << "void __mcxx_write_out_port(const unsigned long long int data, const unsigned short dest, const unsigned char last);";

        if (task_creation || user_calls_nanos_set_lock || user_calls_nanos_try_lock || user_calls_nanos_unset_lock)
        {
            wrapper_decls_before_user_code
                << "ap_uint<8> __mcxx_read_ein_port();";
        }
    }

    if (user_calls_set.count("mcxx_memcpy") > 0 && !IS_C_LANGUAGE)
    {
        // NOTE: The following declaration will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
            << "void *__mcxx_memcpy(void *dest, const void *src, const unsigned int n);";
    }

    if (user_calls_set.count("mcxx_memset") > 0 && !IS_C_LANGUAGE)
    {
        // NOTE: The following declaration will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
            << "void *__mcxx_memset(void *s, int c, unsigned int n);";
    }

    if (user_calls_set.count("mcxx_sqrtf") > 0 && !IS_C_LANGUAGE)
    {
        // NOTE: The following declaration will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
            << "float __mcxx_sqrtf(float x);";
    }

    if (user_calls_set.count("mcxx_usleep") > 0 && !IS_C_LANGUAGE)
    {
        // NOTE: The following declaration will be placed in the source by the codegen in C lang
        wrapper_decls_before_user_code
            << "int __mcxx_usleep(unsigned int usec);";
    }

    if (put_nanos_handle_err_api)
    {
        wrapper_decls_before_user_code
            << "void nanos_handle_error(nanos_err_t err);";
    }

    if (put_instr_nanos_api)
    {
        // NOTE: Postpone the declarations if nanos_err_t is not yet defined
        Source& src = is_nanos_err_declared ? wrapper_decls_before_user_code : wrapper_decls_after_user_code;
        src
            << "nanos_err_t nanos_instrument_burst_begin(nanos_event_key_t event, nanos_event_value_t value);"
            << "nanos_err_t nanos_instrument_burst_end(nanos_event_key_t event, nanos_event_value_t value);"
            << "nanos_err_t nanos_instrument_point_event(nanos_event_key_t event, nanos_event_value_t value);";
    }

    if (instrumentation)
    {
        wrapper_decls_before_user_code
            << "void __mcxx_instr_write(const unsigned int event, const unsigned long long int val, const unsigned int type);";
    }

    if (task_creation)
    {
        if (!IS_C_LANGUAGE)
        {
            // NOTE: The following declarations will be placed in the source by the codegen in C lang
            wrapper_decls_before_user_code
                << "unsigned long long int nanos_fpga_current_wd();"
                << "nanos_err_t nanos_fpga_wg_wait_completion(unsigned long long int uwg, unsigned char avoid_flush);"
                << "void nanos_fpga_create_wd_async(const unsigned long long int type,"
                << "    const unsigned char numArgs, const unsigned long long int * args,"
                << "    const unsigned char numDeps, const unsigned long long int * deps, const unsigned char * depsFlags,"
                << "    const unsigned char numCopies, const nanos_fpga_copyinfo_t * copies);";
        }
    }

    if (periodic_support && !IS_C_LANGUAGE)
    {
        wrapper_decls_before_user_code
            << "unsigned int nanos_get_periodic_task_repetition_num();"
            << "void nanos_cancel_periodic_task();";
    }

    if (user_calls_nanos_set_lock && !IS_C_LANGUAGE)
    {
        wrapper_decls_before_user_code
            << "nanos_err_t nanos_set_lock(nanos_lock_t * lock);";
    }

    if (user_calls_nanos_try_lock && !IS_C_LANGUAGE)
    {
        wrapper_decls_before_user_code
            << "nanos_err_t nanos_try_lock(nanos_lock_t * lock, bool * result);";
    }

    if (user_calls_nanos_unset_lock && !IS_C_LANGUAGE)
    {
        wrapper_decls_before_user_code
            << "nanos_err_t nanos_unset_lock(nanos_lock_t * lock);";
    }

    /*** Full mcxx_ptr_t and mcxx_ref_t definition ***/
    // NOTE: This has to be done here, otherwise the user code cannot instantiate those variable types
    if (task_creation)
    {
        Source ptr_ops;
        if (shared_memory_port_width != "")
        {
            wrapper_decls_before_user_code
                << "template <typename T>"
                << "struct mcxx_ref_t"
                << "{"
                << "  unsigned long long int offset;"
                << "  ap_uint<" << shared_memory_port_width << "> buffer;"
                << "  mcxx_ref_t(const unsigned long long int offset)"
                << "  {"
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n"
                << "    this->buffer = *(" << STR_WRAPPERDATA << " + offset/sizeof(ap_uint<" << shared_memory_port_width << ">));"
                << "    this->offset = offset;"
                << "  }"
                << "  operator T() const"
                << "  {"
                << "    union { unsigned long long int raw; const T typed; } cast_tmp;"
                << "    const size_t off = this->offset%sizeof(ap_uint<" << shared_memory_port_width << ">);"
                << "    cast_tmp.raw = this->buffer.range((off+1)*sizeof(const T)*8-1,off*sizeof(const T)*8);"
                << "    return cast_tmp.typed;"
                << "  }"
                << "  mcxx_ref_t<T>& operator=(const T value)"
                << "  {"
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n"
                << "    union { unsigned long long int raw; T typed; } cast_tmp;"
                << "    cast_tmp.typed = value;"
                << "    const size_t off = this->offset%sizeof(ap_uint<" << shared_memory_port_width << ">);"
                << "    this->buffer.range((off+1)*sizeof(T)*8-1,off*sizeof(T)*8) = cast_tmp.raw;"
                << "    *(" << STR_WRAPPERDATA << " + this->offset/sizeof(ap_uint<" << shared_memory_port_width << ">)) = this->buffer;"
                << "    return *this;"
                << "  }"
                << "  mcxx_ptr_t<T> operator&()"
                << "  {"
                << "    return mcxx_ptr_t<T>(this->offset);"
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

        wrapper_decls_before_user_code
            << "template <typename T>"
            << "struct mcxx_ptr_t"
            << "{"
            << "  unsigned long long int val;"
            << "  mcxx_ptr_t() : val(0) {}"
            << "  mcxx_ptr_t(unsigned long long int val) { this->val = val; }"
            << "  mcxx_ptr_t(T* ptr) { this->val = (unsigned long long int)ptr; }"
            << "  template <typename V>"
            << "  mcxx_ptr_t(mcxx_ptr_t<V> const &ref) { this->val = ref.val; }"
            << "  operator T*() const { return (T *)this->val; }"
            << "  operator unsigned long long int() const { return this->val; }"
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
  const bool task_creation,
  const bool periodic_support,
  const std::set<std::string> user_calls_set,
  const std::string shared_memory_port_width,
  Source& wrapper_defs)
{
    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
    const bool user_calls_nanos_set_lock = user_calls_set.count("nanos_set_lock") > 0;
    const bool user_calls_nanos_try_lock = user_calls_set.count("nanos_try_lock") > 0;
    const bool user_calls_nanos_unset_lock = user_calls_set.count("nanos_unset_lock") > 0;

    wrapper_defs
        << "void __mcxx_write_out_port(const unsigned long long int data, const unsigned short dest, const unsigned char last)"
        << "{"
        << "#pragma HLS INTERFACE ap_hs port=" << STR_OUTPORT << " register\n"
        // NOTE: Pack the axiData_t info: data(64bits) + dest(6bits) + last(2bit). It can be done
        //       with less bits but this way the info is HEX friendly
        << "  ap_uint<72> tmp = data;"
        << "  tmp = (tmp << 8) | ((dest & 0x3F) << 2) | (last & 0x3);"
        << "  " << STR_OUTPORT << " = tmp;"
        << "}"

        << "void __mcxx_send_finished_task_cmd(const unsigned char destId)"
        << "{"
        << "  unsigned long long int header = " << STR_ACCID << ";"
        << "  header = (header << 8) | 0x03;"
        << "  __mcxx_write_out_port(header, destId, 0);"
        << "  __mcxx_write_out_port(" << STR_TASKID << ", destId, 0);"
        << "  __mcxx_write_out_port(" << STR_PARENT_TASKID << ", destId, 1);"
        << "}";

    if (task_creation || user_calls_nanos_set_lock || user_calls_nanos_try_lock || user_calls_nanos_unset_lock)
    {
        wrapper_defs
            << "ap_uint<8> __mcxx_read_ein_port()"
            << "{"
            << "  #pragma HLS INTERFACE ap_hs port=" << STR_GLOB_EINPORT << "\n"
            << "  ap_uint<8> sync = " << STR_GLOB_EINPORT << ";"
            << "  return sync;"
            << "}";
    }

    if (user_calls_set.count("mcxx_memcpy") > 0)
    {
        wrapper_defs
            << "void *__mcxx_memcpy(void *dest, const void *src, const unsigned int n)"
            << "{"
            << "#pragma HLS INLINE\n"
            << "  return memcpy(dest, src, n);"
            << "}";
    }

    if (user_calls_set.count("mcxx_memset") > 0)
    {
        wrapper_defs
            << "void *__mcxx_memset(void *s, int c, unsigned int n)"
            << "{"
            << "#pragma HLS INLINE\n"
            << "  return memset(s, c, n);"
            << "}";
    }

    if (user_calls_set.count("mcxx_sqrtf") > 0)
    {
        wrapper_defs
            << "float __mcxx_sqrtf(float x)"
            << "{"
            << "#pragma HLS INLINE\n"
            << "  return sqrtf(x);"
            << "}";
    }

    if (user_calls_set.count("mcxx_usleep") > 0)
    {
        wrapper_defs
            << "int __mcxx_usleep(unsigned int usec)"
            << "{"
            << "#pragma HLS INLINE\n"
            << "#pragma HLS INTERFACE ap_none port=" << STR_HWCOUNTER_PORT << "\n"
            << "#pragma HLS INTERFACE ap_none port=" << STR_FREQ_PORT << "\n"
            << "  const unsigned int __acc_freq = " << STR_FREQ_PORT << ";"
            << "  const unsigned long long int __usec_cycles = " << STR_HWCOUNTER_PORT << " + usec*__acc_freq;"
            << "  do {"
            << "    wait();"
            << "  } while (" << STR_HWCOUNTER_PORT << " < __usec_cycles);"
            << "  return 0;"
            << "}";
    }

    if (user_calls_set.count("nanos_handle_error") > 0)
    {
        wrapper_defs
            << "void nanos_handle_error(nanos_err_t err)"
            << "{}";
    }

    if (user_calls_nanos_set_lock)
    {
        wrapper_defs
            << "nanos_err_t nanos_set_lock(nanos_lock_t * lock)"
            << "{"
            << "  unsigned long long int tmp = lock[0];"
            << "  tmp = (tmp << 8) & 0x04 /*cmd code*/;"
            << "  ap_uint<8> ack = " << ACK_REJECT_CODE << ";"
            << "  do {"
            << "    __mcxx_write_out_port(tmp , " << HWR_LOCK_ID << ", 1 /*last*/);"
            << "    {\n"
            << "#pragma HLS PROTOCOL fixed\n"
            << "      wait();"
            << "      ack = __mcxx_read_ein_port();"
            << "    }\n"
            << "  } while (ack != " << ACK_OK_CODE << ");"
            << "  return NANOS_OK;"
            << "}";
    }

    if (user_calls_nanos_try_lock)
    {
        wrapper_defs
            << "nanos_err_t nanos_try_lock(nanos_lock_t * lock, bool * result)"
            << "{"
            << "  unsigned long long int tmp = lock[0];"
            << "  tmp = (tmp << 8) & 0x04 /*cmd code*/;"
            << "  ap_uint<8> ack = " << ACK_REJECT_CODE << ";"
            << "  __mcxx_write_out_port(tmp , " << HWR_LOCK_ID << ", 1 /*last*/);"
            << "  {\n"
            << "#pragma HLS PROTOCOL fixed\n"
            << "    wait();"
            << "    ack = __mcxx_read_ein_port();"
            << "  }\n"
            << "  result[0] = (ack == " << ACK_OK_CODE << ");"
            << "  return NANOS_OK;"
            << "}";
    }

    if (user_calls_nanos_unset_lock)
    {
        wrapper_defs
            << "nanos_err_t nanos_unset_lock(nanos_lock_t * lock)"
            << "{"
            << "  unsigned long long int tmp = lock[0];"
            << "  tmp = (tmp << 8) & 0x06 /*cmd code*/;"
            << "  __mcxx_write_out_port(tmp , " << HWR_LOCK_ID << ", 1 /*last*/);"
            << "  wait();"
            << "  return NANOS_OK;"
            << "}";
    }

    if (instrumentation)
    {
        //NOTE: Putting the systemc.h include here to avoid potential collisiong with the user code
        wrapper_defs
            << "void __mcxx_instr_write(const unsigned int event, const unsigned long long int val, const unsigned int type)"
            << "{"
            << "#pragma HLS inline\n"
            << "#pragma HLS protocol fixed\n"
            << "#pragma HLS INTERFACE ap_hs port=" << STR_INSTR_PORT << "\n"
            << "  __mcxx_instrData_t tmp;"
            << "  tmp.range(63, 0) = val;"
            << "  tmp.range(95, 64) = event;"
            << "  tmp.range(103, 96) = type;"
            << "  tmp.bit(104) = 1;"
            << "  wait();"
            << "  " << STR_INSTR_PORT << ".write(tmp);"
            << "  wait();"
            << "}"

            << "nanos_err_t nanos_instrument_burst_begin(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_BURST_OPEN);"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_burst_end(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_BURST_CLOSE);"
            << "  return NANOS_OK;"
            << "}"

            << "nanos_err_t nanos_instrument_point_event(nanos_event_key_t event, nanos_event_value_t value)"
            << "{"
            << "#pragma HLS inline\n"
            << "  __mcxx_instr_write(event, value, MCXX_EVENT_TYPE_POINT);"
            << "  return NANOS_OK;"
            << "}";
    }
    else if (user_calls_set.count("nanos_instrument") > 0)
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
            << "unsigned long long int nanos_fpga_current_wd()"
            << "{"
            << " return " << STR_TASKID << ";"
            << "}"

            << "nanos_err_t nanos_fpga_wg_wait_completion(unsigned long long int uwg, unsigned char avoid_flush)"
            << "{"
            << "  if (" << STR_COMPONENTS_COUNT << " == 0) { return NANOS_OK; }"
            << "  unsigned long long int tmp = " << STR_ACCID << ";"
            << "  tmp = tmp << 48 /*ACC_ID info uses bits [48:55]*/;"
            << "  tmp = 0x8000000100000000 | tmp | " << STR_COMPONENTS_COUNT << ";"
            << "  __mcxx_write_out_port(tmp /*TASKWAIT_DATA_BLOCK*/, " << HWR_TASKWAIT_ID << ", 0 /*last*/);"
            << "  __mcxx_write_out_port(" << STR_TASKID << " /*data*/, " << HWR_TASKWAIT_ID << ", 1 /*last*/);"
            << "  {\n"
            << "#pragma HLS PROTOCOL fixed\n"
            << "    wait();"
            << "    __mcxx_read_ein_port();"
            << "    wait();"
            << "  }\n"
            << "  " << STR_COMPONENTS_COUNT << " = 0;"
            << "  return NANOS_OK;"
            << "}"

            << "void nanos_fpga_create_wd_async(const unsigned long long int type,"
            << "    const unsigned char numArgs, const unsigned long long int * args,"
            << "    const unsigned char numDeps, const unsigned long long int * deps, const unsigned char * depsFlags,"
            << "    const unsigned char numCopies, const nanos_fpga_copyinfo_t * copies)"
            << "{"
            << "#pragma HLS inline\n"
            << "  ap_uint<1> finalMode = 0;"
            << "  unsigned char currentNumDeps = numDeps;"
            << "  ap_uint<8> ack = " << ACK_REJECT_CODE << ";"
            << "  do {"
            << "    const unsigned short destId = currentNumDeps == 0 ? " << HWR_SCHED_ID << " : " << HWR_DEPS_ID << ";"
            //1st word: [ child_number (32b) | num_copies (8b) | num_deps (8b) | num_args (8b) | (8b) ]
            << "    unsigned long long int tmp = " << STR_COMPONENTS_COUNT << ";"
            << "    tmp = (tmp << 8) | numCopies;"
            << "    tmp = (tmp << 8) | currentNumDeps;"
            << "    tmp = (tmp << 8) | numArgs;"
            << "    tmp = tmp << 8;"
            << "    __mcxx_write_out_port(tmp, destId, 0);"
            //2nd word: [ parent_task_id (64b) ]
            << "    __mcxx_write_out_port(" << STR_TASKID << ", destId, 0);"
            //3rd word: [ type_value (64b) ]
            << "    __mcxx_write_out_port(type, destId, 0);"
            << "    for (unsigned char idx = 0; idx < currentNumDeps; ++idx) {"
            << "      tmp = depsFlags[idx];"
            << "      tmp = (tmp << 56) | deps[idx];"
            //dep words: [ arg_flags (8b) | arg_value (56b) ]
            //NOTE: Using numDeps here instead of currentNumDeps, which still correct, to allow compiler optimize the expression
            << "      __mcxx_write_out_port(tmp, destId, (idx == (numDeps - 1))&&(numArgs == 0)&&(numCopies == 0));"
            << "    }"
            //copy words
            << "    for (unsigned char idx = 0; idx < numCopies; ++idx) {"
            //1st copy word: [ address (64b) ]
            << "      tmp = copies[idx].address;"
            << "      __mcxx_write_out_port(tmp, destId, 0);"
            //2nd copy word: [ size (32b) | not_used (16b) | arg_idx (8b) | flags (8b) ]
            << "      tmp = copies[idx].size;"
            << "      tmp = (tmp << 24) | copies[idx].arg_idx;"
            << "      tmp = (tmp << 8) | copies[idx].flags;"
            << "      __mcxx_write_out_port(tmp, destId, 0);"
            //3rd copy word: [ accessed_length (32b) | offset (32b) ]
            << "      tmp = copies[idx].accessed_length;"
            << "      tmp = (tmp << 32) | copies[idx].offset;"
            << "      __mcxx_write_out_port(tmp, destId, (idx == (numCopies - 1))&&(numArgs == 0));"
            << "    }"
            << "    for (unsigned char idx = 0; idx < numArgs; ++idx) {"
            //arg words: [ arg_value (64b) ]
            << "      __mcxx_write_out_port(args[idx], destId, (idx == (numArgs - 1)));"
            << "    }"
            << "    {\n"
            << "#pragma HLS PROTOCOL fixed\n"
            << "      ack = __mcxx_read_ein_port();"
            << "      finalMode = (ack == " << ACK_FINAL_CODE << ");"
            << "      currentNumDeps = ack == " << ACK_FINAL_CODE << " ? 0 : numDeps;"
            << "    }\n"
            << "  } while (ack != " << ACK_OK_CODE << ");"
            << "  ++" << STR_COMPONENTS_COUNT << ";"
            //NOTE: Using numDeps in the if expression to let the compiler remove dead-code when task has no deps
            << "  if (numDeps > 0 && finalMode == 1) {"
            << "    nanos_fpga_wg_wait_completion(" << STR_TASKID << ", 0);"
            << "  }"
            << "}";
    }

    if (periodic_support)
    {
        wrapper_defs
            << "unsigned int nanos_get_periodic_task_repetition_num()"
            << "{"
            << "#pragma HLS inline\n"
            << "  return " << STR_REP_NUM <<";"
            << "}"
            << "void nanos_cancel_periodic_task()"
            << "{"
            << "#pragma HLS inline\n"
            << "  " << STR_NUM_REPS <<" = 0;"
            << "}";
    }
}

} // namespace Nanox
} // namespace TL

#endif // NANOX_FPGA_UTILS_HPP

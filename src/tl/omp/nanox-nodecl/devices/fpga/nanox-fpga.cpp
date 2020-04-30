/*--------------------------------------------------------------------
  (C) Copyright 2006-2020 Barcelona Supercomputing Center
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

#include <errno.h>
#include <stdlib.h>

#include "cxx-profile.h"
#include "tl-devices.hpp"
#include "tl-compilerpipeline.hpp"
#include "tl-multifile.hpp"
#include "tl-source.hpp"
#include "codegen-phase.hpp"
#include "codegen-cxx.hpp"
#include "cxx-cexpr.h"
#include "cxx-driver-utils.h"
#include "cxx-process.h"
#include "cxx-cexpr.h"

#include "nanox-fpga.hpp"
#include "nanox-fpga-utils.hpp"

#include "cxx-nodecl.h"

#include "tl-nanos.hpp"
#include "tl-counters.hpp"

#define STR_MEM_PORT_PREFIX "mcxx_"

using namespace TL;
using namespace TL::Nanox;

TL::Symbol DeviceFPGA::gen_fpga_unpacked(
        TL::Symbol &current_function,
        Nodecl::NodeclBase &outline_placeholder,
        const Nodecl::NodeclBase &num_repetitions_expr,
        const Nodecl::NodeclBase &period_expr,
        CreateOutlineInfo &info,
        Nodecl::Utils::SimpleSymbolMap* &symbol_map)
{
    const Nodecl::NodeclBase& original_statements = info._original_statements;
    TL::ObjectList<OutlineDataItem*> data_items = info._data_items;

    TL::Source dummy_init_statements, dummy_final_statements;
    TL::Symbol unpacked_function = new_function_symbol_unpacked(
        current_function,
        fpga_outline_name(info._outline_name) + "_unpacked",
        info,
        symbol_map,
        dummy_init_statements,
        dummy_final_statements
    );

    // The unpacked function must not be static and must have external linkage because
    // this function is called from the original source
    symbol_entity_specs_set_is_static(unpacked_function.get_internal_symbol(), 0);
    if (IS_C_LANGUAGE)
    {
        symbol_entity_specs_set_linkage_spec(unpacked_function.get_internal_symbol(), "\"C\"");
    }

    Nodecl::NodeclBase unpacked_function_code, unpacked_function_body;
    SymbolUtils::build_empty_body_for_function(
        unpacked_function,
        unpacked_function_code,
        unpacked_function_body
    );

    Nodecl::Utils::append_to_top_level_nodecl(unpacked_function_code);

    // Generate FPGA outline
    Source fpga_outline;

    if (Nanos::Version::interface_is_at_least("fpga", 9))
    {
        fpga_outline
            << "nanos_fpga_task_t nanos_fpga_task_handle;";

        if (!num_repetitions_expr.is_null() || !period_expr.is_null())
        {
            Source numreps_src, period_src;

            if (!num_repetitions_expr.is_null())
            {
                numreps_src << as_expression(num_repetitions_expr);
            }
            else
            {
                numreps_src << "0xFFFFFFFF"; //< largest number of 32b integer means spin forever
                warn_printf_at(period_expr.get_locus(),
                    "Clause 'num_repetitions' no provided, assuming infinite repetitions\n");
            }

            if (!period_expr.is_null())
            {
                period_src << as_expression(period_expr);
            }
            else
            {
                period_src << "0";
                    warn_printf_at(num_repetitions_expr.get_locus(),
                        "Clause 'period' no provided, assuming 0 (which is restart just after finishing)\n");
            }

            fpga_outline
                << "const unsigned int nanos_fpga_periodic_task_numreps = (unsigned int)(" << numreps_src << ");"
                << "const unsigned int nanos_fpga_periodic_task_period = (unsigned int)(" << period_src << ");"
                << "nanos_fpga_create_periodic_task(&nanos_fpga_task_handle, nanos_current_wd(),"
                <<   "nanos_fpga_periodic_task_period, nanos_fpga_periodic_task_numreps);";
                //NOTE: Not checking the value of err0 as nanox internally handles it to show proper errors
                //<< "if (err0 != NANOS_OK) nanos_handle_error(err0);"
        }
        else
        {
            fpga_outline
                << "nanos_fpga_create_task(&nanos_fpga_task_handle, nanos_current_wd());";
                //NOTE: Not checking the value of err0 as nanox internally handles it to show proper errors
                //<< "if (err0 != NANOS_OK) nanos_handle_error(err0);"
        }
    }

    unsigned param_pos = 0;
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++, param_pos++)
    {
        TL::Symbol outline_data_item_sym = (*it)->get_field_symbol();
        TL::Type field_type = (*it)->get_field_type();
        const std::string &field_name = outline_data_item_sym.get_name();
        const TL::ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();
        Scope scope = outline_data_item_sym.get_scope();
        std::stringstream cast_var_name_str;
        cast_var_name_str << "mcc_union_fpga_arg_cast_" << param_pos;
        const std::string cast_var_name = cast_var_name_str.str();

        // If the outline data item has not a valid symbol, skip it
        if (!outline_data_item_sym.is_valid() || field_type.get_size() > sizeof(uint64_t)) {
            fatal_error("Non-valid argument in FPGA task. Data type must be at most 64-bit wide\n");
        }

        bool in_type, out_type;
        in_type = copies.empty() ||
            ((copies.front().directionality & OutlineDataItem::COPY_IN) == OutlineDataItem::COPY_IN);
        out_type = copies.empty() ||
            ((copies.front().directionality & OutlineDataItem::COPY_OUT) == OutlineDataItem::COPY_OUT);

        // Create union in order to reinterpret argument as a uint64_t
        fpga_outline
            << "union {"
            <<     "uint64_t raw;"
            <<     as_type(field_type.get_unqualified_type().no_ref()) << " typed;"
            << "} " << cast_var_name << " = { 0 };";

        // If argument is pointer or array, get physical address (only in old fpga APIs)
        if ((field_type.is_pointer() || field_type.is_array()) && !Nanos::Version::interface_is_at_least("fpga", 3))
        {
            fpga_outline
                << cast_var_name << ".typed = (" << as_type(field_type.get_unqualified_type().no_ref()) << ")"
                << "nanos_fpga_get_phy_address((void *)" << field_name << ");";
        }
        else
        {
            fpga_outline
                << cast_var_name << ".typed = " << field_name << ";";
        }

        // Add argument to task structure
        if (Nanos::Version::interface_is_at_least("fpga", 9))
        {
            fpga_outline
                << "nanos_fpga_set_task_arg(nanos_fpga_task_handle, " << param_pos << ", " << in_type << ", "
                << out_type << ", " << cast_var_name << ".raw);";
        }
        else
        {
            fpga_outline
                << "nanos_fpga_set_task_arg(nanos_current_wd(), " << param_pos << ", " << in_type << ", "
                << out_type << ", " << cast_var_name << ".raw);";
        }
    }

    if (Nanos::Version::interface_is_at_least("fpga", 9))
    {
        fpga_outline
            << "nanos_fpga_submit_task(nanos_fpga_task_handle);";
            //NOTE: Not checking the value of err2 as nanox internally handles it to show proper errors
            //<< "if (err2 != NANOS_OK) nanos_handle_error(err2);"
    }

    Source unpacked_source;
    unpacked_source
        << dummy_init_statements
        << fpga_outline
        << statement_placeholder(outline_placeholder)
        << dummy_final_statements
    ;

    // Add a declaration of the unpacked function symbol in the original source
    if (IS_CXX_LANGUAGE && !unpacked_function.is_member())
    {
        Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDecl::make(
            /* optative context */ nodecl_null(),
            unpacked_function,
            original_statements.get_locus()
        );
        Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);
    }

    Nodecl::NodeclBase new_unpacked_body = unpacked_source.parse_statement(unpacked_function_body);
    unpacked_function_body.replace(new_unpacked_body);

    return unpacked_function;
}

void DeviceFPGA::create_outline(
        CreateOutlineInfo &info,
        Nodecl::NodeclBase &outline_placeholder,
        Nodecl::NodeclBase &output_statements,
        Nodecl::Utils::SimpleSymbolMap* &symbol_map)
{

    if (IS_FORTRAN_LANGUAGE)
        fatal_error("Fortran for FPGA devices is not supported yet\n");

    if (!Nanos::Version::interface_is_at_least("fpga", 2))
        fatal_error("Unsupported Nanos version (fpga support). Please update your Nanos installation\n");

    Lowering* lowering = info._lowering;
    const Nodecl::NodeclBase& original_statements = info._original_statements;
    const TL::Symbol& arguments_struct = info._arguments_struct;
    const TL::Symbol& called_task = info._called_task;
    TL::ObjectList<OutlineDataItem*> data_items = info._data_items;
    const Nodecl::NodeclBase& num_repetitions_expr = get_num_repetitions(info._target_info);
    const Nodecl::NodeclBase& period_expr = get_period(info._target_info);

    lowering->seen_fpga_task = true;

    symbol_map = new Nodecl::Utils::SimpleSymbolMap(&_global_copied_fpga_symbols);

    TL::Symbol current_function = original_statements.retrieve_context().get_related_symbol();
    if (current_function.is_nested_function())
    {
        if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
            fatal_printf_at(original_statements.get_locus(), "nested functions are not supported\n");
    }

    // Add the user function to the intermediate file -> to HLS
    if (called_task.is_valid()) //is a function task
    {
        if ( (IS_C_LANGUAGE || IS_CXX_LANGUAGE) && !called_task.get_function_code().is_null())
        {
            if (_global_copied_fpga_symbols.map(called_task) == called_task)
            {
                const std::string acc_type = get_acc_type(called_task, info._target_info);
                const bool periodic_support = !num_repetitions_expr.is_null() || !period_expr.is_null() || _force_periodic_support;
                const bool creates_children_tasks =
                    (info._num_inner_tasks > 0) ||
                    (_force_fpga_task_creation_ports.find(acc_type) != _force_fpga_task_creation_ports.end());
                FpgaOutlineInfo to_outline_info(
                    called_task.get_name(),
                    get_num_instances(info._target_info),
                    acc_type,
                    periodic_support);

                // Duplicate the called task into the fpga source
                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(
                    called_task, to_outline_info._name + "_moved");
                _global_copied_fpga_symbols.add_map(called_task, new_function);
                Nodecl::NodeclBase fun_code = Nodecl::Utils::deep_copy(
                    called_task.get_function_code(),
                    called_task.get_scope(),
                    *symbol_map);
                new_function.set_value(fun_code);
                symbol_entity_specs_set_is_static(new_function.get_internal_symbol(), 1);

                //print_ast_dot(fun_code, std::string("ast_") + to_outline_info._name + std::string("_moved.dot"));
                FpgaTaskCodeVisitor fpga_task_code_visitor(
                    _function_copy_suffix,
                    TL::CompilationProcess::get_current_file().get_filename(/* fullpath */ true),
                    symbol_map);
                fpga_task_code_visitor.walk(fun_code);

                if (creates_children_tasks)
                {
                    // If the task creates more tasks, replace the pointers by mcxx_ptr_t variables
                    ReplaceTaskCreatorSymbolsVisitor replace_ptr_decl_visitor(symbol_map);
                    replace_ptr_decl_visitor.walk(fun_code);
                }

                // Generate the wrapper interface for the called task
                DeviceFPGA::gen_hls_wrapper(
                    new_function,
                    info._data_items,
                    creates_children_tasks,
                    periodic_support,
                    fpga_task_code_visitor._user_calls_set,
                    to_outline_info.get_wrapper_name(),
                    to_outline_info._wrapper_decls,
                    to_outline_info._wrapper_code);

                // Set the user code for the current task: called source + task user code
                to_outline_info._user_code.append(fpga_task_code_visitor._called_functions);
                to_outline_info._user_code.append(fun_code);

                _outlines.append(to_outline_info);
            }
        }
        else
        {
            //The task code is not available at this point. It may be in a diferent TU
            //If there is not a call to the fpga task in the TU, the fpga task will not be emmited
            fatal_error("Calls to a FPGA task defined in a different TU not supported yet\n");
        }
    }

    Source unpacked_arguments;
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        switch ((*it)->get_sharing())
        {
            case OutlineDataItem::SHARING_PRIVATE:
                break;
            case OutlineDataItem::SHARING_SHARED:
            case OutlineDataItem::SHARING_CAPTURE:
            case OutlineDataItem::SHARING_CAPTURE_ADDRESS:
                {
                    TL::Type param_type = (*it)->get_in_outline_type();
                    Source argument;

                    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
                    {
                        // Normal shared items are passed by reference from a pointer,
                        // derreference here
                        if ((*it)->get_sharing() == OutlineDataItem::SHARING_SHARED &&
                            !(IS_CXX_LANGUAGE && (*it)->get_symbol().get_name() == "this"))
                        {
                            argument << "*(args." << (*it)->get_field_name() << ")";
                        }
                        else
                        {
                            // Any other thing is passed by value
                            argument << "args." << (*it)->get_field_name();
                        }

                        if (IS_CXX_LANGUAGE && (*it)->get_allocation_policy() == OutlineDataItem::ALLOCATION_POLICY_TASK_MUST_DESTROY)
                        {
                            internal_error("Not yet implemented: call the destructor", 0);
                        }
                    }
                    else
                    {
                        internal_error("running error", 0);
                    }

                    unpacked_arguments.append_with_separator(argument, ", ");
                    break;
                }
            case OutlineDataItem::SHARING_REDUCTION:
                {
                    WARNING_MESSAGE("Reductions are not tested for FPGA", "");
                    // Pass the original reduced variable as if it were a shared
                    Source argument;

                    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE)
                    {
                        argument << "*(args." << (*it)->get_field_name() << ")";
                    }
                    else
                    {
                        internal_error("running error", 0);
                    }

                    unpacked_arguments.append_with_separator(argument, ", ");
                    break;
                }
            default:
                {
                    std::cerr << "Warning: Cannot copy function code to the device file" << std::endl;
                }
        }
    }

    // Create the new unpacked function
    TL::Symbol unpacked_function = DeviceFPGA::gen_fpga_unpacked(
        current_function,
        outline_placeholder,
        num_repetitions_expr,
        period_expr,
        info,
        symbol_map
    );

    // Create the outline function
    //The outline function has always only one parameter which name is 'args'
    ObjectList<std::string> structure_name;
    structure_name.append("args");

    //The type of this parameter is an struct (i. e. user defined type)
    ObjectList<TL::Type> structure_type;
    structure_type.append(TL::Type(
        get_user_defined_type(
            arguments_struct.get_internal_symbol())).get_lvalue_reference_to());

    TL::Symbol outline_function = SymbolUtils::new_function_symbol(
        current_function,
        fpga_outline_name(info._outline_name),
        TL::Type::get_void_type(),
        structure_name,
        structure_type
    );

    Nodecl::NodeclBase outline_function_code, outline_function_body;
    SymbolUtils::build_empty_body_for_function(
        outline_function,
        outline_function_code,
        outline_function_body
    );

    Nodecl::Utils::append_to_top_level_nodecl(outline_function_code);

    Source outline_src;
    Source instrument_before, instrument_after;

    if (instrumentation_enabled())
    {
        get_instrumentation_code(
            info._called_task,
            outline_function,
            outline_function_body,
            info._task_label,
            info._instr_locus,
            instrument_before,
            instrument_after
        );
    }

    outline_src
        << "{"
        << instrument_before
        << unpacked_function.get_qualified_name_for_expression(
            /* in_dependent_context */
            (current_function.get_type().is_template_specialized_type()
            && current_function.get_type().is_dependent()))
        << "(" << unpacked_arguments << ");"
        << instrument_after
        << "}"
    ;

    Nodecl::NodeclBase new_outline_body = outline_src.parse_statement(outline_function_body);
    outline_function_body.replace(new_outline_body);

    output_statements = Nodecl::EmptyStatement::make(original_statements.get_locus());

    if (IS_CXX_LANGUAGE && !outline_function.is_member())
    {
        Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDecl::make(
            /* optative context */ nodecl_null(),
            outline_function,
            original_statements.get_locus()
        );

        Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);
    }
}

DeviceFPGA::DeviceFPGA() : DeviceProvider(std::string("fpga")), _bitstream_generation(false),
    _force_fpga_task_creation_ports(), _memory_port_width(""), _unaligned_memory_port(false),
    _force_periodic_support(false), _onto_warn_shown(false)
{
    set_phase_name("Nanox FPGA support");
    set_phase_description("This phase is used by Nanox phases to implement FPGA device support");
    register_parameter("board_name",
        "This is the parameter Board Name that appears in the Vivado Design",
        _board_name,
        "xilinx.com:zc702:part0:1.1");

    register_parameter("device_name",
        "This is the parameter the specific fpga device name of the board",
        _device_name,
        "xc7z020clg484-1");

    register_parameter("FPGA accelerators frequency",
        "This is the parameter to indicate the FPGA accelerators frequency",
        _frequency,
        "10");

    register_parameter("bitstream_generation",
        "Enables/disables the bitstream generation of FPGA accelerators",
        _bitstream_generation_str,
        "0").connect(std::bind(&DeviceFPGA::set_bitstream_generation_from_str, this, std::placeholders::_1));

    register_parameter("vivado_design_path",
        "This is the parameter to indicate where the automatically generated vivado design will be placed",
        _vivado_design_path,
        "$PWD");

    register_parameter("vivado_project_name",
        "This is the parameter to indicate the vivado project name",
        _vivado_project_name,
        "Filename");

    register_parameter("ip_cache_path",
        "This is the parameter to indicate where the cache of ips is placed",
        _ip_cache_path,
        "$PWD/IP_cache");

    register_parameter("dataflow",
        "This is the parameter to indicate where the user wants to use the dataflow optimization: ON/OFF",
        _dataflow,
        "OFF");

    register_parameter("force_fpga_task_creation_ports",
        "This is the parameter to force the use of extra task creation ports in a set of fpga accelerators: <onto value>[,<onto value>][...]",
        _force_fpga_task_creation_ports_str,
        "0").connect(std::bind(&DeviceFPGA::set_force_fpga_task_creation_ports_from_str, this, std::placeholders::_1));

    register_parameter("fpga_memory_port_width",
        "Defines the width (in bits) of memory ports (only for wrapper localmem data) for fpga accelerators",
        _memory_port_width,
        "").connect(std::bind(&DeviceFPGA::set_memory_port_width_from_str, this, std::placeholders::_1));

    register_parameter("fpga_unaligned_memory_port",
        "Enables the logic to support unaligned memory regions handled by the memory port",
        _unaligned_memory_port_str,
        "0").connect(std::bind(&DeviceFPGA::set_unaligned_memory_port_from_str, this, std::placeholders::_1));

    register_parameter("force_fpga_periodic_support",
        "Force enabling the periodic tasks support in FPGA accelerators",
        _force_periodic_support_str,
        "0").connect(std::bind(&DeviceFPGA::set_force_periodic_support_from_str, this, std::placeholders::_1));

    register_parameter("fpga_function_copy_suffix",
        "Forces the suffix placed at the end of functions moved to fpga HLS source",
        _function_copy_suffix,
        "").connect(std::bind(&DeviceFPGA::set_funcion_copy_suffix_from_str, this, std::placeholders::_1));
}

void DeviceFPGA::pre_run(DTO& dto) {
    _root = *std::static_pointer_cast<Nodecl::NodeclBase>(dto["nodecl"]);
}

void DeviceFPGA::run(DTO& dto) {
    DeviceProvider::run(dto);

    if (_bitstream_generation)
    {
        std::cerr << "FPGA bitstream generation phase analysis - ON" << std::endl;
        //std::cerr << "================================================================" << std::endl;
        //std::cerr << "\t Board Name                  : " << _board_name << std::endl;
        //std::cerr << "\t Device Name                 : " << _device_name << std::endl;
        //std::cerr << "\t Frequency                   : " << _frequency << "ns" << std::endl;
        //std::cerr << "\t Bitstream generation active : " << _bitstream_generation << std::endl;
        //std::cerr << "\t Vivado design path          : " << _vivado_design_path << std::endl;
        //std::cerr << "\t Vivado project name         : " << _vivado_project_name << std::endl;
        //std::cerr << "\t IP cache path               : " << _ip_cache_path << std::endl;
        //std::cerr << "\t Dataflow active             : " << _dataflow << std::endl;
        //std::cerr << "================================================================" << std::endl;
    }

}

void DeviceFPGA::get_device_descriptor(DeviceDescriptorInfo& info,
        Source &ancillary_device_description,
        Source &device_descriptor,
        Source &fortran_dynamic_init)
{

    const std::string& outline_name = fpga_outline_name(info._outline_name);
    const std::string& arguments_struct = info._arguments_struct;
    TL::Symbol current_function = info._current_function;

    //FIXME: This is confusing. In a future, we should get the template
    //arguments of the outline function and print them

    //Save the original name of the current function
    std::string original_name = current_function.get_name();

    current_function.set_name(outline_name);
    Nodecl::NodeclBase code = current_function.get_function_code();

    Nodecl::Context context = (code.is<Nodecl::TemplateFunctionCode>())
        ? code.as<Nodecl::TemplateFunctionCode>().get_statements().as<Nodecl::Context>()
        : code.as<Nodecl::FunctionCode>().get_statements().as<Nodecl::Context>();

    bool without_template_args =
        !current_function.get_type().is_template_specialized_type()
        || current_function.get_scope().get_template_parameters()->is_explicit_specialization;

    TL::Scope function_scope = context.retrieve_context();
    std::string qualified_name = current_function.get_qualified_name(function_scope, without_template_args);

    // Restore the original name of the current function
    current_function.set_name(original_name);

    // Generate a unique identifier for the accelerator type
    std::string acc_type = get_acc_type(info._called_task, info._target_info);

    if (!IS_FORTRAN_LANGUAGE)
    {
        // Extra cast for solving some issues of GCC 4.6.* and lowers (this
        // issues seem to be fixed in GCC 4.7 =D)
        std::string ref = IS_CXX_LANGUAGE ? "&" : "*";
        std::string extra_cast = "(void(*)(" + arguments_struct + ref + "))";

        Source args_name;
        args_name << outline_name << "_args";

        ancillary_device_description
            << comment("device argument type")
            << "static nanos_fpga_args_t " << args_name << ";"
            << args_name << ".outline = (void(*)(void*))" << extra_cast << "&" << qualified_name << ";"
        ;

        if (Nanos::Version::interface_is_at_least("fpga", 4))
        {
            ancillary_device_description
                << args_name << ".type = " << acc_type << "U;"
            ;
        }
        else
        {
            ancillary_device_description
                << args_name << ".acc_num = " << acc_type << "U;"
            ;
        }

        device_descriptor
            << "{"
            << /* factory */ "&nanos_fpga_factory, &" << args_name
            << "}"
        ;

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Casting: qualified_name " << qualified_name << std::endl;
        std::cerr << "Casting: device_descriptor" << device_descriptor.get_source() << std::endl;
#endif
    }
    else
    {
        internal_error("Fortran is not supperted in fpga devices", 0);
    }

}

bool DeviceFPGA::remove_function_task_from_original_source() const
{
    return true;
}

//write/close intermediate files, free temporal nodes, etc.
void DeviceFPGA::phase_cleanup(DTO& data_flow)
{
    if (!_outlines.empty() && _bitstream_generation)
    {
        compilation_configuration_t* configuration = ::get_compilation_configuration("auxcc");
        ERROR_CONDITION(configuration == NULL, "auxcc profile is mandatory when using fpgacc/fpgacxx", 0);
        configuration->source_language = SOURCE_LANGUAGE_CXX;

        // Make sure phases are loaded (this is needed for codegen)
        load_compiler_phases(configuration);

        Codegen::CodegenPhase* codegen_phase = reinterpret_cast<Codegen::CodegenPhase*>(configuration->codegen_phase);

        TL::ObjectList<struct FpgaOutlineInfo>::iterator it;
        for (it = _outlines.begin();
            it != _outlines.end();
            it++)
        {
            std::string new_filename = it->get_filename();
            FILE* ancillary_file;

            // Check if file exist
            ancillary_file = fopen(new_filename.c_str(), "r");
            if (ancillary_file != NULL)
            {
                fclose(ancillary_file);
                fatal_error("ERROR: Trying to create '%s' which already exists.\n%s\n%s",
                    new_filename.c_str(),
                    "       If you have two FPGA tasks with the same function name, you should use the onto(type) clause in the target directive.",
                    "       Otherwise, you must clean the running directory before linking.");
            }

            ancillary_file = fopen(new_filename.c_str(), "w+");
            if (ancillary_file == NULL)
            {
                fatal_error("%s: error: cannot open file %s\n",
                    new_filename.c_str(),
                    strerror(errno)
                );
            }
            else if (!CURRENT_CONFIGURATION->do_not_link)
            {
                // If linking is enabled, remove the intermediate HLS source file (like an object file)
                ::mark_file_for_cleanup(new_filename.c_str());
            }

            add_fpga_header(
                ancillary_file,
                instrumentation_enabled(),
                it->_periodic_support,
                it->get_wrapper_name(),
                it->_type,
                it->_num_instances);
            add_included_fpga_files(ancillary_file);

            fprintf(ancillary_file, "%s\n", it->_wrapper_decls.get_source(true).c_str());

//            if (IS_C_LANGUAGE)
//                Source::source_language = SourceLanguage::CPlusPlus;

            if (!_stuff_to_copy.is_null())
            {
                codegen_phase->codegen_top_level(_stuff_to_copy, ancillary_file, new_filename);
            }

            //hls_file << it->_source_code.get_source(true);
            codegen_phase->codegen_top_level(it->_user_code, ancillary_file, new_filename);

//            if (IS_C_LANGUAGE)
//                Source::source_language = SourceLanguage::Current;

            fprintf(ancillary_file, "%s", it->_wrapper_code.get_source(true).c_str());
            add_fpga_footer(ancillary_file);

            fclose(ancillary_file);
        }

        // Do not forget the clear the outlines list
        _outlines.clear();
    }
    _stuff_to_copy = Nodecl::List();

    if (!_nanos_post_init_actions.empty())
    {
        Source nanos_post_init, actions_list;
        if (IS_CXX_LANGUAGE)
        {
            nanos_post_init << "extern \"C\""
                << "{"
                ;
        }
        else if (IS_FORTRAN_LANGUAGE)
        {
            fatal_error("Fortran support not implemeted yet");
        }

        for (TL::ObjectList<struct FpgaNanosPostInitInfo>::iterator it = _nanos_post_init_actions.begin();
            it != _nanos_post_init_actions.end();
            it++)
        {
            std::string action_info = "{" + it->_function + "," + it->_argument + "}";
            actions_list.append_with_separator(action_info, ", ");
        }

        _nanos_post_init_actions.clear();

        //NOTE: This will not work with multiple Translarions Units
        //FIXME: Use a section to support several TUs
        nanos_post_init
            << "nanos_init_desc_t __nanos_post_init[] __attribute__((weak)) = { " << actions_list << " };"
            << "nanos_init_desc_t * __nanos_post_init_begin __attribute__((weak)) = __nanos_post_init;"
            << "nanos_init_desc_t * __nanos_post_init_end __attribute__((weak)) = __nanos_post_init + sizeof(__nanos_post_init) / sizeof(*__nanos_post_init);"
            ;

        if (IS_CXX_LANGUAGE)
        {
            nanos_post_init << "}";
        }

        Nodecl::NodeclBase nanos_post_init_tree = nanos_post_init.parse_global(_root);
        Nodecl::Utils::append_to_top_level_nodecl(nanos_post_init_tree);
    }

    _onto_warn_shown = false;
}

void DeviceFPGA::add_included_fpga_files(FILE* file)
{
    ObjectList<IncludeLine> lines = CurrentFile::get_included_files();
    std::string fpga_header_exts[] = {".fpga\"", ".fpga.h\"", ".fpga.hpp\""};

    for (TL::ObjectList<IncludeLine>::iterator it = lines.begin();
        it != lines.end();
        it++)
    {
        std::string line = (*it).get_preprocessor_line();
        for (size_t idx = 0; idx < sizeof(fpga_header_exts)/sizeof(std::string); ++idx)
        {
            if (line.rfind(fpga_header_exts[idx]) != std::string::npos)
            {
                int output = fprintf(file, "%s\n", line.c_str());
                if (output < 0)
                    internal_error("Error trying to write the intermediate fpga file\n", 0);
                break;
            }
        }
    }
}

static TL::Type get_element_type_pointer_to(TL::Type type)
{
    TL::Type points_to = type;

    while(points_to.is_pointer() || points_to.is_pointer_to_class() || points_to.is_array())
    {
        if (points_to.is_pointer())
        {
            points_to = points_to.points_to();
        }
        else if (points_to.is_pointer_to_class())
        {
            points_to = points_to.pointed_class();
        }
        else if (points_to.is_array())
        {
            points_to = points_to.array_element();
        }
    }

    return points_to;
}

static Source get_copy_elements_all_dimensions_src(TL::Type copy_type)
{
    Source ArrayExpression;
    std::string  dimension_str;

    if (copy_type.is_pointer())
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        fprintf(stderr, "Pointer declaration is array, num dimensions: 1\n");
#endif
        dimension_str = "( 1 )";
        ArrayExpression << dimension_str;
        return ArrayExpression;
    }
    else if (copy_type.is_array()) //it's a shape
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        fprintf(stderr, "Type declaration is array, num dimensions %d\n" , copy_type.get_num_dimensions());
#endif
        int total_dimensions = copy_type.get_num_dimensions();
        int n_dimensions=0;
        Nodecl::NodeclBase array_get_expr;
        array_get_expr = copy_type.array_get_size();
        dimension_str = "(" + array_get_expr.prettyprint() + ")";
        ArrayExpression << dimension_str;
        n_dimensions++;
        while (n_dimensions<total_dimensions)
        {
            copy_type  = copy_type.array_element();
            array_get_expr = copy_type.array_get_size();
            dimension_str = "(" + array_get_expr.prettyprint() + ")";
            ArrayExpression << "*" << dimension_str;
            n_dimensions++;
        }
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Expression:" << ArrayExpression.get_source() << std::endl;
        std::cerr << std::endl << std::endl;
#endif
        return ArrayExpression;
    }
    else if (copy_type.array_is_region())
    {
        Nodecl::NodeclBase cp_size = copy_type.array_get_region_size();
        dimension_str = "(" + cp_size.prettyprint() + ")";
        ArrayExpression <<  dimension_str;
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Region Expression:" << ArrayExpression.get_source() << std::endl;
        std::cerr << std::endl << std::endl;
#endif
        return ArrayExpression;

    }
#if _DEBUG_AUTOMATIC_COMPILER_
    internal_error("ERROR! in get_copy_elements_all_dimensions_src\n");
#endif

    return ArrayExpression;
}

static int find_parameter_position(const ObjectList<Symbol> param_list, const Symbol &param_symbol)
{
    const std::string field_name_ref = param_symbol.get_name();
    int position = 0;
    for (ObjectList<Symbol>::const_iterator it = param_list.begin(); it != param_list.end();
        it++)
    {
        const std::string field_name = it->get_name();
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Symbol name parameter function: " << field_name << " ref_name : "<< field_name_ref << std::endl;
#endif
        if (field_name == field_name_ref)
        {
            return position;
        }
        position++;
    }

    return -1;
}

/*
 * Create wrapper function for HLS to unpack streamed arguments
 *
 */
void DeviceFPGA::gen_hls_wrapper(const Symbol &func_symbol, ObjectList<OutlineDataItem*>& data_items,
    const bool creates_children_tasks, const bool periodic_support, const std::set<std::string> user_calls_set,
    const std::string wrapper_func_name, Source &wrapper_decls, Source &wrapper_source)
{
    //Check that we are calling a function task (this checking may be performed earlyer in the code)
    if (!func_symbol.is_function()) {
        fatal_error("Only function-tasks are supported at this moment");
    }

    Scope func_scope = func_symbol.get_scope();
    ObjectList<Symbol> param_list = func_symbol.get_function_parameters();
    char function_parameters_passed[param_list.size()];
    Source pragmas_src, params_src, clear_components_count, user_function_args,
        local_decls_src, reset_src, handle_task_execution_cmd_src, init_hw_instr_cmd_src,
        wrapper_decls_after_user_code, condition_task_execution_cmd_src;
    Source periodic_command_read, periodic_command_pre, periodic_command_post;
    Source in_copies, out_copies, in_copies_switch_body, out_copies_switch_body;
    Source profiling_0, profiling_1, profiling_2, profiling_3;
    unsigned int n_params_id = 0;
    unsigned int n_params_out = 0;
    unsigned int wrapper_memport_width;
    const std::string wrapper_memport_width_str = (_memory_port_width == "" && creates_children_tasks) ?
            "64" : _memory_port_width;

    memset(function_parameters_passed, 0, param_list.size());

    params_src
        << "axiStream_t& " << STR_INPUTSTREAM
        << ", axiStream_t& " << STR_OUTPUTSTREAM;

    pragmas_src
        << "#pragma HLS INTERFACE ap_ctrl_none port=return\n"
        << "#pragma HLS INTERFACE axis port=" << STR_INPUTSTREAM << "\n"
        << "#pragma HLS INTERFACE axis port=" << STR_OUTPUTSTREAM << "\n";

    local_decls_src
        << "unsigned long long int __commandArgs, __bufferData;"
        << "unsigned char __commandCode;"
        << "static ap_uint<1> __reset = 0;"
        << "#pragma HLS RESET variable=__reset\n";

    handle_task_execution_cmd_src
        << "  unsigned char __i, __param_id;"
        << "  ap_uint<8> __copyFlags;";

    if (instrumentation_enabled())
    {
        profiling_0 //copy in begin
            << "  nanos_instrument_burst_begin(" << EV_DEVCOPYIN << ", " << STR_TASKID << ");";

        profiling_1 //copy in end, task exec begin
            << "  nanos_instrument_burst_end(" << EV_DEVCOPYIN << ", " << STR_TASKID << ");"
            << "  nanos_instrument_burst_begin(" << EV_DEVEXEC << ", " << STR_TASKID << ");";

        profiling_2 //task exec end, copy out end
            << "  nanos_instrument_burst_end(" << EV_DEVEXEC << ", " << STR_TASKID << ");"
            << "  nanos_instrument_burst_begin(" << EV_DEVCOPYOUT << ", " << STR_TASKID << ");";

        profiling_3 //copy out end
            << "  nanos_instrument_burst_end(" << EV_DEVCOPYOUT << ", " << STR_TASKID << ");";

        init_hw_instr_cmd_src
            << "else if (__commandCode == 2) {"
            << "  __mcxx_instrData_t tmpSetup;"
            << "  tmpSetup.range(63,0) = " << STR_INPUTSTREAM << ".read().data;"
            << "  tmpSetup.range(79,64) = __commandArgs&0xFFFFFF;"
            << "  tmpSetup.bit(104) = 0;"
            << "  " << STR_INSTR_PORT << ".write(tmpSetup);"
            << "}";
    }

    if (creates_children_tasks)
    {
        clear_components_count
            << "  " << STR_COMPONENTS_COUNT << " = 0;";

        reset_src
            << "  " << STR_COMPONENTS_COUNT << " = 0;";
    }

    //Create the memory access port shared between all task parameters
    if (wrapper_memport_width_str != "")
    {
        std::istringstream iss(wrapper_memport_width_str);
        iss >> wrapper_memport_width;
        if (iss.fail())
        {
            error_printf_at(func_symbol.get_locus(),
                "FPGA memory port width '%s' is not an unsigned integer\n",
                wrapper_memport_width_str.c_str());
            fatal_error("Unsupported value");
        }
        else if (wrapper_memport_width == 0 || wrapper_memport_width%8 != 0)
        {
            error_printf_at(func_symbol.get_locus(),
                "FPGA memory port width '%s' is not >0 and/or multiple of 8\n",
                wrapper_memport_width_str.c_str());
            fatal_error("Unsupported value");
        }

        if (!creates_children_tasks)
        {
            // NOTE: If the task creates children tasks, the port will de created in the global scope
            const std::string port_declaration =
                "ap_uint<" + wrapper_memport_width_str + "> * " + STR_WRAPPERDATA;
            params_src.append_with_separator(port_declaration, ", ");

            pragmas_src
                << "#pragma HLS INTERFACE m_axi port=" << STR_WRAPPERDATA << "\n";
        }

        handle_task_execution_cmd_src
            << "  unsigned int __j, __k, __o;";
    }

    /*
     * Generate wrapper code
     * We are going to keep original parameter name for the original function
     *
     * input/output parameters are received concatenated one after another.
     * The wrapper must create local variables for each input/output and unpack
     * streamed input/output data into that local variables.
     *
     * Scalar parameters are going to be copied as long as no unpacking is needed
     */

    // Go through all the data items
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin(); it != data_items.end(); it++)
    {
        TL::Symbol param_symbol = (*it)->get_field_symbol();
        const std::string &field_name = (*it)->get_field_name();
        const Scope &scope = (*it)->get_symbol().get_scope();
        const ObjectList<Nodecl::NodeclBase> &localmem = (*it)->get_localmem();

        user_function_args.append_with_separator(field_name, ", ");

        if (!localmem.empty() && !creates_children_tasks)
        {
            if (localmem.size() > 1)
            {
                warn_printf_at(localmem.front().get_locus(),
                    "Multiple implicit/explicit definitions of localmem for argument '%s'. Using first one\n", field_name.c_str());
            }

            TL::Type localmem_type = localmem.front().get_type().no_ref();
            if (localmem_type.is_array() && localmem_type.array_is_vla())
            {
                //NOTE: VLAs cannot be cached in the wrapper as we don't know the array size yet
                warn_printf_at(localmem.front().get_locus(),
                    "Disabling localmem of FPGA accelerator for argument '%s' (size not know at compile time)\n", field_name.c_str());
                continue;
            }
            else if (!localmem_type.is_array())
            {
                internal_error("%d: Unexpected type in localmem info. '%d' is not an array",
                    localmem.front().get_locus_str().c_str(), localmem_type.print_declarator().c_str());
            }

            int param_id = find_parameter_position(param_list, param_symbol);
            function_parameters_passed[param_id] = 1;
            n_params_id++;

            Source n_elements_src;
            n_elements_src = get_copy_elements_all_dimensions_src(localmem_type);

            TL::Type elem_type = get_element_type_pointer_to(localmem_type);
            TL::Type unql_type = elem_type.get_unqualified_type();
            const std::string casting_const_pointer = unql_type.get_const_type().get_pointer_to().get_declaration(scope, "");
            const std::string casting_sizeof = elem_type.get_declaration(scope, "");

            local_decls_src
                << " static " << localmem_type.get_unqualified_type().get_declaration(scope, field_name) << ";";

            in_copies_switch_body
                << "    case " << param_id << ":\n";

            //NOTE: If the elements are const qualified they cannot be an output
            if (!elem_type.is_const())
            {
                in_copies_switch_body
                    << "      __paramInfo_out[" << n_params_out << "] = __paramInfo[" << param_id << "];";

                out_copies_switch_body
                    << "    case " << param_id << ":\n"
                    << "      if(__copyFlags[5])";

                n_params_out++;
            }

            in_copies_switch_body
                << "      if(__copyFlags[4])";

            //FIXME: Add suport for arrays when the memory_port_width is defined
            if (wrapper_memport_width_str != "" && !localmem_type.array_element().is_array())
            {
                //NOTE: The following check may not be vaild when cross-compiling
                if (wrapper_memport_width%(elem_type.get_size()*8) != 0)
                {
                    error_printf_at(param_symbol.get_locus(),
                        "Memory port width '%s' is not multiple of parameter '%s' width\n",
                        wrapper_memport_width_str.c_str(), field_name.c_str());
                    fatal_error("Unsupported value");
                }

                const std::string port_name = STR_WRAPPERDATA;
                const std::string mem_ptr_type = "ap_uint<" + wrapper_memport_width_str + ">";
                const std::string n_elems_read = "(sizeof(" + mem_ptr_type + ")/sizeof(" + casting_sizeof + "))";
                const std::string unaligned_offset = _unaligned_memory_port ? "-__o" : "";
                const std::string unaligned_extra = _unaligned_memory_port ? "+__o" : "";

                in_copies_switch_body << "{";

                if (_unaligned_memory_port)
                {
                    in_copies_switch_body << "      __o = __param[" << param_id << "]%sizeof(" << mem_ptr_type << ")/sizeof(" << casting_sizeof << ");";
                }

                in_copies_switch_body
                    << "      for (__j=0;"
                    << "       __j<(((" << n_elements_src << unaligned_extra << ")*sizeof(" << casting_sizeof << ") - 1)/sizeof(" << mem_ptr_type << ")+1);"
                    << "       __j++) {"
                    << "        " <<  mem_ptr_type << " __tmpBuffer;"
                    << "        __tmpBuffer = *(" << port_name << " + __param[" << param_id << "]/sizeof(" << mem_ptr_type << ") + __j);"
                    << "        #pragma HLS PIPELINE\n"
                    << "        #pragma HLS UNROLL region\n"
                    << "        for (__k=0;"
                    << "         __k<(" << n_elems_read << ");"
                    << "         __k++) {"
                    << "          if (" << (_unaligned_memory_port ? "((__j==0) && (__k<__o)) || " : "")
                    <<              "((__j*" << n_elems_read << "+__k) >= (" << n_elements_src << unaligned_extra << "))) continue;"
                    << "          union {"
                    << "            unsigned long long int raw;"
                    << "            " << casting_sizeof << " typed;"
                    << "          } cast_tmp;"
                    << "          cast_tmp.raw = __tmpBuffer.range("
                    <<            "(__k+1)*sizeof(" << casting_sizeof << ")*8-1,"
                    <<            "__k*sizeof(" << casting_sizeof << ")*8);"
                    << "          #pragma HLS DEPENDENCE variable=" << field_name << " inter false\n"
                    << "          " << field_name << "[__j*" << n_elems_read << "+__k" << unaligned_offset << "] = cast_tmp.typed;"
                    << "        }"
                    << "      }"
                    << "}";

                //NOTE: If the elements are const qualified they cannot be an output
                if (!elem_type.is_const())
                {
                    out_copies_switch_body << "{";

                    if (_unaligned_memory_port)
                    {
                        out_copies_switch_body << "      __o = __param[" << param_id << "]%sizeof(" << mem_ptr_type << ")/sizeof(" << casting_sizeof << ");";
                    }

                    out_copies_switch_body
                        << "      for (__j=0;"
                        << "       __j<(((" << n_elements_src << unaligned_extra << ")*sizeof(" << casting_sizeof << ")-1)/sizeof(" << mem_ptr_type << ")+1);"
                        << "       __j++) {"
                        << "        " << mem_ptr_type << " __tmpBuffer;"
                        << "        #pragma HLS PIPELINE\n"
                        << "        #pragma HLS UNROLL region\n"
                        << "        for (__k=0;"
                        << "         __k<(" << n_elems_read << ");"
                        << "         __k++) {"
                        << "          if (" << (_unaligned_memory_port ? "((__j==0) && (__k<__o)) || " : "")
                        <<              "((__j*" << n_elems_read << "+__k) >= (" << n_elements_src << unaligned_extra << "))) continue;"
                        << "          union {"
                        << "            unsigned long long int raw;"
                        << "            " << casting_sizeof << " typed;"
                        << "          } cast_tmp;"
                        << "          cast_tmp.typed = " << field_name << "[__j*" << n_elems_read << "+__k" << unaligned_offset << "];"
                        << "          __tmpBuffer.range("
                        <<            "(__k+1)*sizeof(" << casting_sizeof << ")*8-1,"
                        <<            "__k*sizeof(" << casting_sizeof << ")*8) = cast_tmp.raw;"
                        << "        }";

                    if (_unaligned_memory_port)
                    {
                        out_copies_switch_body
                            << "        const int rem = " << n_elements_src << unaligned_extra << "-__j*" << n_elems_read << ";"
                            << "        const unsigned int bit_f = (__j == 0) ? __o*sizeof(" << casting_sizeof << ")*8 : 0;";
                    }
                    else
                    {
                        out_copies_switch_body
                            << "        const int rem = " << n_elements_src << "-(__j*" << n_elems_read << ");"
                            << "        const unsigned int bit_f = 0;";
                    }

                    out_copies_switch_body
                        << "        const unsigned int bit_l = rem >= " << n_elems_read << " ? "
                        <<            "(sizeof(" << mem_ptr_type << ")*8-1) : (rem*sizeof(" << casting_sizeof << ")*8-1);"
                        << "        " << port_name << "[__param[" << param_id << "]/sizeof(" << mem_ptr_type << ") + __j].range("
                        <<            "bit_l, bit_f) = __tmpBuffer.range(sizeof(" << mem_ptr_type << ")*8-1, bit_f);"
                        << "      }"
                        << "}";
                }
            }
            else
            {
                if (wrapper_memport_width_str != "" && localmem_type.array_element().is_array())
                {
                    warn_printf_at(localmem.front().get_locus(),
                        "Disabling shared memory port for argument '%s' (array of arrays)\n", field_name.c_str());
                }

                const std::string port_name = STR_MEM_PORT_PREFIX + field_name;
                const std::string port_declaration =
                    elem_type.get_pointer_to().get_declaration(scope, port_name, TL::Type::PARAMETER_DECLARATION);
                params_src.append_with_separator(port_declaration, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << port_name << "\n";

                in_copies_switch_body
                    << "      memcpy(" << field_name << ", "
                    <<        "(" << casting_const_pointer << ")("
                    <<         port_name << " + __param[" << param_id << "]/sizeof(" << casting_sizeof << ")), "
                    <<         n_elements_src << "*sizeof(" << casting_sizeof << "));";

                if (!elem_type.is_const())
                {
                    out_copies_switch_body
                        << "      memcpy(" << port_name <<  " + __param[" << param_id << "]/sizeof(" << casting_sizeof << "), "
                        <<        "(" << casting_const_pointer << ")" << field_name << ", "
                        <<         n_elements_src << "*sizeof(" << casting_sizeof << "));";
                }
            }

            in_copies_switch_body
                << "      break;";

            //NOTE: If the elements are const qualified they cannot be an output
            if (!elem_type.is_const())
            {
                out_copies_switch_body
                    << "      break;";
            }
        }
        else
        {
            //NOTE: It will be handled in the below loop
        }
    }

    int param_id = 0;
    for (ObjectList<Symbol>::const_iterator it = param_list.begin(); it != param_list.end(); it++, param_id++)
    {
        //Ignore already processed parameters
        if (function_parameters_passed[param_id]) continue;

        const Scope &scope = it->get_scope();
        const std::string &param_name = it->get_name();
        TL::Type param_type = it->get_type().no_ref();
        TL::Type unql_type = param_type.get_unqualified_type();
        const std::string param_decl = unql_type.get_declaration(scope, param_name);
        const bool is_mcxx_ptr_t = param_decl.find("mcxx_ptr_t") != std::string::npos;

        local_decls_src
            << param_decl << ";";

        if (param_type.is_pointer() || param_type.is_array())
        {
            TL::Type elem_type = param_type.is_pointer() ? param_type.points_to() : param_type.array_element();
            if (elem_type.is_array())
            {
                //NOTE: Vivado HLS is not properly handling pointers to multi-dimensional arrays
                error_printf_at(it->get_locus(),
                    "Multi-dimensional arrays are only supported when cached in the wrapper."
                    " Change the parameter type or enable the localmem for '%s' parameter\n",
                    param_name.c_str());
                fatal_error("Unsupported fpga task parameter");
            }

            const std::string casting_pointer = unql_type.get_declaration(scope, "");
            const std::string casting_sizeof = elem_type.get_declaration(scope, "");
            const std::string port_name = STR_MEM_PORT_PREFIX + param_name;
            const std::string port_declaration = unql_type.get_declaration(scope, port_name, TL::Type::PARAMETER_DECLARATION);

            params_src.append_with_separator(port_declaration, ", ");

            pragmas_src
                << "#pragma HLS INTERFACE m_axi port=" << port_name << "\n";

            in_copies_switch_body
                << "    case " << param_id << ":\n"
                << "      " << param_name << " = (" << casting_pointer << ")"
                <<        "(" << port_name << " + __param[" << param_id << "]/sizeof(" << casting_sizeof << "));"
                << "      break;";
        }
        else if (param_type.is_scalar_type())
        {
            in_copies_switch_body
                << "    case " << param_id << ":\n"
                << "      union {"
                << "        " << unql_type.get_declaration(scope, param_name) << ";"
                << "        unsigned long long int "<< param_name << "_task_arg;"
                << "      } mcc_arg_" << param_id << ";"
                << "      mcc_arg_" << param_id << "." << param_name << "_task_arg = __param[" << param_id << "];"
                << "      " << param_name << " = mcc_arg_" << param_id << "." << param_name << ";"
                << "      break;";
        }
        else if (creates_children_tasks && is_mcxx_ptr_t)
        {
            in_copies_switch_body
                << "    case " << param_id << ":\n"
                << "      " << param_name << " = __param[" << param_id << "];"
                << "      break;";
        }
        else
        {
            error_printf_at(it->get_locus(),
                "Unsupported data type '%s' in fpga task parameter '%s'\n",
                print_declarator(param_type.get_internal_type()), param_name.c_str());
            fatal_error("Unsupported fpga task parameter");
        }

        //function_parameters_passed[param_id] = 1;
        n_params_id++;
    }

    if (n_params_id)
    {
        handle_task_execution_cmd_src
            << "  unsigned long long int __param[" << n_params_id << "], __paramInfo[" << n_params_id << "];";

        in_copies
            << "  for (__i = 0; __i < " << n_params_id << "; __i++) {"
            << "    __paramInfo[__i] = " << STR_INPUTSTREAM << ".read().data;"
            << "    __param[__i] = " << STR_INPUTSTREAM << ".read().data;"
            << "  }"
            << ""
            << "  for (__i = 0; __i < " << n_params_id << "; __i++) {"
            << "    __copyFlags = __paramInfo[__i];"
            << "    __param_id = __paramInfo[__i] >> 32;"
            << "    switch (__param_id) {"
            <<      in_copies_switch_body
            << "    default:;"
            << "    }"
            << "  }";
    }

    if (n_params_out)
    {
        handle_task_execution_cmd_src
            << "  unsigned long long int __paramInfo_out[" << n_params_out << "];"
        ;

        out_copies
            << "  for (__i = 0; __i < (" << n_params_out << "); __i++) {"
            << "    __copyFlags = __paramInfo_out[__i]; "
            << "    __param_id = __paramInfo_out[__i] >> 32;"
            << "    switch (__param_id) {"
            <<      out_copies_switch_body
            << "    default:;"
            << "    }"
            << "  }";
    }

    condition_task_execution_cmd_src
        << "__commandCode == 1";

    if (periodic_support)
    {
        params_src.append_with_separator("volatile unsigned long long int &" STR_HWCOUNTER_PORT, ", ");

        pragmas_src
            << "#pragma HLS INTERFACE ap_none port=" << STR_HWCOUNTER_PORT << "\n";

        condition_task_execution_cmd_src
            << " || __commandCode == 5";

        periodic_command_read
            << "  unsigned int __task_period = 0;"
            << "  " << STR_NUM_REPS << " = 1;"
            << "  if (__commandCode == 5) {"
            << "    __bufferData = " << STR_INPUTSTREAM << ".read().data;"
            << "    " << STR_NUM_REPS << " = __bufferData;"
            << "    __task_period = __bufferData>>32;"
            << "  }";

        periodic_command_pre
            << "  for (" << STR_REP_NUM << " = 0" << "; "
            <<      STR_REP_NUM << " < " << STR_NUM_REPS << " || 0xFFFFFFFF == " << STR_NUM_REPS << "; "
            <<      "++" << STR_REP_NUM << ")"
            << "  {"
            << "    const unsigned long long int __time_delay = " << STR_HWCOUNTER_PORT << " + __task_period;";

        periodic_command_post
            << "    do {"
            << "      wait();"
            << "    } while (" << STR_HWCOUNTER_PORT << " < __time_delay);"
            << "  }";
    }

    handle_task_execution_cmd_src
        << "  unsigned char __comp_needed, __destID;"
        << "  " << STR_TASKID << " = " << STR_INPUTSTREAM << ".read().data;"
        << "  " << STR_PARENT_TASKID << " = " << STR_INPUTSTREAM << ".read().data;"
        << "  __comp_needed = __commandArgs>>24;"
        << "  __destID = __commandArgs>>32;"
        <<    periodic_command_read
        <<    profiling_0
        <<    in_copies
        <<    profiling_1
        << "  if (__comp_needed) {"
        <<      clear_components_count
        <<      periodic_command_pre
        << "    " << func_symbol.get_name() << "(" << user_function_args << ");"
        <<      periodic_command_post
        << "  }"
        <<    profiling_2
        <<    out_copies
        <<    profiling_3
        << "  send_finished_task_cmd: {"
        << "    #pragma HLS PROTOCOL fixed\n"
        << "    __mcxx_send_finished_task_cmd(" << STR_OUTPUTSTREAM << ", __destID);"
        << "  }";

    // Get the declarations of wrapper types, variables and functions that are non-local
    get_hls_wrapper_decls(
        instrumentation_enabled(),
        creates_children_tasks,
        periodic_support,
        wrapper_memport_width_str,
        user_calls_set,
        wrapper_decls,
        wrapper_decls_after_user_code,
        pragmas_src);

    wrapper_source
        << wrapper_decls_after_user_code

        << "void " << wrapper_func_name << "(" << params_src << ") {"
        << pragmas_src
        << "\n"
        << local_decls_src
        << "\n"
        << "if (__reset == 0) {"
        << "  __reset = 1;"
        <<    reset_src
        << "}"
        << "__bufferData = " << STR_INPUTSTREAM << ".read().data;"
        << "__commandCode = __bufferData;"
        << "__commandArgs = __bufferData>>8;"
        << "if (" << condition_task_execution_cmd_src << ") {"
        <<    handle_task_execution_cmd_src
        << "}"
        << init_hw_instr_cmd_src
        << "}";

    // Get the definition of wrapper functions that are non-local
    get_hls_wrapper_defs(
        instrumentation_enabled(),
        creates_children_tasks,
        periodic_support,
        user_calls_set,
        wrapper_memport_width_str,
        wrapper_source);
}

void DeviceFPGA::copy_stuff_to_device_file(const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied)
{
    for (TL::ObjectList<Nodecl::NodeclBase>::const_iterator it = stuff_to_be_copied.begin();
            it != stuff_to_be_copied.end();
            ++it)
    {
        if (it->is<Nodecl::FunctionCode>()
                || it->is<Nodecl::TemplateFunctionCode>())
        {
            TL::Symbol source = it->get_symbol();
            TL::Symbol dest = SymbolUtils::new_function_symbol_for_deep_copy(source, source.get_name() + "_moved");

            _global_copied_fpga_symbols.add_map(source, dest);
            _stuff_to_copy.append(Nodecl::Utils::deep_copy(*it, *it, _global_copied_fpga_symbols));
        }
        else
        {
            _stuff_to_copy.append(Nodecl::Utils::deep_copy(*it, *it));
        }
    }
}

std::string DeviceFPGA::get_acc_type(const TL::Symbol& task, const TargetInformation& target_info)
{
    std::string value = "INVALID_ONTO_VALUE";

    // Check onto information
    ObjectList<Nodecl::NodeclBase> onto_clause = target_info.get_onto();
    if (onto_clause.size() >= 1)
    {
        Nodecl::NodeclBase onto_val = onto_clause[0];
        if (!_onto_warn_shown)
        {
            warn_printf_at(onto_val.get_locus(),
                "The use of onto clause is no longer needed unless you have a collision between two FPGA tasks that yeld the same type hash\n");
            _onto_warn_shown = true;
        }
        if (onto_clause.size() > 1)
        {
            error_printf_at(onto_val.get_locus(),
                "The syntax 'onto(type, count)' is no longer supported. Use 'onto(type) num_instances(count)' instead\n");
            fatal_error("Unsupported clause syntax");
        }

        if (onto_clause[0].is_constant())
        {
            const_value_t *ct_val = onto_val.get_constant();
            if (!const_value_is_integer(ct_val))
            {
                error_printf_at(onto_val.get_locus(), "Constant is not integer type in onto clause\n");
                fatal_error("Unsupported clause syntax");
            }
            else
            {
                const unsigned long long int type = const_value_cast_to_unsigned_long_long_int(ct_val);
                value = std::to_string(type);

                //Check that arch bits are set
                if ((type&0x300000000) == 0)
                {
                    error_printf_at(onto_val.get_locus(),
                        "Architecture bits not set in the onto clause: 33 -> SMP arch, 32 -> FPGA arch\n");
                    fatal_error("Unsupported clause syntax");
                }
                else if (type > 0x3FFFFFFFF)
                {
                    error_printf_at(onto_val.get_locus(),
                        "Task type to wide: 2bits arch + 32bits hash\n");
                    fatal_error("Unsupported clause syntax");
                }
            }
        }
        else
        {
            error_printf_at(onto_val.get_locus(), "Non-constant type in onto clause\n");
            fatal_error("Unsupported clause syntax");
        }
    }
    else
    {
        // Not using the line number to allow future modifications of source code without
        // afecting the accelerator hash
        std::stringstream type_str;
        type_str << task.get_filename() << " " << task.get_name();
        unsigned long long int type = simple_hash_str(type_str.str().c_str())&0xFFFFFFFF; //< Ensure that it its upto 32b

        const ObjectList<std::string>& devices = target_info.get_device_names();
        for (ObjectList<std::string>::const_iterator it2 = devices.begin();
                it2 != devices.end();
                ++it2)
        {
            //Architecture bits are:
            //  - 63th bit is set if task has SMP support
            //  - 62th bit is set if task has FPGA support
            std::string device_name = *it2;
            if (device_name == "smp")
            {
                type |= 0x200000000;
            }
            else if (device_name == "fpga")
            {
                type |= 0x100000000;
            }
            else
            {
                fatal_error("FPGA device only can create tasks for smp and fpga devices.\n");
            }
        }
        value = std::to_string(type);
    }
    return value;
}

std::string DeviceFPGA::get_num_instances(const TargetInformation& target_info)
{
    std::string value = "1"; //Default is 1 instance

    const ObjectList<Nodecl::NodeclBase>& numins_clause = target_info.get_num_instances();
    if (numins_clause.size() >= 1)
    {
        Nodecl::NodeclBase numins_value = numins_clause[0];
        if (numins_clause.size() > 1)
        {
            warn_printf_at(numins_value.get_locus(), "More than one argument in num_instances clause. Using only first one\n");
        }

        if (numins_value.is_constant())
        {
            const_value_t *ct_val = numins_value.get_constant();
            if (!const_value_is_integer(ct_val))
            {
                error_printf_at(numins_value.get_locus(), "Constant is not integer type in onto clause: num_instances\n");
                fatal_error("Unsupported clause syntax");
            }
            else
            {
                int acc_instances = const_value_cast_to_signed_int(ct_val);
                std::stringstream tmp_str_instances;
                tmp_str_instances << acc_instances;
                value = tmp_str_instances.str();
                if (acc_instances <= 0)
                {
                    error_printf_at(numins_value.get_locus(), "Constant in num_instances should be an integer longer than 0\n");
                    fatal_error("Unsupported clause syntax");
                }
            }
        }
        else
        {
            error_printf_at(numins_value.get_locus(), "num_instances clause does not contain a constant expresion\n");
            fatal_error("Unsupported clause syntax");
        }
    }
    return value;
}

Nodecl::NodeclBase DeviceFPGA::get_num_repetitions(const TargetInformation& target_info)
{
    Nodecl::NodeclBase clause_value;

    const ObjectList<Nodecl::NodeclBase>& clause_info = target_info.get_num_repetitions();
    if (clause_info.size() >= 1)
    {
        clause_value = clause_info[0];
        if (clause_info.size() > 1)
        {
            warn_printf_at(clause_value.get_locus(), "More than one argument in 'num_repetitions' clause. Using only first one\n");
        }
    }
    return clause_value;
}

Nodecl::NodeclBase DeviceFPGA::get_period(const TargetInformation& target_info)
{
    Nodecl::NodeclBase clause_value;

    const ObjectList<Nodecl::NodeclBase>& clause_info = target_info.get_period();
    if (clause_info.size() >= 1)
    {
        clause_value = clause_info[0];
        if (clause_info.size() > 1)
        {
            warn_printf_at(clause_value.get_locus(), "More than one argument in 'period' clause. Using only first one\n");
        }
    }
    return clause_value;
}

void DeviceFPGA::emit_async_device(
        Nodecl::NodeclBase construct,
        TL::Symbol current_function,
        TL::Symbol called_task,
        TL::Symbol structure_symbol,
        Nodecl::NodeclBase statements,
        Nodecl::NodeclBase priority_expr,
        Nodecl::NodeclBase if_condition,
        Nodecl::NodeclBase final_condition,
        Nodecl::NodeclBase task_label,
        bool is_untied,
        OutlineInfo& outline_info,
        /* this is non-NULL only for function tasks */
        OutlineInfo* parameter_outline_info,
        /* this is non-NULL only for task expressions */
        Nodecl::NodeclBase* placeholder_task_expr_transformation)
{
    TL::ObjectList<OutlineDataItem*> data_items = outline_info.get_data_items();
    DeviceHandler device_handler = DeviceHandler::get_device_handler();
    const OutlineInfo::implementation_table_t& implementation_table = outline_info.get_implementation_table();
    TL::Scope function_scope = current_function.get_scope();

    std::string acc_type;

    // Check devices of all implementations
    for (OutlineInfo::implementation_table_t::const_iterator it = implementation_table.begin();
            it != implementation_table.end();
            ++it)
    {
        const TargetInformation& target_info = it->second;
        // FIXME: Consider information from all implementations not only last one
        acc_type = get_acc_type(called_task, target_info);
    }

    Source spawn_code, args_list, deps_list, deps_flags_list, copies_list;
    size_t num_args = 0, num_deps = 0, num_copies = 0;

    // Go through all the arguments and fill the arguments_list
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        if (!(*it)->get_symbol().is_valid())
            continue;

        TL::ObjectList<OutlineDataItem::DependencyItem> dependences = (*it)->get_dependences();
        for (TL::ObjectList<OutlineDataItem::DependencyItem>::iterator dep_it = dependences.begin();
                dep_it != dependences.end();
                dep_it++)
        {
            if ((dep_it->directionality & (~OutlineDataItem::DEP_INOUT)) != OutlineDataItem::DEP_NONE)
            {
                fatal_printf_at((*it)->get_base_address_expression().get_locus(),
                    "Only in/out/inout dependences are suported when creating a task in a FPGA device\n");
            }

            // Dependence value
            Source dep_value;
            TL::DataReference dep_expr(dep_it->expression);
            Nodecl::NodeclBase dep_expr_offset = dep_expr.get_offsetof_dependence();
            dep_value << "(unsigned long long int)(" << as_expression(dep_expr.get_base_address())  << ") + " << as_expression(dep_expr_offset);
            deps_list.append_with_separator( dep_value, ", " );

            // Dependence flags
            Source dep_flags;
            if ((dep_it->directionality & OutlineDataItem::DEP_IN) == OutlineDataItem::DEP_IN)
            {
                dep_flags.append_with_separator("NANOS_ARGFLAG_DEP_IN", " | ");
            }
            if ((dep_it->directionality & OutlineDataItem::DEP_OUT) == OutlineDataItem::DEP_OUT)
            {
                dep_flags.append_with_separator("NANOS_ARGFLAG_DEP_OUT", " | ");
            }
            deps_flags_list.append_with_separator( dep_flags, ", " );

            ++num_deps;
        }

        Source arg_value;
        switch ((*it)->get_sharing())
        {
            case OutlineDataItem::SHARING_CAPTURE:
                {
                    if (((*it)->get_allocation_policy() & OutlineDataItem::ALLOCATION_POLICY_OVERALLOCATED)
                            == OutlineDataItem::ALLOCATION_POLICY_OVERALLOCATED)
                    {
                        fatal_error("Argument overallocation not supported yet\n");
                    }
                    else
                    {
                        TL::Type sym_type = (*it)->get_symbol().get_type();
                        if (sym_type.is_any_reference())
                            sym_type = sym_type.references_to();

                        if (sym_type.is_array())
                        {
                            fatal_error("Array argument not supported yet\n");
                        }
                        else if (sym_type.get_size() > sizeof(unsigned long long int))
                        {
                            fatal_error("Task argument to wide when creating the task inside a FPGA device");
                        }
                        else
                        {
                            sym_type = sym_type.no_ref().get_unqualified_type();
                            if (sym_type.is_pointer() || sym_type.is_integral_type())
                            {
                                Nodecl::NodeclBase captured = (*it)->get_captured_value();
                                arg_value
                                    << "(unsigned long long int)(" << as_expression(captured.shallow_copy()) << ")";
                            }
                            else
                            {
                                std::string cast_obj_name = "fpga_spawn_cast_" + (*it)->get_symbol().get_name();
                                Symbol cast_obj_sym = declare_casting_union(sym_type, construct);
                                spawn_code
                                    << cast_obj_sym.get_user_defined_type().get_declaration(function_scope, cast_obj_name) << ";"
                                    << cast_obj_name << ".typed = " << as_symbol((*it)->get_symbol()) << ";";

                                arg_value
                                    << cast_obj_name << ".raw";
                            }
                        }

                    }
                    break;
                }
            case OutlineDataItem::SHARING_SHARED:
            case OutlineDataItem::SHARING_REDUCTION:
            case OutlineDataItem::SHARING_SHARED_ALLOCA:
            case OutlineDataItem::SHARING_CAPTURE_ADDRESS:
            case OutlineDataItem::SHARING_PRIVATE:
            case OutlineDataItem::SHARING_ALLOCA:
                {
                    fatal_error("Argument type not supported yet\n");
                    break;
                }
            default:
                {
                    internal_error("Unexpected sharing kind", 0);
                }
        };
        args_list.append_with_separator( arg_value, ", " );

        // Check if argument is also a copy
        TL::ObjectList<OutlineDataItem::CopyItem> copies = (*it)->get_copies();
        for (TL::ObjectList<OutlineDataItem::CopyItem>::iterator copy_it = copies.begin();
                copy_it != copies.end();
                copy_it++)
        {
            TL::DataReference copy_expr(copy_it->expression);
            if (copy_expr.is_multireference())
            {
                error_printf_at((*it)->get_symbol().get_locus(),
                        "Dynamic copies during task spawn are not supported in fpga context\n");
                fatal_error("Unsupported fpga task parameter");
            }

            TL::Type copy_type = copy_expr.get_data_type();
            int num_dimensions_of_copy = copy_type.get_num_dimensions();
            TL::Type base_type;
            ObjectList<Nodecl::NodeclBase> lower_bounds, upper_bounds, dims_sizes;
            if (num_dimensions_of_copy == 0)
            {
                base_type = copy_type;
                lower_bounds.append(const_value_to_nodecl(const_value_get_signed_int(0)));
                upper_bounds.append(const_value_to_nodecl(const_value_get_signed_int(0)));
                dims_sizes.append(const_value_to_nodecl(const_value_get_signed_int(1)));
                num_dimensions_of_copy++;
            }
            else if (num_dimensions_of_copy == 1)
            {
                compute_array_info(construct, copy_expr, copy_type, base_type, lower_bounds, upper_bounds, dims_sizes);

                // Sanity check
                ERROR_CONDITION(num_dimensions_of_copy != (signed)lower_bounds.size()
                        || num_dimensions_of_copy != (signed)upper_bounds.size()
                        || num_dimensions_of_copy != (signed)dims_sizes.size(),
                        "Mismatch between dimensions", 0);
            }
            else
            {
                error_printf_at((*it)->get_symbol().get_locus(),
                        "Copies with multiple dimensions not supported in fpga context\n");
                fatal_error("Unsupported fpga task parameter");
            }

            Nodecl::NodeclBase address_of_object = copy_expr.get_address_of_symbol();
            int input = ((copy_it->directionality & OutlineDataItem::COPY_IN) == OutlineDataItem::COPY_IN);
            int output = ((copy_it->directionality & OutlineDataItem::COPY_OUT) == OutlineDataItem::COPY_OUT);

            Source copy_value;
            copy_value << "{"
                << "  .address = (unsigned long long int)(" << as_expression(address_of_object) << ")"
                << ", .flags = ";

            if (input && output)
            {
                copy_value << "(NANOS_ARGFLAG_COPY_IN | NANOS_ARGFLAG_COPY_OUT)";
            }
            else if (input)
            {
                copy_value << "NANOS_ARGFLAG_COPY_IN";
            }
            else if (output)
            {
                copy_value << "NANOS_ARGFLAG_COPY_OUT";
            }
            else
            {
                fatal_error("Found copy that is not input nor output\n");
            }

            copy_value
                << ", .arg_idx = " << num_args
                << ", ._padding = 0"
                << ", .size = "
                << "((unsigned int)(" << as_expression(dims_sizes[0].shallow_copy()) << ") *"
                << " (unsigned int)sizeof(" << as_type(base_type) << "))"
                << ", .offset = "
                << "((unsigned int)(" << as_expression(lower_bounds[0].shallow_copy()) << ") *"
                << " (unsigned int)sizeof(" << as_type(base_type) << "))"
                << ", .accessed_length = "
                << "((unsigned int)((" << as_expression(upper_bounds[0].shallow_copy()) << ") - ("
                << as_expression(lower_bounds[0].shallow_copy()) << ") + 1) *"
                << " (unsigned int)sizeof(" << as_type(base_type) << "))"
                << "}";

            copies_list.append_with_separator( copy_value, ", " );
            ++num_copies;
        }

        ++num_args;
    }

    if (!Nanos::Version::interface_is_at_least("fpga", 10))
        fatal_error("Your Nanos version is not supported for cration of tasks inside the FPGA. Please update your Nanos installation\n");

    if (num_args > 0)
    {
        spawn_code
            << "unsigned long long int mcxx_args[] = {"
            << args_list
            << "};";
    }

    if (num_deps > 0)
    {
        spawn_code
            << "unsigned long long int mcxx_deps[] = {"
            << deps_list
            << "};"
            << "unsigned char mcxx_deps_flags[] = {"
            << deps_flags_list
            << "};";
    }

    if (num_copies > 0)
    {
        spawn_code
            << "nanos_fpga_copyinfo_t mcxx_copies[] = {"
            << copies_list
            << "};";
    }
    else if (/*num_copies == 0 &&*/ IS_C_LANGUAGE)
    {
        spawn_code
            << "nanos_fpga_copyinfo_t __mcxx_fix_element_nanos_fpga_copyinfo_t;"
            << "__mcxx_fix_element_nanos_fpga_copyinfo_t._padding = 1;";
    }

    spawn_code
        << "nanos_fpga_create_wd_async(" << acc_type << "U, "
        << num_args << ", " << (num_args > 0 ? "mcxx_args" : "(unsigned long long int *)0") << ", "
        << num_deps << ", " << (num_deps > 0 ? "mcxx_deps, mcxx_deps_flags" : "(unsigned long long int *)0, (unsigned char *)0") << ", "
        << num_copies << ", " << (num_copies > 0 ? "mcxx_copies" : "(nanos_fpga_copyinfo_t *)0") << ");";

    Nodecl::NodeclBase spawn_code_tree = spawn_code.parse_statement(construct);
    construct.replace(spawn_code_tree);

    if (_registered_tasks.find(acc_type) == _registered_tasks.end())
    {
        register_task_creation(construct, task_label, current_function, called_task, structure_symbol, outline_info, acc_type, num_copies);
        _registered_tasks.insert(acc_type);
    }
}

bool is_not_alnum(int charact) {
    return !std::isalnum(charact);
}

void DeviceFPGA::register_task_creation(
        Nodecl::NodeclBase construct,
        Nodecl::NodeclBase task_label,
        TL::Symbol current_function,
        TL::Symbol called_task,
        TL::Symbol structure_symbol,
        OutlineInfo& outline_info,
        std::string acc_type,
        size_t const num_copies)
{
    //NOTE: Most of the code used to implement this function comes for the tl-lower-task.cpp.
    //      It is a simplification of the existing there as the FPGA device capabilities are limited
    Nodecl::NodeclBase code = current_function.get_function_code();
    Nodecl::Context context = (code.is<Nodecl::TemplateFunctionCode>())
        ? code.as<Nodecl::TemplateFunctionCode>().get_statements().as<Nodecl::Context>()
        : code.as<Nodecl::FunctionCode>().get_statements().as<Nodecl::Context>();
    DeviceHandler device_handler = DeviceHandler::get_device_handler();
    TL::ObjectList<OutlineDataItem*> data_items = outline_info.get_data_items();

    Source struct_arg_type_name;
    struct_arg_type_name
         << ((structure_symbol.get_type().is_template_specialized_type()
                     &&  structure_symbol.get_type().is_dependent()) ? "typename " : "")
         << structure_symbol.get_qualified_name(context.retrieve_context());

     bool without_template_args =
         !current_function.get_type().is_template_specialized_type()
         || current_function.get_scope().get_template_parameters()->is_explicit_specialization;

    TL::Scope function_scope = context.retrieve_context();
    std::string qualified_name = current_function.get_qualified_name(function_scope, without_template_args);

    Source device_descriptions, ancillary_device_descriptions, unpacked_arguments;
    size_t num_devices = 0, arg_cnt = 0;

    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        Source arg_value;
        switch ((*it)->get_sharing())
        {
            case OutlineDataItem::SHARING_CAPTURE:
                {
                    if (((*it)->get_allocation_policy() & OutlineDataItem::ALLOCATION_POLICY_OVERALLOCATED)
                            == OutlineDataItem::ALLOCATION_POLICY_OVERALLOCATED)
                    {
                        fatal_error("Argument overallocation not supported yet\n");

                    }
                    else
                    {
                        TL::Type sym_type = (*it)->get_symbol().get_type();
                        if (sym_type.is_any_reference())
                            sym_type = sym_type.references_to();

                        if (sym_type.is_array())
                        {
                            fatal_error("Array argument not supported yet\n");
                        }
                        else if (sym_type.get_size() > sizeof(uintptr_t))
                        {
                            fatal_error("Task argument to wide when creating the task inside a FPGA device");
                        }
                        else if (sym_type.is_pointer())
                        {
                            sym_type = sym_type.no_ref().get_unqualified_type();
                            arg_value << "(" << as_type(sym_type) << ")((uintptr_t)args[" << arg_cnt << "])";
                        }
                        else
                        {
                            sym_type = sym_type.no_ref().get_unqualified_type();
                            arg_value << "(" << as_type(sym_type) << ")args[" << arg_cnt << "]";
                        }
                    }
                    break;
                }
            case OutlineDataItem::SHARING_SHARED:
            case OutlineDataItem::SHARING_REDUCTION:
            case OutlineDataItem::SHARING_SHARED_ALLOCA:
            case OutlineDataItem::SHARING_CAPTURE_ADDRESS:
            case OutlineDataItem::SHARING_PRIVATE:
            case OutlineDataItem::SHARING_ALLOCA:
                {
                    fatal_error("Argument type not supported yet\n");
                    break;
                }
            default:
                {
                    internal_error("Unexpected sharing kind", 0);
                }
        };

        unpacked_arguments.append_with_separator(arg_value, ", ");
        arg_cnt++;
    }

    // Check devices of all implementations and put the device information in device_descriptions
    const OutlineInfo::implementation_table_t& implementation_table = outline_info.get_implementation_table();
    for (OutlineInfo::implementation_table_t::const_iterator it = implementation_table.begin();
            it != implementation_table.end();
            ++it)
    {
        TL::Symbol implementor_symbol = it->first;
        const TargetInformation& target_info = it->second;
        const ObjectList<std::string>& devices = target_info.get_device_names();
        std::string implementor_outline_name = target_info.get_outline_name();

        // The symbol 'real_called_task' will be invalid if the current task is
        // a inline task. Otherwise, It will be the implementor symbol
        TL::Symbol real_called_task =
            called_task.is_valid() ?
            implementor_symbol : TL::Symbol::invalid();

        for (ObjectList<std::string>::const_iterator it2 = devices.begin();
                it2 != devices.end();
                ++it2)
        {
            Source ancillary_device_description, device_description, aux_fortran_init;
            int fortran_device_index = 0;

            std::string device_name = *it2;
            DeviceProvider* device = device_handler.get_device(device_name);
            ERROR_CONDITION(device == NULL, " Device '%s' has not been loaded.", device_name.c_str());

            std::string arguments_structure = struct_arg_type_name.get_source();

            DeviceDescriptorInfo info_implementor(
                    implementor_outline_name,
                    arguments_structure,
                    current_function,
                    target_info,
                    fortran_device_index,
                    outline_info.get_data_items(),
                    real_called_task);

            device->get_device_descriptor(
                    info_implementor,
                    ancillary_device_description,
                    device_description,
                    aux_fortran_init);

            //FIXME: This should be get using the device
            const std::string& fpga_spawn_outline_name = device_name + "_" + implementor_outline_name + "_fpga_spawn";

            device_descriptions.append_with_separator(device_description, ", ");
            ancillary_device_descriptions << ancillary_device_description
                << device_name + "_" + implementor_outline_name << "_args.outline = (void(*)(void*))(void(*)(uint64_t *))&"
                << fpga_spawn_outline_name << ";";
            ++num_devices;

            // Create the outline function for task spwan from fpga device
            //The outline function has always only one parameter which name is 'args'
            ObjectList<std::string> arg_name;
            arg_name.append("args");

            //The type of this parameter is an struct (i. e. user defined type)
            ObjectList<TL::Type> arg_type;
            arg_type.append(TL::Type(TL::Type::get_unsigned_long_long_int_type()).get_pointer_to());

            TL::Symbol outline_function = SymbolUtils::new_function_symbol(
                current_function,
                fpga_spawn_outline_name,
                TL::Type::get_void_type(),
                arg_name,
                arg_type
            );

            Nodecl::NodeclBase outline_function_code, outline_function_body;
            SymbolUtils::build_empty_body_for_function(
                outline_function,
                outline_function_code,
                outline_function_body
            );

            Nodecl::Utils::append_to_top_level_nodecl(outline_function_code);

            Source outline_src;
            Source instrument_before, instrument_after;

            if (instrumentation_enabled())
            {
                device->get_instrumentation_code(
                    called_task,
                    outline_function,
                    outline_function_body,
                    task_label,
                    construct.get_locus(),
                    instrument_before,
                    instrument_after
                );
            }

            outline_src
                << "{"
                << instrument_before
                << device_name << "_" << implementor_outline_name << "_unpacked"
                << "(" << unpacked_arguments << ");"
                << instrument_after
                << "}"
            ;

            Nodecl::NodeclBase new_outline_body = outline_src.parse_statement(outline_function_body);
            outline_function_body.replace(new_outline_body);
        }
    }

    Source reference_to_xlate;

    if (num_copies == 0)
    {
        reference_to_xlate << "(nanos_translate_args_t)0";
    }
    else
    {
        TL::Counter &fun_num = TL::CounterManager::get_counter("nanos++-translation-functions");
        std::string filename = TL::CompilationProcess::get_current_file().get_filename();
        //Remove non-alphanumeric characters from the string
        filename.erase(std::remove_if(filename.begin(), filename.end(), (bool(*)(int))is_not_alnum), filename.end());
        Source fun_name;
        fun_name << "nanos_xlate_fun_" << filename << "_" << fun_num;
        fun_num++;

        TL::Type argument_type = ::get_user_defined_type(structure_symbol.get_internal_symbol());
        argument_type = argument_type.get_lvalue_reference_to();

        ObjectList<std::string> parameter_names;
        ObjectList<TL::Type> parameter_types;

        parameter_names.append("arg");
        parameter_types.append(argument_type);

        TL::Symbol sym_nanos_wd_t = ReferenceScope(construct).get_scope().get_symbol_from_name("nanos_wd_t");
        ERROR_CONDITION(!sym_nanos_wd_t.is_valid(), "Typename nanos_wd_t not found", 0);
        parameter_names.append("wd");
        parameter_types.append(sym_nanos_wd_t.get_user_defined_type());

        TL::Symbol enclosing_function = Nodecl::Utils::get_enclosing_function(construct);

        const TL::Symbol& xlate_function_symbol = SymbolUtils::new_function_symbol(
                enclosing_function,
                fun_name.get_source(),
                TL::Type::get_void_type(),
                parameter_names,
                parameter_types);

        Nodecl::NodeclBase function_code, empty_statement;
        SymbolUtils::build_empty_body_for_function(
                xlate_function_symbol,
                function_code,
                empty_statement);

        Source translations;

        // Check the data items and generate the translation function
        int current_copy_num = 0;
        for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
                it != data_items.end();
                it++)
        {
            TL::ObjectList<OutlineDataItem::CopyItem> copies = (*it)->get_copies();

            if (copies.empty())
                continue;

            //ERROR_CONDITION((*it)->get_sharing() != OutlineDataItem::SHARING_SHARED, "Unexpected sharing\n", 0);

            int num_static_copies = 0;

            for (TL::ObjectList<OutlineDataItem::CopyItem>::iterator
                    copy_it = copies.begin();
                    copy_it != copies.end();
                    copy_it++)
            {
                TL::DataReference copy_expr(copy_it->expression);
                if (!copy_expr.is_multireference())
                    num_static_copies++;
            }

            if (num_static_copies == 0)
                continue;

            translations
                << "{"
                << "void *device_base_address;"
                << "nanos_err_t nanos_err;"

                << "device_base_address = 0;"
                << "nanos_err = nanos_get_addr(" << current_copy_num << ", &device_base_address, wd);"
                << "if (nanos_err != NANOS_OK) nanos_handle_error(nanos_err);"
                ;

            if (((*it)->get_symbol().get_type().no_ref().is_fortran_array()
                        && (*it)->get_symbol().get_type().no_ref().array_requires_descriptor())
                    || ((*it)->get_symbol().get_type().no_ref().is_pointer()
                        && (*it)->get_symbol().get_type().no_ref().points_to().is_fortran_array()
                        && (*it)->get_symbol().get_type().no_ref().points_to().array_requires_descriptor()))
            {
                fatal_error("Fortran arrays not supported in a FPGA task spawn");
            }
            else
            {
                // Currently we do not support copies on non-shared stuff, so this should be always a pointer
                ERROR_CONDITION(!(*it)->get_field_type().is_pointer(), "Invalid type, expecting a pointer", 0);

                translations
                    << "arg." << (*it)->get_field_name() << " = (" << as_type((*it)->get_field_type()) << ")device_base_address;"
                    << "}"
                    ;
            }

            current_copy_num += num_static_copies;
        }

        Nodecl::NodeclBase translations_tree = translations.parse_statement(empty_statement);
        empty_statement.replace(translations_tree);
        Nodecl::Utils::prepend_to_enclosing_top_level_location(construct, function_code);

        if (xlate_function_symbol.get_type().is_template_specialized_type())
        {
            // Extra cast needed for g++ 4.6 or lower (otherwise the
            // compilation may fail or the template function not be emitted)
            reference_to_xlate << "(" << as_type(xlate_function_symbol.get_type().get_pointer_to()) << ")";
        }
        reference_to_xlate << "(nanos_translate_args_t)" << xlate_function_symbol.get_qualified_name();
    }

    Source register_task;
    if (IS_CXX_LANGUAGE)
    {
        register_task << "extern \"C\""
            << "{"
            ;
    }
    else if (IS_FORTRAN_LANGUAGE)
    {
        fatal_error("Fortran support not implemeted yet");
    }

    TL::Symbol devices_class = function_scope.get_symbol_from_name("nanos_device_t");
    ERROR_CONDITION(!devices_class.is_valid(), "Invalid symbol", 0);

    register_task
        << "void __mcxx_fpga_register_" << acc_type << "(void* p __attribute__((unused)))"
        << "{"
        <<   ancillary_device_descriptions
        <<   devices_class.get_qualified_name(context.retrieve_context()) << " devices[] = {"
        <<     device_descriptions
        <<   "};"
        <<   "nanos_fpga_register_wd_info(" << acc_type << "U, " << num_devices << ", devices, " << reference_to_xlate << ");"
        << "}"
        ;

    if (IS_CXX_LANGUAGE)
    {
        register_task << "}";
    }

    Nodecl::NodeclBase register_task_tree = register_task.parse_global(_root);
    Nodecl::Utils::append_to_top_level_nodecl(register_task_tree);

    FpgaNanosPostInitInfo info;
    info._function = "__mcxx_fpga_register_" + acc_type;
    info._argument = "0";
    _nanos_post_init_actions.append(info);
}

void DeviceFPGA::set_bitstream_generation_from_str(const std::string& in_str)
{
    // NOTE: Support "ON" as "1"
    std::string str = in_str == "ON" ? "1" : in_str;
    TL::parse_boolean_option("bitstream_generation", str, _bitstream_generation, "Assuming false.");
}

void DeviceFPGA::set_force_fpga_task_creation_ports_from_str(const std::string& str)
{
    std::string val;
    std::istringstream stream(str);
    while (std::getline(stream, val, ','))
    {
        _force_fpga_task_creation_ports.insert(val);
    }
}

void DeviceFPGA::set_memory_port_width_from_str(const std::string& in_str)
{
    _memory_port_width = in_str;
}

void DeviceFPGA::set_unaligned_memory_port_from_str(const std::string& str)
{
    TL::parse_boolean_option("fpga_unaligned_memory_port", str, _unaligned_memory_port, "Assuming false.");
}

void DeviceFPGA::set_force_periodic_support_from_str(const std::string& str)
{
    TL::parse_boolean_option("force_fpga_periodic_support", str, _force_periodic_support, "Assuming false.");
}

void DeviceFPGA::set_funcion_copy_suffix_from_str(const std::string& in_str)
{
    _function_copy_suffix = in_str;
}

std::string DeviceFPGA::FpgaOutlineInfo::get_filename() const
{
    return _type + ":" + _num_instances + ":" + _name + "_hls_automatic_mcxx.cpp";
}

std::string DeviceFPGA::FpgaOutlineInfo::get_wrapper_name() const
{
    return this->_name + "_hls_automatic_mcxx_wrapper";
}

EXPORT_PHASE(TL::Nanox::DeviceFPGA);

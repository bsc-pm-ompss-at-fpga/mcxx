/*--------------------------------------------------------------------
  (C) Copyright 2006-2019 Barcelona Supercomputing Center
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

#include <fstream>

#include "cxx-diagnostic.h"
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
#include "tl-symbol-utils.hpp"
#include "tl-counters.hpp"

using namespace TL;
using namespace TL::Nanox;

Source DeviceFPGA::gen_fpga_outline(ObjectList<Symbol> param_list, TL::ObjectList<OutlineDataItem*> data_items) {
    unsigned param_pos = 0;
    Source fpga_outline;

    for (ObjectList<Symbol>::const_iterator it = param_list.begin(); it != param_list.end(); it++, param_pos++) {
        TL::Symbol unpacked_argument = (*it);

        TL::Symbol outline_data_item_sym = data_items[param_pos]->get_symbol();
        TL::Type field_type = data_items[param_pos]->get_field_type();
        const std::string &field_name = outline_data_item_sym.get_name();
        Scope scope = data_items[param_pos]->get_symbol().get_scope();
        std::string arg_simple_decl = field_type.get_simple_declaration(scope, unpacked_argument.get_name());

        // If the outline data item has not a valid symbol, skip it
        if (!outline_data_item_sym.is_valid() || field_type.get_size() > sizeof(uint64_t)) {
            #if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << "Argument " << param_pos << ": " << arg_simple_decl << " is not valid" << std::endl << std::endl;
            #endif

            fatal_error("Non-valid argument in FPGA task. Data type must be at most 64-bit wide\n");
        }

        //const ObjectList<OutlineDataItem::CopyItem> &copies = data_items[param_pos]->get_copies();
        bool in_type, out_type;

        //in_type = false;
        //out_type = false;
        //if (!copies.empty()) {
        //    if (copies.front().directionality == OutlineDataItem::COPY_IN) {
        //        in_type = true;
        //    } else if (copies.front().directionality == OutlineDataItem::COPY_OUT) {
        //        out_type = true;
        //    } else if (copies.front().directionality == OutlineDataItem::COPY_INOUT) {
        //        in_type = true;
        //        out_type = true;
        //    }
        //}
        //NOTE: For now, always allow the wrapper to decide whether to do the copy or not.
        //      This supports tasks without copies but with localmem
        in_type = true;
        out_type = true;

        // Create union in order to reinterpret argument as a uint64_t
        fpga_outline
            << "union {"
            <<     as_type(unpacked_argument.get_type().get_unqualified_type().no_ref()) << " " << unpacked_argument.get_name() << ";"
            <<     "uint64_t " << unpacked_argument.get_name() << "_task_arg;"
            << "} " << field_name << ";"
        ;

        // If argument is pointer or array, get physical address
        if (field_type.is_pointer() || field_type.is_array()) {
            fpga_outline
                << field_name << "." << unpacked_argument.get_name() << " = (" << as_type(unpacked_argument.get_type().get_unqualified_type().no_ref()) << ")nanos_fpga_get_phy_address((void *)" << unpacked_argument.get_name() << ");"
            ;
        } else {
            fpga_outline
                << field_name << "." << unpacked_argument.get_name() << " = " << unpacked_argument.get_name() << ";"
            ;
        }

        // Add argument to task structure
        fpga_outline
            << "nanos_fpga_set_task_arg(nanos_current_wd(), " << param_pos << ", " << in_type << ", " << out_type << ", " << field_name << "." << unpacked_argument.get_name() << "_task_arg);"
        ;

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Adding argument " << param_pos << ": " << as_type(unpacked_argument.get_type().get_unqualified_type().no_ref()) << " " << unpacked_argument.get_name() << std::endl << std::endl;
#endif
    }

    return fpga_outline;
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

    // Unpack DTO
    Lowering* lowering = info._lowering;
    const std::string& device_outline_name = fpga_outline_name(info._outline_name);
    // const Nodecl::NodeclBase& task_statements = info._task_statements;
    const Nodecl::NodeclBase& original_statements = info._original_statements;
    // bool is_function_task = info._called_task.is_valid();

    const TL::Symbol& arguments_struct = info._arguments_struct;
    const TL::Symbol& called_task = info._called_task;
    TL::ObjectList<OutlineDataItem*> data_items = info._data_items;

    lowering->seen_fpga_task = true;

    symbol_map = new Nodecl::Utils::SimpleSymbolMap(&_copied_fpga_functions);

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
            if (_copied_fpga_functions.map(called_task) == called_task)
            {
                //new task-> add it to the list
                const std::string acc_type = get_acc_type(called_task, info._target_info);
                const bool creates_children_tasks =
                    (info._num_inner_tasks > 0) ||
                    (_force_fpga_task_creation_ports.find(acc_type) != _force_fpga_task_creation_ports.end());
                const std::string wrapper_name = fpga_wrapper_name(called_task.get_name());

                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(
                    called_task, called_task.get_name() + "_moved");
                _copied_fpga_functions.add_map(called_task, new_function);

                Nodecl::NodeclBase fun_code = Nodecl::Utils::deep_copy(
                    called_task.get_function_code(),
                    called_task.get_scope(),
                    *symbol_map);
                new_function.set_value(fun_code);

                if (creates_children_tasks)
                {
                    ReplacePtrDeclVisitor replacePtrDeclVisitor;
                    replacePtrDeclVisitor.walk(fun_code);
                }

                Source wrapper_decls, wrapper_code;
                DeviceFPGA::gen_hls_wrapper(
                    new_function,
                    info._data_items,
                    creates_children_tasks,
                    wrapper_name,
                    wrapper_decls,
                    wrapper_code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << std::endl << std::endl;
                std::cerr << " ===================================================================0\n";
                std::cerr << "First call to called_functions_sources_list... going through:\n";
                std::cerr << " ===================================================================0\n";
                __number_of_calls=1;
#endif

                TL::ObjectList<Nodecl::NodeclBase> expand_code;
                expand_code.append(fun_code);

                TL::ObjectList<Source> called_functions_sources_list;
                called_functions_sources_list = get_called_functions_sources(expand_code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << " ===================================================================0\n";
                std::cerr << "End First call to called_functions_sources_list... \n";
                std::cerr << " ===================================================================0\n";
                std::cerr << std::endl << std::endl;
#endif

                Source outline_src;

                //1st: Definition of called functions
                for (TL::ObjectList<Source>::iterator it = called_functions_sources_list.begin();
                        it != called_functions_sources_list.end();
                        it++)
                {
                    outline_src << *it;
                }

                //Then: wrapper declarations, function code and wrapper definition
                std::string const fun_code_str = fun_code.prettyprint();
                outline_src
                    << wrapper_decls
                    << "\n"
                    << fun_code_str
                    << "\n"
                    << wrapper_code
                ;


                FpgaOutlineInfo to_outline_info;
                to_outline_info._type = acc_type;
                to_outline_info._num_instances = get_num_instances(info._target_info);
                to_outline_info._name = called_task.get_name();
                to_outline_info._source_code = outline_src;
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

    Source unpacked_arguments, private_entities;

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
    TL::Source dummy_init_statements, dummy_final_statements;
    TL::Symbol unpacked_function = new_function_symbol_unpacked(
        current_function,
        device_outline_name + "_unpacked",
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
    Source fpga_outline = gen_fpga_outline(unpacked_function.get_function_parameters(), data_items);

#if _DEBUG_AUTOMATIC_COMPILER_
    std::cerr << " ===================================================================0\n";
    std::cerr << "FPGA Outline function:\n";
    std::cerr << " ===================================================================0\n";
    std::cerr << fpga_outline.get_source() << std::endl << std::endl;
#endif

    Source unpacked_source;
    unpacked_source
        << dummy_init_statements
        << private_entities
        << fpga_outline
        << statement_placeholder(outline_placeholder)
        << dummy_final_statements
    ;

    // Add a declaration of the unpacked function symbol in the original source
    if (IS_CXX_LANGUAGE)
    {
        //DJG TO remove???
        if (!unpacked_function.is_member())
        {
#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "No member... actually Doing delcaration insertion unpacked function:" << unpacked_function.get_name() << std::endl;
#endif

            Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDecl::make(
                /* optative context */ nodecl_null(),
                unpacked_function,
                original_statements.get_locus()
            );
            Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);
        }
    }

    Nodecl::NodeclBase new_unpacked_body = unpacked_source.parse_statement(unpacked_function_body);
    unpacked_function_body.replace(new_unpacked_body);

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
        device_outline_name,
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

    if (IS_CXX_LANGUAGE)
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Doing delcaration insertion unpacked function:" << outline_function.get_name() << std::endl;
#endif

        if (!outline_function.is_member())
        {

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "No member... actually Doing delcaration insertion unpacked function:" << outline_function.get_name() << std::endl;
#endif

            Nodecl::NodeclBase nodecl_decl = Nodecl::CxxDecl::make(
                /* optative context */ nodecl_null(),
                outline_function,
                original_statements.get_locus()
            );

            Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);
        }
    }
}

DeviceFPGA::DeviceFPGA() : DeviceProvider(std::string("fpga")), _bitstream_generation(false),
    _force_fpga_task_creation_ports(), _onto_warn_shown(false)
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
                << args_name << ".type = " << acc_type << ";"
            ;
        }
        else
        {
            ancillary_device_description
                << args_name << ".acc_num = " << acc_type << ";"
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
        TL::ObjectList<struct FpgaOutlineInfo>::iterator it;
        for (it = _outlines.begin();
            it != _outlines.end();
            it++)
        {
            std::string original_filename = TL::CompilationProcess::get_current_file().get_filename();
            std::string new_filename = it->get_filename();

            std::fstream hls_file;

            // Check if file exist
            hls_file.open(new_filename.c_str(), std::ios_base::in);
            if (hls_file.is_open())
            {
                hls_file.close();
                fatal_error("ERROR: Trying to create '%s' which already exists.\n%s\n%s",
                    new_filename.c_str(),
                    "       If you have two FPGA tasks with the same function name, you should use the onto(type) clause in the target directive.",
                    "       Otherwise, you must clean the running directory before linking.");
            }
            hls_file.clear();

            hls_file.open(new_filename.c_str(), std::ios_base::out); //open as output
            if (! hls_file.is_open())
            {
                fatal_error("%s: error: cannot open file %s\n",
                    new_filename.c_str(),
                    strerror(errno)
                );
            }

            hls_file << "///////////////////\n"
                     << "// Automatic IP Generated by OmpSs@FPGA compiler\n"
                     << "///////////////////\n"
                     << "// Top IP Function:  " << it->get_wrapper_name() << "\n"
                     << "// Accelerator type: " << it->_type << "\n"
                     << "// Num. instances:   " << it->_num_instances << "\n"
                     << "///////////////////\n"
                     << "\n"
                     << "#include <stdint.h>\n"
                     << "#include <iostream>\n"
                     << "#include <string.h>\n"
                     << "#include <strings.h>\n"
                     << "#include <hls_stream.h>\n"
                     << "#include <ap_axi_sdata.h>\n"
                     << "\n";

            add_included_fpga_files(hls_file);
            add_stuff_to_copy(hls_file);

            hls_file << it->_source_code.get_source(true);

            hls_file.close();

            if(!CURRENT_CONFIGURATION->do_not_link)
            {
                //If linking is enabled, remove the intermediate HLS source file (like an object file)
                ::mark_file_for_cleanup(new_filename.c_str());
            }
        }

        // Do not forget the clear the outlines list
        _outlines.clear();
    }
    _stuff_to_copy.clear();

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

void DeviceFPGA::add_included_fpga_files(std::ostream &hls_file)
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
                hls_file << line << std::endl;
                break;
            }
        }
    }
}

void DeviceFPGA::add_stuff_to_copy(std::ostream &hls_file)
{
    for (TL::ObjectList<Nodecl::NodeclBase>::iterator it = _stuff_to_copy.begin();
        it != _stuff_to_copy.end();
        it++)
    {
        hls_file << it->prettyprint();
    }
}

static void get_inout_decl(ObjectList<OutlineDataItem*>& data_items, std::string &in_type, std::string &out_type)
{
    in_type = "";
    out_type = "";
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
        it != data_items.end();
        it++)
    {
        const ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();
        if (!copies.empty())
        {
            Scope scope = (*it)->get_symbol().get_scope();
            if (copies.front().directionality == OutlineDataItem::COPY_IN && in_type == "")
            {
                in_type = (*it)->get_field_type().get_simple_declaration(scope, "");
            }
            else if (copies.front().directionality == OutlineDataItem::COPY_OUT && out_type == "")
            {
                out_type = (*it)->get_field_type().get_simple_declaration(scope, "");
            }
            else if (copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {
                //If we find an inout, set both input and output types and return
                out_type = (*it)->get_field_type().get_simple_declaration(scope, "");
                in_type = out_type;
                return;
            }
        }
    }
}

static std::string get_element_type_pointer_to(TL::Type type, std::string field_name, TL::Scope scope)
{

    TL::Type points_to = type;
    size_t position;

    while(points_to.is_pointer() || points_to.is_pointer_to_class())
    {
        if (points_to.is_pointer())
        {
            points_to = points_to.points_to();
        }
        else if (points_to.is_pointer_to_class())
        {
            points_to = points_to.pointed_class();
        }
    }

    std::string pointed_to_string_simple = points_to.get_simple_declaration(scope,field_name);

    position=pointed_to_string_simple.rfind(field_name);

    std::string reference_type = pointed_to_string_simple.substr(0,position-1);

    return reference_type;
}

static Source get_type_pointer_to_arrays_src(TL::Type copy_type, TL::Type type, bool is_only_pointer)
{
    Source ArrayExpression;
    std::string  dimension_str;

    if (is_only_pointer) //it's a shape
    {
        dimension_str = "[1]";
        ArrayExpression << dimension_str;
        return ArrayExpression;
    }
    else if (copy_type.is_array()) //it's a shape
    {
        int total_dimensions = copy_type.get_num_dimensions();
        int n_dimensions=0;
        Nodecl::NodeclBase array_get_expr;
        while (n_dimensions<total_dimensions-1)
        {
            array_get_expr = copy_type.array_get_size();
            dimension_str = "[" + array_get_expr.prettyprint() + "]";
            ArrayExpression << dimension_str;
            n_dimensions++;
            copy_type  = copy_type.array_element();
            // array_get_expr = type.array_get_size();
            // dimension_str = "[" + array_get_expr.prettyprint() + "]";
            // ArrayExpression << dimension_str;
            // n_dimensions++;
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
        dimension_str = "[" + cp_size.prettyprint() + "]";
        ArrayExpression <<  dimension_str;
        //elems = const_value_cast_to_4(cp_size.get_constant());
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Region Expression:" << ArrayExpression.get_source() << std::endl;
        std::cerr << std::endl << std::endl;
#endif
        return ArrayExpression;

    }
#if _DEBUG_AUTOMATIC_COMPILER_
    fprintf(stderr,"ERROR in dimensions! :%d\n",type.get_num_dimensions());
#endif

    return ArrayExpression;

}

static Source get_type_arrays_src(TL::Type copy_type, TL::Type type, bool is_only_pointer)
{
    Source ArrayExpression;
    std::string  dimension_str;

    if (is_only_pointer) //it's a shape
    {
        dimension_str = "[1]";
        ArrayExpression << dimension_str;
        return ArrayExpression;
    }
    else if (copy_type.is_array()) //it's a shape
    {
        int total_dimensions = copy_type.get_num_dimensions();
        int n_dimensions=0;
        Nodecl::NodeclBase array_get_expr;
        array_get_expr = copy_type.array_get_size();
        dimension_str = "[" + array_get_expr.prettyprint() + "]";
        ArrayExpression << dimension_str;
        n_dimensions++;
        while (n_dimensions<total_dimensions)
        {
            copy_type  = copy_type.array_element();
            array_get_expr = copy_type.array_get_size();
            dimension_str = "[" + array_get_expr.prettyprint() + "]";
            ArrayExpression << dimension_str;
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
        dimension_str = "[" + cp_size.prettyprint() + "]";
        ArrayExpression <<  dimension_str;
        //elems = const_value_cast_to_4(cp_size.get_constant());
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Region Expression:" << ArrayExpression.get_source() << std::endl;
        std::cerr << std::endl << std::endl;
#endif
        return ArrayExpression;

    }
#if _DEBUG_AUTOMATIC_COMPILER_
    fprintf(stderr,"ERROR in dimensions! :%d\n",type.get_num_dimensions());
#endif

    return ArrayExpression;

}

static Source get_copy_elements_all_dimensions_src(TL::Type copy_type, TL::Type type, bool is_only_pointer)
{
    Source ArrayExpression;
    std::string  dimension_str;

    if (is_only_pointer)
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        fprintf(stderr, "Pointer declaration  is array, num dimensions:\n");
        std::cerr << type.print_declarator() << std::endl;
#endif
        dimension_str = "( 1 )";
        ArrayExpression << dimension_str;
        return ArrayExpression;
    }
    else if (copy_type.is_array()) //it's a shape
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        fprintf(stderr, "Type declaration  is array, num dimensions %d\n" , copy_type.get_num_dimensions());
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
    internal_error("ERROR! :%d\n",type.get_num_dimensions());
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

static int num_parameters(const ObjectList<Symbol> param_list)
{

   ObjectList<Symbol>::const_iterator it_start = param_list.begin();
   ObjectList<Symbol>::const_iterator it_end = param_list.end();
   return it_end - it_start;

}

/*
 * Create wrapper function for HLS to unpack streamed arguments
 *
 */
void DeviceFPGA::gen_hls_wrapper(const Symbol &func_symbol, ObjectList<OutlineDataItem*>& data_items,
    const bool creates_children_tasks, const std::string wrapper_func_name,
    Source &wrapper_decls, Source &wrapper_source)
{
    //Check that we are calling a function task (this checking may be performed earlyer in the code)
    if (!func_symbol.is_function()) {
        fatal_error("Only function-tasks are supported at this moment");
    }

    Scope func_scope = func_symbol.get_scope();
    ObjectList<Symbol> param_list = func_symbol.get_function_parameters();
    int size = num_parameters(param_list);
    char function_parameters_passed[size];

    memset(function_parameters_passed, 0, size);

    /*
     * The wrapper function must have:
     *      An input and an output parameters
     *      with respective pragmas needed for streaming
     *      For each scalar parameter, another scalar patameter
     *      IN THE SAME ORDER AS THE ORIGINAL FUNCTION as long as we are generating
     *      scalar parameter passing based on original function task parameters
     */

    //Source wrapper_params;
    std::string in_dec, out_dec;
    get_inout_decl(data_items, in_dec, out_dec);
    Source pragmas_src, params_src, clear_components_count, var_decls_src, aux_decls_src;

    var_decls_src
        << "typedef ap_axis<64,1,1,5> axiData_t;"
        << "typedef hls::stream<axiData_t> axiStream_t;"
        << "typedef uint64_t counter_t;"
        << "typedef uint64_t nanos_wd_t;"
        << "extern const uint8_t " << STR_ACCID << ";"
        << "static nanos_wd_t " << STR_TASKID << ";"
        << "static uint64_t " << STR_INSTRCOUNTER << ", " << STR_INSTRBUFFER << ";"
        << "static unsigned int " << STR_INSTRSLOTS << ", " << STR_INSTRCURRENTSLOT << ";"
        << "static int " << STR_INSTROVERFLOW << ";"
    ;

    aux_decls_src
        << "void write_stream(axiStream_t &stream, uint64_t data, unsigned short dest, unsigned char last) {"
        << "#pragma HLS INTERFACE axis port=stream\n"
        << "\taxiData_t __data = {0, 0, 0, 0, 0, 0, 0};"
        << "\t__data.keep = 0xFF;"
        << "\t__data.dest = dest;"
        << "\t__data.last = last;"
        << "\t__data.data = data;"
        << "\tstream.write(__data);"
        << "}"
        //<< "uint64_t read_stream(axiStream_t &stream) {"
        //<< "#pragma HLS INTERFACE axis port=stream\n"
        //<< "\treturn stream.read().data;"
        //<< "}"
    ;

    params_src
        << "axiStream_t& " << STR_INPUTSTREAM
        << ", axiStream_t& " << STR_OUTPUTSTREAM
    ;

    pragmas_src
        << "#pragma HLS INTERFACE ap_ctrl_none port=return\n"
        << "#pragma HLS INTERFACE axis port=" << STR_INPUTSTREAM << "\n"
        << "#pragma HLS INTERFACE axis port=" << STR_OUTPUTSTREAM << "\n"
    ;

    if (instrumentation_enabled())
    {
        //instrumentation declarations/definitions
        var_decls_src
            << "extern volatile counter_t * " << STR_INSTRDATA << ";"

            << "typedef struct {"
            << "    uint64_t eventId;"
            << "    uint64_t value;"
            << "    uint64_t timestamp;"
            << "}" << STR_EVENTSTRUCT << ";"

            << "typedef enum {"
            STR_PREFIX << "_EVENT_TYPE_BURST_OPEN = 0,\n"
            STR_PREFIX << "_EVENT_TYPE_BURST_CLOSE = 1,\n"
            STR_PREFIX << "_EVENT_TYPE_POINT = 2,\n"
            STR_PREFIX << "_EVENT_TYPE_LAST = 0XFFFFFFFF\n"
            << "} " << STR_EVENTTYPE << ";"
            << "#define " << STR_PREFIX << "_EVENT_VAL_OK   0\n"
            << "#define " << STR_PREFIX << "_EVENT_VAL_OVERFLOW   0xFFFFFFFF\n"
        ;

        //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
        aux_decls_src
            << "counter_t get_time() {"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRDATA << "\n"
            << "#pragma HLS inline off\n"
            << "return *(" STR_INSTRDATA " + ( " STR_INSTRCOUNTER "/sizeof(counter_t)));"
            << "}"
            //write events

            << "void " << STR_WRITEEVENT << "(uint32_t event, uint64_t val, uint32_t type) {"
            << "#pragma HLS inline off\n"
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRDATA << "\n"

            << "\t" << STR_EVENTSTRUCT << " fpga_event;"
            //Save last slot for end status
            //In case of instrumentation buffer size = 0, do not emit events
            << "\tif (" << STR_INSTRCURRENTSLOT << "< (int)" << STR_INSTRSLOTS << "-1) {"
            << "\t\tfpga_event.eventId = ((uint64_t)type<<32) | event;"
            << "\t\tfpga_event.value = val;"
            << "\t\tfpga_event.timestamp = get_time();"
            << "\t\tmemcpy((void*)(mcxx_data + ((" << STR_INSTRBUFFER " + " << STR_INSTRCURRENTSLOT
            << " * " "sizeof(" << STR_EVENTSTRUCT << "))/sizeof(uint64_t))),"
            << "&fpga_event, sizeof(" << STR_EVENTSTRUCT << "));"
            << STR_INSTRCURRENTSLOT << "++;"
            << "\t} else if (" << STR_INSTRCURRENTSLOT << " == " STR_INSTRSLOTS "  -1) {" //last event overflow
            << "\t\t" << STR_INSTROVERFLOW << "++;"   //First time overflow=1
            << "\t\tfpga_event.eventId = ((uint64_t) " << STR_PREFIX << "_EVENT_TYPE_LAST << 32) | " << STR_INSTROVERFLOW << ";"
            << "\t\tfpga_event.value = " << STR_INSTROVERFLOW << ";"
            << "\t\tmemcpy((void *)(" << STR_INSTRDATA " + ((" << STR_INSTRBUFFER << " + " << STR_INSTRCURRENTSLOT << " * sizeof(" << STR_EVENTSTRUCT << "))/sizeof(uint64_t))),"
            << "&fpga_event, sizeof(" << STR_EVENTSTRUCT << "));"
            << "\t}"
            << "}"

            << "void nanos_instrument_burst_begin(uint32_t event, uint64_t value) {"
            << "#pragma HLS inline\n"
            << STR_WRITEEVENT << "(event, value, " << STR_PREFIX << "_EVENT_TYPE_BURST_OPEN );"
            << "}"

            << "void nanos_instrument_burst_end(uint32_t event, uint64_t value) {"
            << "#pragma HLS inline\n"
            << STR_WRITEEVENT << "(event, value, " << STR_PREFIX << "_EVENT_TYPE_BURST_CLOSE );"
            << "}"

            << "void nanos_instrument_point_event(uint32_t event, uint64_t value) {"
            << "#pragma HLS inline\n"
            << STR_WRITEEVENT << "(event, value, " << STR_PREFIX << "_EVENT_TYPE_POINT );"
            << "}"


            << "void " << STR_INSTREND << "() {"
            << "#pragma HLS inline\n"
            << STR_WRITEEVENT << "(" << STR_PREFIX << "_EVENT_TYPE_LAST, " << STR_PREFIX << "_EVENT_VAL_OK, " << STR_PREFIX << "_EVENT_TYPE_LAST );"
            //<< "    _emit_xtasks_ins_event(EVENT_ID_LAST, EVENT_VAL_OK, XTASKS_EVENT_TYPE_LAST);"
            << "}"
        ;

        pragmas_src
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRDATA << "\n"
        ;
    }
    else
    {
        //Define empty instrument calls when instrumentation is not enabled
        aux_decls_src
            << "void nanos_instrument_burst_begin(uint32_t event, uint64_t value) {"
            << "}"

            << "void nanos_instrument_burst_end(uint32_t event, uint64_t value) {"
            << "}"

            << "void nanos_instrument_point_event(uint32_t event, uint64_t value) {"
            << "}"
        ;
    }

    if (creates_children_tasks)
    {
        var_decls_src
            << "extern ap_uint<72> " << STR_GLOB_OUTPORT << ";"
            << "extern volatile ap_uint<2> " << STR_GLOB_TWPORT << ";"
            << "static ap_uint<32> " << STR_COMPONENTS_COUNT << ";"
        ;

        aux_decls_src
            << get_aux_task_creation_source()
            << get_nanos_wait_completion_source()
            << get_nanos_create_wd_source()
            << get_mcxx_ptr_source()
        ;

        pragmas_src
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_OUTPORT << "\n"
            << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_TWPORT << "\n"
        ;

        clear_components_count
            << "\t\t" << STR_COMPONENTS_COUNT << " = 0;"
        ;
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
    Source in_copies, out_copies;
    Source in_copies_aux, out_copies_aux;
    Source fun_params;
    Source local_decls;
    Source profiling_0;
    Source profiling_1;
    Source profiling_2;
    Source profiling_3;
    Source sync_output_code;
    Source end_instrumentation;
    Source generic_initial_code;

    in_copies_aux
        << "\t\t__copyFlags_id = " << STR_INPUTSTREAM << ".read().data;"
        << "\t\t__copyFlags = __copyFlags_id;"
        << "\t\t__param_id = __copyFlags_id >> 32;"
        << "\t\tswitch (__param_id) {"
    ;

    out_copies_aux
        << "\t\t__copyFlags_id = __copyFlags_id_out[__i];"
        << "\t\t__copyFlags = __copyFlags_id; "
        << "\t\t__param_id = __copyFlags_id >> 32;"
        << "\t\t__param = __param_out[__i];"
        << "\t\tswitch (__param_id) {"
    ;

    int n_params_id = 0;
    int n_params_in = 0;
    int n_params_out = 0;

    // Go through all the data items
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin(); it != data_items.end(); it++)
    {
        const std::string &field_name = (*it)->get_field_name();
        fun_params.append_with_separator(field_name, ", ");

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Variable: " << field_name << std::endl;
        std::cerr << std::endl << std::endl;
#endif

        TL::Symbol param_symbol = (*it)->get_field_symbol();
        const Scope &scope = (*it)->get_symbol().get_scope();
        const ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();
        const ObjectList<Nodecl::NodeclBase> &localmem = (*it)->get_localmem();

        if (!localmem.empty() && !creates_children_tasks)
        {
            TL::Symbol symbol_copy = (*it)->get_field_symbol();
            Nodecl::NodeclBase expr_base = (*it)->get_base_address_expression();

            if (copies.size() > 1 || localmem.size() > 1)
            {
                internal_error("Only one copy/localmem per object (in/out/inout) is allowed (%s)",
                localmem.front().get_locus_str().c_str());
            }

            const Type& field_type = (*it)->get_field_type().no_ref();
            const Type &type = localmem.front().get_type().no_ref();
            std::string field_simple_decl = field_type.get_simple_declaration(scope, field_name);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "filed type declaration: " << field_simple_decl << std::endl;
#endif

            std::string type_simple_decl = type.get_simple_declaration(scope, field_name);
            size_t position = type_simple_decl.find("[");
            bool is_only_pointer = (position == std::string::npos);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "type declaration: " << type_simple_decl << " position: " << position << std::endl;
            std::cerr << std::endl << std::endl;
#endif

            Source dimensions_array;
            Source dimensions_pointer_array;
            Source n_elements_src;
            Type elem_type;
            Type basic_elem_type;
            std::string basic_elem_type_name;

            if (field_type.is_pointer() || field_type.is_array() || field_type.array_is_region())
            {
                n_elements_src = get_copy_elements_all_dimensions_src(type, field_type, is_only_pointer);
                dimensions_array = get_type_arrays_src(type, field_type, is_only_pointer);
                dimensions_pointer_array = get_type_pointer_to_arrays_src(type, field_type, is_only_pointer);
            }
            else
            {
                internal_error("Invalid field type, only pointer and array is allowed (%d)",
                localmem.front().get_locus_str().c_str());
            }

            if (is_only_pointer)
            {
                elem_type = field_type.points_to();
                basic_elem_type = field_type.basic_type();
                basic_elem_type_name = basic_elem_type.print_declarator();
            }
            else if (type.is_array())
            {
                elem_type = type.array_element();
                basic_elem_type = type.basic_type();
                basic_elem_type_name = basic_elem_type.print_declarator();
            }
            else
            {
                internal_error("Invalid type for input/output, only pointer and array is allowed (%d)",
                localmem.front().get_locus_str().c_str());
            }

            std::string par_simple_decl = elem_type.get_simple_declaration(scope, field_name);
            TL::Type basic_par_type = field_type.basic_type();
            std::string basic_par_type_decl = basic_par_type.print_declarator();

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "BASIC PAR TYPE DECL :" << basic_par_type_decl << std::endl;
#endif

            std::string par_decl = field_type.get_declaration(scope, field_name);
            TL::Type field_type_points_to;

            if (field_type.is_pointer())
            {
                field_type_points_to = field_type.points_to();
            }
            else if (field_type.is_pointer_to_class())
            {
                field_type_points_to = field_type.pointed_class();
            }

            std::string pointed_to_string = field_type_points_to.print_declarator();
            std::string pointed_to_string_simple = field_type_points_to.get_simple_declaration(scope, field_name);
            position = par_simple_decl.rfind(field_name);
            std::string type_par_decl = par_simple_decl.substr(0, position);
            std::string type_basic_par_decl = get_element_type_pointer_to(field_type, field_name, scope);

            int param_id = find_parameter_position(param_list, param_symbol);
            function_parameters_passed[param_id] = 1;
            n_params_id++;

            local_decls
                << "\tstatic " << type_basic_par_decl << " " << field_name << dimensions_array << ";"
            ;

            const std::string field_port_name = STR_PREFIX + field_name;
            const std::string field_name_param = type_basic_par_decl + " *" + field_port_name;

            if (copies.empty() || copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {

                const std::string field_port_name_i = field_port_name + "_i";
                const std::string field_port_name_o = field_port_name + "_o";
                const std::string field_name_param_i = field_name_param + "_i";
                const std::string field_name_param_o = field_name_param + "_o";

                params_src.append_with_separator(field_name_param_i, ", ");
                params_src.append_with_separator(field_name_param_o, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name_i << "\n"
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name_o << "\n"
                ;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                    << "\t\t\t\tif(__copyFlags[4])\n"
                    << "\t\t\t\t\tmemcpy(" << field_name << ", (const " << type_basic_par_decl << " *)(" << field_port_name_i << " + __param/sizeof(" << type_basic_par_decl << ")), " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\t__copyFlags_id_out[" << n_params_out << "] = __copyFlags_id;"
                    << "\t\t\t\t__param_out[" << n_params_out << "] = __param;"
                ;

                out_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(__copyFlags[5])\n"
                    << "\t\t\t\t\tmemcpy(" << field_port_name_o <<  " + __param/sizeof(" << type_basic_par_decl << "), (const " << type_basic_par_decl << " *)" << field_name << ", " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;
                n_params_out++;
            }
            else if (copies.front().directionality == OutlineDataItem::COPY_IN)
            {
                params_src.append_with_separator(field_name_param, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
                ;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                    << "\t\t\t\tif(__copyFlags[4])\n"
                    << "\t\t\t\t\tmemcpy(" << field_name << ", (const " << type_basic_par_decl << " *)(" << field_port_name << " + __param/sizeof(" << type_basic_par_decl << ")), " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                ;

                n_params_in++;
            }
            else if (copies.front().directionality == OutlineDataItem::COPY_OUT)
            {
                params_src.append_with_separator(field_name_param, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
                ;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t__copyFlags_id_out[" << n_params_out << "] = __copyFlags_id;"
                    << "\t\t\t\t__param_out[" << n_params_out << "] = " << STR_INPUTSTREAM << ".read().data;"
                ;

                out_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(__copyFlags[5])\n"
                    << "\t\t\t\t\tmemcpy( " << field_port_name <<  " + __param/sizeof(" << type_basic_par_decl << "), (const " << type_basic_par_decl << " *)" << field_name << ", " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_out++;
            }
            else {
                internal_error("Copy type not valid", 0);
            }

            in_copies_aux
                << "\t\t\t\tbreak;"
            ;
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

        local_decls
            << "\t" << param_decl << ";"
        ;

        if (param_type.is_pointer() || param_type.is_array())
        {
            TL::Type elem_type = param_type.is_pointer() ? param_type.points_to() : param_type.array_element();
            const std::string casting_pointer = unql_type.get_declaration(scope, "");
            const std::string casting_sizeof = elem_type.get_declaration(scope, "");
            const std::string port_name = STR_PREFIX + param_name;
            const std::string port_declaration = unql_type.get_declaration(scope, port_name, TL::Type::PARAMETER_DECLARATION);

            params_src.append_with_separator(port_declaration, ", ");

            pragmas_src
                << "#pragma HLS INTERFACE m_axi port=" << port_name << "\n"
            ;

            in_copies_aux
                << "\t\t\tcase " << param_id << ":\n"
                << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                << "\t\t\t\t" << param_name << " = (" << casting_pointer << ")"
                << "\t\t\t\t(" << port_name << " + __param/sizeof(" << casting_sizeof << "));"
                << "\t\t\t\tbreak;"
            ;
        }
        else if (param_type.is_scalar_type())
        {
            in_copies_aux
                << "\t\t\tcase " << param_id << ":\n"
                << "\t\t\t\tunion {"
                << "\t\t\t\t\t" << unql_type.get_declaration(scope, param_name) << ";"
                << "\t\t\t\t\tuint64_t "<< param_name << "_task_arg;"
                << "\t\t\t\t} mcc_arg_" << param_id << ";"
                << "\t\t\t\tmcc_arg_" << param_id << "." << param_name << "_task_arg = " << STR_INPUTSTREAM << ".read().data;"
                << "\t\t\t\t" << param_name << " = mcc_arg_" << param_id << "." << param_name << ";"
                << "\t\t\t\tbreak;"
            ;
        }
        else if (creates_children_tasks && is_mcxx_ptr_t)
        {
            in_copies_aux
                << "\t\t\tcase " << param_id << ":\n"
                << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                << "\t\t\t\t" << param_name << " = __param;"
                << "\t\t\t\tbreak;"
            ;
        }
        else
        {
            error_printf_at(it->get_locus(),
                "Unsupported data type '%s' in fpga task parameter '%s'\n",
                print_declarator(param_type.get_internal_type()), param_name.c_str()
            );
            internal_error("Unsupported parameter type for fpga task", 0);
        }

        //function_parameters_passed[param_id] = 1;
        n_params_in++;
        n_params_id++;
    }

    in_copies
        << "\tfor (__i = 0; __i < " << n_params_id << "; __i++) {"
        << in_copies_aux
        << "\t\t\tdefault:;"
        << "\t\t}"
        << "\t}"
    ;

    if (n_params_out)
    {
        out_copies
            << "\tfor (__i = 0; __i < (" << n_params_out << "); __i++) {"
            << out_copies_aux
            << "\t\t\tdefault:;"
            << "\t\t}"
            << "\t}"
            << "\n"
        ;
    }

    if (instrumentation_enabled())
    {
        local_decls
            << "\tcounter_t  __counter_reg[4] = {0xA, 0xBAD, 0xC0FFE, 0xDEAD};"
        ;
    }

    local_decls
        << "\tunsigned int __i;"
        << "\tuint64_t __accHeader;"
        << "\tap_uint<8> __copyFlags, __destID;"
        << "\tuint32_t __comp_needed;"
        << "\tunsigned long long __param, __copyFlags_id;"
        << "\tunsigned int __param_id, __n_params_in, __n_params_out;"
        << "\tuint64_t __bufferData;"
    ;

    if (n_params_out)
    {
        local_decls
            << "\tunsigned long long __copyFlags_id_out[" << n_params_out << "];"
            << "\tunsigned long long __param_out[" << n_params_out << "];"
        ;
    }

    if (instrumentation_enabled())
    {
        profiling_0 //copy in begin
            << "\tnanos_instrument_burst_begin(" << EV_DEVCOPYIN << ", " << STR_TASKID << ");"
        ;

        profiling_1 //copy in end, task exec begin
            << "nanos_instrument_burst_end(" << EV_DEVCOPYIN << ", " << STR_TASKID << ");"
            << "nanos_instrument_burst_begin(" << EV_DEVEXEC << ", " << STR_TASKID << ");"
        ;

        profiling_2 //task exec end, copy out end
            << "nanos_instrument_burst_end(" << EV_DEVEXEC << ", " << STR_TASKID << ");"
            << "nanos_instrument_burst_begin(" << EV_DEVCOPYOUT << ", " << STR_TASKID << ");"
        ;

        profiling_3 //copy out end
            << "nanos_instrument_burst_end(" << EV_DEVCOPYOUT << ", " << STR_TASKID << ");"
        ;

    }

    generic_initial_code
        << "\t" << STR_TASKID << " = " << STR_INPUTSTREAM << ".read().data;"
        << "\t" << STR_INSTRCOUNTER << " = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__bufferData = " << STR_INPUTSTREAM << ".read().data;"
        //<< "\t" << STR_INSTRBUFFER << " = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__accHeader = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__comp_needed = __accHeader;"
        << "\t__destID = __accHeader>>32;"
    ;

    if (instrumentation_enabled())
    {
        generic_initial_code
            << "\t" << STR_INSTRBUFFER << " = __bufferData & ((1ULL<<48)-1);" //48 lower bits
            << "\t" << STR_INSTRSLOTS << " =__bufferData >> 48;"
            << "\t" << STR_INSTROVERFLOW << " = 0;"
            << "\t" << STR_INSTRCURRENTSLOT << " = 0;"
        ;

    }

    sync_output_code
        << "\tend_task: {"
        << "\t#pragma HLS PROTOCOL fixed\n"
        << "\t\tend_acc_task(" << STR_OUTPUTSTREAM << ", __destID);"
        << "\t}"
        << " "
    ;

    if (instrumentation_enabled())
        end_instrumentation << STR_INSTREND << "();" ;

    aux_decls_src
        << "void end_acc_task(axiStream_t& stream, uint32_t destId) {"
        << end_instrumentation
        << "\tuint64_t data = " << STR_ACCID << ";"
        << "\tdata = (data << 56) | (" << STR_TASKID << " & 0x00FFFFFFFFFFFFFF);"
        << "\twrite_stream(stream, data, destId, 1);"
        << "}"
    ;

    wrapper_source
        << "void " << wrapper_func_name << "(" << params_src << ") {"
        << pragmas_src
        << "\n"
        << local_decls
        << "\n"
        << generic_initial_code
        << "\n"
        << profiling_0
        << "\n"
        << in_copies
        << "\n"
        << profiling_1
        << "\n"
        << "\tif (__comp_needed) {\n"
        << clear_components_count
        << "\t\t" << func_symbol.get_name() << "(" << fun_params << ");"
        << "\t}"
        << profiling_2
        << "\n"
        << out_copies
        << profiling_3
        << "\n"
        << sync_output_code
        << "\n"
        << "}"
    ;

    wrapper_decls
        << var_decls_src
        << aux_decls_src
    ;
}

void DeviceFPGA::copy_stuff_to_device_file(const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied)
{
    _stuff_to_copy.insert(stuff_to_be_copied);
}

ObjectList<Source> DeviceFPGA::get_called_functions_sources(const TL::ObjectList<Nodecl::NodeclBase>& function_code)
{
#if _DEBUG_AUTOMATIC_COMPILER_
    __number_of_calls++;
#endif
    ObjectList<Source> result;

    for (TL::ObjectList<Nodecl::NodeclBase>::const_iterator it = function_code.begin();
        it != function_code.end(); ++it)
    {
        if (it->is<Nodecl::FunctionCode>() || it->is<Nodecl::TemplateFunctionCode>())
        {
            TL::ObjectList<Nodecl::Symbol> called_function_code =
                Nodecl::Utils::get_nonlocal_symbols_first_occurrence(*it);
            for (TL::ObjectList<Nodecl::Symbol>::const_iterator its = called_function_code.begin();
                its != called_function_code.end(); ++its)
            {
                TL::Symbol sym = its->get_symbol();
                std::string original_filename = TL::CompilationProcess::get_current_file().get_filename(/* fullpath */ true);

                if (sym.is_function() && (sym.get_filename() == original_filename))
                {
                    Nodecl::NodeclBase code = sym.get_function_code();

                    if (!code.is_null())
                    {
                        TL::ObjectList<Nodecl::NodeclBase> called_function_code_list;
                        called_function_code_list.insert(code);

                        result = get_called_functions_sources(called_function_code_list);

                        Source called_function_code_src;
                        called_function_code_src << code.prettyprint();
                        result.insert(called_function_code_src);

#if _DEBUG_AUTOMATIC_COMPILER_
                        std::cerr << std::endl << std::endl;
                        std::cerr << " ===================================================================0\n";
                        std::cerr << "call " << __number_of_calls << ": " + sym.get_name() + " new function called and expanded\n";
                        std::cerr << " ===================================================================0\n";
#endif
                    }
                }
            }
        }
    }
#if _DEBUG_AUTOMATIC_COMPILER_
    __number_of_calls--;
#endif
    return result;
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
        }

        if (onto_clause[0].is_constant())
        {
            const_value_t *ct_val = onto_val.get_constant();
            if (!const_value_is_integer(ct_val))
            {
                error_printf_at(onto_val.get_locus(), "Constant is not integer type in onto clause\n");
            }
            else
            {
                int acc = const_value_cast_to_signed_int(ct_val);
                value = std::to_string(acc);
            }
        }
        else
        {
            if (onto_val.get_symbol().is_valid())
            {
                value = as_symbol(onto_val.get_symbol());
                //Nodecl::Utils::SimpleSymbolMap param_to_args_map = info._target_info.get_param_arg_map();
                //as_symbol(param_to_args_map.map(onto_val.get_symbol()));
            }
        }
    }
    else
    {
        // Not using the line number to allow future modifications of source code without
        // afecting the accelerator hash
        std::stringstream type_str;
        type_str << task.get_filename() << " " << task.get_name();
        value = std::to_string(simple_hash_str(type_str.str().c_str()));
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
            }
            else
            {
                int acc_instances = const_value_cast_to_signed_int(ct_val);
                std::stringstream tmp_str_instances;
                tmp_str_instances << acc_instances;
                value = tmp_str_instances.str();
                if (acc_instances <= 0)
                    error_printf_at(numins_value.get_locus(), "Constant in num_instances should be an integer longer than 0\n");
            }
        }
        else
        {
            error_printf_at(numins_value.get_locus(), "num_instances clause does not contain a constant expresion\n");
        }
    }
    return value;
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

    std::string acc_type;

    Source arch_mask;

    // Check devices of all implementations
    for (OutlineInfo::implementation_table_t::const_iterator it = implementation_table.begin();
            it != implementation_table.end();
            ++it)
    {
        const TargetInformation& target_info = it->second;
        acc_type = get_acc_type(called_task, target_info);

        const ObjectList<std::string>& devices = target_info.get_device_names();
        for (ObjectList<std::string>::const_iterator it2 = devices.begin();
                it2 != devices.end();
                ++it2)
        {
            //Architecture mask is a 24b bitmask where:
            //  - 23th bit is set if task has SMP support
            //  - 22th bit is set if task has FPGA support
            std::string device_name = *it2;
            if (device_name == "smp")
            {
                arch_mask.append_with_separator("NANOS_FPGA_ARCH_SMP", " | ");
            }
            else if (device_name == "fpga")
            {
                arch_mask.append_with_separator("NANOS_FPGA_ARCH_FPGA", " | ");
            }
            else
            {
                fatal_error("FPGA device only can create tasks for smp and fpga devices.\n");
            }
        }
    }

    Source spawn_code, args_list, args_flags_list, copies_list;
    size_t num_args = 0, num_deps = 0, num_copies = 0;

    // Go through all the arguments and fill the arguments_list
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        if (!(*it)->get_symbol().is_valid())
            continue;

        bool is_dep = false;
        Source arg_flags;
        arg_flags.append_with_separator("NANOS_ARGFLAG_NONE", " | "); // Default argument flag
        TL::ObjectList<OutlineDataItem::DependencyItem> dependences = (*it)->get_dependences();
        for (TL::ObjectList<OutlineDataItem::DependencyItem>::iterator dep_it = dependences.begin();
                dep_it != dependences.end();
                dep_it++)
        {
            if ((dep_it->directionality & OutlineDataItem::DEP_IN) == OutlineDataItem::DEP_IN)
            {
                arg_flags.append_with_separator("NANOS_ARGFLAG_DEP_IN", " | ");
                is_dep = true;
            }
            if ((dep_it->directionality & OutlineDataItem::DEP_OUT) == OutlineDataItem::DEP_OUT)
            {
                arg_flags.append_with_separator("NANOS_ARGFLAG_DEP_OUT", " | ");
                is_dep = true;
            }
            if ((dep_it->directionality & (~OutlineDataItem::DEP_INOUT)) != OutlineDataItem::DEP_NONE)
            {
                fatal_printf_at((*it)->get_base_address_expression().get_locus(),
                    "Only in/out/inout dependences are suported when creating a task in a FPGA device\n");
            }
        }
        num_deps += is_dep;

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
                        else
                        {
                            sym_type = sym_type.no_ref().get_unqualified_type();
                            if ((*it)->get_captured_value().is_null())
                            {
                                if (IS_CXX_LANGUAGE
                                        && (sym_type.is_dependent()
                                            || (sym_type.is_class()
                                                && !sym_type.is_pod())))
                                {
                                    fatal_error("This kind of argument is not supported yet\n");
                                }
                                else
                                {
                                    // Plain assignment is enough
                                    arg_value
                                        << "(uintptr_t)("
                                        << as_symbol((*it)->get_symbol())
                                        << ")";
                                }
                            }
                            else
                            {
                                Nodecl::NodeclBase captured = (*it)->get_captured_value();
                                Nodecl::NodeclBase condition = (*it)->get_conditional_capture_value();
                                if (!condition.is_null())
                                {
                                    warn_printf_at(condition.get_locus(),
                                        "Conditional capture not supported yet. Ignoring it.\n");
                                }

                                if (IS_CXX_LANGUAGE
                                        && (sym_type.is_dependent()
                                            || (sym_type.is_class()
                                                && !sym_type.is_pod())))
                                {
                                    fatal_error("This kind of argument is not supported yet\n");
                                }
                                else
                                {
                                    arg_value
                                        << "(uintptr_t)("
                                        << as_expression(captured.shallow_copy())
                                        << ")";
                                }
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
        args_flags_list.append_with_separator( arg_flags, ", " );
        ++num_args;

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
            }

            Nodecl::NodeclBase address_of_object = copy_expr.get_address_of_symbol();
            int input = ((copy_it->directionality & OutlineDataItem::COPY_IN) == OutlineDataItem::COPY_IN);
            int output = ((copy_it->directionality & OutlineDataItem::COPY_OUT) == OutlineDataItem::COPY_OUT);

            Source copy_value;
            copy_value << "{"
                << "  .address = (uintptr_t)(" << as_expression(address_of_object) << ")"
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
                << ", .size = "
                << "(" << as_expression(dims_sizes[0].shallow_copy()) << ") * sizeof(" << as_type(base_type) << ")"
                << ", .offset = "
                << "(" << as_expression(lower_bounds[0].shallow_copy()) << ") * sizeof(" << as_type(base_type) << ")"
                << ", .accessed_length = "
                << "((" << as_expression(upper_bounds[0].shallow_copy()) << ") - ("
                << as_expression(lower_bounds[0].shallow_copy()) << ") + 1) * sizeof(" << as_type(base_type) << ")"
                << "}";

            copies_list.append_with_separator( copy_value, ", " );
            ++num_copies;
        }
    }

    if (!Nanos::Version::interface_is_at_least("fpga", 6))
        fatal_error("Your Nanos version is not supported for cration of tasks inside the FPGA. Please update your Nanos installation\n");

    if (num_args > 0)
    {
        spawn_code
            << "uint64_t mcxx_args[] = {"
            << args_list
            << "};"
            << "uint8_t mcxx_args_flags[] = {"
            << args_flags_list
            << "};";
    }

    if (num_copies > 0)
    {
        spawn_code
            << "nanos_fpga_copyinfo_t mcxx_copies[] = {"
            << copies_list
            << "};";
    }

    spawn_code
        << "nanos_fpga_create_wd_async(" << arch_mask << ", " << acc_type << ", " << num_deps << ", " << num_args << ", "
        << (num_args > 0 ? "mcxx_args, mcxx_args_flags" : "(uint64_t *)0, (uint8_t *)0") << ", "
        << num_copies << ", " << (num_copies > 0 ? "mcxx_copies" : "(nanos_fpga_copyinfo_t *)0") << ");";

    Nodecl::NodeclBase spawn_code_tree = spawn_code.parse_statement(construct);
    construct.replace(spawn_code_tree);

    if (_registered_tasks.find(acc_type) == _registered_tasks.end())
    {
        register_task_creation(construct, current_function, called_task, structure_symbol, outline_info, acc_type, num_copies);
        _registered_tasks.insert(acc_type);
    }
}

bool is_not_alnum(int charact) {
    return !std::isalnum(charact);
}

void DeviceFPGA::register_task_creation(
        Nodecl::NodeclBase construct,
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
            arg_type.append(TL::Type(TL::Type::get_unsigned_long_int_type()).get_pointer_to());

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

            outline_src
                << "{"
                << device_name << "_" << implementor_outline_name << "_unpacked"
                << "(" << unpacked_arguments << ");"
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
        <<   "nanos_fpga_register_wd_info(" << acc_type << ", " << num_devices << ", devices, " << reference_to_xlate << ");"
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

std::string DeviceFPGA::FpgaOutlineInfo::get_filename() const
{
    return _type + ":" + _num_instances + ":" + _name + "_hls_automatic_mcxx.cpp";
}

std::string DeviceFPGA::FpgaOutlineInfo::get_wrapper_name() const
{
    return fpga_wrapper_name(this->_name);
}

EXPORT_PHASE(TL::Nanox::DeviceFPGA);

/*--------------------------------------------------------------------
  (C) Copyright 2006-2014 Barcelona Supercomputing Center
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

#include "cxx-nodecl.h"
#include "cxx-graphviz.h"

#include "tl-nanos.hpp"
#include "tl-symbol-utils.hpp"

using namespace TL;
using namespace TL::Nanox;

const std::string DeviceFPGA::HLS_VPREF = "_hls_var_";
const std::string DeviceFPGA::HLS_I = HLS_VPREF + "i";
const std::string DeviceFPGA::hls_in = HLS_VPREF + "in";
const std::string DeviceFPGA::hls_out = HLS_VPREF + "out";
static int _base_acc_num = 0;

static std::string fpga_outline_name(const std::string &name) {
    return "fpga_" + name;
}

UNUSED_PARAMETER static void print_ast_dot(const Nodecl::NodeclBase &node) {
    std::cerr << std::endl << std::endl;
    ast_dump_graphviz(nodecl_get_ast(node.get_internal_nodecl()), stderr);
    std::cerr << std::endl << std::endl;
}

void DeviceFPGA::create_outline(CreateOutlineInfo &info, Nodecl::NodeclBase &outline_placeholder, Nodecl::NodeclBase &output_statements, Nodecl::Utils::SimpleSymbolMap* &symbol_map) {

    if (IS_FORTRAN_LANGUAGE)
        fatal_error("Fortran for FPGA devices is not supported yet\n");

    // Unpack DTO
    Lowering* lowering = info._lowering;
    const std::string& device_outline_name = fpga_outline_name(info._outline_name);
    // const Nodecl::NodeclBase& task_statements = info._task_statements;
    const Nodecl::NodeclBase& original_statements = info._original_statements;
    // bool is_function_task = info._called_task.is_valid();

    const TL::Symbol& arguments_struct = info._arguments_struct;
    const TL::Symbol& called_task = info._called_task;

    lowering->seen_fpga_task = true;

    symbol_map = new Nodecl::Utils::SimpleSymbolMap(&_copied_fpga_functions);
    _internal_symbol_map = *symbol_map;

    TL::Symbol current_function = original_statements.retrieve_context().get_related_symbol();
    if (current_function.is_nested_function()) {
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
                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(called_task, called_task.get_name());

                TL::Symbol new_function_wrapper = SymbolUtils::new_function_symbol_for_deep_copy(called_task, called_task.get_name() + "_hls_automatic_mcxx_wrapper");

                Source instrument_before, called_source, instrument_after;

                DeviceFPGA::gen_hls_wrapper(called_task, new_function, new_function_wrapper, info._data_items, instrument_before, called_source, instrument_after);

                Source outline_src;
                Source full_src;
                Source func_syn_output_code;
                Source func_read_profiling_code;
                Source func_write_profiling_code;

                func_syn_output_code
                    <<  "void end_acc_task(hls::stream<axiData> &" << STR_OUTPUTSTREAM << ", uint32_t accID, uint32_t __destID) {"
                    <<  "\taxiData __output = {0, 0xFF, 0, 0, 0, 0, 0};"
                    <<  "\t__output.data = accID;"
                    <<  "\t__output.dest = __destID;"
                    <<  "\t__output.last = 1;"
                    <<  ""
                    <<  "\toutStream.write(__output);"
                    <<  "}"
                    <<  "\n"
                    <<  "\n"
                ;

                func_read_profiling_code
                    << "counter_t get_time(const volatile counter_t * counter){"
                    << "\treturn *counter;"
                    << "}"
                ;

                func_write_profiling_code
                    <<  "void write_profiling_registers(counter_t * counter_dest, counter_t * counters_src) {"
                    <<  "\tmemcpy(counter_dest, counters_src, 4*sizeof(counter_t));"
                    <<  "}"
                    <<  ""
                    <<  ""
                ;


                Nodecl::NodeclBase fun_code = info._called_task.get_function_code();
                outline_src
                    << func_syn_output_code
                    << func_read_profiling_code
                    << func_write_profiling_code
                    << fun_code.prettyprint()
                    << " "
                    << " "
                    << instrument_before
                    << called_source
                    << instrument_after
                ;

                _copied_fpga_functions.add_map(called_task, new_function_wrapper);
                TL::ObjectList<Nodecl::NodeclBase> expand_code;
                TL::Symbol expand_function = original_statements.retrieve_context().get_related_symbol();
                Nodecl::NodeclBase code = info._called_task.get_function_code();

                expand_code.append(code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << std::endl << std::endl;
                std::cerr << " ===================================================================0\n";
                std::cerr << "First call to copy_stuff_to_device... going through:\n";
                std::cerr << " ===================================================================0\n";
                std::cerr << code.prettyprint(); __number_of_calls=1;
#endif

                copy_stuff_to_device_file_expand(expand_code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << " ===================================================================0\n";
                std::cerr << "End First call to copy_stuff_to_device... \n";
                std::cerr << " ===================================================================0\n";
                std::cerr << std::endl << std::endl;
#endif

                TL::Scope scope = code.retrieve_context();
                preappend_list_sources_and_reset(outline_src, full_src, scope);

                _fpga_source_codes.append(full_src);
                _fpga_source_name.append(_acc_type + ":" + _num_acc_instances + ":" +  called_task.get_name() + "_hls_automatic_mcxx");
            }
        }
        else if (IS_FORTRAN_LANGUAGE)
        {
            fatal_error("There is no fortran support for FPGA devices\n");
        }
        else
        {
            fatal_error("Inline tasks not supported yet\n");
        }
    }

    Source unpacked_arguments, private_entities;

    TL::ObjectList<OutlineDataItem*> data_items = info._data_items;

    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin(); it != data_items.end(); it++)
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

                    if (IS_C_LANGUAGE || IS_CXX_LANGUAGE) {
                        argument << "*(args." << (*it)->get_field_name() << ")";
                    } else {
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

    Source fpga_params;

    Source unpacked_source;
    unpacked_source
        << dummy_init_statements
        << private_entities
        << fpga_params
        << statement_placeholder(outline_placeholder)
        << dummy_final_statements
    ;

#if _DEBUG_AUTOMATIC_COMPILER_
    std::cerr << "unpacked source function:" << unpacked_source.get_source() << std::endl;
#endif

    // Add a declaration of the unpacked function symbol in the original source
    if (IS_CXX_LANGUAGE)
    {
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "unpacked function:" << unpacked_function.get_function_code().prettyprint() << std::endl;
#endif

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
            original_statements.get_locus(),
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

#if _DEBUG_AUTOMATIC_COMPILER_
    std::cerr << "outline function:" << outline_function.get_function_code().prettyprint() << std::endl;
    std::cerr << "outline source code:" << outline_src.get_source() << std::endl;
#endif

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

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "Before inserting prettyprint:" << original_statements.prettyprint() << std::endl;
            std::cerr << "Before inserting get text:" << original_statements.get_text() << std::endl;
#endif

            Nodecl::Utils::prepend_to_enclosing_top_level_location(original_statements, nodecl_decl);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "After inserting prettyprint:" << original_statements.prettyprint() << std::endl;
            std::cerr << "After inserting get text:" << original_statements.get_text() << std::endl;
#endif
        }
    }
}

DeviceFPGA::DeviceFPGA():DeviceProvider(std::string("fpga")) {
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
        "This is the parameter to activate the bitstream generation:ON/OFF",
        _bitstream_generation,
        "OFF");

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
        "This is the parameter to indicate where the user wants to use the dataflow optimization:ON/OFF",
        _dataflow,
        "OFF");

}

void DeviceFPGA::pre_run(DTO& dto) {
}

void DeviceFPGA::run(DTO& dto) {
    DeviceProvider::run(dto);

    if (_bitstream_generation == "ON")
    {
        _current_base_acc_num = _base_acc_num;
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
    // NOTE: Not using the line number to allow future modifications of source code without afecting the accelerator hash
    std::stringstream type_str;
    type_str << current_function.get_filename() << /*" " << current_function.get_line() <<*/ " " << qualified_name;
    _acc_type = std::to_string(simple_hash_str(type_str.str().c_str()));

    //get onto information
    ObjectList<Nodecl::NodeclBase> onto_clause = info._target_info.get_onto();
    Nodecl::Utils::SimpleSymbolMap param_to_args_map = info._target_info.get_param_arg_map();
    if (onto_clause.size() >= 1)
    {
        Nodecl::NodeclBase onto_acc = onto_clause[0];
        warn_printf_at(onto_acc.get_locus(),
            "The use of onto clause is no longer needed unless you have a collision between two FPGA tasks that yeld the same type hash\n");
        if (onto_clause.size() > 1)
        {
            error_printf_at(onto_acc.get_locus(),
                "The syntax 'onto(type, count)' is no longer supported. Use 'onto(type) num_instances(count)' instead\n");
        }

        if (onto_clause[0].is_constant())
        {
            const_value_t *ct_val = onto_acc.get_constant();
            if (!const_value_is_integer(ct_val))
            {
                error_printf_at(onto_acc.get_locus(), "Constant is not integer type in onto clause\n");
            }
            else
            {
                int acc = const_value_cast_to_signed_int(ct_val);
                _acc_type = std::to_string(acc);
            }
        }
        else
        {
            if (onto_acc.get_symbol().is_valid())
            {
                _acc_type = as_symbol(onto_acc.get_symbol());
                //as_symbol(param_to_args_map.map(onto_acc.get_symbol()));
            }
        }
    }

    //get num_instances information
    ObjectList<Nodecl::NodeclBase> numins_clause = info._target_info.get_num_instances();

    _num_acc_instances = "1";  //Default is 1 instance
    if (numins_clause.size() >= 1)
    {
        Nodecl::NodeclBase numins_acc = numins_clause[0];
        if (numins_clause.size() > 1)
        {
            warn_printf_at(numins_acc.get_locus(), "More than one argument in num_instances clause. Using only first one\n");
        }

        if (numins_acc.is_constant())
        {
            const_value_t *ct_val = numins_acc.get_constant();
            if (!const_value_is_integer(ct_val))
            {
                error_printf_at(numins_acc.get_locus(), "Constant is not integer type in onto clause: num_instances\n");
            }
            else
            {
                int acc_instances = const_value_cast_to_signed_int(ct_val);
                std::stringstream tmp_str_instances;
                tmp_str_instances << acc_instances;
                _num_acc_instances = tmp_str_instances.str();
                if (acc_instances <= 0)
                    error_printf_at(numins_acc.get_locus(), "Constant in num_instances should be an integer longer than 0\n");
                _base_acc_num += acc_instances;
#if _DEBUG_AUTOMATIC_COMPILER_
                fprintf(stderr," Accelerator base acc:%d\n",_base_acc_num);
#endif
            }
        }
        else
        {
            error_printf_at(numins_acc.get_locus(), "num_instances clause does not contain a constant expresion\n");
        }
    }
    else
    {
        _base_acc_num++;
#if _DEBUG_AUTOMATIC_COMPILER_
        fprintf(stderr," Accelerator base acc (no info found):%d\n",_base_acc_num);
#endif
    }

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
            << args_name << ".acc_num = " << _acc_type << ";"
        ;

        Source ancillary_device_description_2;
        ancillary_device_description_2
            << comment("device argument type")
            << "static nanos_fpga_args_t " << args_name << ";"
            << args_name << ".outline = (void(*)(void*)) " << extra_cast << " &" << qualified_name << ";"
            << args_name << ".acc_num = " << _acc_type << ";"
        ;

        device_descriptor
            << "{"
            << /* factory */ "&nanos_fpga_factory, &" << outline_name << "_args"
            << "}"
        ;

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Casting: qualified_name " << qualified_name << std::endl;
        std::cerr << "Casting: ancillary " << ancillary_device_description_2.get_source()<< std::endl;
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


void DeviceFPGA::preappend_list_sources_and_reset(Source outline_src, Source& full_src, TL::Scope scope)
{

    Source each;

    for (ObjectList<Source>::iterator it4 = _expand_fpga_source_codes.begin(); it4 != _expand_fpga_source_codes.end(); it4++)
    {
        full_src << *it4;
        each << *it4;
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << each.get_source() << "preappend new function called and expanded \n";
        std::cerr << std::endl << std::endl;
#endif

        each = TL::Source();
    }

    full_src << outline_src;
    _expand_fpga_source_codes = TL::ObjectList<Source>();

}

//write/close intermediate files, free temporal nodes, etc.
void DeviceFPGA::phase_cleanup(DTO& data_flow)
{

    if (!_fpga_source_codes.empty() && (_bitstream_generation=="ON"))
    {
        TL::ObjectList<Source>::iterator it;
        TL::ObjectList<std::string>::iterator it2;
        for (it = _fpga_source_codes.begin(), it2=_fpga_source_name.begin();
            it != _fpga_source_codes.end();
            it++, it2++)
        {
            std::string original_filename = TL::CompilationProcess::get_current_file().get_filename();
            std::string new_filename = (*it2) + ".cpp";

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

            hls_file << "/////////////////// Automatic IP Generated by OmpSs@FPGA compiler\n"
                     << "///////////////////\n"
                     << "// Top IP Function: "<< (*it2) << "_wrapper AccID: " << _acc_type << " #Instances: "<<_num_acc_instances << "\n"
                     << "///////////////////\n"
                     << "\n"
                     << "#include <stdint.h>\n"
                     << "#include <iostream>\n"
                     << "#include <string.h>\n"
                     << "#include <strings.h>\n"
                     << "#include <hls_stream.h>\n"
                     << "#include <ap_axi_sdata.h>\n"
                     << "\n"
                     << "\n"
                     << "typedef ap_axis<64,1,1,5> axiData;\n"
                     << "typedef uint64_t counter_t;\n"
                     << "\n"
                     << "\n"
                     << std::endl;

            add_included_fpga_files(hls_file);

            hls_file << it->get_source(true);

            hls_file.close();

            if(!CURRENT_CONFIGURATION->do_not_link)
            {
                //If linking is enabled, remove the intermediate HLS source file (like an object file)
                ::mark_file_for_cleanup(new_filename.c_str());
            }
        }

        // Do not forget the clear the code for next files
        _fpga_file_code = Nodecl::List();
        _fpga_source_codes = TL::ObjectList<Source>();
        // _expand_fpga_source_codes = TL::ObjectList<Source>();
    }
}

void DeviceFPGA::add_included_fpga_files(std::ostream &hls_file)
{
    ObjectList<IncludeLine> lines = CurrentFile::get_included_files();
    std::string fpga_header_exts[] = {".fpga\"", ".fpga.h\"", ".fpga.hpp\""};

    for (ObjectList<IncludeLine>::iterator it = lines.begin(); it != lines.end(); it++)
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

static void get_inout_decl(ObjectList<OutlineDataItem*>& data_items, std::string &in_type, std::string &out_type, std::string &in_addr_type, std::string &out_addr_type)
{
    in_type = "";
    out_type = "";
    in_addr_type = "";
    out_addr_type = "";
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
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


static std::string get_type_pointer_to(TL::Type type, std::string field_name, TL::Scope scope)
{
    TL::Type points_to = type;
    size_t position;

    if (points_to.is_pointer())
    {
        points_to = points_to.points_to();
    }
    else if (points_to.is_pointer_to_class())
    {
        points_to = points_to.pointed_class();
    }

    std::string pointed_to_string_simple = points_to.get_simple_declaration(scope,field_name);
    position = pointed_to_string_simple.rfind(field_name);
    std::string reference_type = pointed_to_string_simple.replace(position,field_name.length(),"TO_CHANGE_MCXX");

    return reference_type;
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

    std::string reference_type = pointed_to_string_simple.substr(0,position);

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
        fprintf(stderr, "Type declaration  is array, num dimensions\n");
        fprintf(stderr, "Type declaration  is array, num dimensions %d\n" , copy_type.get_num_dimensions());
#endif
        int total_dimensions = copy_type.get_num_dimensions();
        int n_dimensions=0;
        Nodecl::NodeclBase array_get_expr;
#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << "Type declaration  is array, num dimensions " << copy_type.array_get_size().prettyprint() << std::endl;
        std::cerr << "Type declaration  is array, num dimensions " << copy_type.array_get_size().prettyprint() << std::endl;
#endif
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
void DeviceFPGA::gen_hls_wrapper(const Symbol &called_task, const Symbol &func_symbol_original, const Symbol &func_symbol, ObjectList<OutlineDataItem*>& data_items, Source &wrapper_before, Source &called_source, Source &wrapper_after)
{
    //Check that we are calling a function task (this checking may be performed earlyer in the code)

    if (!func_symbol_original.is_function()) {
        fatal_error("Only function-tasks are supported at this moment");
    }

    Scope func_scope = func_symbol_original.get_scope();
    ObjectList<Symbol> param_list = called_task.get_function_parameters();
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
    std::string in_dec, out_dec, in_addr_dec, out_addr_dec;
    get_inout_decl(data_items, in_dec, out_dec, in_addr_dec, out_addr_dec);
    Source pragmas_src;

    Source args;
    args
        << "hls::stream<axiData> &" << STR_INPUTSTREAM << ", hls::stream<axiData> &" << STR_OUTPUTSTREAM << ", counter_t *" << STR_DATA << ", uint32_t accID"
    ;

    pragmas_src
        << "#pragma HLS INTERFACE ap_ctrl_none port=return\n"
        << "#pragma HLS INTERFACE axis port=" << STR_INPUTSTREAM << "\n"
        << "#pragma HLS INTERFACE axis port=" << STR_OUTPUTSTREAM << "\n"
        << "#pragma HLS INTERFACE m_axi port=" << STR_DATA << "\n"
    ;

    /*
     * Generate wrapper code
     * We are going to keep original parameter name for the original function
     *
     * input/outlut parameters are received concatenated one after another.
     * The wrapper must create local variables for each input/output and unpack
     * streamed input/output data into that local variables.
     *
     * Scalar parameters are going to be copied as long as no unpacking is needed
     */
    Source copies_src;
    Source in_copies, out_copies, out_copies_addr;
    Source in_copies_aux, out_copies_aux;
    Source fun_params;
    Source fun_params_wrapper;
    Source local_decls;
    Source profiling_0;
    Source profiling_1;
    Source profiling_2;
    Source profiling_3;
    Source sync_output_code;
    Source generic_initial_code;

    in_copies_aux
        << "\t\t__cached_id = " << STR_INPUTSTREAM << ".read().data;"
        << "\t\t__cached = __cached_id;"
        << "\t\t__param_id = __cached_id >> 32;"
        << "\t\t__addr = " << STR_INPUTSTREAM << ".read().data;"
        << "\t\tswitch (__param_id) {"
    ;

    out_copies_aux
        << "\t__cached_id = __cached_id_out[__i];"
        << "\t__cached = __cached_id; "
        << "\t__param_id = __cached_id >> 32;"
        << "\t__addr = __addr_out[__i];"
        << "\tswitch (__param_id) {"
    ;

    int n_params_id = 0;
    int n_params_in = 0;
    int n_params_out = 0;

    // Go through all the parameters. The iteration below goes through the copies.
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin(); it != data_items.end(); it++)
    {

        fun_params.append_with_separator((*it)->get_field_name(), ", ");
        const std::string &field_name = (*it)->get_field_name();

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Variable: " << field_name << std::endl;
        std::cerr << std::endl << std::endl;
#endif

        const Scope &scope = (*it)->get_symbol().get_scope();
        const ObjectList<OutlineDataItem::CopyItem> &copies = (*it)->get_copies();

        if (!copies.empty())
        {
            Nodecl::NodeclBase expr = copies.front().expression;
            TL::Symbol symbol_copy= (*it)->get_field_symbol();
            Nodecl::NodeclBase expr_base = (*it)->get_base_address_expression();

            if (copies.size() > 1)
            {
                internal_error("Only one copy per object (in/out/inout) is allowed (%s)",
                expr.get_locus_str().c_str());
            }

            const Type& field_type = (*it)->get_field_type().no_ref();
            const Type &type = expr.get_type().no_ref();
            std::string field_simple_decl = field_type.get_simple_declaration(scope, field_name);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "filed type declaration: " << field_simple_decl << std::endl;
#endif

            std::string type_simple_decl = type.get_simple_declaration(scope, field_name);
            size_t position = type_simple_decl.find("[");
            bool is_only_pointer = (position == std::string::npos);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << "type declaration: " << type_simple_decl << " position: " << position << std::endl;
#endif

            DataReference datareference(expr);

#if _DEBUG_AUTOMATIC_COMPILER_
            std::cerr << std::endl << std::endl;
            std::cerr << "expresion of copies.front " << expr.prettyprint() << std::endl;
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
                internal_error("ERROR!\n",0);
            }

            if (is_only_pointer)
            {
                elem_type = field_type.points_to();
                basic_elem_type=field_type.basic_type();
                basic_elem_type_name= basic_elem_type.print_declarator();
            }
            else if (type.is_array())
            {
                elem_type = type.array_element();
                basic_elem_type=type.basic_type();
                basic_elem_type_name= basic_elem_type.print_declarator();
            }
            else
            {
                internal_error("invalid type for input/output, only pointer and array is allowed (%d)",
                expr.get_locus_str().c_str());
            }

            std::string par_simple_decl = elem_type.get_simple_declaration(scope, field_name);
            TL::Type basic_par_type= field_type.basic_type();
            std::string basic_par_type_decl= basic_par_type.print_declarator();

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
            position=par_simple_decl.rfind(field_name);
            std::string type_par_decl = par_simple_decl.substr(0, position);
            std::string type_basic_par_decl = get_element_type_pointer_to(field_type, field_name, scope);

            if (copies.front().directionality == OutlineDataItem::COPY_INOUT)
            {

                const std::string field_port_name_i = STR_PREFIX + field_name + "_i";
                const std::string field_port_name_o = STR_PREFIX + field_name + "_o";

                local_decls
                    << "\t" << type_basic_par_decl << " " << field_name << dimensions_array << ";"
                ;

                const std::string field_name_param_i = type_basic_par_decl + "*" + field_port_name_i;
                const std::string field_name_param_o = type_basic_par_decl + "*" + field_port_name_o;

                fun_params_wrapper.append_with_separator(field_name_param_i, ", ");
                fun_params_wrapper.append_with_separator(field_name_param_o, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name_i << "\n"
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name_o << "\n"
                ;

                TL::Symbol param_symbol = (*it)->get_field_symbol();
                int param_id = find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(!__cached[4])\n"
                    << "\t\t\t\t\tmemcpy(" << field_name << ", (const " << type_basic_par_decl << " *)(" << field_port_name_i << " + __addr/sizeof(" << type_basic_par_decl << ")), " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\t__cached_id_out[" << n_params_out << "] = __cached_id;"
                    << "\t\t\t\t__addr_out[" << n_params_out << "] = __addr;"
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;

                out_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(!__cached[5])\n"
                    << "\t\t\t\t\tmemcpy(" << field_port_name_o <<  " + __addr/sizeof(" << type_basic_par_decl << "), (const " << type_basic_par_decl << " *)" << field_name << ", " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_out++;

                n_params_id++;

            }

            if (copies.front().directionality == OutlineDataItem::COPY_IN)
            {
                const std::string field_port_name = STR_PREFIX + field_name;
                const std::string field_name_param = type_basic_par_decl + "*" + field_port_name;

                local_decls
                    << "\t" << type_basic_par_decl << " " << field_name << dimensions_array << ";"
                ;

                fun_params_wrapper.append_with_separator(field_name_param, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
                ;

                TL::Symbol param_symbol = (*it)->get_field_symbol();
                int param_id = find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(!__cached[4])\n"
                    << "\t\t\t\t\tmemcpy(" << field_name << ", (const " << type_basic_par_decl << " *)(" << field_port_name << " + __addr/sizeof(" << type_basic_par_decl << ")), " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;
                n_params_id++;
            }

            if (copies.front().directionality == OutlineDataItem::COPY_OUT)
            {
                const std::string field_port_name = STR_PREFIX + field_name;
                const std::string field_name_param = type_basic_par_decl + "*" + field_port_name;

                local_decls
                    << "\t" << type_basic_par_decl << " " << field_name << dimensions_array << ";"
                ;

                fun_params_wrapper.append_with_separator(field_name_param, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
                ;

                TL::Symbol param_symbol = (*it)->get_field_symbol();
                int param_id = find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t__cached_id_out[" << n_params_out << "] = __cached_id;"
                    << "\t\t\t\t__addr_out[" << n_params_out << "] = __addr;"
                    << "\t\t\t\tbreak;"
                ;

                out_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tif(!__cached[5])\n"
                    << "\t\t\t\t\tmemcpy( " << field_port_name <<  " + __addr/sizeof(" << type_basic_par_decl << "), (const " << type_basic_par_decl << " *)" << field_name << ", " << n_elements_src << "*sizeof(" << type_basic_par_decl << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_out++;
                n_params_id++;
            }
        }
        else
        {
            Source dimensions_array; // = get_type_arrays(expr);
            Source dimensions_pointer_array; // = get_type_arrays(expr);
            Source n_elements_src;

            const Type &field_type = (*it)->get_field_type();
            std::string field_simple_decl = field_type.get_simple_declaration(scope, field_name);
            std::string type_simple_decl = field_type.get_simple_declaration(scope, field_name);
            size_t position = type_simple_decl.find("[");

            Type elem_type;
            Type basic_elem_type;
            std::string basic_elem_type_name;

            if (field_type.is_pointer() || field_type.is_array())
            {
                if (field_type.is_pointer())
                {
                    elem_type = field_type.points_to();
                    basic_elem_type=field_type.basic_type();
                    basic_elem_type_name= basic_elem_type.print_declarator();
                }
                else if (field_type.is_array())
                {
                    elem_type = field_type.array_element();
                    basic_elem_type=field_type.basic_type();
                    basic_elem_type_name= basic_elem_type.print_declarator();
                }
                else
                {
                    internal_error("Reference not valid", 0);
                }

                std::string par_simple_decl = elem_type.get_simple_declaration(scope, field_name);
                TL::Type basic_par_type = field_type.basic_type();
                std::string basic_par_type_decl = basic_par_type.print_declarator();
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
                position = par_decl.rfind(field_name);
                std::string type_par_decl = par_decl.substr(0, position);
                std::string type_basic_par_decl = get_element_type_pointer_to(field_type, field_name, scope);
                std::string type_mcxx_par_decl = get_type_pointer_to(field_type, field_name, scope);

                position = type_mcxx_par_decl.find("TO_CHANGE_MCXX");
                const std::string field_port_name = STR_PREFIX + field_name;
                std::string name_parameter_dimension = "*" + field_port_name;
                std::string declaration_param_wrapper = type_mcxx_par_decl;
                declaration_param_wrapper.replace(position, 14, name_parameter_dimension);

                std::string local_variable = "(*" + field_name + ")";
                std::string declaration_local_wrapper = type_mcxx_par_decl;
                declaration_local_wrapper.replace(position, 14, local_variable);

                std::string casting_sizeof = type_mcxx_par_decl;
                casting_sizeof.replace(position, 14, "");
                std::string casting_pointer = type_mcxx_par_decl;
                casting_pointer.replace(position, 14, "(*)");

                const std::string field_name_param = declaration_param_wrapper;
                fun_params_wrapper.append_with_separator(field_name_param, ", ");

                pragmas_src
                    << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
                ;

                local_decls
                    << "\t" << declaration_local_wrapper << ";"
                ;

                TL::Symbol param_symbol = (*it)->get_field_symbol();
                int param_id= find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t" << field_name << " = (" << casting_pointer << ")(" << field_port_name << " + __addr/sizeof(" << casting_sizeof << "));"
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;
                n_params_id++;
            }
        }
    }

    int param_pos = 0;
    for (ObjectList<Symbol>::const_iterator it = param_list.begin(); it != param_list.end(); it++, param_pos++)
    {

        if (function_parameters_passed[param_pos])
        {
            continue;
        }

        const Scope &scope = it->get_scope();
        Source dimensions_array; // = get_type_arrays(expr);
        Source dimensions_pointer_array; // = get_type_arrays(expr);
        Source n_elements_src;

        const Type &field_type = it->get_type();
        Type elem_type;
        Type basic_elem_type; //=field_type.basic_type();
        std::string basic_elem_type_name; //= basic_elem_type.print_declarator();
        const std::string &field_name = it->get_name();
        std::string type_simple_decl = field_type.get_simple_declaration(scope, field_name);
        size_t position = type_simple_decl.find("[");
        bool is_only_pointer = (position == std::string::npos);

        if (field_type.is_pointer() || field_type.is_array())
        {
            n_elements_src = get_copy_elements_all_dimensions_src(field_type, field_type, is_only_pointer);
            dimensions_array = get_type_arrays_src(field_type, field_type, is_only_pointer);
            dimensions_pointer_array = get_type_pointer_to_arrays_src(field_type, field_type, is_only_pointer);
        }
        else
        {
            error_printf_at(it->get_locus(),
                "Unsupported data type '%s' in fpga task parameter '%s' (only pointer parameters are supported)\n",
                print_declarator(field_type.get_internal_type()), field_name.c_str()
            );
        }

        if (field_type.is_pointer() || field_type.is_array())
        {
            if (field_type.is_pointer())
            {
                elem_type = field_type.points_to();
                basic_elem_type=field_type.basic_type();
                basic_elem_type_name= basic_elem_type.print_declarator();
            }
            else
            {
                elem_type = field_type.array_element();
                basic_elem_type=field_type.basic_type();
                basic_elem_type_name= basic_elem_type.print_declarator();
            }

            std::string par_simple_decl = elem_type.get_simple_declaration(scope, field_name);
            TL::Type basic_par_type= field_type.basic_type();
            std::string basic_par_type_decl= basic_par_type.print_declarator();
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
            position = par_decl.rfind(field_name);
            std::string type_par_decl = par_decl.substr(0, position);
            std::string type_basic_par_decl = get_element_type_pointer_to(field_type, field_name, scope);

            const std::string field_port_name = STR_PREFIX + field_name;
            //const std::string field_name_param = type_basic_par_decl+ "*" +field_port_name;
            const std::string field_name_param = type_basic_par_decl + "*" + field_port_name;
            // DJG ONE ONLY PORT - REMOVING PORTS PER ARGUMENT
            fun_params_wrapper.append_with_separator(field_name_param, ", ");

            pragmas_src
                << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
            ;

            local_decls
                << "\t" << type_basic_par_decl << "*" << field_name << dimensions_pointer_array << ";"
            ;

            function_parameters_passed[param_pos] = 1;

            in_copies_aux
                << "\t\t\tcase " << param_pos << ":\n"
                << "\t\t\t\t" << field_name << " = (" << type_basic_par_decl << " * " << dimensions_pointer_array << ")(" << field_port_name << " + __addr/sizeof(" << type_basic_par_decl << "));"
                << "\t\t\t\tbreak;"
            ;

            n_params_in++;
            n_params_id++;

        }
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
            << "\t\tdefault:;"
            << "\t\t}"
            << "\t}"
            << "\n"
        ;
    }

    Nodecl::NodeclBase fun_code = func_symbol_original.get_function_code();
    Source wrapper_src;

    // DJG ONE ONLY PORT - REMOVING PORTS PER ARGUMENT
    wrapper_src
        << "void " << func_symbol.get_name() << "(" << args << ", " << fun_params_wrapper << ") {"
    ;

    local_decls
        << "\tcounter_t __counter_reg[4] = {0xA, 0xBAD, 0xC0FFE, 0xDEAD};"
        << "\tunsigned int __i;"
        << "\tuint64_t __addrRd, __addrWr, __accHeader;"
        << "\tap_uint<8> __cached, __destID;"
        << "\tuint32_t __comp_needed;"
        << "\tunsigned long long __addr, __cached_id;"
        << "\tunsigned int __param_id, __n_params_in, __n_params_out;"
    ;

    if (n_params_out)
    {
        local_decls
            << "\tunsigned long long __cached_id_out[" << n_params_out << "];"
            << "\tunsigned long long __addr_out[" << n_params_out << "];"
        ;
    }

    profiling_0
        //<< "\tmemcpy(&__counter_reg[0], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t)), sizeof(counter_t)); "
        << "\t__counter_reg[0] =get_time((const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
    ;

    profiling_1
        //<< "\tmemcpy(&__counter_reg[1], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t)), sizeof(counter_t)); "
        // << "\tread_profiling_reg(&__counter_reg[1], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
        << "\t__counter_reg[1] =get_time((const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
    ;

    profiling_2
        //<< "\tmemcpy(&__counter_reg[2], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t)), sizeof(counter_t)); "
        //<< "\tread_profiling_reg(&__counter_reg[2], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
        << "\t__counter_reg[2] =get_time((const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
    ;

    profiling_3
        //<< "\tmemcpy(&__counter_reg[3], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t)), sizeof(counter_t)); "
        //<< "\tmemcpy((void *)(" << STR_DATA << " + __addrWr/sizeof(counter_t)), __counter_reg, 4*sizeof(counter_t));"
        //<< "\tread_profiling_reg(&__counter_reg[3], (const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
        << "\t__counter_reg[3] =get_time((const counter_t *)(" << STR_DATA << " + __addrRd/sizeof(counter_t))); "
        << "\twrite_profiling_registers((counter_t *)(" << STR_DATA << " + __addrWr/sizeof(counter_t)), __counter_reg);"
    ;

    generic_initial_code
        << "\t__addrRd = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__addrWr = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__accHeader = " << STR_INPUTSTREAM << ".read().data;"
        << "\t__comp_needed = __accHeader;"
        << "\t__destID = __accHeader>>32;"
    ;


    sync_output_code
        << "\tend_task: {"
        << "\t#pragma HLS PROTOCOL fixed\n"
        << "\t\tend_acc_task(outStream, accID, __destID);"
        << "\t}"
        << " "
    ;

    wrapper_before
        << wrapper_src
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
        //<< out_copies_addr
        << profiling_1
        << "\n"
    ;

    called_source
        << "\tif (__comp_needed)\n"
        << "\t\t" << func_symbol_original.get_name() << "(" << fun_params << ");"
        << "\n"
    ;

    wrapper_after
        << profiling_2
        << "\n"
        << out_copies
        << profiling_3
        << "\n"
        << sync_output_code
        << "\n"
        << "}"
    ;

}

void DeviceFPGA::copy_stuff_to_device_file(const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied)
{

    for (TL::ObjectList<Nodecl::NodeclBase>::const_iterator it = stuff_to_be_copied.begin(); it != stuff_to_be_copied.end(); ++it)
    {
        if (it->is<Nodecl::FunctionCode>() || it->is<Nodecl::TemplateFunctionCode>())
        {
            TL::Symbol function = it->get_symbol();
            TL::Symbol new_function = SymbolUtils::new_function_symbol(function, function.get_name());

            _copied_fpga_functions.add_map(function, new_function);
            _fpga_file_code.append(Nodecl::Utils::deep_copy(*it, *it, _copied_fpga_functions));

            TL::ObjectList<Nodecl::Symbol> symbol_stuff_to_be_copied= Nodecl::Utils::get_nonlocal_symbols_first_occurrence(*it);

            for (TL::ObjectList<Nodecl::Symbol>::const_iterator its = symbol_stuff_to_be_copied.begin(); its != symbol_stuff_to_be_copied.end(); ++its)
            {
                TL::Symbol sym = its->get_symbol();
                if (sym.is_function())
                {
                    Nodecl::NodeclBase code = sym.get_function_code();
                    TL::ObjectList<Nodecl::NodeclBase> expand_code;
                    expand_code.append(code);
                    copy_stuff_to_device_file(expand_code);
                }
            }
        }
        else
        {
            _fpga_file_code.append(Nodecl::Utils::deep_copy(*it, *it));
        }
    }
}

void DeviceFPGA::copy_stuff_to_device_file_expand(const TL::ObjectList<Nodecl::NodeclBase> stuff_to_be_copied)
{
    __number_of_calls++;

    for (TL::ObjectList<Nodecl::NodeclBase>::const_iterator it = stuff_to_be_copied.begin();
        it != stuff_to_be_copied.end(); ++it)
    {
        if (it->is<Nodecl::FunctionCode>() || it->is<Nodecl::TemplateFunctionCode>())
        {
            TL::Symbol function = it->get_symbol();
            Nodecl::NodeclBase code_function = function.get_function_code();
            Source outline_src_function_caller;
            outline_src_function_caller << code_function.prettyprint();

            TL::ObjectList<Nodecl::Symbol> symbol_stuff_to_be_copied =
                Nodecl::Utils::get_nonlocal_symbols_first_occurrence(*it);

            for (TL::ObjectList<Nodecl::Symbol>::const_iterator its = symbol_stuff_to_be_copied.begin();
                its != symbol_stuff_to_be_copied.end(); ++its)
            {
                TL::Symbol sym = its->get_symbol();
                std::string original_filename = TL::CompilationProcess::get_current_file().get_filename();

                if (sym.is_function() && (sym.get_filename()==original_filename))
                {
                    Nodecl::NodeclBase code = sym.get_function_code();
                    TL::ObjectList<Nodecl::NodeclBase> expand_code;
                    ObjectList<Source> outline_code;
                    TL::Source outline_src_function_1l;

                    if (code.is_null())
                    {

#if _DEBUG_AUTOMATIC_COMPILER_
                        std::cerr << "NOT VALID!!!!!\n" << std::endl << std::endl;
                        std::cerr << "Name: " << sym.get_name() << std::endl << std::endl;
#endif

                    }
                    else
                    {
                        outline_src_function_1l << code.prettyprint();

#if _DEBUG_AUTOMATIC_COMPILER_
                        std::cerr << std::endl << std::endl;
                        std::cerr << " ===================================================================0\n";
                        std::cerr << "call " << __number_of_calls << ": Adding function to expand_fpga_source_codes:"
                            + sym.get_name() + " new function called and expanded\n";
                        std::cerr << " ===================================================================0\n";
#endif

                        //outline_code.insert(outline_src_function_1l);
                        ObjectList<Source> result;

                        result.insert(outline_src_function_1l);

                        for (ObjectList<Source>::const_iterator it_expand =_expand_fpga_source_codes.begin();
                            it_expand !=_expand_fpga_source_codes.end(); it_expand++)
                        {
                            if (!((it_expand->get_source()) == outline_src_function_1l.get_source()))
                            {
                                result.append(*it_expand);
                            }
                        }

                        _expand_fpga_source_codes = result;

                        expand_code.insert(code);
                        copy_stuff_to_device_file_expand(expand_code);
                    }
                }
            }
        }
    }
    __number_of_calls--;
}

EXPORT_PHASE(TL::Nanox::DeviceFPGA);

/*--------------------------------------------------------------------
  (C) Copyright 2006-2018 Barcelona Supercomputing Center
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

#include "cxx-nodecl.h"
#include "cxx-graphviz.h"

#include "tl-nanos.hpp"
#include "tl-symbol-utils.hpp"

using namespace TL;
using namespace TL::Nanox;

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
        if (!outline_data_item_sym.is_valid() or field_type.get_size() > sizeof(uint64_t)) {
            #if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << "Argument " << param_pos << ": " << arg_simple_decl << " is not valid" << std::endl << std::endl;
            #endif

            fatal_error("Non-valid argument in FPGA task. Data type must be at most 64-bit wide\n");
        }

        const ObjectList<OutlineDataItem::CopyItem> &copies = data_items[param_pos]->get_copies();
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
        //      This supports tasks without copies but with localmemwhether
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

                TL::Symbol new_function = SymbolUtils::new_function_symbol_for_deep_copy(
                    called_task, called_task.get_name());

                TL::Symbol new_function_wrapper = SymbolUtils::new_function_symbol_for_deep_copy(
                    called_task, called_task.get_name() + "_hls_automatic_mcxx_wrapper");

                Source wrapper_decls, wrapper_code;

                DeviceFPGA::gen_hls_wrapper(
                    called_task,
                    new_function,
                    new_function_wrapper,
                    info._data_items,
                    wrapper_decls,
                    wrapper_code,
                    creates_children_tasks);

                Source outline_src;
                Source full_src;
                Source func_aux_code;

                func_aux_code
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

                if (instrumentation_enabled())
                {
                    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
                    func_aux_code
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
                        << STR_WRITEEVENT << "(" << STR_PREFIX << "_EVENT_TYPE_LAST, " << STR_PREFIX << "_EVENT_VAL_OK, " << STR_PREFIX << "_EVENT_TYPE_POINT );"
                        //<< "    _emit_xtasks_ins_event(EVENT_ID_LAST, EVENT_VAL_OK, XTASKS_EVENT_TYPE_LAST);"
                        << "}"


                    ;
                }
                else
                {
                    //Define empty instrument calls when instrumentation is not enabled
                    func_aux_code
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
                    //NOTE: Do not remove the '\n' characters at the end of some lines. Otherwise, the generated source is not well formated
                    func_aux_code
                        << "void write_outstream(uint64_t data, unsigned short dest, unsigned char last) {"
                        << "#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_OUTPORT << "\n"
                        //NOTE: Using 72 to have the data shifted 8 positions. This way is easily shown in HEX
                        << "/* Pack the axiData_t info: data(64bits) + dest(6bits) + last(2bit) */\n"
                        << "\tap_uint<72> tmp = data;"
                        << "\ttmp = (tmp << 8) | ((dest & 0x3F) << 2) | (last & 0x3);"
                        << "\t" << STR_GLOB_OUTPORT << " = tmp;"
                        << "}"
                        << "void wait_tw_signal() {"
                        << "\t#pragma HLS INTERFACE ap_hs port=" << STR_GLOB_TWPORT << "\n"
                        << "\tap_uint<2> sync = " << STR_GLOB_TWPORT << ";"
                        << "}"
                        << "void " << STR_WAIT_TASKS << "() {"
                        << "\tconst unsigned short TM_TW = 0x13;"
                        << "\tuint64_t tmp = " << STR_ACCID << ";"
                        << "\ttmp = tmp << 48 /*ACC_ID info uses bits [48:55]*/;"
                        << "\ttmp = 0x8000000100000000 | tmp | " << STR_COMPONENTS_COUNT << ";"
                        << "\twrite_outstream(tmp /*TASKWAIT_DATA_BLOCK*/, TM_TW, 0 /*last*/);"
                        << "\twrite_outstream(" << STR_TASKID << " /*data*/, TM_TW, 1 /*last*/);"
                        << "\t{\n"
                        << "\t\t#pragma HLS PROTOCOL fixed\n"
                        << "\t\twait_tw_signal();"
                        << "\t}\n"
                        << "}"
                        << "enum { ARGFLAG_DEP_IN  = 0x08, ARGFLAG_DEP_OUT  = 0x04,"
                        << "       ARGFLAG_COPY_IN = 0x02, ARGFLAG_COPY_OUT = 0x01,"
                        << "       ARGFLAG_NONE    = 0x00 };"
                        << "void " << STR_CREATE_TASK << "(uint32_t archMask, uint64_t onto, uint16_t numArgs, uint64_t * args, uint8_t * argsFlags) {"
                        << "\t++" << STR_COMPONENTS_COUNT << ";"
                        << "\tconst unsigned short TM_NEW = 0x12;"
                        << "\tuint64_t tmp = archMask;"
                        << "\ttmp = ((tmp << 16) | numArgs) << 16;"
                        << "\twrite_outstream(tmp, TM_NEW, 0);"
                        << "\twrite_outstream(" << STR_TASKID << ", TM_NEW, 0);"
                        << "\twrite_outstream(onto, TM_NEW, 0);"
                        << "\tfor (uint16_t idx = 0; idx < numArgs; ++idx) {"
                        << "\t\ttmp = argsFlags[idx];"
                        << "\t\ttmp = (tmp << 56) | args[idx];"
                        << "\t\twrite_outstream(tmp, TM_NEW, idx == (numArgs - 1));"
                        << "\t}"
                        << "}"
                    ;
                }

                Nodecl::NodeclBase fun_code = called_task.get_function_code();

                outline_src
                    << wrapper_decls
                    << func_aux_code
                    << " "
                    << fun_code.prettyprint()
                    << " "
                    << wrapper_code
                ;

                _copied_fpga_functions.add_map(called_task, new_function_wrapper);
                TL::ObjectList<Nodecl::NodeclBase> expand_code;
                TL::Symbol expand_function = original_statements.retrieve_context().get_related_symbol();

                expand_code.append(fun_code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << std::endl << std::endl;
                std::cerr << " ===================================================================0\n";
                std::cerr << "First call to copy_stuff_to_device... going through:\n";
                std::cerr << " ===================================================================0\n";
                std::cerr << fun_code.prettyprint(); __number_of_calls=1;
#endif

                copy_stuff_to_device_file_expand(expand_code);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << " ===================================================================0\n";
                std::cerr << "End First call to copy_stuff_to_device... \n";
                std::cerr << " ===================================================================0\n";
                std::cerr << std::endl << std::endl;
#endif

                TL::Scope scope = fun_code.retrieve_context();
                preappend_list_sources_and_reset(outline_src, full_src, scope);

                FpgaOutlineInfo to_outline_info;
                to_outline_info._type = acc_type;
                to_outline_info._num_instances = get_num_instances(info._target_info);
                to_outline_info._name = called_task.get_name();
                to_outline_info._source_code = full_src;
                _outlines.append(to_outline_info);
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

DeviceFPGA::DeviceFPGA() : DeviceProvider(std::string("fpga")), _bitstream_generation(false), _force_fpga_task_creation_ports()
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
    std::string acc_type = get_acc_type(current_function, info._target_info);

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
            << /* factory */ "&nanos_fpga_factory, &" << outline_name << "_args"
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
                     << "\n"
                     << "\n"
                     << "typedef ap_axis<64,1,1,5> axiData_t;\n"
                     << "typedef hls::stream<axiData_t> axiStream_t;\n"
                     << "typedef uint64_t counter_t;\n"
                     << "\n"
                     << "\n"
                     << std::endl;

            add_included_fpga_files(hls_file);

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

static void get_inout_decl(ObjectList<OutlineDataItem*>& data_items, std::string &in_type, std::string &out_type)
{
    in_type = "";
    out_type = "";
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
void DeviceFPGA::gen_hls_wrapper(const Symbol &called_task, const Symbol &func_symbol_original,
    const Symbol &func_symbol, ObjectList<OutlineDataItem*>& data_items,
    Source &wrapper_decls, Source &wrapper_source, const bool creates_children_tasks)
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
    std::string in_dec, out_dec;
    get_inout_decl(data_items, in_dec, out_dec);
    Source pragmas_src;
    Source fun_params_wrapper;
    Source clear_components_count;

    wrapper_decls
        << "extern const uint8_t " << STR_ACCID << ";"
        << "static uint64_t " << STR_TASKID << ";"
        << "static uint64_t " << STR_INSTRCOUNTER << ", " << STR_INSTRBUFFER << ";"
        << "static unsigned int " << STR_INSTRSLOTS << ", " << STR_INSTRCURRENTSLOT << ";"
        << "static int " << STR_INSTROVERFLOW << ";"
    ;

    fun_params_wrapper
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
        wrapper_decls
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

        pragmas_src
            << "#pragma HLS INTERFACE m_axi port=" << STR_INSTRDATA << "\n"
        ;
    }

    if (creates_children_tasks)
    {
        wrapper_decls
            << "extern ap_uint<72> " << STR_GLOB_OUTPORT << ";"
            << "extern volatile ap_uint<2> " << STR_GLOB_TWPORT << ";"
            << "static ap_uint<32> " << STR_COMPONENTS_COUNT << ";"
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
    Source sync_output_decl, sync_output_code;
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

    // Go through all the parameters. The iteration below goes through the copies.
    for (ObjectList<OutlineDataItem*>::iterator it = data_items.begin(); it != data_items.end(); it++)
    {
        const std::string &field_name = (*it)->get_field_name();
        fun_params.append_with_separator(field_name, ", ");

#if _DEBUG_AUTOMATIC_COMPILER_
        std::cerr << std::endl << std::endl;
        std::cerr << "Variable: " << field_name << std::endl;
        std::cerr << std::endl << std::endl;
#endif

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
            std::cerr << "expresion of copies.front " << localmem.front().prettyprint() << std::endl;
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

            TL::Symbol param_symbol = (*it)->get_field_symbol();
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

                fun_params_wrapper.append_with_separator(field_name_param_i, ", ");
                fun_params_wrapper.append_with_separator(field_name_param_o, ", ");

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
                fun_params_wrapper.append_with_separator(field_name_param, ", ");

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
                fun_params_wrapper.append_with_separator(field_name_param, ", ");

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
                std::string name_parameter_dimension = "(*" + field_port_name + ")";
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
                int param_id = find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                ;

                if (creates_children_tasks)
                {
                    wrapper_decls
                        << "static " << casting_pointer << STR_REAL_PARAM_PREFIX << field_name << ";"
                    ;

                    in_copies_aux
                        << "\t\t\t\t" << field_name << " = (uintptr_t)__param;"
                        << "\t\t\t\t" << STR_REAL_PARAM_PREFIX << field_name << " = (uintptr_t)__param;"
                    ;
                }
                else
                {
                    in_copies_aux
                        << "\t\t\t\t" << field_name << " = (" << casting_pointer << ")("
                        << field_port_name << " + __param/sizeof(" << casting_sizeof << "));"
                    ;

                }

                in_copies_aux
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;
                n_params_id++;
            }
            else if (field_type.is_scalar_type())
            {
                std::string basic_par_type_decl = get_element_type_pointer_to(field_type, field_name, scope);

#if _DEBUG_AUTOMATIC_COMPILER_
                std::cerr << "BASIC PAR TYPE DECL :" << basic_par_type_decl << std::endl;
#endif

                local_decls
                    << "\t" << basic_par_type_decl << " " << field_name << ";"
                ;

                TL::Symbol param_symbol = (*it)->get_field_symbol();
                int param_id = find_parameter_position(param_list, param_symbol);
                function_parameters_passed[param_id] = 1;

                in_copies_aux
                    << "\t\t\tcase " << param_id << ":\n"
                    << "\t\t\t\tunion {"
                    << "\t\t\t\t\t" << basic_par_type_decl << " " << field_name << ";"
                    << "\t\t\t\t\tuint64_t "<< field_name << "_task_arg;"
                    << "\t\t\t\t} mcc_arg_" << param_id << ";"
                    << "\t\t\t\tmcc_arg_" << param_id << "." << field_name << "_task_arg = " << STR_INPUTSTREAM << ".read().data;"
                    << "\t\t\t\t" << field_name << " = mcc_arg_" << param_id << "." << field_name << ";"
                    << "\t\t\t\tbreak;"
                ;

                n_params_in++;
                n_params_id++;
            }
            else
            {
                internal_error("Unsupported field_type", 0);
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
            const std::string field_name_param = type_basic_par_decl + " *" + field_port_name;
            // DJG ONE ONLY PORT - REMOVING PORTS PER ARGUMENT
            fun_params_wrapper.append_with_separator(field_name_param, ", ");

            pragmas_src
                << "#pragma HLS INTERFACE m_axi port=" << field_port_name << "\n"
            ;

            local_decls
                << "\tstatic " << type_basic_par_decl << " (* " << field_name << ")" << dimensions_pointer_array << ";"
            ;

            function_parameters_passed[param_pos] = 1;

            in_copies_aux
                << "\t\t\tcase " << param_pos << ":\n"
                << "\t\t\t\t__param = " << STR_INPUTSTREAM << ".read().data;"
                << "\t\t\t\t" << field_name << " = (" << type_basic_par_decl << " * " << dimensions_pointer_array << ")(" << field_port_name << " + __param/sizeof(" << type_basic_par_decl << "));"
            ;

            if (creates_children_tasks)
            {
                wrapper_decls
                    << "static " << type_basic_par_decl << " (* " << STR_REAL_PARAM_PREFIX << field_name << ")" << dimensions_pointer_array << ";"
                ;

                in_copies_aux
                    << "\t\t\t\t" << STR_REAL_PARAM_PREFIX << field_name << " = (uintptr_t)__param;"
                ;
            }

            in_copies_aux
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

    sync_output_decl
        << "void end_acc_task(axiStream_t& stream, uint32_t destId) {"
        << end_instrumentation
        << "\tuint64_t data = " << STR_ACCID << ";"
        << "\tdata = (data << 56) | (" << STR_TASKID << " & 0x00FFFFFFFFFFFFFF);"
        << "\twrite_stream(stream, data, destId, 1);"
        << "}"
    ;

    wrapper_source
        << sync_output_decl
        << "void " << func_symbol.get_name() << "(" << fun_params_wrapper << ") {"
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
        << "\t\t" << func_symbol_original.get_name() << "(" << fun_params << ");"
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
                std::string original_filename = TL::CompilationProcess::get_current_file().get_filename(/* fullpath */ true);

                if (sym.is_function() && (sym.get_filename() == original_filename))
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

std::string DeviceFPGA::get_acc_type(const TL::Symbol& task, const TargetInformation& target_info)
{
    std::string value = "INVALID_ONTO_VALUE";

    // Check onto information
    ObjectList<Nodecl::NodeclBase> onto_clause = target_info.get_onto();
    if (onto_clause.size() >= 1)
    {
        Nodecl::NodeclBase onto_val = onto_clause[0];
        warn_printf_at(onto_val.get_locus(),
            "The use of onto clause is no longer needed unless you have a collision between two FPGA tasks that yeld the same type hash\n");
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
        std::stringstream type_str;
        Nodecl::NodeclBase code = task.get_function_code();
        Nodecl::Context context = (code.is<Nodecl::TemplateFunctionCode>())
            ? code.as<Nodecl::TemplateFunctionCode>().get_statements().as<Nodecl::Context>()
            : code.as<Nodecl::FunctionCode>().get_statements().as<Nodecl::Context>();

        bool without_template_args = !task.get_type().is_template_specialized_type()
            || task.get_scope().get_template_parameters()->is_explicit_specialization;

        // Not using the line number to allow future modifications of source code without
        // afecting the accelerator hash
        type_str
            << task.get_filename()
            << " " << task.get_qualified_name(context.retrieve_context(), without_template_args);
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
    bool is_function_task = called_task.is_valid();
    TL::ObjectList<OutlineDataItem*> data_items = outline_info.get_data_items();
    DeviceHandler device_handler = DeviceHandler::get_device_handler();
    const OutlineInfo::implementation_table_t& implementation_table = outline_info.get_implementation_table();

    // Declare argument structure
    TL::Symbol structure_symbol = TL::Symbol::invalid();

    std::string acc_type = "0";
    uint32_t arch_mask = 0;

    // Check devices of all implementations
    for (OutlineInfo::implementation_table_t::const_iterator it = implementation_table.begin();
            it != implementation_table.end();
            ++it)
    {
        const TargetInformation& target_info = it->second;

        const ObjectList<std::string>& devices = target_info.get_device_names();
        for (ObjectList<std::string>::const_iterator it2 = devices.begin();
                it2 != devices.end();
                ++it2)
        {
            std::string device_name = *it2;
            if (device_name == "smp")
            {
                arch_mask |= 0x80000000;
            }
            else if (device_name == "fpga")
            {
                arch_mask |= 0x40000000;
                acc_type = get_acc_type(called_task, target_info);
            }
            else
            {
                fatal_error("FPGA device only can create tasks for smp and fpga devices.\n");
            }
        }
    }

    Source spawn_code, args_list, args_flags_list;
    size_t num_args = 0;

    // Go through all the arguments and fill the arguments_list
    for (TL::ObjectList<OutlineDataItem*>::iterator it = data_items.begin();
            it != data_items.end();
            it++)
    {
        if (!(*it)->get_symbol().is_valid())
            continue;

        Source arg_value;
        Source arg_flags;

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
                                    args_list.append_with_separator( as_symbol((*it)->get_symbol()), ", " );
                                    args_flags_list.append_with_separator("0xC0", ", ");
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
                                    arg_flags
                                        << "0xC0";
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

        const std::string arg_name = STR_REAL_PARAM_PREFIX + (*it)->get_field_name();
        args_list.append_with_separator( arg_value, ", " );
        args_flags_list.append_with_separator( arg_flags, ", " );
        ++num_args;
    }

    spawn_code
        << "uint64_t mcxx_args[] = {"
        << args_list
        << "};"
        << "uint8_t mcxx_args_flags[] = {"
        << args_flags_list
        << "};"
        << STR_CREATE_TASK << "(" << arch_mask << ", " << acc_type << ", " << num_args << ", mcxx_args, mcxx_args_flags);";

    Nodecl::NodeclBase spawn_code_tree = spawn_code.parse_statement(construct);
    construct.replace(spawn_code_tree);
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

EXPORT_PHASE(TL::Nanox::DeviceFPGA);

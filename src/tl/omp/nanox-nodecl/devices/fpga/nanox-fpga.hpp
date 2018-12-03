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



#ifndef NANOX_FPGA_HPP
#define NANOX_FPGA_HPP

#include "tl-compilerphase.hpp"
#include "tl-devices.hpp"
#include "tl-source.hpp"

#define STR_OUTPUTSTREAM       "outStream"
#define STR_GLOB_OUTPORT       "__mcxx_outPort"
#define STR_INPUTSTREAM        "inStream"
#define STR_GLOB_TWPORT        "__mcxx_twPort"
#define STR_INSTRDATA          "mcxx_data"
#define STR_ACCID              "accID"
#define STR_TASKID             "__mcxx_taskId"
#define STR_INSTRCOUNTER       "__mcxx_instrCounter"
#define STR_INSTRBUFFER        "__mcxx_instrBuffer"
#define STR_PREFIX             "mcxx_"
#define STR_REAL_PARAM_PREFIX  "__mcxx_param_"
#define STR_INSTRSLOTS         "__mcxx_instrSlots"
#define STR_INSTRCURRENTSLOT   "__mcxx_instrCurrentSlot"
#define STR_INSTROVERFLOW      "__mcxx_instrNumOverflow"
#define STR_EVENTTYPE          "__mcxx_eventType"
#define STR_EVENTSTRUCT        "__mcxx_event_t"
#define STR_WRITEEVENT         "__mcxx_writeInstrEvent"
#define STR_INSTREND           "__mcxx_instrEnd"
#define STR_COMPONENTS_COUNT   "__mcxx_taskComponents"
#define STR_CREATE_TASK        "nanos_fpga_create_wd_async"
#define STR_WAIT_TASKS         "nanos_wg_wait_completion"

//Default events
#define EV_DEVCOPYIN            77
#define EV_DEVCOPYOUT           78
#define EV_DEVEXEC              79


//#define _DEBUG_AUTOMATIC_COMPILER_ 1


namespace TL
{
    namespace Nanox
    {
        class DeviceFPGA : public DeviceProvider
        {
            public:

                virtual void run(DTO& dto);
                virtual void pre_run(DTO& dto);

                DeviceFPGA();

                virtual ~DeviceFPGA() { }

                virtual void phase_cleanup(DTO& data_flow);

                virtual void create_outline(CreateOutlineInfo &info,
                        Nodecl::NodeclBase &outline_placeholder,
                        Nodecl::NodeclBase &output_statements,
                        Nodecl::Utils::SimpleSymbolMap* &symbol_map);

                virtual void get_device_descriptor(
                        DeviceDescriptorInfo& info,
                        Source &ancillary_device_description,
                        Source &device_descriptor,
                        Source &fortran_dynamic_init);

                virtual bool remove_function_task_from_original_source() const;

                virtual void copy_stuff_to_device_file(
                        const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied);
            private:
                typedef std::set<std::string> str_set_t;
                std::string _board_name;
                std::string _device_name;
                std::string _frequency;
                std::string _bitstream_generation_str;
                std::string _vivado_design_path;
                std::string _vivado_project_name;
                std::string _ip_cache_path;
                std::string _dataflow;
                std::string _force_fpga_task_creation_ports_str;
                bool        _bitstream_generation;
                str_set_t   _force_fpga_task_creation_ports;

                struct FpgaOutlineInfo {
                    std::string _type;
                    std::string _num_instances;
                    std::string _name;
                    Source      _source_code;

                    std::string get_filename()
                    {
                        return _type + ":" + _num_instances + ":" + _name + "_hls_automatic_mcxx.cpp";
                    }

                    std::string get_wrapper_name()
                    {
                        return _name + "_hls_automatic_mcxx_wrapper";
                    }
                };
                TL::ObjectList< struct FpgaOutlineInfo > _outlines;

                void set_bitstream_generation_from_str(const std::string& str);
                void set_force_fpga_task_creation_ports_from_str(const std::string& str);

                Nodecl::List _fpga_file_code; //TODO: Check the usage of this variable
                TL::ObjectList<Source> _expand_fpga_source_codes; //TODO: Check the usage of this variable
                unsigned int __number_of_calls;
                Nodecl::Utils::SimpleSymbolMap _copied_fpga_functions;

                TL::Source fpga_param_code(
                        TL::ObjectList<OutlineDataItem*> &data_items,
                        Nodecl::Utils::SymbolMap *,
                        TL::Scope
                        );

                void add_hls_pragmas(
                        Nodecl::NodeclBase &,
                        TL::ObjectList<TL::Nanox::OutlineDataItem*>&
                        );

                //Nodecl::NodeclBase gen_hls_wrapper(
                //DJG instrumented Source gen_hls_wrapper(
                void gen_hls_wrapper(
                        const TL::Symbol& called_task,
                        const TL::Symbol& func_symbol_original,
                        const TL::Symbol& func_symbol,
                        TL::ObjectList<TL::Nanox::OutlineDataItem*>&,
                        Source& wrapper_decls,
                        Source& wrapper_source,
                        const bool creates_children_tasks
                        );
                Source gen_fpga_outline(ObjectList<Symbol> param_list, TL::ObjectList<OutlineDataItem*> data_items);
                bool task_has_scalars(TL::ObjectList<OutlineDataItem*> &);
//                void copy_symbols_to_device_file( const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied);
                void copy_stuff_to_device_file_expand( const TL::ObjectList<Nodecl::NodeclBase> stuff_to_be_copied);
                void preappend_list_sources_and_reset(Source outline_src, Source& full_src, TL::Scope scope);

                void add_included_fpga_files(std::ostream &hls_file);

                std::string get_acc_type(const TL::Symbol& task, const TargetInformation& target_info);
                std::string get_num_instances(const TargetInformation& target_info);

                virtual void emit_async_device(
                        Nodecl::NodeclBase construct,
                        TL::Symbol function_symbol,
                        TL::Symbol called_task,
                        Nodecl::NodeclBase statements,
                        Nodecl::NodeclBase priority_expr,
                        Nodecl::NodeclBase if_condition,
                        Nodecl::NodeclBase final_condition,
                        Nodecl::NodeclBase task_label,
                        bool is_untied,
                        OutlineInfo& outline_info,
                        OutlineInfo* parameter_outline_info,
                        Nodecl::NodeclBase* placeholder_task_expr_transformation);
        };
    }
}

#endif // NANOX_FPGA_HPP

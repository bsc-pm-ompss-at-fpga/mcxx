/*--------------------------------------------------------------------
  (C) Copyright 2006-2013 Barcelona Supercomputing Center
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

#define STR_OUTPUTSTREAM "outStream"
#define STR_INPUTSTREAM "inStream"
#define STR_TWSTREAM "twStream"
#define STR_DATA "mcxx_data"
#define STR_PREFIX "mcxx_"
#define STR_BUS "__mcxx_bus"
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
                std::string _board_name;
                std::string _device_name;
                std::string _frequency;
                std::string _bitstream_generation_str;
                std::string _vivado_design_path;
                std::string _vivado_project_name;
                std::string _ip_cache_path;
                std::string _dataflow;
                std::string _fpga_task_creation;
                bool        _bitstream_generation;

                void set_bitstream_generation_from_str(const std::string& str);

                std::string _acc_type;
                std::string _num_acc_instances;
                int         _current_base_acc_num;

                Nodecl::List _fpga_file_code;
                TL::ObjectList<Source> _fpga_source_codes;
                TL::ObjectList<Source> _expand_fpga_source_codes;
                unsigned int __number_of_calls;
                TL::ObjectList<std::string> _fpga_source_name;
                Nodecl::Utils::SimpleSymbolMap _copied_fpga_functions;
                TL::ObjectList<TL::Nanox::OutlineDataItem*> _internal_data_items;
                TL::Symbol _internal_new_function;
                Nodecl::Utils::SimpleSymbolMap _internal_symbol_map;

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
                        Source& wrapper_before,
                        Source& called_source,
                        Source& wrapper_after
                        );
                bool task_has_scalars(TL::ObjectList<OutlineDataItem*> &);
//                void copy_symbols_to_device_file( const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied);
                void copy_stuff_to_device_file_expand( const TL::ObjectList<Nodecl::NodeclBase> stuff_to_be_copied);
                void preappend_list_sources_and_reset(Source outline_src, Source& full_src, TL::Scope scope);

                void add_included_fpga_files(std::ostream &hls_file);
        };
    }
}

#endif // NANOX_FPGA_HPP

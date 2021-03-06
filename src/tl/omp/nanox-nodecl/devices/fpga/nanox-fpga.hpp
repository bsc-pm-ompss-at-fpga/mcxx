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



#ifndef NANOX_FPGA_HPP
#define NANOX_FPGA_HPP

#include "tl-compilerphase.hpp"
#include "tl-devices.hpp"
#include "tl-source.hpp"

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

                virtual void emit_async_device(
                        Nodecl::NodeclBase construct,
                        TL::Symbol function_symbol,
                        TL::Symbol called_task,
                        TL::Symbol structure_symbol,
                        Nodecl::NodeclBase statements,
                        Nodecl::NodeclBase priority_expr,
                        Nodecl::NodeclBase if_condition,
                        Nodecl::NodeclBase final_condition,
                        Nodecl::NodeclBase task_label,
                        bool is_untied,
                        OutlineInfo& outline_info,
                        OutlineInfo* parameter_outline_info,
                        Nodecl::NodeclBase* placeholder_task_expr_transformation);

            private:
                typedef std::set<std::string> str_set_t;

                struct FpgaOutlineInfo {
                    const std::string  _name;
                    const std::string  _num_instances;
                    const std::string  _type;
                    const bool         _creates_children_tasks;
                    const bool         _periodic_support;
                    bool               _needs_systemc_header;
                    Source             _wrapper_decls;
                    Source             _wrapper_code;
                    Nodecl::List       _user_code;

                    FpgaOutlineInfo(const std::string name, const std::string num,
                            const std::string type, const bool creates_children_tasks, const bool periodic_support) :
                            _name(name), _num_instances(num), _type(type), _creates_children_tasks(creates_children_tasks),
                            _periodic_support(periodic_support), _needs_systemc_header(false),
                            _wrapper_decls(), _wrapper_code(), _user_code() {}

                    std::string get_filename() const;
                    std::string get_wrapper_name() const;
                };

                struct FpgaNanosPostInitInfo {
                    std::string _function;
                    std::string _argument;
                };

                std::string _bitstream_generation_str;
                bool        _bitstream_generation;
                std::string _force_fpga_spawn_ports_str;
                str_set_t   _force_fpga_spawn_ports;
                std::string _memory_port_width;
                std::string _unaligned_memory_port_str;
                bool        _unaligned_memory_port;
                std::string _check_limits_memory_port_str;
                bool        _check_limits_memory_port;
                std::string _force_periodic_support_str;
                bool        _force_periodic_support;
                std::string _ignore_deps_spawn_str;
                bool        _ignore_deps_spawn;
                std::string _unordered_args_str;
                bool        _unordered_args;
                std::string _data_pack_str;
                bool        _data_pack;
                std::string _function_copy_suffix;
                std::string _memory_ports_mode;
                str_set_t   _registered_tasks;
                Nodecl::NodeclBase _root;
                TL::ObjectList< struct FpgaOutlineInfo >       _outlines;
                TL::ObjectList< struct FpgaNanosPostInitInfo > _nanos_post_init_actions;

                void set_bitstream_generation_from_str(const std::string& str);
                void set_force_fpga_spawn_ports_from_str(const std::string& str);
                void set_memory_port_width_from_str(const std::string& str);
                void set_unaligned_memory_port_from_str(const std::string& str);
                void set_check_limits_memory_port_from_str(const std::string& str);
                void set_force_periodic_support_from_str(const std::string& str);
                void set_function_copy_suffix_from_str(const std::string& str);
                void set_memory_ports_mode_from_str(const std::string& str);
                void set_ignore_deps_spawn_from_str(const std::string& str);
                void set_unordered_args_from_str(const std::string& str);
                void set_data_pack_from_str(const std::string& str);

                Nodecl::Utils::SimpleSymbolMap                 _global_copied_fpga_symbols;
                Nodecl::List                                   _stuff_to_copy;

                void gen_hls_wrapper(
                        const TL::Symbol& func_symbol,
                        TL::ObjectList<TL::Nanox::OutlineDataItem*>&,
                        const bool creates_children_tasks,
                        const bool periodic_support,
                        const std::set<std::string> user_calls_set,
                        const std::string wrapper_func_name,
                        Source& wrapper_decls, //< out
                        Source& wrapper_source); //< out

                TL::Symbol gen_fpga_unpacked(
                        TL::Symbol &current_function,
                        Nodecl::NodeclBase &outline_placeholder,
                        const Nodecl::NodeclBase &num_repetitions_expr,
                        const Nodecl::NodeclBase &period_expr,
                        CreateOutlineInfo &info,
                        Nodecl::Utils::SimpleSymbolMap* &symbol_map);

                void add_included_fpga_files(FILE* file);

                std::string get_acc_type(const TL::Symbol& task, const TargetInformation& target_info);
                Nodecl::NodeclBase get_acc_instance(const TargetInformation& target_info);
                std::string get_num_instances(const TargetInformation& target_info);
                Nodecl::NodeclBase get_num_repetitions(const TargetInformation& target_info);
                Nodecl::NodeclBase get_period(const TargetInformation& target_info);

                void register_task_creation(
                        Nodecl::NodeclBase construct,
                        Nodecl::NodeclBase task_label,
                        TL::Symbol current_function,
                        TL::Symbol called_task,
                        TL::Symbol structure_symbol,
                        OutlineInfo& outline_info,
                        std::string acc_type,
                        size_t const num_copies);
        };
    }
}

#endif // NANOX_FPGA_HPP

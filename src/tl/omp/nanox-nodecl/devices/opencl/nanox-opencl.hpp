/*--------------------------------------------------------------------
  (C) Copyright 2006-2011 Barcelona Supercomputing Center 
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


#ifndef NANOX_OPENCL_HPP
#define NANOX_OPENCL_HPP

#include "tl-compilerphase.hpp"
#include "tl-devices.hpp"

namespace TL
{

    namespace Nanox
    {

        class DeviceOpenCL : public DeviceProvider
        {
            private:

                  Nodecl::List _ocl_file_code;
                  
                  void generate_ndrange_code(
                          const TL::Symbol& called_task,
                          const TL::Symbol& unpacked_function,
                          const TL::ObjectList<Nodecl::NodeclBase>& ndrange_args,
                          const std::string filename,
                          TL::Source& code_ndrange);

                  void add_included_opencl_files(FILE* file);
            public:

                // This phase does nothing
                virtual void pre_run(DTO& dto);
                virtual void run(DTO& dto);

                DeviceOpenCL();

                virtual ~DeviceOpenCL() { }

             virtual void create_outline(CreateOutlineInfo &info,
                     Nodecl::NodeclBase &outline_placeholder,
                     Nodecl::NodeclBase &output_statements,
                     Nodecl::Utils::SymbolMap* &symbol_map);

             virtual void get_device_descriptor(DeviceDescriptorInfo& info,
                     Source &ancillary_device_description,
                     Source &device_descriptor,
                     Source &fortran_dynamic_init);

            virtual void copy_stuff_to_device_file(
                    const TL::ObjectList<Nodecl::NodeclBase>& stuff_to_be_copied);

             bool allow_mandatory_creation();

             virtual bool is_gpu_device() const;

             virtual void phase_cleanup(DTO& data_flow);
        };

    }

}

#endif // NANOX_CUDA_HPP
/*--------------------------------------------------------------------
(C) Copyright 2006-2009 Barcelona Supercomputing Center 
Centro Nacional de Supercomputacion

This file is part of Mercurium C/C++ source-to-source compiler.

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


#ifndef TL_CFG_RENAMING_VISITOR_HPP
#define TL_CFG_RENAMING_VISITOR_HPP

#include <map>

#include "tl-nodecl-visitor.hpp"

namespace TL
{
    //! This class traverses a Nodecl and creates a new Nodecl renaming the symbols inside
    //! depending on the renaming map. 
    /*!
     * The list may contain:
     * - Zero elements: no renames has been performed
     * - More element: renamed nodecls
     */
    class LIBTL_CLASS CfgRenamingVisitor : public Nodecl::NodeclVisitor<Nodecl::NodeclBase>
    {
        private:
            std::map<Symbol, Nodecl::NodeclBase> _const_map;
            const char* _filename;
            int _line;
            
            
            // *** Auxiliar methods *** //
            
            nodecl_t create_nodecl_list(ObjectList<Nodecl::NodeclBase> list);

            template <typename T>
            Ret visit_binary(const T& n);

            template <typename T>
            Ret visit_unary(const T& n);
            
        public:
            // *** Constructors *** //
            CfgRenamingVisitor(std::map<Symbol, Nodecl::NodeclBase> rename_map, const char* _filename, int _line);
            CfgRenamingVisitor(const CfgRenamingVisitor& rename_v);
           
           
            // *** Visitors *** //
            Ret unhandled_node(const Nodecl::NodeclBase& n);
            Ret visit(const Nodecl::Symbol& n);
            Ret visit(const Nodecl::ArraySubscript& n);
            Ret visit(const Nodecl::ArraySection& n);
            Ret visit(const Nodecl::ClassMemberAccess& n);
            Ret visit(const Nodecl::Derreference& n);
            Ret visit(const Nodecl::Reference& n);
            Ret visit(const Nodecl::Add& n);
            Ret visit(const Nodecl::Minus& n);
            Ret visit(const Nodecl::Mul& n);
            Ret visit(const Nodecl::Div& n);
            Ret visit(const Nodecl::Mod& n);
            Ret visit(const Nodecl::Power& n);
            Ret visit(const Nodecl::LogicalAnd& n);
            Ret visit(const Nodecl::LogicalOr& n);
            Ret visit(const Nodecl::BitwiseAnd& n);
            Ret visit(const Nodecl::BitwiseOr& n);
            Ret visit(const Nodecl::BitwiseXor& n);
            Ret visit(const Nodecl::Shr& n);
            Ret visit(const Nodecl::Shl& n);
            Ret visit(const Nodecl::Predecrement& n);
            Ret visit(const Nodecl::Postdecrement& n);
            Ret visit(const Nodecl::Preincrement& n);
            Ret visit(const Nodecl::Postincrement& n);
            Ret visit(const Nodecl::Plus& n);
            Ret visit(const Nodecl::Neg& n);     
            Ret visit(const Nodecl::BitwiseNot& n);
            Ret visit(const Nodecl::LogicalNot& n);

//             
    };
}

#endif      // TL_CFG_RENAMING_VISITOR_HPP
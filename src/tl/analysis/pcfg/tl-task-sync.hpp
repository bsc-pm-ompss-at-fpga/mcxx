/*--------------------------------------------------------------------
 (C) Copyright 2006-2014 Barcelona Supercomputing Center             *
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

#ifndef TL_TASK_SYNC_ANALYSIS_HPP
#define TL_TASK_SYNC_ANALYSIS_HPP

#include "tl-extensible-graph.hpp"
#include "tl-nodecl-visitor.hpp"

namespace TL { 
namespace Analysis {
namespace TaskAnalysis {

    // **************************************************************************************************** //
    // *************************** Class implementing task PCFG synchronization *************************** //

    typedef std::pair<Node*, SyncKind> PointOfSyncInfo;
    typedef ObjectList<PointOfSyncInfo> PointOfSyncList;
    typedef std::map<Node*, PointOfSyncList> PointsOfSync;

    struct LIBTL_CLASS TaskSynchronizations
    {
    private:
        ExtensibleGraph* _graph;

        NBase match_dependencies(Node* source, Node* target);

        void compute_task_synchronization_labels();
        void compute_task_synchronization_conditions_rec(Node* current);
        void compute_task_synchronization_conditions();

    public:
        TaskSynchronizations(ExtensibleGraph* graph, bool is_ompss_enabled);

        void compute_task_synchronizations();
    };

    // ************************* END class implementing task PCFG synchronization ************************* //
    // **************************************************************************************************** //
} 
}
}

#endif      // TL_TASK_SYNC_ANALYSIS_HPP

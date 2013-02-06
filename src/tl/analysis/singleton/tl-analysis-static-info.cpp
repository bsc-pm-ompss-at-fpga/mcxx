/*--------------------------------------------------------------------
  (C) Copyright 2006-2012 Barcelona Supercomputing Center
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

#include "cxx-process.h"
#include "tl-analysis-utils.hpp"
#include "tl-analysis-static-info.hpp"

namespace TL  {
namespace Analysis {

    // ********************************************************************************************* //
    // ********************** Class to define which analysis are to be done ************************ //

    WhichAnalysis::WhichAnalysis( Analysis_tag a )
            : _which_analysis( a )
    {}

    WhichAnalysis::WhichAnalysis( int a )
            : _which_analysis( Analysis_tag( a ) )
    {}

    WhichAnalysis WhichAnalysis::operator|( WhichAnalysis a )
    {
        return WhichAnalysis( int( this->_which_analysis ) | int( a._which_analysis ) );
    }

    WhereAnalysis::WhereAnalysis( Nested_analysis_tag a )
            : _where_analysis( a )
    {}

    WhereAnalysis::WhereAnalysis( int a )
            : _where_analysis( Nested_analysis_tag( a ) )
    {}

    WhereAnalysis WhereAnalysis::operator|( WhereAnalysis a )
    {
        return WhereAnalysis( int(this->_where_analysis) | int( a._where_analysis ) );
    }

    // ******************** END class to define which analysis are to be done ********************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************** Class to retrieve analysis info about one specific nodecl ****************** //

    NodeclStaticInfo::NodeclStaticInfo( ObjectList<Analysis::Utils::InductionVariableData*> induction_variables,
                                        Utils::ext_sym_set killed )
            : _induction_variables( induction_variables ), _killed( killed )
    {}

    bool NodeclStaticInfo::is_constant( const Nodecl::NodeclBase& n ) const
    {
        return ( _killed.find( Utils::ExtendedSymbol( n ) ) == _killed.end( ) );
    }

    bool NodeclStaticInfo::is_induction_variable( const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
             it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n ) )
            {
                result = true;
                break;
            }
        }

        return result;
    }

    const_value_t* NodeclStaticInfo::get_induction_variable_increment( const Nodecl::NodeclBase& n ) const
    {
        const_value_t* result = NULL;
        bool incr_is_complex = false;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
            it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n ) )
            {
                result = ( *it )->get_increment( ).get_constant( );
                if( result == NULL )
                    incr_is_complex = true;
                break;
            }
        }

        if( result && !incr_is_complex )
            WARNING_MESSAGE( "You are asking for the increment of an Object ( %s ) "\
                             "which is not an Induction Variable\n", n.prettyprint( ).c_str( ) );

        return result;
    }

    ObjectList<Utils::InductionVariableData*> NodeclStaticInfo::NodeclStaticInfo::get_induction_variables( const Nodecl::NodeclBase& n ) const
    {
        return _induction_variables;
    }

    // ************** END class to retrieve analysis info about one specific nodecl **************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************************** User interface for static analysis ***************************** //

    AnalysisStaticInfo::AnalysisStaticInfo( const Nodecl::NodeclBase& n, WhichAnalysis analysis_mask,
                                            WhereAnalysis nested_analysis_mask, int nesting_level)
    {
        TL::Analysis::AnalysisSingleton& analysis = TL::Analysis::AnalysisSingleton::get_analysis( );

        TL::Analysis::PCFGAnalysis_memento analysis_state;

        // Compute "dynamic" analysis

        ObjectList<Analysis::Utils::InductionVariableData*> induction_variables;
        ObjectList<Nodecl::NodeclBase> constants;

        if( analysis_mask._which_analysis & WhichAnalysis::PCFG_ANALYSIS )
        {
            analysis.parallel_control_flow_graph( analysis_state, n );
        }
        if( analysis_mask._which_analysis & ( WhichAnalysis::USAGE_ANALYSIS
                              | WhichAnalysis::CONSTANTS_ANALYSIS ) )
        {
            analysis.use_def( analysis_state, n );
        }
        if( analysis_mask._which_analysis & WhichAnalysis::LIVENESS_ANALYSIS )
        {
            analysis.liveness( analysis_state, n );
        }
        if( analysis_mask._which_analysis & WhichAnalysis::REACHING_DEFS_ANALYSIS )
        {
            analysis.reaching_definitions( analysis_state, n );
        }
        if( analysis_mask._which_analysis & WhichAnalysis::INDUCTION_VARS_ANALYSIS )
        {
            analysis.induction_variables( analysis_state, n );
        }

        // Save static analysis
        NestedBlocksStaticInfoVisitor v( analysis_mask, nested_analysis_mask, analysis_state, nesting_level );
        v.walk( n );
        static_info_map_t nested_blocks_static_info = v.get_analysis_info( );
        _static_info_map.insert( nested_blocks_static_info.begin( ), nested_blocks_static_info.end( ) );
    }


    bool AnalysisStaticInfo::is_constant( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            nodecl_t scope_t = scope.get_internal_nodecl( );
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether it is constant.'",
                             codegen_to_str( scope_t, nodecl_retrieve_context( scope_t ) ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.is_constant( n );
        }
        return result;
    }

    bool AnalysisStaticInfo::is_induction_variable( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            nodecl_t scope_t = scope.get_internal_nodecl( );
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether it is an induction variable.'",
                             codegen_to_str( scope_t, nodecl_retrieve_context( scope_t ) ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result =  current_info.is_induction_variable( n );
        }

        return result;
    }

    const_value_t* AnalysisStaticInfo::get_induction_variable_increment( const Nodecl::NodeclBase& scope,
                                                                         const Nodecl::NodeclBase& n ) const
    {
        const_value_t* result = NULL;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            nodecl_t scope_t = scope.get_internal_nodecl( );
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. Cannot get induction variables.'",
                             codegen_to_str( scope_t, nodecl_retrieve_context( scope_t ) ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.get_induction_variable_increment( n );
        }

        return result;
    }

    ObjectList<Utils::InductionVariableData*> AnalysisStaticInfo::get_induction_variables( const Nodecl::NodeclBase& scope,
                                                                                           const Nodecl::NodeclBase& n ) const
    {
        ObjectList<Utils::InductionVariableData*> result;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            nodecl_t scope_t = scope.get_internal_nodecl( );
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. Cannot get induction variable increment.'",
                             codegen_to_str( scope_t, nodecl_retrieve_context( scope_t ) ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.get_induction_variables( n );
        }

        return result;
    }

    // ************************** END User interface for static analysis *************************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // ********************* Visitor retrieving the analysis of a given Nodecl ********************* //

    NestedBlocksStaticInfoVisitor::NestedBlocksStaticInfoVisitor( WhichAnalysis analysis_mask,
                                                                  WhereAnalysis nested_analysis_mask,
                                                                  PCFGAnalysis_memento state,
                                                                  int nesting_level)
            : _state( state ), _analysis_mask( analysis_mask ), _nested_analysis_mask( nested_analysis_mask ),
              _nesting_level( nesting_level ), _current_level( 0 ),  _analysis_info( )
    {}

    NestedBlocksStaticInfoVisitor::Ret NestedBlocksStaticInfoVisitor::join_list( ObjectList<static_info_map_t>& list )
    {
        for( ObjectList<static_info_map_t>::iterator it = list.begin( ); it != list.end( );  ++it)
        {
            _analysis_info.insert( it->begin( ), it->end( ) );
        }
    }

    static_info_map_t NestedBlocksStaticInfoVisitor::get_analysis_info( )
    {
        return _analysis_info;
    }

    void NestedBlocksStaticInfoVisitor::retrieve_current_node_static_info( Nodecl::NodeclBase n )
    {
        // The queries to the analysis info depend on the mask
        Utils::ext_sym_set cs;
        ObjectList<Analysis::Utils::InductionVariableData*> ivs;
        if( _analysis_mask._which_analysis & WhichAnalysis::CONSTANTS_ANALYSIS )
        {
            cs = _state.get_killed( n );
        }
        if( _analysis_mask._which_analysis & WhichAnalysis::INDUCTION_VARS_ANALYSIS )
        {
            ivs = _state.get_induction_variables( n );
        }

        NodeclStaticInfo static_info( ivs, cs );
        _analysis_info.insert( static_info_pair_t( n, static_info ) );
    }

    NestedBlocksStaticInfoVisitor::Ret NestedBlocksStaticInfoVisitor::visit(const Nodecl::DoStatement& n)
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_DO_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level)
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_statement( ) );
            }
        }
    }

    NestedBlocksStaticInfoVisitor::Ret NestedBlocksStaticInfoVisitor::visit(const Nodecl::IfElseStatement& n)
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_IF_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level)
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_then( ) );
                walk( n.get_else( ) );
            }
        }
    }

    NestedBlocksStaticInfoVisitor::Ret NestedBlocksStaticInfoVisitor::visit(const Nodecl::ForStatement& n)
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_FOR_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level)
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_statement( ) );
            }
        }
    }

    NestedBlocksStaticInfoVisitor::Ret NestedBlocksStaticInfoVisitor::visit(const Nodecl::WhileStatement& n)
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_WHILE_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level)
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodecl info
                walk( n.get_statement( ) );
            }
        }
    }

}
}
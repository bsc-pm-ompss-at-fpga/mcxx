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
                                        Utils::ext_sym_set killed, Node* autoscoped_task )
            : _induction_variables( induction_variables ), _killed( killed ),
              _autoscoped_task( autoscoped_task )
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
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n, /* skip conversion nodes */ true ) )
            {
                result = true;
                break;
            }
        }

        return result;
    }

    bool NodeclStaticInfo::is_basic_induction_variable( const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
            it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n, /* skip conversion nodes */ true ) )
            {
                result = ( *it )->is_basic( );
                break;
            }
        }

        return result;
    }

    const_value_t* NodeclStaticInfo::get_induction_variable_increment( const Nodecl::NodeclBase& n ) const
    {
        const_value_t* result = NULL;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
             it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n, /* skip conversion nodes */ true ) )
            {
                result = ( *it )->get_increment( ).get_constant( );
                break;
            }
        }

        if( result == NULL )
        {
            WARNING_MESSAGE( "You are asking for the increment of an Object ( %s ) "\
                             "which is not an Induction Variable\n", n.prettyprint( ).c_str( ) );
        }

        return result;
    }

    bool NodeclStaticInfo::is_induction_variable_increment_one( const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
             it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n, /* skip conversion nodes */ true ) )
            {
                result = ( *it )->is_increment_one( );
                break;
            }
        }

        return result;
    }

    Utils::InductionVariableData* NodeclStaticInfo::get_induction_variable( const Nodecl::NodeclBase& n ) const
    {
        Utils::InductionVariableData* iv = NULL;

        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
            it != _induction_variables.end( ); ++it )
        {
            if ( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), n, /* skip conversion nodes */ true ) )
            {
                iv = *it;
                break;
            }
        }

        return iv;
    }

    ObjectList<Utils::InductionVariableData*> NodeclStaticInfo::get_induction_variables( const Nodecl::NodeclBase& n ) const
    {
        return _induction_variables;
    }

    bool NodeclStaticInfo::is_adjacent_access( const Nodecl::NodeclBase& n ) const
    {
        bool result = true;

        if( n.is<Nodecl::ArraySubscript>( ) )
        {
            Nodecl::List subscript = n.as<Nodecl::ArraySubscript>( ).get_subscripts( ).as<Nodecl::List>( );
            Nodecl::List::iterator it = subscript.begin( );
            for( ; it != subscript.end( ) - 1; ++it )
            {   // All dimensions but the less significant must be constant
                if( !it->is_constant( ) )
                {
                    result = false;
                    break;
                }
            }
            // The less significant dimension must be accessed by an (+/-)c +/- IV, where c is a constant
            if( it == subscript.end( ) - 1 )
            {
                Nodecl::Utils::ReduceExpressionVisitor v;
                Nodecl::NodeclBase s = it->shallow_copy( );
                v.walk( s );

                InductionVarExpressionVisitor iv_v( _induction_variables );
                iv_v.walk( s );

                if( !is_induction_variable( s ) )
                {
                    if( s.is<Nodecl::Add>( ) || s.is<Nodecl::Minus>( ) )
                    {
                        Nodecl::NodeclBase lhs, rhs;
                        if( s.is<Nodecl::Add>( ) )
                        {
                            lhs = s.as<Nodecl::Add>( ).get_lhs( );
                            rhs = s.as<Nodecl::Add>( ).get_rhs( );
                        }
                        else
                        {
                            lhs = s.as<Nodecl::Minus>( ).get_lhs( );
                            rhs = s.as<Nodecl::Minus>( ).get_rhs( );
                        }
                        if( lhs.is<Nodecl::Neg>( ) )
                        {
                            lhs = lhs.as<Nodecl::Neg>( ).get_rhs( );
                        }

                        if( is_induction_variable( lhs ) && get_induction_variable( lhs )->is_increment_one( ) )
                        {
                            // All memory accesses in rhs must be constant within the context
                            ObjectList<Nodecl::NodeclBase> rhs_mem_accesses = Nodecl::Utils::get_all_memory_accesses( rhs );
                            for( ObjectList<Nodecl::NodeclBase>::iterator itm = rhs_mem_accesses.begin( );
                                 itm != rhs_mem_accesses.end( ); ++itm )
                            {
                                if( Utils::ext_sym_set_contains_nodecl( *itm, _killed ) )
                                {
                                    result = false;
                                    break;
                                }
                            }
                        }
                        else
                        {
                            result = false;
                        }
                    }
                    else
                    {
                        result = false;
                    }
                }
                else if ( /*is_induction_variable( s )*/ !get_induction_variable( s )->is_increment_one( ) )
                {
                    result = false;
                }
            }
        }

        return result;
    }

    void NodeclStaticInfo::print_auto_scoping_results( ) const
    {
        if( _autoscoped_task != NULL )
            _autoscoped_task->print_auto_scoping( );
    }

    // ************** END class to retrieve analysis info about one specific nodecl **************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************************** User interface for static analysis ***************************** //

    InductionVarExpressionVisitor::InductionVarExpressionVisitor( ObjectList<Analysis::Utils::InductionVariableData*> ivs )
            : _induction_variables( ivs )
    {}

    void InductionVarExpressionVisitor::visit_post( const Nodecl::Add& n )
    {
        Nodecl::NodeclBase rhs = n.get_rhs( );

        bool iv_in_rhs = false;
        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
             it != _induction_variables.end( ); ++it )
        {
            if( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), rhs, /* skip conversion nodes */ true ) )
            {
                iv_in_rhs = true;
                break;
            }
        }

        if( iv_in_rhs )
        {
            Nodecl::NodeclBase lhs = n.get_lhs( );
            Nodecl::NodeclBase new_rhs = lhs.shallow_copy( );
            Nodecl::NodeclBase new_lhs = rhs.shallow_copy( );
            Nodecl::Utils::replace( rhs, new_rhs );
            Nodecl::Utils::replace( lhs, new_lhs );
        }
    }

    void InductionVarExpressionVisitor::visit_post( const Nodecl::Minus& n )
    {
        Nodecl::NodeclBase rhs = n.get_rhs( );

        bool iv_in_rhs = false;
        for( ObjectList<Analysis::Utils::InductionVariableData*>::const_iterator it = _induction_variables.begin( );
            it != _induction_variables.end( ); ++it )
        {
            if( Nodecl::Utils::equal_nodecls( ( *it )->get_variable( ).get_nodecl( ), rhs, /* skip conversion nodes */ true ) )
            {
                iv_in_rhs = true;
                break;
            }
        }

        if( iv_in_rhs )
        {
            Nodecl::Neg new_lhs = Nodecl::Neg::make( rhs, rhs.get_type( ) );
            Nodecl::Add reordered_minus = Nodecl::Add::make( new_lhs, n.get_lhs( ), rhs.get_type( ) );
            Nodecl::Utils::replace( n, reordered_minus );
        }
    }

    // ************************** END User interface for static analysis *************************** //
    // ********************************************************************************************* //



    // ********************************************************************************************* //
    // **************************** User interface for static analysis ***************************** //

    AnalysisStaticInfo::AnalysisStaticInfo( const Nodecl::NodeclBase& n, WhichAnalysis analysis_mask,
                                            WhereAnalysis nested_analysis_mask, int nesting_level)
    {
        _node = n;

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
        if( analysis_mask._which_analysis & WhichAnalysis::AUTO_SCOPING )
        {
            analysis.auto_scoping( analysis_state, n );
        }

        // Save static analysis
        NestedBlocksStaticInfoVisitor v( analysis_mask, nested_analysis_mask, analysis_state, nesting_level );
        v.walk( n );
        static_info_map_t nested_blocks_static_info = v.get_analysis_info( );
        _static_info_map.insert( nested_blocks_static_info.begin( ), nested_blocks_static_info.end( ) );
    }

    static_info_map_t AnalysisStaticInfo::get_static_info_map( ) const
    {
        return _static_info_map;
    }

    Nodecl::NodeclBase AnalysisStaticInfo::get_nodecl_origin( ) const
    {
        return _node;
    }

    bool AnalysisStaticInfo::is_constant( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether '%s' is constant.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
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
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether '%s' is an induction variable.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result =  current_info.is_induction_variable( n );
        }

        return result;
    }

    bool AnalysisStaticInfo::is_basic_induction_variable( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether '%s' is an induction variable.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result =  current_info.is_basic_induction_variable( n );
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
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot get the increment of the induction variable '%s'.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.get_induction_variable_increment( n );
        }

        return result;
    }

    bool AnalysisStaticInfo::is_induction_variable_increment_one( const Nodecl::NodeclBase& scope,
                                                                  const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether the increment of '%s' is one.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.is_induction_variable_increment_one( n );
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
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot get the increment of the induction variable '%s'.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.get_induction_variables( n );
        }

        return result;
    }

    bool AnalysisStaticInfo::is_adjacent_access( const Nodecl::NodeclBase& scope, const Nodecl::NodeclBase& n ) const
    {
        bool result = false;

        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot resolve whether the accesses to '%s' are adjacent.'",
                             scope.prettyprint( ).c_str( ), n.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            result = current_info.is_adjacent_access( n );
        }

        return result;
    }

    void AnalysisStaticInfo::print_auto_scoping_results( const Nodecl::NodeclBase& scope )
    {
        static_info_map_t::const_iterator scope_static_info = _static_info_map.find( scope );
        if( scope_static_info == _static_info_map.end( ) )
        {
            WARNING_MESSAGE( "Nodecl '%s' is not contained in the current analysis. "\
                             "Cannot print its auto-scoping results.'",
                             scope.prettyprint( ).c_str( ) );
        }
        else
        {
            NodeclStaticInfo current_info = scope_static_info->second;
            current_info.print_auto_scoping_results( );
        }
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

    void NestedBlocksStaticInfoVisitor::join_list( ObjectList<static_info_map_t>& list )
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
        ObjectList<Analysis::Utils::InductionVariableData*> induction_variables;
        Utils::ext_sym_set constants;
        Node* autoscoped_task;
        if( _analysis_mask._which_analysis & WhichAnalysis::INDUCTION_VARS_ANALYSIS )
        {
            induction_variables = _state.get_induction_variables( n );
        }
        if( _analysis_mask._which_analysis & WhichAnalysis::CONSTANTS_ANALYSIS )
        {
            constants = _state.get_killed( n );
        }
        if( ( _analysis_mask._which_analysis & WhichAnalysis::AUTO_SCOPING )
            && n.is<Nodecl::OpenMP::Task>( ) )
        {
            autoscoped_task = _state.get_autoscoped_task( n );
        }

        NodeclStaticInfo static_info( induction_variables, constants, autoscoped_task );
        _analysis_info.insert( static_info_pair_t( n, static_info ) );
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::DoStatement& n )
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_DO_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level )
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_statement( ) );
            }
        }
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::ForStatement& n )
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_FOR_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level )
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_statement( ) );
            }
        }
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::FunctionCode& n )
    {   // Assume that functions are always wanted to be parsed
        // Otherwise, this code should not be called...
        _current_level++;
        if( _current_level <= _nesting_level )
        {
            // Current nodecl info
            retrieve_current_node_static_info( n );

            // Nested nodecl info
            walk( n.get_statements( ) );
        }
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::IfElseStatement& n )
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_IF_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level )
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodes info
                walk( n.get_then( ) );
                walk( n.get_else( ) );
            }
        }
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::OpenMP::Task& n )
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_OPENMP_TASK_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level )
            {
                // Current nodecl info
                retrieve_current_node_static_info( n );

                // Nested nodecl info
                walk( n.get_statements( ) );
            }
        }
    }

    void NestedBlocksStaticInfoVisitor::visit( const Nodecl::WhileStatement& n )
    {
        if( _nested_analysis_mask._where_analysis & WhereAnalysis::NESTED_WHILE_STATIC_INFO )
        {
            _current_level++;
            if( _current_level <= _nesting_level )
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

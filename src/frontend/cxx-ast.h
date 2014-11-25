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




#ifndef CXX_AST_H
#define CXX_AST_H

/*
 * Abstract syntax tree
 */

#include "libmcxx-common.h"
#include "cxx-macros.h"
#include "cxx-locus.h"
#include "cxx-ast-decls.h"
#include "cxx-nodecl-decls.h"
#include "cxx-asttype.h"
#include "cxx-type-decls.h"
#include "cxx-limits.h"


MCXX_BEGIN_DECLS

// Returns the parent node or NULL if none
static inline AST ast_get_parent(const_AST a);

// Sets the parent (but does not update the parent
// to point 'a')
static inline void ast_set_parent(AST a, AST parent);

// Returns the line of the node
static inline unsigned int ast_get_line(const_AST a);

// Returns the related bit of text of the node
static inline const char* ast_get_text(const_AST a);

// Returns the kind of the node
static inline node_t ast_get_kind(const_AST a);

// Sets the related text
static inline void ast_set_text(AST a, const char* str);

// Sets the kind
static inline void ast_set_kind(AST a, node_t node_type);

// Returns the children 'num_child'. Might be
// NULL
static inline AST ast_get_child(const_AST a, int num_child);

// Sets the children, this one is preferred over ASTSon{0,1,2,3}
// Note that this one DOES set the parent of 'new_child' to be 'a'
static inline void ast_set_child(AST a, int num_child, AST new_children);

// Sets the children, this one is preferred over ASTSon{0,1,2,3}
// Note that this one does NOT set the parent of 'new_child' to be 'a'
static inline void ast_set_child_but_parent(AST a, int num_child, AST new_child);

// Main routine to create a node
static inline AST ast_make(node_t type, int num_children,
        AST son0,
        AST son1,
        AST son2,
        AST son3,
        const locus_t* location,
        const char *text);

// Returns the number of children as defined
// by ASTMake{1,2,3} or ASTLeaf
static inline int ast_num_children(const_AST a);

// Returns the locus
static inline const locus_t* ast_get_locus(const_AST a);

// Sets the locus
static inline void ast_set_locus(AST a, const locus_t* locus);

// Returns the filename
static inline const char *ast_get_filename(const_AST a);


// A list leaf (a special kind of node used to store lists in reverse order as
// generated by the LR parser)
//
// This is a leaf (so, a list with only one element)
static inline AST ast_list_leaf(AST elem);

// Creates a tree where last_element has been appended onto previous_list
static inline AST ast_list(AST previous_list, AST last_element);

// Returns the head of a list
static inline AST ast_list_head(AST list);

// Concatenates two lists
static inline AST ast_list_concat(AST before, AST after);

// Splits a list in two parts, head (a list containing only the first element)
// and tail (a list of the remainder elements)
LIBMCXX_EXTERN void ast_list_split_head_tail(AST list, AST *head, AST* tail);

// States if this portion of the tree is properly linked
LIBMCXX_EXTERN char ast_check(const_AST a);

static inline void ast_free(AST a);

// Gives a copy of all the tree but extended data is the same as original trees
LIBMCXX_EXTERN AST ast_copy(const_AST a);

// Duplicates a node but none of its children. The result is always a leaf
LIBMCXX_EXTERN AST ast_duplicate_one_node(AST orig);

// Gives a copy of all the tree but extended data is the same as original trees
LIBMCXX_EXTERN AST ast_copy_clearing_extended_data(const_AST a);

// Gives a copy of all the tree but removing dependent types
LIBMCXX_EXTERN AST ast_copy_for_instantiation(const_AST a);

// Allows clearing extended data if needed
LIBMCXX_EXTERN void ast_clear_extended_data(AST a);

// This makes a bitwise copy. You must know what you
// are doing here! *dest = *src
static inline void ast_replace(AST dest, const_AST src);

// Returns a string with a pair 'filename:line'
static inline const char* ast_location(const_AST a);

// Returns a printed version of the node name
static inline const char* ast_node_type_name(node_t n);

// States if two trees are the same
LIBMCXX_EXTERN char ast_equal (const_AST ast1, const_AST ast2);

// It only checks that the two nodes contain
// the same basic information
LIBMCXX_EXTERN char ast_equal_node (const_AST ast1, const_AST ast2);

// Returns the number (as it would be used in ast_get_child or ast_set_child)
// of the child 'child'. Returns -1 if 'child' is not a child of 'a'
static inline int ast_num_of_given_child(const_AST a, const_AST child);

// Synthesizes an ambiguous node given two nodes (possibly ambiguous too)
static inline AST ast_make_ambiguous(AST son0, AST son1);

// Returns the number of ambiguities
static inline int ast_get_num_ambiguities(const_AST a);

// Returns the ambiguity 'num'
static inline AST ast_get_ambiguity(const_AST a, int num);

// Replace a node with the ambiguity 'num'. This is
// used when solving ambiguities
static inline void ast_replace_with_ambiguity(AST a, int num);

// Fixes parents inside an interpretation, this is needed
// for proper navigation inside the interpretation of an ambiguity
static inline void ast_fix_parents_inside_intepretation(AST node);

static inline struct nodecl_expr_info_tag* ast_get_expr_info(const_AST a);
static inline void ast_set_expr_info(AST a, struct nodecl_expr_info_tag*);

// Used by memory report
static inline int ast_node_size(void);

/*
 * Macros
 *
 * Most of them are here for compatibility purposes. New code
 * should prefer the functions described above (except for those
 * otherwise stated)
 */

#define ASTKind(a) ast_get_kind(a)
#define ASTParent(a) (ast_get_parent(a))
#define ASTLine(a) ast_get_line(a)
#define ASTText(a) ast_get_text(a)
#define ASTSon0(a) (ast_get_child(a, 0))
#define ASTSon1(a) (ast_get_child(a, 1))
#define ASTSon2(a) (ast_get_child(a, 2))
#define ASTSon3(a) (ast_get_child(a, 3))

#define ASTLeaf(node, location, text) ast_make(node, 0,  NULL, NULL, NULL, NULL, location, text)
#define ASTMake1(node, son0, location, text) ast_make(node, 1, son0, NULL, NULL, NULL, location, text)
#define ASTMake2(node, son0, son1, location, text) ast_make(node, 2, son0, son1, NULL, NULL, location, text)
#define ASTMake3(node, son0, son1, son2, location, text) ast_make(node, 3, son0, son1, son2, NULL, location, text)
#define ASTMake4(node, son0, son1, son2, son3, location, text) ast_make(node, 4, son0, son1, son2, son3, location, text)

// Convenience macros
#define ASTChild0(a) ASTChild(a, 0)
#define ASTChild1(a) ASTChild(a, 1)
#define ASTChild2(a) ASTChild(a, 2)
#define ASTChild3(a) ASTChild(a, 3)
#define ASTChild(a, n) (ast_get_child(a, n))

#define ASTNumChildren(a) (ast_num_children(a))

#define ASTFileName(a) (ast_get_filename(a))

#define ASTListLeaf(a) ast_list_leaf(a)
#define ASTList(list, element) ast_list(list,element)

#define ASTCheck ast_check

#define ast_duplicate ast_copy
#define ast_duplicate_for_instantiation ast_copy_for_instantiation

#define ast_print_node_type ast_node_type_name

#define get_children_num ast_num_of_given_children

#define node_information ast_location

// Eases iterating forward in AST_NODE_LISTs
#define for_each_element(list, iter) \
    iter = (list); while (ASTSon0(iter) != NULL) iter = ASTSon0(iter); \
    for(; iter != NULL; iter = (iter != (list)) ? ASTParent(iter) : NULL)

// Name of operators
#define STR_OPERATOR_ADD "operator +"
#define STR_OPERATOR_MULT "operator *"
#define STR_OPERATOR_DIV "operator /"
#define STR_OPERATOR_MOD "operator %"
#define STR_OPERATOR_MINUS "operator -"
#define STR_OPERATOR_SHIFT_LEFT "operator <<"
#define STR_OPERATOR_SHIFT_RIGHT "operator >>"
#define STR_OPERATOR_LOWER_THAN "operator <"
#define STR_OPERATOR_GREATER_THAN "operator >"
#define STR_OPERATOR_LOWER_EQUAL "operator <="
#define STR_OPERATOR_GREATER_EQUAL "operator >="
#define STR_OPERATOR_EQUAL "operator =="
#define STR_OPERATOR_DIFFERENT "operator !="
#define STR_OPERATOR_BIT_AND "operator &"
#define STR_OPERATOR_REFERENCE "operator &"
#define STR_OPERATOR_BIT_OR "operator |"
#define STR_OPERATOR_BIT_XOR "operator ^"
#define STR_OPERATOR_LOGIC_AND "operator &&"
#define STR_OPERATOR_LOGIC_OR "operator ||"
#define STR_OPERATOR_DERREF STR_OPERATOR_MULT
#define STR_OPERATOR_UNARY_PLUS STR_OPERATOR_ADD
#define STR_OPERATOR_UNARY_NEG STR_OPERATOR_MINUS
#define STR_OPERATOR_LOGIC_NOT "operator !"
#define STR_OPERATOR_BIT_NOT "operator ~"
#define STR_OPERATOR_ASSIGNMENT "operator ="
#define STR_OPERATOR_ADD_ASSIGNMENT "operator +="
#define STR_OPERATOR_MINUS_ASSIGNMENT "operator -="
#define STR_OPERATOR_MUL_ASSIGNMENT "operator *="
#define STR_OPERATOR_DIV_ASSIGNMENT "operator /="
#define STR_OPERATOR_SHL_ASSIGNMENT "operator <<="
#define STR_OPERATOR_SHR_ASSIGNMENT "operator >>="
#define STR_OPERATOR_AND_ASSIGNMENT "operator &="
#define STR_OPERATOR_OR_ASSIGNMENT "operator |="
#define STR_OPERATOR_XOR_ASSIGNMENT "operator ^="
#define STR_OPERATOR_MOD_ASSIGNMENT "operator %="
#define STR_OPERATOR_PREINCREMENT "operator ++"
#define STR_OPERATOR_POSTINCREMENT STR_OPERATOR_PREINCREMENT
#define STR_OPERATOR_PREDECREMENT "operator --"
#define STR_OPERATOR_POSTDECREMENT STR_OPERATOR_PREDECREMENT
#define STR_OPERATOR_COMMA "operator ,"
#define STR_OPERATOR_NEW "operator new"
#define STR_OPERATOR_NEW_ARRAY "operator new[]"
#define STR_OPERATOR_DELETE "operator delete"
#define STR_OPERATOR_DELETE_ARRAY "operator delete[]"
#define STR_OPERATOR_ARROW "operator ->"
#define STR_OPERATOR_ARROW_POINTER "operator ->*"
#define STR_OPERATOR_CALL "operator ()"
#define STR_OPERATOR_SUBSCRIPT "operator []"

MCXX_END_DECLS

// Inline functions go here
#include "cxx-ast-inline.h"

#endif // CXX_AST_H

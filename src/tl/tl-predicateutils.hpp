/*
    Mercurium C/C++ Compiler
    Copyright (C) 2006-2007 - Roger Ferrer Ibanez <roger.ferrer@bsc.es>
    Barcelona Supercomputing Center - Centro Nacional de Supercomputacion
    Universitat Politecnica de Catalunya

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef TL_PREDICATEUTILS_HPP
#define TL_PREDICATEUTILS_HPP

#include "tl-builtin.hpp"
#include "tl-predicate.hpp"
#include "tl-objectlist.hpp"

namespace TL
{
    template<class T>
    class AlwaysTrue : public Predicate<T>
    {
        private:
        public:
            virtual bool operator()(T& t) const
            {
                return true;
            }
    };

    template <class T>
    class FunctionPredicate : public Predicate<T>
    {
        private:
            FunctionAdapter<bool, T> funct_adapter;
        public:
            FunctionPredicate(bool (*pf)(T&))
                : funct_adapter(pf)
            {
            }

            virtual bool operator()(T& t) const
            {
                return pf(t);
            }

            ~FunctionPredicate()
            {
            }
    };

    template <class T, class Q>
    class MemberFunctionPredicate : public Predicate<T>
    {
        private:
            MemberFunctionAdapter<bool, T, Q> mem_funct_adapter;
        public:
            MemberFunctionPredicate(bool (Q::*pmf)(T& t), Q& q)
                : mem_funct_adapter(pmf, q)
            {
            }

            virtual bool operator()(T& t) const
            {
                return mem_funct_adapter(t);
            }

            ~MemberFunctionPredicate()
            {
            }
    };


    template <class T>
    class ThisMemberFunctionPredicate : public Predicate<T>
    {
        private:
            ThisMemberFunctionAdapter<bool, T> this_mem_funct_adapter;
        public:
            ThisMemberFunctionPredicate(bool (T::*pmf)())
                : this_mem_funct_adapter(pmf)
            {
            }

            virtual bool operator()(T& t) const
            {
                return this_mem_funct_adapter(t);
            }

            ~ThisMemberFunctionPredicate()
            {
            }
    };

    template <class T>
    class ThisMemberFunctionConstPredicate : public Predicate<T>
    {
        private:
                ThisMemberFunctionConstAdapter<bool, T> this_mem_funct_adapter;
        public:
            ThisMemberFunctionConstPredicate(bool (T::*pmf)() const)
                : this_mem_funct_adapter(pmf)
            {
            }

            virtual bool operator()(T& t) const
            {
                return this_mem_funct_adapter(t);
            }

            ~ThisMemberFunctionConstPredicate()
            {
            }
    };

    template <class T>
    FunctionPredicate<T> predicate(bool (*pf)(T&))
    {
        return FunctionPredicate<T>(pf);
    }

    template <class T, class Q>
    MemberFunctionPredicate<T, Q> predicate(bool (Q::* pf)(T& t), Q& q)
    {
        return MemberFunctionPredicate<T, Q>(pf, q);
    }

    template <class T>
    ThisMemberFunctionPredicate<T> predicate(bool (T::* pf)())
    {
        return ThisMemberFunctionPredicate<T>(pf);
    }

    template <class T>
    ThisMemberFunctionConstPredicate<T> predicate(bool (T::* pf)() const)
    {
        return ThisMemberFunctionConstPredicate<T>(pf);
    }

    template <class T>
    class InSetPredicate : public Predicate<T>
    {
        private:
            ObjectList<T>& _list;
        public:
            InSetPredicate(ObjectList<T>& list)
                : _list(list)
            {
            }

            virtual bool operator()(T& t) const
            {
                return (find(_list.begin(), _list.end(), t) != _list.end());
            }
    };

    template <class T, class Q>
    class InSetPredicateFunctor : public Predicate<T>
    {
        private:
            ObjectList<Q> _list;
            const Functor<Q, T>& _f;
        public:
            InSetPredicateFunctor(ObjectList<Q>& list, const Functor<Q, T>& f)
                : _list(list), _f(f)
            {
            }

            virtual bool operator()(T& t) const
            {
                return (find(_list.begin(), _list.end(), _f(t)) != _list.end());
            }
    };

    template <class T>
    class NotInSetPredicate : public InSetPredicate<T>
    {
        public:
            NotInSetPredicate(ObjectList<T>& list)
                : InSetPredicate<T>(list)
            {
            }

            virtual bool operator()(T& t) const
            {
                return !(InSetPredicate<T>::operator()(t));
            }
    };

    template <class T, class Q>
    class NotInSetPredicateFunctor : public InSetPredicateFunctor<T, Q>
    {
        public:
            NotInSetPredicateFunctor(ObjectList<Q>& list, const Functor<Q, T>& f)
                : InSetPredicateFunctor<T, Q>(list, f)
            {
            }

            virtual bool operator()(T& t) const
            {
                return !(InSetPredicateFunctor<T, Q>::operator()(t));
            }
    };

    template <class T>
    InSetPredicate<T> in_set(ObjectList<T>& list)
    {
        return InSetPredicate<T>(list);
    }

    template <class T, class Q>
    InSetPredicateFunctor<T, Q> in_set(ObjectList<Q>& list, const Functor<Q, T>& f)
    {
        return InSetPredicateFunctor<T, Q>(list, f);
    }

    template <class T, class Q>
    InSetPredicateFunctor<T, Q> in_set(ObjectList<T>& list, const Functor<Q, T>& f)
    {
        ObjectList<Q> mapped_list = list.map(f);
        return InSetPredicateFunctor<T, Q>(mapped_list, f);
    }

    template <class T>
    NotInSetPredicate<T> not_in_set(ObjectList<T>& list)
    {
        return NotInSetPredicate<T>(list);
    }

    template <class T, class Q>
    NotInSetPredicateFunctor<T, Q> not_in_set(ObjectList<Q>& list, const Functor<Q, T>& f)
    {
        return NotInSetPredicateFunctor<T, Q>(list, f);
    }

    template <class T, class Q>
    NotInSetPredicateFunctor<T, Q> not_in_set(ObjectList<T>& list, const Functor<Q, T>& f)
    {
        ObjectList<Q> mapped_list = list.map(f);
        return NotInSetPredicateFunctor<T, Q>(mapped_list, f);
    }

    template <class T>
    class NotPredicate : public Predicate<T>
    {
        private:
            const Predicate<T>& _pred;
        public:
            NotPredicate(const Predicate<T>& pred)
                : _pred(pred)
            {
            }

            virtual bool operator()(T& t) const
            {
                return !(_pred(t));
            }

            ~NotPredicate()
            {
            }
    };

    template <class T>
    NotPredicate<T> negate(const Predicate<T>& pred)
    {
        return NotPredicate<T>(pred);
    }
}

#endif

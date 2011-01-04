/*
***** BEGIN LICENSE BLOCK *****

This file is part of the EPI (Erlang Plus Interface) Library.

Copyright (C) 2010 Serge Aleynikov <saleyn@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

***** END LICENSE BLOCK *****
*/

#include <boost/test/unit_test.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/bind.hpp>
#include <eixx/alloc_pool_st.hpp>
#include <eixx/eixx.hpp>

using namespace EIXX_NAMESPACE;

BOOST_AUTO_TEST_CASE( test_match1 )
{
    allocator_t alloc;

    // Populate the inner list with: [1,2]
    list l(2, alloc);
    l.push_back(1);
    l.push_back(2);
    l.close();

    // Create the outer list of terms: [test, 123, []]
    eterm ol[] = { 
        eterm(atom("test")),
        eterm(123),
        eterm(l)
    };

    {
        // Create the tuple from list: {test, 123, [1,2]}
        tuple tfl(ol);
        eterm tup(tfl);

        list ll(2, alloc);
        ll.push_back(eterm(1));
        ll.push_back(eterm(var()));
        ll.close();

        // Add pattern = {test, _, [1, _]}
        tuple ptup(3, alloc);
        ptup.push_back(eterm(atom("test")));
        ptup.push_back(var());
        ptup.push_back(ll);

        eterm pattern(ptup);
        // Perform pattern match on the tuple
        bool res = tup.match(pattern);
        BOOST_REQUIRE(res);
    }
}

namespace {
    struct cb_t {
        int match[10];
        cb_t() { bzero(match, sizeof(match)); }

        bool operator() (const eterm& a_pattern,
                         const varbind& a_varbind,
                         long a_opaque)
        {
            const eterm* n_var = a_varbind.find("N");
            if (n_var && n_var->type() == LONG) {
                int n = n_var->to_long();
                switch (a_opaque) {
                    case 1:
                        match[0]++;
                        BOOST_REQUIRE_EQUAL(1, n);
                        BOOST_REQUIRE(a_varbind.find("A") != NULL);
                        break;
                    case 2:
                        match[1]++;
                        BOOST_REQUIRE_EQUAL(2, n);
                        BOOST_REQUIRE(a_varbind.find("B") != NULL);
                        break;
                    case 3:
                        match[2]++;
                        BOOST_REQUIRE_EQUAL(3, n);
                        BOOST_REQUIRE(a_varbind.find("Reason") != NULL);
                        BOOST_REQUIRE_EQUAL(ATOM, a_varbind.find("Reason")->type());
                        break;
                    case 4:
                        match[3]++;
                        BOOST_REQUIRE_EQUAL(4, n);
                        BOOST_REQUIRE(a_varbind.find("X") != NULL);
                        BOOST_REQUIRE_EQUAL(TUPLE, a_varbind.find("X")->type());
                        break;
                    default:
                        throw "Invalid opaque value!";
                }
                return true;
            }
            return true;
        }
    };
}

BOOST_AUTO_TEST_CASE( test_match2 )
{
    allocator_t alloc;

    eterm_pattern_matcher etm;
    cb_t cb;

    // Add some patterns to check 
    // Capital literals represent variable names that will be bound 
    // if a match is successful. When some pattern match succeeds
    // cb_t::operator() is called with the second parameter containing
    // a varbind map of bound variables with their values.
    // Each pattern contains a variable N that will be bound with
    // the "pattern number" in each successful match.
    etm.push_back(eterm::format("{test, N, A}"), 
        boost::bind(&cb_t::operator(), &cb, _1, _2, _3), 1);
    etm.push_back(eterm::format("{ok, N, B, _}"),
        boost::bind(&cb_t::operator(), &cb, _1, _2, _3), 2);
    etm.push_back(eterm::format("{error, N, Reason}"),
        boost::bind(&cb_t::operator(), &cb, _1, _2, _3), 3);
    // Remember the reference to last pattern. We'll try to delete it later.
    const eterm_pattern_action& action =
        etm.push_back(eterm::format("{xxx, [_, _, {c, N}], \"abc\", X}"),
            boost::bind(&cb_t::operator(), &cb, _1, _2, _3), 4);

    // Make sure we registered 4 pattern above.
    BOOST_REQUIRE_EQUAL(4, etm.size());

    // Match the following terms against registered patters.
    BOOST_REQUIRE(etm.match(eterm::format("{test, 1, 123}")));  // N = 1
    BOOST_REQUIRE(etm.match(eterm::format("{test, 1, 234}")));  // N = 1

    BOOST_REQUIRE(etm.match(eterm::format("{ok, 2, 3, 4}")));   // N = 2
    BOOST_REQUIRE(!etm.match(eterm::format("{ok, 2}")));

    BOOST_REQUIRE(etm.match(eterm::format("{error, 3, not_found}"))); // N = 3

    BOOST_REQUIRE(etm.match(
        eterm::format("{xxx, [{a, 1}, {b, 2}, {c, 4}], \"abc\", {5,6,7}}"))); // N = 4
    BOOST_REQUIRE(!etm.match(
        eterm::format("{xxx, [1, 2, 3, {c, 4}], \"abc\", 5}")));

    // Verify number of successful matches for each pattern.
    BOOST_REQUIRE_EQUAL(2, cb.match[0]);
    BOOST_REQUIRE_EQUAL(1, cb.match[1]);
    BOOST_REQUIRE_EQUAL(1, cb.match[2]);
    BOOST_REQUIRE_EQUAL(1, cb.match[3]);

    // Make sure we registered 4 pattern above.
    BOOST_REQUIRE_EQUAL(4, etm.size());

    // Test pattern deletion.
    etm.erase(action);
    // Make sure we registered 4 pattern above.
    BOOST_REQUIRE_EQUAL(3, etm.size());
}

static void run(int n)
{
    static const int iterations = ::getenv("ITERATIONS") ? atoi(::getenv("ITERATIONS")) : 1;
    static cb_t cb;
    static allocator_t alloc;
    static eterm_pattern_matcher::init_struct list[] = {
        { eterm::format(alloc, "{ok, N, A}"),     1 },
        { eterm::format(alloc, "{error, N, B}"),  2 },
        { eterm(atom("some_atom")),               3 },
        { eterm(12345),                           4 }
    };

    static eterm_pattern_matcher sp(list, boost::ref(cb), alloc);

    BOOST_REQUIRE_EQUAL(n == 1 ? 4 : 3, sp.size());

    for(int i=0; i < iterations; i++) {
        BOOST_REQUIRE(!sp.match(eterm::format(alloc, "{ok, 1, 3, 4}")));
        BOOST_REQUIRE_EQUAL((n == 1), sp.match(eterm::format(alloc, "{ok, 1, 2}")));   // N = 1

        BOOST_REQUIRE(sp.match(eterm::format(alloc, "{error, 2, not_found}"))); // N = 2
        BOOST_REQUIRE(!sp.match(eterm::format(alloc, "{test, 3}")));
    }

    // Verify number of successful matches for each pattern.
    BOOST_REQUIRE_EQUAL(iterations,   cb.match[0]);
    BOOST_REQUIRE_EQUAL(n*iterations, cb.match[1]);

    if (n == 1) {
        sp.erase(sp.front());
        BOOST_REQUIRE_EQUAL(3, sp.size());   // N = 1
    }
}

BOOST_AUTO_TEST_CASE( test_match_static )
{
    run(1);
    run(2);
    run(3);
}

BOOST_AUTO_TEST_CASE( test_subst )
{
    eterm p = eterm::format("{perc, ID, List}");
    varbind binding;
    binding.bind("ID", eterm(123));
    list t(4);
    t.push_back(eterm(4));
    t.push_back(eterm(2.0));
    t.push_back(eterm(string("test")));
    t.push_back(eterm(atom("abcd")));
    t.close();
    binding.bind("List", eterm(t));

    eterm p1;
    BOOST_REQUIRE(p.subst(p1, &binding));
    BOOST_REQUIRE_EQUAL("{perc,123,[4,2.0,\"test\",abcd]}", p1.to_string());
}

BOOST_AUTO_TEST_CASE( test_match_list )
{
    const eterm data = eterm::format("{add_symbols, ['QQQQ', 'IBM']}");
    static eterm s_set_status  = eterm::format("{set_status,  I}");
    static eterm s_add_symbols = eterm::format("{add_symbols, S}");

    eixx::varbind l_binding;

    BOOST_REQUIRE(s_set_status.match(data, &l_binding) == false);
    bool b = s_add_symbols.match(data, &l_binding);
    BOOST_REQUIRE(b);
}

BOOST_AUTO_TEST_CASE( test_match_list_tail )
{
    BOOST_WARN_MESSAGE(false, "SKIPPING test_match_list_tail - needs extension to list matching!");
    return;

    eterm pattern = eterm::format("[{A, O} | T]");
    eterm term    = eterm::format("[{a, 1}, {b, ok}, {c, 2.0}]");

    varbind vars;
    // Match [{a, 1} | T]
    BOOST_REQUIRE(pattern.match(term, &vars));
    const eterm* name  = vars.find("A");
    const eterm* value = vars.find("O");
    const eterm* tail  = vars.find("T");

    BOOST_REQUIRE(name  != NULL);
    BOOST_REQUIRE(value != NULL);
    BOOST_REQUIRE(tail  != NULL);

    BOOST_REQUIRE_EQUAL(ATOM,    name->type());
    BOOST_REQUIRE_EQUAL("a",     name->to_string());
    BOOST_REQUIRE_EQUAL(LONG,    value->type());
    BOOST_REQUIRE_EQUAL(LIST,    tail->type());

    // Match [{b, ok} | T]
    vars.clear();
    BOOST_REQUIRE(term.match(*tail, &vars));
    name  = vars.find("A");
    value = vars.find("O");
    tail  = vars.find("T");

    BOOST_REQUIRE(name  != NULL);
    BOOST_REQUIRE(value != NULL);
    BOOST_REQUIRE(tail  != NULL);

    BOOST_REQUIRE_EQUAL(ATOM,    name->type());
    BOOST_REQUIRE_EQUAL("b",     name->to_string());
    BOOST_REQUIRE_EQUAL(ATOM,    value->type());
    BOOST_REQUIRE_EQUAL(LIST,    tail->type());

    // Match [{c, 2.0} | T]
    vars.clear();
    BOOST_REQUIRE(term.match(*tail, &vars));
    name  = vars.find("A");
    value = vars.find("O");
    tail  = vars.find("T");

    BOOST_REQUIRE(name  != NULL);
    BOOST_REQUIRE(value != NULL);
    BOOST_REQUIRE(tail  != NULL);

    BOOST_REQUIRE_EQUAL(ATOM,    name->type());
    BOOST_REQUIRE_EQUAL("c",     name->to_string());
    BOOST_REQUIRE_EQUAL(DOUBLE,  value->type());
    BOOST_REQUIRE_EQUAL(LIST,    tail->type());
}


// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/TypeInfer.h"

#include "Fixture.h"

#include "doctest.h"

#include <algorithm>

LUAU_FASTFLAG(LuauLowerBoundsCalculation)

using namespace Luau;

TEST_SUITE_BEGIN("ProvisionalTests");

// These tests check for behavior that differs from the final behavior we'd
// like to have.  They serve to document the current state of the typechecker.
// When making future improvements, its very likely these tests will break and
// will need to be replaced.

/*
 * This test falls into a sort of "do as I say" pit of consequences:
 * Technically, the type of the type() function is <T>(T) -> string
 *
 * We thus infer that the argument to f is a free type.
 * While we can still learn something about this argument, we can't seem to infer a union for it.
 *
 * Is this good?  Maybe not, but I'm not sure what else we should do.
 */
TEST_CASE_FIXTURE(Fixture, "typeguard_inference_incomplete")
{
    const std::string code = R"(
        function f(a)
            if type(a) == "boolean" then
                local a1 = a
            elseif a.fn() then
                local a2 = a
            end
        end
    )";

    const std::string expected = R"(
        function f(a:{fn:()->(a,b...)}): ()
            if type(a) == 'boolean'then
                local a1:boolean=a
            elseif a.fn()then
                local a2:{fn:()->(a,b...)}=a
            end
        end
    )";

    CHECK_EQ(expected, decorateWithTypes(code));
}

TEST_CASE_FIXTURE(BuiltinsFixture, "xpcall_returns_what_f_returns")
{
    const std::string code = R"(
        local a, b, c = xpcall(function() return 1, "foo" end, function() return "foo", 1 end)
    )";

    const std::string expected = R"(
        local a:boolean,b:number,c:string=xpcall(function(): (number,string)return 1,'foo'end,function(): (string,number)return'foo',1 end)
    )";

    CHECK_EQ(expected, decorateWithTypes(code));
}

// We had a bug where if you have two type packs that looks like:
//   { x, y }, ...
//   { x }, ...
// It would infinitely grow the type pack because one WeirdIter is trying to catch up, but can't.
// However, the following snippet is supposed to generate an OccursCheckFailed, but it doesn't.
TEST_CASE_FIXTURE(Fixture, "weirditer_should_not_loop_forever")
{
    // this flag is intentionally here doing nothing to demonstrate that we exit early via case detection
    ScopedFastInt sfis{"LuauTypeInferTypePackLoopLimit", 50};

    CheckResult result = check(R"(
        local function toVertexList(vertices, x, y, ...)
            if not (x and y) then return vertices end  -- no more arguments
            vertices[#vertices + 1] = {x = x, y = y}   -- set vertex
            return toVertexList(vertices, ...)         -- recurse
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

// This should also generate an OccursCheckFailed error too, like the above toVertexList snippet.
// at least up until we can get Luau to recognize this code as a valid function that iterates over a list of values in the pack.
TEST_CASE_FIXTURE(Fixture, "it_should_be_agnostic_of_actual_size")
{
    CheckResult result = check(R"(
        local function f(x, y, ...)
            if not y then return x end
            return f(x, ...)
        end

        f(3, 2, 1, 0)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

// Ideally setmetatable's second argument would be an optional free table.
// For now, infer it as just a free table.
TEST_CASE_FIXTURE(BuiltinsFixture, "setmetatable_constrains_free_type_into_free_table")
{
    CheckResult result = check(R"(
        local a = {}
        local b
        setmetatable(a, b)
        b = 1
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    TypeMismatch* tm = get<TypeMismatch>(result.errors[0]);
    REQUIRE(tm);
    CHECK_EQ("{-  -}", toString(tm->wantedType));
    CHECK_EQ("number", toString(tm->givenType));
}

// Luau currently doesn't yet know how to allow assignments when the binding was refined.
TEST_CASE_FIXTURE(Fixture, "while_body_are_also_refined")
{
    CheckResult result = check(R"(
        type Node<T> = { value: T, child: Node<T>? }

        local function visitor<T>(node: Node<T>, f: (T) -> ())
            local current = node

            while current do
                f(current.value)
                current = current.child -- TODO: Can't work just yet. It thinks 'current' can never be nil. :(
            end
        end
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    CHECK_EQ("Type 'Node<T>?' could not be converted into 'Node<T>'", toString(result.errors[0]));
}

// Originally from TypeInfer.test.cpp.
// I dont think type checking the metamethod at every site of == is the correct thing to do.
// We should be type checking the metamethod at the call site of setmetatable.
TEST_CASE_FIXTURE(BuiltinsFixture, "error_on_eq_metamethod_returning_a_type_other_than_boolean")
{
    CheckResult result = check(R"(
        local tab = {a = 1}
        setmetatable(tab, {__eq = function(a, b): number
            return 1
        end})
        local tab2 = tab

        local a = tab2 == tab
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    GenericError* ge = get<GenericError>(result.errors[0]);
    REQUIRE(ge);
    CHECK_EQ("Metamethod '__eq' must return type 'boolean'", ge->message);
}

// Requires success typing to confidently determine that this expression has no overlap.
TEST_CASE_FIXTURE(Fixture, "operator_eq_completely_incompatible")
{
    CheckResult result = check(R"(
        local a: string | number = "hi"
        local b: {x: string}? = {x = "bye"}

        local r1 = a == b
        local r2 = b == a
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

// Belongs in TypeInfer.refinements.test.cpp.
// We'll need to not only report an error on `a == b`, but also to refine both operands as `never` in the `==` branch.
TEST_CASE_FIXTURE(Fixture, "lvalue_equals_another_lvalue_with_no_overlap")
{
    CheckResult result = check(R"(
        local function f(a: string, b: boolean?)
            if a == b then
                local foo, bar = a, b
            else
                local foo, bar = a, b
            end
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);

    CHECK_EQ(toString(requireTypeAtPosition({3, 33})), "string");   // a == b
    CHECK_EQ(toString(requireTypeAtPosition({3, 36})), "boolean?"); // a == b

    CHECK_EQ(toString(requireTypeAtPosition({5, 33})), "string");   // a ~= b
    CHECK_EQ(toString(requireTypeAtPosition({5, 36})), "boolean?"); // a ~= b
}

// Also belongs in TypeInfer.refinements.test.cpp.
// Just needs to fully support equality refinement. Which is annoying without type states.
TEST_CASE_FIXTURE(Fixture, "discriminate_from_x_not_equal_to_nil")
{
    CheckResult result = check(R"(
        type T = {x: string, y: number} | {x: nil, y: nil}

        local function f(t: T)
            if t.x ~= nil then
                local foo = t
            else
                local bar = t
            end
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);

    CHECK_EQ("{| x: string, y: number |}", toString(requireTypeAtPosition({5, 28})));

    // Should be {| x: nil, y: nil |}
    CHECK_EQ("{| x: nil, y: nil |} | {| x: string, y: number |}", toString(requireTypeAtPosition({7, 28})));
}

TEST_CASE_FIXTURE(Fixture, "bail_early_if_unification_is_too_complicated" * doctest::timeout(0.5))
{
    ScopedFastInt sffi{"LuauTarjanChildLimit", 1};
    ScopedFastInt sffi2{"LuauTypeInferIterationLimit", 1};

    CheckResult result = check(R"LUA(
        local Result
        Result = setmetatable({}, {})
        Result.__index = Result
        function Result.new(okValue)
            local self = setmetatable({}, Result)
            self:constructor(okValue)
            return self
        end
        function Result:constructor(okValue)
            self.okValue = okValue
        end
        function Result:ok(val) return Result.new(val) end
        function Result:a(p0, p1, p2, p3, p4) return Result.new((self.okValue)) or p0 or p1 or p2 or p3 or p4 end
        function Result:b(p0, p1, p2, p3, p4) return Result:ok((self.okValue)) or p0 or p1 or p2 or p3 or p4 end
        function Result:c(p0, p1, p2, p3, p4) return Result:ok((self.okValue)) or p0 or p1 or p2 or p3 or p4 end
        function Result:transpose(a)
            return a and self.okValue:z(function(some)
                return Result:ok(some)
            end) or Result:ok(self.okValue)
        end
    )LUA");

    auto it = std::find_if(result.errors.begin(), result.errors.end(), [](TypeError& a) {
        return nullptr != get<UnificationTooComplex>(a);
    });
    if (it == result.errors.end())
    {
        dumpErrors(result);
        FAIL("Expected a UnificationTooComplex error");
    }
}

// Should be in TypeInfer.tables.test.cpp
// It's unsound to instantiate tables containing generic methods,
// since mutating properties means table properties should be invariant.
// We currently allow this but we shouldn't!
TEST_CASE_FIXTURE(Fixture, "invariant_table_properties_means_instantiating_tables_in_call_is_unsound")
{
    CheckResult result = check(R"(
        --!strict
        local t = {}
        function t.m(x) return x end
        local a : string = t.m("hi")
        local b : number = t.m(5)
        function f(x : { m : (number)->number })
            x.m = function(x) return 1+x end
        end
        f(t) -- This shouldn't typecheck
        local c : string = t.m("hi")
    )");

    // TODO: this should error!
    // This should be fixed by replacing generic tables by generics with type bounds.
    LUAU_REQUIRE_NO_ERRORS(result);
}

// FIXME: Move this test to another source file when removing FFlag::LuauLowerBoundsCalculation
TEST_CASE_FIXTURE(Fixture, "do_not_ice_when_trying_to_pick_first_of_generic_type_pack")
{
    ScopedFastFlag sff[]{
        {"LuauReturnAnyInsteadOfICE", true},
    };

    // In-place quantification causes these types to have the wrong types but only because of nasty interaction with prototyping.
    // The type of f is initially () -> free1...
    // Then the prototype iterator advances, and checks the function expression assigned to g, which has the type () -> free2...
    // In the body it calls f and returns what f() returns. This binds free2... with free1..., causing f and g to have same types.
    // We then quantify g, leaving it with the final type <a...>() -> a...
    // Because free1... and free2... were bound, in combination with in-place quantification, f's return type was also turned into a...
    // Then the check iterator catches up, and checks the body of f, and attempts to quantify it too.
    // Alas, one of the requirements for quantification is that a type must contain free types. () -> a... has no free types.
    // Thus the quantification for f was no-op, which explains why f does not have any type parameters.
    // Calling f() will attempt to instantiate the function type, which turns generics in type binders into to free types.
    // However, instantiations only converts generics contained within the type binders of a function, so instantiation was also no-op.
    // Which means that calling f() simply returned a... rather than an instantiation of it. And since the call site was not in tail position,
    // picking first element in a... triggers an ICE because calls returning generic packs are unexpected.
    CheckResult result = check(R"(
        local function f() end

        local g = function() return f() end

        local x = (f()) -- should error: no return values to assign from the call to f
    )");

    LUAU_REQUIRE_NO_ERRORS(result);

    if (FFlag::LuauLowerBoundsCalculation)
    {
        CHECK_EQ("() -> ()", toString(requireType("f")));
        CHECK_EQ("() -> ()", toString(requireType("g")));
        CHECK_EQ("nil", toString(requireType("x")));
    }
    else
    {
        // f and g should have the type () -> ()
        CHECK_EQ("() -> (a...)", toString(requireType("f")));
        CHECK_EQ("<a...>() -> (a...)", toString(requireType("g")));
        CHECK_EQ("any", toString(requireType("x"))); // any is returned instead of ICE for now
    }
}

TEST_CASE_FIXTURE(Fixture, "specialization_binds_with_prototypes_too_early")
{
    CheckResult result = check(R"(
        local function id(x) return x end
        local n2n: (number) -> number = id
        local s2s: (string) -> string = id
    )");

    LUAU_REQUIRE_ERRORS(result); // Should not have any errors.
}

TEST_CASE_FIXTURE(Fixture, "weird_fail_to_unify_variadic_pack")
{
    ScopedFastFlag sff[] = {
        {"LuauLowerBoundsCalculation", false},
    };

    CheckResult result = check(R"(
        --!strict
        local function f(...) return ... end
        local g = function(...) return f(...) end
    )");

    LUAU_REQUIRE_ERRORS(result); // Should not have any errors.
}

TEST_CASE_FIXTURE(Fixture, "lower_bounds_calculation_is_too_permissive_with_overloaded_higher_order_functions")
{
    ScopedFastFlag sff[] = {
        {"LuauLowerBoundsCalculation", true},
    };

    CheckResult result = check(R"(
        function foo(f)
            f(5, 'a')
            f('b', 6)
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);

    // We incorrectly infer that the argument to foo could be called with (number, number) or (string, string)
    // even though that is strictly more permissive than the actual source text shows.
    CHECK("<a...>((number | string, number | string) -> (a...)) -> ()" == toString(requireType("foo")));
}

// Once fixed, move this to Normalize.test.cpp
TEST_CASE_FIXTURE(Fixture, "normalization_fails_on_certain_kinds_of_cyclic_tables")
{
#if defined(_DEBUG) || defined(_NOOPT)
    ScopedFastInt sfi("LuauNormalizeIterationLimit", 500);
#endif

    ScopedFastFlag flags[] = {
        {"LuauLowerBoundsCalculation", true},
    };

    // We use a function and inferred parameter types to prevent intermediate normalizations from being performed.
    // This exposes a bug where the type of y is mutated.
    CheckResult result = check(R"(
        function strange(x, y)
            x.x = y
            y.x = x

            type R = {x: typeof(x)} & {x: typeof(y)}
            local r: R

            return r
        end
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);

    CHECK(nullptr != get<NormalizationTooComplex>(result.errors[0]));
}

// Belongs in TypeInfer.builtins.test.cpp.
TEST_CASE_FIXTURE(BuiltinsFixture, "pcall_returns_at_least_two_value_but_function_returns_nothing")
{
    CheckResult result = check(R"(
        local function f(): () end
        local ok, res = pcall(f)
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
    CHECK_EQ("Function only returns 1 value. 2 are required here", toString(result.errors[0]));
    // LUAU_REQUIRE_NO_ERRORS(result);
    // CHECK_EQ("boolean", toString(requireType("ok")));
    // CHECK_EQ("any", toString(requireType("res")));
}

// Belongs in TypeInfer.builtins.test.cpp.
TEST_CASE_FIXTURE(BuiltinsFixture, "choose_the_right_overload_for_pcall")
{
    CheckResult result = check(R"(
        local function f(): number
            if math.random() > 0.5 then
                return 5
            else
                error("something")
            end
        end

        local ok, res = pcall(f)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
    CHECK_EQ("boolean", toString(requireType("ok")));
    CHECK_EQ("number", toString(requireType("res")));
    // CHECK_EQ("any", toString(requireType("res")));
}

// Belongs in TypeInfer.builtins.test.cpp.
TEST_CASE_FIXTURE(BuiltinsFixture, "function_returns_many_things_but_first_of_it_is_forgotten")
{
    CheckResult result = check(R"(
        local function f(): (number, string, boolean)
            if math.random() > 0.5 then
                return 5, "hello", true
            else
                error("something")
            end
        end

        local ok, res, s, b = pcall(f)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
    CHECK_EQ("boolean", toString(requireType("ok")));
    CHECK_EQ("number", toString(requireType("res")));
    // CHECK_EQ("any", toString(requireType("res")));
    CHECK_EQ("string", toString(requireType("s")));
    CHECK_EQ("boolean", toString(requireType("b")));
}

TEST_CASE_FIXTURE(Fixture, "constrained_is_level_dependent")
{
    ScopedFastFlag sff[]{
        {"LuauLowerBoundsCalculation", true},
        {"LuauNormalizeFlagIsConservative", true},
        {"LuauQuantifyConstrained", true},
    };

    CheckResult result = check(R"(
        local function f(o)
            local t = {}
            t[o] = true

            local function foo(o)
                o:m1()
                t[o] = nil
            end

            local function bar(o)
                o:m2()
                t[o] = true
            end

            return t
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
    // TODO: We're missing generics b...
    CHECK_EQ("<a...>(t1) -> {| [t1]: boolean |} where t1 = t2 ; t2 = {+ m1: (t1) -> (a...), m2: (t2) -> (b...) +}", toString(requireType("f")));
}

TEST_CASE_FIXTURE(BuiltinsFixture, "greedy_inference_with_shared_self_triggers_function_with_no_returns")
{
    ScopedFastFlag sff{"DebugLuauSharedSelf", true};

    CheckResult result = check(R"(
        local T = {}
        T.__index = T

        function T.new()
            local self = setmetatable({}, T)
            return self:ctor() or self
        end

        function T:ctor()
            -- oops, no return!
        end
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
    CHECK_EQ("Not all codepaths in this function return '{ @metatable T, {|  |} }, a...'.", toString(result.errors[0]));
}

TEST_SUITE_END();

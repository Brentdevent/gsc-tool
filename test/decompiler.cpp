// Copyright 2026 xensik. All rights reserved.
//
// Use of this source code is governed by a GNU GPLv3 license
// that can be found in the LICENSE file.

#include "common.hpp"
#include "xsk/gsc/engine/h2.hpp"
#include "xsk/gsc/engine/iw5_ps.hpp"
#include "xsk/gsc/engine/iw7.hpp"
#include "xsk/gsc/engine/iw8.hpp"
#include "xsk/gsc/engine/iw9.hpp"
#include "xsk/gsc/engine/s1_pc.hpp"
#include "xsk/gsc/engine/s1_xb.hpp"
#include "xsk/gsc/engine/s2.hpp"
#include "xsk/gsc/engine/s4.hpp"
#include <catch_amalgamated.hpp>

namespace xsk::test
{

// The IW6 console contexts cannot be constructed: their method tables declare
// 1066 entries but initialize only 1050, leaving null names in the tables.
#define GSC_CONTEXTS gsc::iw5_pc::context, gsc::iw5_ps::context, gsc::iw5_xb::context, gsc::iw6_pc::context, gsc::iw7::context, gsc::iw8::context, gsc::iw9::context, gsc::s1_pc::context, gsc::s1_ps::context, gsc::s1_xb::context, gsc::s2::context, gsc::s4::context, gsc::h1::context, gsc::h2::context
#define WAITFRAME_CONTEXTS gsc::iw8::context, gsc::iw9::context, gsc::s1_pc::context, gsc::s1_ps::context, gsc::s1_xb::context, gsc::s2::context, gsc::s4::context, gsc::h1::context, gsc::h2::context

namespace
{

template <typename Ctx>
auto round_trip(std::string_view source) -> std::string
{
    auto c = Ctx{ gsc::instance::server };
    c.init(gsc::build::prod, {});
    auto input = std::vector<u8>{ source.begin(), source.end() };
    auto assembly = c.compiler().compile("regression.gsc", input);
    auto const [script, stack, map] = c.assembler().assemble(*assembly);
    auto const original_script = std::vector<u8>{ script.data, script.data + script.size };
    auto const original_stack = std::vector<u8>{ stack.data, stack.data + stack.size };
    auto disassembly = c.disassembler().disassemble(original_script, original_stack);
    auto ast = c.decompiler().decompile(*disassembly);
    auto text = c.printer().print(*ast);
    auto const output = std::string{ text.begin(), text.end() };
    INFO(output);
    auto recompiled = c.compiler().compile("regression.gsc", text);
    auto const [new_script, new_stack, new_map] = c.assembler().assemble(*recompiled);
    REQUIRE(std::vector<u8>(new_script.data, new_script.data + new_script.size) == original_script);
    REQUIRE(std::vector<u8>(new_stack.data, new_stack.data + new_stack.size) == original_stack);
    return output;
}

} // namespace

TEMPLATE_TEST_CASE("decompiler: foreach keeps its compiler temporaries", "[decompiler][gsc]", GSC_CONTEXTS)
{
    SECTION("last statement in a function")
    {
        auto const out = round_trip<TestType>(R"(
main(items)
{
    foreach (item in items)
    {
        self notify("item", item);
    }
}
)");
        REQUIRE(out.find("foreach") != std::string::npos);
    }
    SECTION("nested loops with break and continue")
    {
        round_trip<TestType>(R"(
main(items)
{
    foreach (item in items)
    {
        if (!isdefined(item)) continue;
        foreach (child in item.children)
        {
            if (child.done) break;
            self notify("child", child);
        }
    }
    self notify("done");
}
)");
    }
    SECTION("return from a loop inside a condition")
    {
        round_trip<TestType>(R"(
main(items, enabled)
{
    if (enabled)
    {
        foreach (item in items)
        {
            if (item.ready) return item;
        }
    }
    return undefined;
}
)");
    }
}

TEMPLATE_TEST_CASE("decompiler: manual array iteration is not foreach", "[decompiler][gsc]", GSC_CONTEXTS)
{
    SECTION("manual loop with no container temporary")
    {
        auto const out = round_trip<TestType>(R"(
main(items)
{
    key = getfirstarraykey(items);
    while (isdefined(key))
    {
        self notify("key", key);
        key = getnextarraykey(items, key);
    }
}
)");
        REQUIRE(out.find("foreach") == std::string::npos);
    }
    SECTION("manual outer loop with a real foreach inside")
    {
        // Reduced from bulge_p47: the outer loop iterates a level field manually,
        // while its body contains an actual foreach with compiler-owned temporaries.
        round_trip<TestType>(R"(
main()
{
    for (key = getfirstarraykey(level.items); isdefined(key); key = getnextarraykey(level.items, key))
    {
        foreach (item in level.items[key].children)
        {
            if (isdefined(item)) self notify("item", item);
        }
        wait 0.05;
    }
}
)");
    }
}

TEMPLATE_TEST_CASE("decompiler: nested continue after return", "[decompiler][gsc]", GSC_CONTEXTS)
{
    round_trip<TestType>(R"(
main(a, b)
{
    while (a)
    {
        if (b)
        {
            if (self.ready)
            {
                wait 0.05;
                if (!isdefined(self)) return;
                continue;
            }
            self notify("stop");
            return;
        }
        self notify("work");
        wait 0.05;
        if (!isdefined(self)) return;
    }
}
)");
}

TEMPLATE_TEST_CASE("decompiler: nested conditions share a continue boundary", "[decompiler][gsc]", GSC_CONTEXTS)
{
    SECTION("foreach and return")
    {
        round_trip<TestType>(R"(
main(items)
{
    foreach (item in items)
    {
        if (item.ready)
        {
            if (item.active) return item;
            else continue;
        }
    }
    return undefined;
}
)");
    }
    SECTION("break followed by continue")
    {
        round_trip<TestType>(R"(
main(a, b)
{
    for (;;)
    {
        wait 0.05;
        if (a)
        {
            if (b)
            {
                if (self.done) break;
                continue;
            }
        }
        self notify("reset");
    }
}
)");
    }
}

TEMPLATE_TEST_CASE("decompiler: waiting for iteration clause", "[decompiler][gsc]", GSC_CONTEXTS)
{
    SECTION("wait and nested continue")
    {
        auto const out = round_trip<TestType>(R"(
main(a, b)
{
    for (;; wait 0.05)
    {
        if (a)
        {
            self notify("update");
            if (b) continue;
        }
        if (self.done) break;
        self notify("work");
    }
}
)");
        REQUIRE(out.find("for (;; wait 0.05 )") != std::string::npos);
    }
    SECTION("a branch may skip a trailing wait")
    {
        round_trip<TestType>(R"(
main(a)
{
    for (;;)
    {
        self notify("work");
        if (a) self notify("update");
        else wait 0.05;
    }
}
)");
    }
    SECTION("continue skips a wait in the body")
    {
        round_trip<TestType>(R"(
main(a, b)
{
    for (;;)
    {
        if (a)
        {
            if (b) continue;
        }
        self notify("work");
        wait 0.05;
    }
}
)");
    }
    SECTION("an if else before a shared wait keeps its local lifetime")
    {
        round_trip<TestType>(R"(
main(a, b)
{
    for (;;)
    {
        if (a)
        {
            if (!b) b = 1;
            origin = self.origin;
            self.origin = origin;
        }
        else if (b) b = 0;
        wait 0.05;
    }
}
)");
    }
}

TEMPLATE_TEST_CASE("decompiler: real else branches at the continue boundary", "[decompiler][gsc]", GSC_CONTEXTS)
{
    SECTION("return before else at the end of a loop")
    {
        round_trip<TestType>(R"(
main(a, b)
{
    while (a)
    {
        if (b) return;
        else self notify("work");
    }
}
)");
    }
    SECTION("break before else in a nested condition")
    {
        round_trip<TestType>(R"(
main(a, b)
{
    for (;;)
    {
        wait 0.05;
        if (a)
        {
            if (b) break;
            else self notify("work");
        }
    }
}
)");
    }
}

TEMPLATE_TEST_CASE("decompiler: waitframe iteration clause", "[decompiler][gsc]", WAITFRAME_CONTEXTS)
{
    auto const out = round_trip<TestType>(R"(
main(a, b)
{
    for (;; waitframe())
    {
        if (a)
        {
            if (b) continue;
        }
        self notify("work");
    }
}
)");
    REQUIRE(out.find("for (;; waitframe();") == std::string::npos);
    REQUIRE(out.find("for (;; waitframe() )") != std::string::npos);
}

} // namespace xsk::test
